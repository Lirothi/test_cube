#include "editor/intent/EditorMcpServer.h"
#if WITH_EDITOR

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// winsock2 BEFORE windows.h, as in LlmClient.cpp: windows.h pulls in the original winsock and
// the two declare the same names differently.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <future>
#include <iterator>

#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "core/logging/Log.h"
#include "editor/EditorContext.h"
#include "editor/commands/EditorCommand.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/intent/EditorActionRegistry.h"
#include "editor/intent/EditorIntentResolver.h"
#include "editor/intent/EditorSceneQuery.h"
#include "editor/intent/IntentPrompt.h"
#include "editor/intent/IntentSchema.h"
#include "rendering/core/Screenshot.h"

namespace
{
    using nlohmann::json;
    using editormcp::ToolResult;

    constexpr float kPi = 3.14159265358979f;
    constexpr int kScreenshotSettleFrames = 6;

    ToolResult Error(std::string text)
    {
        ToolResult result;
        result.text = std::move(text);
        result.isError = true;
        return result;
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::string Base64(const std::string& bytes)
    {
        static const char* kTable =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((bytes.size() + 2) / 3 * 4);
        std::size_t i = 0;
        while (i + 2 < bytes.size())
        {
            const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) |
                (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                static_cast<unsigned char>(bytes[i + 2]);
            out += kTable[(v >> 18) & 63];
            out += kTable[(v >> 12) & 63];
            out += kTable[(v >> 6) & 63];
            out += kTable[v & 63];
            i += 3;
        }
        if (i < bytes.size())
        {
            unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
            if (i + 1 < bytes.size())
            {
                v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
            }
            out += kTable[(v >> 18) & 63];
            out += kTable[(v >> 12) & 63];
            out += i + 1 < bytes.size() ? kTable[(v >> 6) & 63] : '=';
            out += '=';
        }
        return out;
    }

    // THE LOCALHOST RULE, twice. Binding to 127.0.0.1 keeps other machines out; it does not keep
    // out a web page, because a browser on this machine can POST to 127.0.0.1 too -- and a
    // text/plain POST needs no CORS preflight, so "we never answer OPTIONS" is not a defence.
    // Browsers name the page in `Origin`, and a DNS-rebinding page shows up in `Host`. Anything
    // that is not this machine in either is refused. Claude Code sends neither a foreign Origin
    // nor a foreign Host, so it never notices this exists.
    bool IsLocalAuthority(const std::string& value)
    {
        std::string v = Lower(value);
        for (const char* scheme : { "http://", "https://" })
        {
            if (v.rfind(scheme, 0) == 0)
            {
                v.erase(0, std::char_traits<char>::length(scheme));
            }
        }
        const std::size_t colon = v.find(':');
        const std::string host = colon == std::string::npos ? v : v.substr(0, colon);
        return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
    }

    // ------------------------------------------------------------------ the screenshot handoff

    struct ShotRequest
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool pending = false;
        bool done = false;
        bool ok = false;
        int framesLeft = 0;
        unsigned maxWidth = 0;
        std::string path;
    };

    ShotRequest& Shot()
    {
        static ShotRequest request;
        return request;
    }

    std::string ShotPath()
    {
        wchar_t temp[MAX_PATH] = {};
        const DWORD length = ::GetTempPathW(MAX_PATH, temp);
        std::wstring wide = length > 0 ? std::wstring(temp, length) : std::wstring(L".\\");
        wide += L"test_cube_mcp_shot.png";
        const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string narrow(static_cast<std::size_t>(bytes), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
            narrow.data(), bytes, nullptr, nullptr);
        return narrow;
    }

    // Socket thread. Asks the frame loop for a capture and waits for it -- a few frames late on
    // purpose, so whatever the last call changed has been drawn and the temporal filters have
    // stopped smearing it.
    ToolResult TakeScreenshot(const json& args)
    {
        const int requested = args.is_object() ? args.value("maxWidth", 1280) : 1280;
        const unsigned maxWidth = static_cast<unsigned>(std::clamp(requested, 320, 3840));
        const std::string path = ShotPath();
        std::remove(path.c_str());

        ShotRequest& shot = Shot();
        {
            std::lock_guard<std::mutex> lock(shot.mutex);
            if (shot.pending)
            {
                return Error("A screenshot is already being taken; ask again when it returns.");
            }
            shot.pending = true;
            shot.done = false;
            shot.ok = false;
            shot.framesLeft = kScreenshotSettleFrames;
            shot.maxWidth = maxWidth;
            shot.path = path;
        }

        std::unique_lock<std::mutex> lock(shot.mutex);
        if (!shot.changed.wait_for(lock, std::chrono::seconds(15), [&] { return shot.done; }))
        {
            shot.pending = false;
            return Error("The editor did not present a frame for 15 s -- is the window "
                "minimised, or is the editor closed?");
        }
        const bool ok = shot.ok;
        lock.unlock();
        if (!ok)
        {
            return Error("The capture failed; the session log says why.");
        }

        std::ifstream file(path, std::ios::binary);
        std::string png((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        std::remove(path.c_str());
        if (png.size() < 24)
        {
            return Error("The capture produced no image.");
        }
        // Width and height straight from the PNG header, so the text says what the image is.
        const auto be32 = [&](std::size_t at)
        {
            return (static_cast<unsigned>(static_cast<unsigned char>(png[at])) << 24) |
                (static_cast<unsigned>(static_cast<unsigned char>(png[at + 1])) << 16) |
                (static_cast<unsigned>(static_cast<unsigned char>(png[at + 2])) << 8) |
                static_cast<unsigned>(static_cast<unsigned char>(png[at + 3]));
        };
        ToolResult result;
        result.text = "The whole editor window, " + std::to_string(be32(16)) + "x" +
            std::to_string(be32(20)) + ", captured " + std::to_string(kScreenshotSettleFrames) +
            " frames after this call arrived. The 3D view is the large pane in the middle; the "
            "panels around it are the editor's own.";
        result.pngBase64 = Base64(png);
        return result;
    }

    // ------------------------------------------------------------------ tools

    json ObjectSchema(json properties = json::object(), json required = json::array())
    {
        json schema = { { "type", "object" }, { "properties", std::move(properties) } };
        if (!required.empty())
        {
            schema["required"] = std::move(required);
        }
        return schema;
    }

    json Vec3Schema(const char* description)
    {
        return { { "type", "array" }, { "items", { { "type", "number" } } },
                 { "minItems", 3 }, { "maxItems", 3 }, { "description", description } };
    }

    const char* kCommandDescription =
        "One editor command, in exactly the shape the editor's own model emits: "
        "{\"action\": \"<registry id>\", \"target\": {...}, \"params\": {...}}. `kind` may be "
        "left out. The action ids, what each takes and the exact filter/asset/zone names THIS "
        "level uses are in editor_guide -- read it before the first call.";

    std::string CommandTitle(const std::string& tool, const EditorIntent& intent)
    {
        return intent.action.empty() ? tool : tool + " " + intent.action;
    }

    // What a camera looks like as numbers, the same way round the `--cam-rot` code reads it:
    // yaw = atan2(forward.x, forward.z), pitch = atan2(-forward.y, horizontal length).
    std::string CameraText(const Camera& camera)
    {
        const Math::float3& p = camera.GetPosition();
        const Math::float3& d = camera.GetDirection();
        char text[256];
        std::snprintf(text, sizeof(text),
            "{\"position\":[%.2f,%.2f,%.2f],\"yawDeg\":%.1f,\"pitchDeg\":%.1f,"
            "\"forward\":[%.3f,%.3f,%.3f]}",
            p.x, p.y, p.z, camera.GetYaw() * 180.0f / kPi, camera.GetPitch() * 180.0f / kPi,
            d.x, d.y, d.z);
        return text;
    }

    bool ReadVec3(const json& args, const char* key, Math::float3& out)
    {
        const auto it = args.find(key);
        if (it == args.end() || !it->is_array() || it->size() != 3)
        {
            return false;
        }
        for (const json& value : *it)
        {
            if (!value.is_number())
            {
                return false;
            }
        }
        out = Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
        return true;
    }
}

namespace editormcp
{
    namespace
    {
        ImportHost* g_importHost = nullptr; // frame thread only, like every document tool
    }

    void SetImportHost(ImportHost* host)
    {
        g_importHost = host;
    }

    json ToolList()
    {
        return json::array({
            {
                { "name", "editor_guide" },
                { "description",
                  "Everything the editor knows how to do and what the OPEN level contains: "
                  "every action with its parameters, every query, the exact filter/asset/zone/"
                  "setting names this level uses, and the grammar the local model is held to. "
                  "It is the local model's own system prompt, rebuilt from the live document, "
                  "so it is never stale. Read it first; read it again after anything that "
                  "adds names (a new zone, a spawn of a new kind, a rename)." },
                { "inputSchema", ObjectSchema() },
            },
            {
                { "name", "run_action" },
                { "description",
                  "Run ONE editor action on the open level: spawn, delete, move, rotate, "
                  "scale, randomizeRotation, group, rename, setColor, traceZone, createZone, "
                  "thin, align, distribute, snap, isolate, select, frame and the rest -- "
                  "editor_guide lists them all. It goes through the editor's own resolver, so "
                  "it is refused with a reason rather than half-applied, lands as ONE undo "
                  "entry, and answers with the same status line the person sees (\"Spawned 143 "
                  "... into Rocks; asked for 200; too crowded: ...\"). It shows up in the "
                  "person's command bar as it happens. It never saves the level; nothing here "
                  "can." },
                { "inputSchema", ObjectSchema({ { "command", {
                    { "type", "object" }, { "description", kCommandDescription } } } },
                    json::array({ "command" })) },
            },
            {
                { "name", "preview_action" },
                { "description",
                  "What run_action WOULD do, without doing it: how many objects the target "
                  "reaches and in which groups, or why it would be refused. Worth it before "
                  "anything destructive and for checking that a filter reaches what you "
                  "meant." },
                { "inputSchema", ObjectSchema({ { "command", {
                    { "type", "object" }, { "description", kCommandDescription } } } },
                    json::array({ "command" })) },
            },
            {
                { "name", "query" },
                { "description",
                  "Ask the level questions that change nothing -- bounds of a filter, the "
                  "water level, ground height at a point, what is selected, what the camera is "
                  "looking at, a summary of what is in the level. `ask` is a list, "
                  "[{\"query\": \"bounds\", \"target\": {\"filter\": [\"models/tent.mesh.json\"]}}, "
                  "{\"query\": \"waterLevel\"}], answered in one JSON object keyed by query. "
                  "editor_guide lists the queries and what each takes." },
                { "inputSchema", ObjectSchema({ { "ask", {
                    { "type", "array" }, { "items", { { "type", "object" } } },
                    { "description", "the questions, each {query, target?, params?}" } } } },
                    json::array({ "ask" })) },
            },
            {
                { "name", "get_camera" },
                { "description", "Where the editor's camera is and which way it faces." },
                { "inputSchema", ObjectSchema() },
            },
            {
                { "name", "set_camera" },
                { "description",
                  "Put the camera somewhere, so a screenshot shows what you want to look at. "
                  "Give `lookAt` to aim at a point (the easy way), or yawDeg/pitchDeg; with "
                  "neither it keeps its heading. It moves the person's view too, and they will "
                  "see it move -- so move it when you need to look, not for fun." },
                { "inputSchema", ObjectSchema({
                    { "position", Vec3Schema("world position [x, y, z]; y is up") },
                    { "lookAt", Vec3Schema("a world point to face") },
                    { "yawDeg", { { "type", "number" },
                        { "description", "heading, 0 = +z, 90 = +x" } } },
                    { "pitchDeg", { { "type", "number" },
                        { "description", "positive looks DOWN" } } } },
                    json::array({ "position" })) },
            },
            {
                { "name", "screenshot" },
                { "description",
                  "A picture of the editor window as it is now, a few frames after the call so "
                  "the last change is drawn. The way to see whether what you placed looks "
                  "right. Point the camera first with set_camera." },
                { "inputSchema", ObjectSchema({ { "maxWidth", { { "type", "integer" },
                    { "description", "downscale to at most this many pixels wide, 320-3840 "
                                     "(default 1280)" } } } }) },
            },
            {
                { "name", "undo" },
                { "description",
                  "Undo the newest entry on the editor's undo stack -- WHOEVER made it, you or "
                  "the person. The answer names what was undone. One entry per call, the same "
                  "as Ctrl+Z." },
                { "inputSchema", ObjectSchema() },
            },
            {
                { "name", "list_staging" },
                { "description",
                  "What import_staging/ holds, as the editor's Import window sees it: each "
                  "folder's name, kind (mesh / textureSet / skybox), triangle and material "
                  "summary, longest side in metres, how many top-level nodes it could be split "
                  "into, its license line, and whether it is already in the project." },
                { "inputSchema", ObjectSchema() },
            },
            {
                { "name", "import_asset" },
                { "description",
                  "Import one MESH folder from import_staging/ into the project, exactly as the "
                  "Import window's Import button does: textures to DDS, geometry baked with LODs, "
                  "models/<name>.mesh.json written, a CREDITS.md entry added. It WRITES FILES "
                  "(models/, data/materials/, CREDITS.md) and is not undoable -- only import what "
                  "the person asked for. It returns at once; the work runs in the background, "
                  "one import at a time, and finishes only while the Import window is open (this "
                  "opens it). Poll import_status. `targetSizeM` normalises the longest side to "
                  "that many metres inside the vertices (0 or absent keeps the source size -- "
                  "check longestSideM in list_staging first, Sketchfab sources are often in "
                  "centimetres); `split` makes one asset per top-level node, "
                  "models/<name>_node_<node>.mesh.json." },
                { "inputSchema", ObjectSchema({
                    { "name", { { "type", "string" },
                        { "description", "the folder name under import_staging/" } } },
                    { "targetSizeM", { { "type", "number" },
                        { "description", "longest side in metres after import; 0 = keep" } } },
                    { "split", { { "type", "boolean" },
                        { "description", "one asset per top-level node (default false)" } } } },
                    json::array({ "name" })) },
            },
            {
                { "name", "import_status" },
                { "description",
                  "Whether an import is running, its texture progress, the Import window's "
                  "status line (\"Imported <name> ...\" or \"Import FAILED ... see "
                  "asset_import.log\"), and the name of the last import that finished." },
                { "inputSchema", ObjectSchema() },
            },
        });
    }

    std::optional<json> HandleMessage(const json& message, ToolHost& host)
    {
        const auto reply = [&](json result) -> json
        {
            return { { "jsonrpc", "2.0" }, { "id", message.value("id", json()) },
                     { "result", std::move(result) } };
        };
        const auto fail = [&](int code, const std::string& text) -> json
        {
            return { { "jsonrpc", "2.0" },
                     { "id", message.is_object() ? message.value("id", json()) : json() },
                     { "error", { { "code", code }, { "message", text } } } };
        };

        if (!message.is_object())
        {
            return fail(-32600, "a JSON-RPC message is an object");
        }
        const std::string method = message.value("method", "");
        // No id: a notification -- `notifications/initialized` and friends -- which is answered
        // with nothing. No method: a reply to a request this server never sends.
        if (!message.contains("id") || method.empty())
        {
            return std::nullopt;
        }

        const json params = message.value("params", json::object());
        if (method == "initialize")
        {
            // Whatever version the client speaks: this server uses nothing but tools, which
            // every published revision has in the same shape.
            const std::string version = params.is_object()
                ? params.value("protocolVersion", std::string("2025-06-18"))
                : std::string("2025-06-18");
            return reply({
                { "protocolVersion", version },
                { "capabilities", { { "tools", { { "listChanged", false } } } } },
                { "serverInfo", { { "name", "test_cube-editor" }, { "version", "1.0" } } },
                { "instructions",
                  "This is the test_cube level editor, live. Call editor_guide first: it is the "
                  "complete list of what the editor can do and of the names the open level uses. "
                  "Then run_action / preview_action to edit, query to ask, set_camera + "
                  "screenshot to look. Every edit is one undo entry and appears in the person's "
                  "command bar. Nothing here saves the level -- that is the person's call." },
            });
        }
        if (method == "ping")
        {
            return reply(json::object());
        }
        if (method == "tools/list")
        {
            return reply({ { "tools", ToolList() } });
        }
        if (method == "tools/call")
        {
            const std::string name = params.is_object() ? params.value("name", "") : "";
            bool known = false;
            for (const json& tool : ToolList())
            {
                known = known || tool.value("name", "") == name;
            }
            if (!known)
            {
                return fail(-32602, "no tool called '" + name + "'");
            }
            const json arguments = params.is_object()
                ? params.value("arguments", json::object()) : json::object();
            const ToolResult result = host.Call(name, arguments);
            json content = json::array({ { { "type", "text" }, { "text", result.text } } });
            if (!result.pngBase64.empty())
            {
                content.push_back({ { "type", "image" }, { "data", result.pngBase64 },
                                    { "mimeType", "image/png" } });
            }
            return reply({ { "content", std::move(content) }, { "isError", result.isError } });
        }
        return fail(-32601, "no method '" + method + "'");
    }

    ToolResult RunDocumentTool(const EditorActionContext& actionCtx,
        EditorCommandStack& commandStack,
        const std::string& name,
        const json& arguments,
        CallRecord& outRecord)
    {
        outRecord = CallRecord{};
        outRecord.title = name;
        EditorContext& ctx = actionCtx.editor;

        if (name == "editor_guide")
        {
            const intentschema::Vocabulary vocabulary =
                intentschema::BuildVocabulary(ctx.document, actionCtx.assets);
            const std::string gbnf = intentschema::BuildGbnf(vocabulary);
            ToolResult result;
            result.text =
                "WHAT FOLLOWS IS THE EDITOR'S OWN MODEL'S SYSTEM PROMPT, built just now from the "
                "open level. The ACTIONS and QUERIES sections, the name lists and the grammar are "
                "the contract, and they are yours as they stand. The parts about thinking, "
                "answering in one JSON object per turn and choosing needs_api are written for a "
                "small local model and do not bind you: pass run_action the "
                "{action, target, params} object and read what comes back. Where the editor "
                "cannot do something, it is C++ in sources/editor/intent/EditorActionRegistry.cpp "
                "and can be added.\n\n" +
                intentprompt::BuildSystemPrompt(ctx.document, actionCtx.assets, vocabulary,
                    "Claude Code (over MCP)", gbnf);
            return result;
        }

        if (name == "run_action" || name == "preview_action")
        {
            const bool dryRun = name == "preview_action";
            json command = arguments.contains("command") ? arguments["command"] : arguments;
            if (!command.is_object())
            {
                outRecord.failed = true;
                return Error("`command` must be an object: {\"action\": ..., \"target\": ..., "
                    "\"params\": ...}");
            }
            if (!command.contains("kind"))
            {
                command["kind"] = "command";
            }
            EditorIntent intent;
            std::string error;
            if (!intentschema::ParseAnswer(command.dump(), intent, error))
            {
                outRecord.failed = true;
                outRecord.verdict = error;
                return Error("Could not read that command: " + error);
            }
            if (intent.kind != EditorIntentKind::Command)
            {
                outRecord.failed = true;
                return Error("run_action takes a command; questions go to `query`.");
            }
            intent.sourceLabel = "claude";
            outRecord.title = CommandTitle(name, intent);

            const EditorIntentPreview preview = BuildIntentPreview(actionCtx, intent);
            if (!preview.executable)
            {
                outRecord.failed = !dryRun;
                outRecord.verdict = preview.problem;
                return Error(preview.problem.empty() ? "Refused, with no reason given."
                                                     : preview.problem);
            }
            // A question dressed as an action (`count`) is answered by the preview itself;
            // "running" it would only say that there is nothing to run.
            if (preview.answerOnly || dryRun)
            {
                ToolResult result;
                result.text = (dryRun ? "Would run: " : "") + preview.summary;
                if (dryRun)
                {
                    result.text += " (" + std::to_string(preview.targets.size()) + " targets";
                    for (const EditorIntentPreview::Group& group : preview.groups)
                    {
                        result.text += ", " + group.label + " x" + std::to_string(group.count);
                    }
                    result.text += ")";
                }
                return result;
            }

            std::string status;
            const bool ran = ExecuteIntent(actionCtx, commandStack, preview, status);
            outRecord.action = intent.action;
            outRecord.verdict = status;
            outRecord.ran = ran;
            outRecord.failed = !ran;
            outRecord.show = true;
            ToolResult result;
            result.text = status.empty() ? preview.summary : status;
            result.isError = !ran;
            return result;
        }

        if (name == "query")
        {
            const json query = { { "kind", "query" },
                                 { "ask", arguments.value("ask", json::array()) } };
            EditorIntent intent;
            std::string error;
            if (!intentschema::ParseAnswer(query.dump(), intent, error) ||
                intent.kind != EditorIntentKind::Query)
            {
                return Error("Could not read those questions: " + error);
            }
            ToolResult result;
            result.text = editorquery::Answer(actionCtx, intent);
            return result;
        }

        if (name == "get_camera")
        {
            ToolResult result;
            result.text = CameraText(ctx.scene.CameraRef());
            return result;
        }

        if (name == "set_camera")
        {
            Math::float3 position;
            if (!ReadVec3(arguments, "position", position))
            {
                return Error("`position` must be [x, y, z].");
            }
            Camera& camera = ctx.scene.CameraRef();
            float yaw = camera.GetYaw();
            float pitch = camera.GetPitch();
            Math::float3 target;
            if (ReadVec3(arguments, "lookAt", target))
            {
                const Math::float3 d = target - position;
                const float flat = std::sqrt(d.x * d.x + d.z * d.z);
                if (flat < 1e-4f && std::fabs(d.y) < 1e-4f)
                {
                    return Error("`lookAt` is the camera's own position; there is nothing to face.");
                }
                yaw = std::atan2(d.x, d.z);
                pitch = std::atan2(-d.y, flat);
            }
            else
            {
                if (arguments.contains("yawDeg") && arguments["yawDeg"].is_number())
                {
                    yaw = arguments["yawDeg"].get<float>() * kPi / 180.0f;
                }
                if (arguments.contains("pitchDeg") && arguments["pitchDeg"].is_number())
                {
                    pitch = arguments["pitchDeg"].get<float>() * kPi / 180.0f;
                }
            }
            camera.SetPosition(position);
            camera.SetYawPitchRoll(yaw, pitch, 0.0f);
            camera.CalcMatrices(&ctx.renderer);
            // A jump, not a pan: the temporal filters must not blend the old view into the new.
            camera.ResetHistory();
            outRecord.verdict = "camera moved";
            outRecord.show = true;
            ToolResult result;
            result.text = CameraText(camera);
            return result;
        }

        if (name == "undo")
        {
            if (!commandStack.CanUndo())
            {
                return Error("Nothing to undo.");
            }
            const EditorCommand* newest = commandStack.HistoryEntry(commandStack.AppliedCount() - 1);
            const std::string label = newest ? std::string(newest->HistoryLabel()) : "an edit";
            commandStack.Undo(ctx);
            outRecord.verdict = "undid " + label;
            outRecord.ran = true;
            outRecord.show = true;
            ToolResult result;
            result.text = "Undid: " + label;
            return result;
        }

        if (name == "list_staging" || name == "import_asset" || name == "import_status")
        {
            if (!g_importHost)
            {
                return Error("There is no Import window here to import with.");
            }
            ToolResult result;
            if (name == "list_staging")
            {
                result.text = g_importHost->Staging();
                return result;
            }
            if (name == "import_status")
            {
                result.text = g_importHost->Status();
                return result;
            }
            const std::string asset = arguments.value("name", std::string());
            if (asset.empty())
            {
                return Error("import_asset needs `name`, a folder under import_staging/.");
            }
            const float targetSizeM = arguments.contains("targetSizeM") &&
                arguments["targetSizeM"].is_number() ? arguments["targetSizeM"].get<float>() : 0.0f;
            const bool split = arguments.value("split", false);
            std::string text;
            const bool started = g_importHost->Start(asset, targetSizeM, split, text);
            outRecord.title = "import_asset " + asset;
            outRecord.verdict = text;
            outRecord.failed = !started;
            outRecord.show = true;
            result.text = text;
            result.isError = !started;
            return result;
        }

        return Error("'" + name + "' is not a document tool.");
    }

    // ------------------------------------------------------------------ the server

    struct Server::Job
    {
        std::string name;
        json arguments;
        std::promise<ToolResult> done;
        std::atomic<bool> abandoned{ false };
    };

    class Server::QueueHost final : public ToolHost
    {
    public:
        explicit QueueHost(Server& server) : server_(server) {}

        ToolResult Call(const std::string& name, const json& arguments) override
        {
            ++server_.callCount_;
            {
                std::lock_guard<std::mutex> lock(server_.lastCallMutex_);
                server_.lastCall_ = name;
            }
            LOG_INFO(logging::LogCategory::Editor, "MCP: tools/call {}", name);

            if (name == "screenshot")
            {
                return TakeScreenshot(arguments);
            }

            auto job = std::make_shared<Job>();
            job->name = name;
            job->arguments = arguments;
            std::future<ToolResult> future = job->done.get_future();
            {
                std::lock_guard<std::mutex> lock(server_.jobsMutex_);
                server_.jobs_.push_back(job);
            }
            // The frame thread picks it up within a frame. Minutes would mean the editor is
            // not drawing at all -- closed, or its window not updating -- and the call is then
            // withdrawn rather than left to run whenever it wakes, long after anyone waited.
            if (future.wait_for(std::chrono::seconds(60)) != std::future_status::ready)
            {
                job->abandoned = true;
                return Error("The editor did not take this call within 60 s -- is the Level "
                    "Editor open? It was withdrawn, unless it had already started.");
            }
            return future.get();
        }

    private:
        Server& server_;
    };

    Server::Server() = default;

    Server::~Server()
    {
        Stop();
    }

    bool Server::Start(int port, std::string& outError)
    {
        if (running_)
        {
            return true;
        }
        static const bool winsockReady = []
        {
            WSADATA data{};
            return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        if (!winsockReady)
        {
            outError = "Winsock would not start";
            return false;
        }

        const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET)
        {
            outError = "could not open a socket (error " + std::to_string(::WSAGetLastError()) + ")";
            return false;
        }
        // EXCLUSIVE, not reusable: a second editor must fail to bind -- loudly, in its panel --
        // rather than quietly share the port and leave it to chance which one Claude reaches.
        BOOL exclusive = TRUE;
        ::setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<u_short>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        {
            const int code = ::WSAGetLastError();
            ::closesocket(listener);
            outError = code == WSAEADDRINUSE || code == WSAEACCES
                ? "port " + std::to_string(port) + " is taken -- another editor is probably "
                  "serving MCP there already"
                : "could not bind 127.0.0.1:" + std::to_string(port) + " (error " +
                  std::to_string(code) + ")";
            return false;
        }
        if (::listen(listener, 8) != 0)
        {
            outError = "could not listen (error " + std::to_string(::WSAGetLastError()) + ")";
            ::closesocket(listener);
            return false;
        }

        listenSocket_ = static_cast<std::uintptr_t>(listener);
        port_ = port;
        host_ = std::make_unique<QueueHost>(*this);
        stopping_ = false;
        running_ = true;
        thread_ = std::thread([this] { AcceptLoop(); });
        LOG_INFO(logging::LogCategory::Editor,
            "MCP: listening on http://127.0.0.1:{}/mcp -- Claude Code reaches the editor here",
            port);
        return true;
    }

    void Server::Stop()
    {
        if (!running_)
        {
            return;
        }
        stopping_ = true;

        // Everyone waiting is told first, so the socket thread is not sitting on a future
        // for a minute while this waits to join it.
        {
            std::lock_guard<std::mutex> lock(jobsMutex_);
            for (const std::shared_ptr<Job>& job : jobs_)
            {
                job->done.set_value(Error("The editor is closing."));
            }
            jobs_.clear();
        }
        {
            ShotRequest& shot = Shot();
            std::lock_guard<std::mutex> lock(shot.mutex);
            if (shot.pending)
            {
                shot.ok = false;
                shot.done = true;
                shot.pending = false;
            }
        }
        Shot().changed.notify_all();

        // Closing the listener is what makes a blocked accept() return; shutting the current
        // client does the same for a recv() halfway through a request.
        ::closesocket(static_cast<SOCKET>(listenSocket_));
        const std::uintptr_t client = currentClient_.load();
        if (client != ~static_cast<std::uintptr_t>(0))
        {
            ::shutdown(static_cast<SOCKET>(client), SD_BOTH);
        }
        if (thread_.joinable())
        {
            thread_.join();
        }
        running_ = false;
        LOG_INFO(logging::LogCategory::Editor, "MCP: stopped listening on port {}", port_);
    }

    std::string Server::LastCall() const
    {
        std::lock_guard<std::mutex> lock(lastCallMutex_);
        return lastCall_;
    }

    std::vector<CallRecord> Server::Service(const EditorActionContext& actionCtx,
        EditorCommandStack& commandStack)
    {
        std::vector<std::shared_ptr<Job>> taken;
        {
            std::lock_guard<std::mutex> lock(jobsMutex_);
            taken.swap(jobs_);
        }
        std::vector<CallRecord> records;
        for (const std::shared_ptr<Job>& job : taken)
        {
            if (job->abandoned)
            {
                continue;
            }
            CallRecord record;
            ToolResult result;
            // A malformed argument surfaces as a json exception from deep inside a reader; it
            // is the caller's mistake and must come back as an answer, not take the editor down.
            try
            {
                result = RunDocumentTool(actionCtx, commandStack, job->name, job->arguments,
                    record);
            }
            catch (const std::exception& e)
            {
                result = Error(std::string("That call threw: ") + e.what());
                record.failed = true;
                record.verdict = result.text;
            }
            records.push_back(record);
            job->done.set_value(std::move(result));
        }
        return records;
    }

    void Server::AcceptLoop()
    {
        logging::SetCurrentThreadName("EditorMcp");
        const SOCKET listener = static_cast<SOCKET>(listenSocket_);
        while (!stopping_)
        {
            const SOCKET client = ::accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET)
            {
                if (stopping_)
                {
                    break;
                }
                ::Sleep(10);
                continue;
            }
            currentClient_ = static_cast<std::uintptr_t>(client);
            HandleConnection(static_cast<std::uintptr_t>(client));
            currentClient_ = ~static_cast<std::uintptr_t>(0);

            // A GRACEFUL close, not a bare closesocket. A refusal (403, 404, 405) is sent
            // before the request body has been read, and closing a socket with unread bytes
            // in it makes Windows send a RESET -- which the client sees as "connection
            // dropped" before it ever reads the refusal. Measured: a foreign Origin came back
            // as a network error instead of 403. So: say we are done sending, swallow what is
            // still arriving for a moment, and only then close.
            ::shutdown(client, SD_SEND);
            DWORD drainMs = 500;
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&drainMs),
                sizeof(drainMs));
            char sink[4096];
            for (int drained = 0; drained < (1 << 20);)
            {
                const int n = ::recv(client, sink, sizeof(sink), 0);
                if (n <= 0)
                {
                    break;
                }
                drained += n;
            }
            ::closesocket(client);
        }
    }

    void Server::HandleConnection(std::uintptr_t clientHandle)
    {
        const SOCKET client = static_cast<SOCKET>(clientHandle);
        DWORD timeoutMs = 10000;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs),
            sizeof(timeoutMs));

        const auto send = [client](int code, const char* reason, const std::string& body,
                              const char* extraHeaders = "")
        {
            std::string response = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
            if (!body.empty())
            {
                response += "Content-Type: application/json\r\n";
            }
            response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            response += "Connection: close\r\n";
            response += extraHeaders;
            response += "\r\n";
            response += body;
            std::size_t sent = 0;
            while (sent < response.size())
            {
                const int n = ::send(client, response.data() + sent,
                    static_cast<int>(std::min<std::size_t>(response.size() - sent, 1 << 20)), 0);
                if (n <= 0)
                {
                    return;
                }
                sent += static_cast<std::size_t>(n);
            }
        };

        std::string data;
        char buffer[8192];
        std::size_t headerEnd = std::string::npos;
        while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos)
        {
            if (data.size() > 64 * 1024)
            {
                send(431, "Request Header Fields Too Large", "");
                return;
            }
            const int n = ::recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0)
            {
                return;
            }
            data.append(buffer, static_cast<std::size_t>(n));
        }

        const std::string head = data.substr(0, headerEnd);
        std::string body = data.substr(headerEnd + 4);
        const std::size_t lineEnd = head.find("\r\n");
        const std::string requestLine = head.substr(0, lineEnd);
        const std::size_t firstSpace = requestLine.find(' ');
        const std::size_t secondSpace = requestLine.find(' ', firstSpace + 1);
        if (firstSpace == std::string::npos || secondSpace == std::string::npos)
        {
            send(400, "Bad Request", "");
            return;
        }
        const std::string method = requestLine.substr(0, firstSpace);
        std::string path = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        const std::size_t query = path.find('?');
        if (query != std::string::npos)
        {
            path.erase(query);
        }

        std::size_t contentLength = 0;
        std::string origin;
        std::string host;
        std::size_t at = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
        while (at < head.size())
        {
            std::size_t end = head.find("\r\n", at);
            if (end == std::string::npos)
            {
                end = head.size();
            }
            const std::string line = head.substr(at, end - at);
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos)
            {
                const std::string key = Lower(line.substr(0, colon));
                std::string value = line.substr(colon + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                if (key == "content-length")
                {
                    contentLength = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
                }
                else if (key == "origin")
                {
                    origin = value;
                }
                else if (key == "host")
                {
                    host = value;
                }
            }
            at = end + 2;
        }

        if ((!origin.empty() && origin != "null" && !IsLocalAuthority(origin)) ||
            (!host.empty() && !IsLocalAuthority(host)))
        {
            LOG_WARNING(logging::LogCategory::Editor,
                "MCP: refused a request from origin '{}' host '{}' -- not this machine",
                origin, host);
            send(403, "Forbidden", "");
            return;
        }
        if (path != "/mcp" && path != "/mcp/")
        {
            send(404, "Not Found", "");
            return;
        }
        // POST only. GET would open a stream for messages the server starts, and this one
        // never starts any; DELETE ends a session, and there are none to end.
        if (method != "POST")
        {
            send(405, "Method Not Allowed", "", "Allow: POST\r\n");
            return;
        }
        if (contentLength > 16u * 1024u * 1024u)
        {
            send(413, "Payload Too Large", "");
            return;
        }
        while (body.size() < contentLength)
        {
            const int n = ::recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0)
            {
                return;
            }
            body.append(buffer, static_cast<std::size_t>(n));
        }
        body.resize(contentLength);

        json message = json::parse(body, nullptr, false);
        if (message.is_discarded())
        {
            send(400, "Bad Request", json{ { "jsonrpc", "2.0" }, { "id", nullptr },
                { "error", { { "code", -32700 }, { "message", "that is not JSON" } } } }.dump());
            return;
        }

        // A batch is an array of messages; the current revision dropped batching, older
        // clients still send it, and answering both costs one loop.
        json replies = json::array();
        const auto handle = [&](const json& one)
        {
            std::optional<json> reply;
            try
            {
                reply = HandleMessage(one, *host_);
            }
            catch (const std::exception& e)
            {
                reply = json{ { "jsonrpc", "2.0" },
                    { "id", one.is_object() ? one.value("id", json()) : json() },
                    { "error", { { "code", -32603 }, { "message", e.what() } } } };
            }
            if (reply)
            {
                replies.push_back(std::move(*reply));
            }
        };
        if (message.is_array())
        {
            for (const json& one : message)
            {
                handle(one);
            }
        }
        else
        {
            handle(message);
        }

        if (replies.empty())
        {
            send(202, "Accepted", "");   // notifications only: nothing to say back
            return;
        }
        send(200, "OK", message.is_array() ? replies.dump() : replies[0].dump());
    }

    void ServiceScreenshot(Renderer& renderer)
    {
        ShotRequest& shot = Shot();
        std::unique_lock<std::mutex> lock(shot.mutex);
        if (!shot.pending || shot.done)
        {
            return;
        }
        if (--shot.framesLeft > 0)
        {
            return;
        }
        const std::string path = shot.path;
        const unsigned maxWidth = shot.maxWidth;
        lock.unlock();

        const bool ok = Screenshot::SaveBackbufferPng(renderer, path, maxWidth);
        if (!ok)
        {
            LOG_WARNING(logging::LogCategory::Editor, "MCP: screenshot to {} failed", path);
        }

        lock.lock();
        shot.ok = ok;
        shot.done = true;
        shot.pending = false;
        lock.unlock();
        shot.changed.notify_all();
    }
}

#endif // WITH_EDITOR
