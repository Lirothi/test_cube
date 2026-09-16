#include "editor/intent/IntentPrompt.h"
#if WITH_EDITOR

#include <algorithm>
#include <string>
#include <vector>

#include "editor/assets/AssetRegistry.h"
#include "editor/intent/EditorActionRegistry.h"
#include "editor/scene/EditorSceneDocument.h"

namespace
{
    const char* ParamKindWord(EditorParamKind kind)
    {
        switch (kind)
        {
        case EditorParamKind::Number: return "number";
        case EditorParamKind::Bool:   return "true/false";
        case EditorParamKind::String: return "string";
        case EditorParamKind::Range:  return "[low, high]";
        case EditorParamKind::Vec3:   return "[x, y, z]";
        case EditorParamKind::Enum:   return "enum";
        case EditorParamKind::Any:    return "value";
        }
        return "value";
    }

    void AppendList(std::string& text, const std::vector<std::string>& values, std::size_t limit)
    {
        for (std::size_t i = 0; i < values.size() && i < limit; ++i)
        {
            text += "  " + values[i] + "\n";
        }
        if (values.size() > limit)
        {
            text += "  ... and " + std::to_string(values.size() - limit) + " more\n";
        }
    }
}

namespace intentprompt
{
    std::string BuildSystemPrompt(const EditorSceneDocument& document,
        const AssetRegistry& assets,
        const intentschema::Vocabulary& vocabulary)
    {
        (void)assets;

        std::string p;
        p += "You translate a level designer's instruction into ONE editor command.\n";
        p += "The designer usually writes Russian; the JSON you emit is always English.\n";
        p += "Answer with a single JSON object and nothing else.\n\n";

        p += "THREE ANSWERS ARE ALLOWED, and choosing between them is your actual job:\n";
        p += "  {\"kind\":\"command\", ...}    -- the editor can do this\n";
        p += "  {\"kind\":\"needs_api\", ...}  -- the editor has NO action for this\n";
        p += "  {\"kind\":\"unclear\", ...}    -- you need one more detail to be safe\n\n";

        // This is the paragraph the whole three-branch design exists for. Instruct models
        // are trained to be helpful and will reach for a near-miss unless told, in these
        // words, that refusing is the better answer.
        p += "PREFER needs_api OVER A SIMILAR ACTION. A command that does not run costs\n";
        p += "nothing. A command that runs and was the wrong one damages the level. If the\n";
        p += "designer asks to paint something red and there is no colour action, say\n";
        p += "needs_api -- do NOT pick the nearest action that happens to exist.\n";
        p += "In why_existing_dont_fit, name the actions you considered and why each fails.\n";
        p += "That field is also your protection against refusing something that IS possible:\n";
        p += "if you cannot name a reason, the action probably exists and you should use it.\n\n";

        p += "ACTIONS (use the descriptions, not the names -- 'bury' means push into the\n";
        p += "ground, it does NOT mean hide):\n";
        for (const EditorActionDesc& action : EditorActionRegistry::Builtin().Actions())
        {
            p += "- " + std::string(action.id) + ": " + std::string(action.description) + "\n";
            switch (action.target)
            {
            case EditorTargetKind::Objects:
                p += "    target: objects already in the level (use target.filter)\n";
                break;
            case EditorTargetKind::Asset:
                p += "    target: an asset to create from (use target.asset)\n";
                break;
            case EditorTargetKind::Environment:
                p += "    target: one of the settings listed below (use target.setting)\n";
                break;
            case EditorTargetKind::None:
                p += "    target: none\n";
                break;
            }
            for (const EditorActionParam& param : action.params)
            {
                p += "    params." + std::string(param.name) + " (" + ParamKindWord(param.kind);
                if (param.required)
                {
                    p += ", required";
                }
                p += "): " + std::string(param.description);
                if (!param.values.empty())
                {
                    p += " [";
                    for (std::size_t i = 0; i < param.values.size(); ++i)
                    {
                        if (i > 0)
                        {
                            p += " | ";
                        }
                        p += std::string(param.values[i]);
                    }
                    p += "]";
                }
                p += "\n";
            }
        }

        p += "\nTARGET.FILTER names what is ALREADY in the level. Use the exact strings below;\n";
        p += "a Russian word like \"palms\" must become the asset string that means it.\n";
        AppendList(p, vocabulary.needles, 80);

        p += "\nTARGET.ASSET names something to CREATE, and may be any of these, whether or not\n";
        p += "the level already uses it. TARGET.ASSETS is the same list when a phrase asks for\n";
        p += "several KINDS at once (\"palms of different types\") -- one spawn, not three:\n";
        AppendList(p, vocabulary.assets, 80);

        if (!vocabulary.materials.empty())
        {
            p += "\nsetMaterial's `material` parameter must be one of these:\n";
            AppendList(p, vocabulary.materials, 80);
        }

        if (!vocabulary.zones.empty())
        {
            p += "\nZONES are named regions someone drew on this level. Two ways to use one,\n";
            p += "and which one depends on whether objects are being MADE or FOUND:\n";
            p += "  spawn        -> params.zone: \"<name>\"   fills the region with new things\n";
            p += "  anything else-> target.where.zone: \"<name>\"  narrows to what is ALREADY\n";
            p += "                 inside it -- \"удали пальмы в зоне Beach\", \"сколько камней\n";
            p += "                 в зоне Meadow\". Leave radius and anchor alone when using it.\n";
            p += "This level has:\n";
            AppendList(p, vocabulary.zones, 80);
        }

        // The CURRENT values are the useful half of this list. "Make the fog thicker" is
        // only answerable against what it is now, and a model that can see 0.012 can pick
        // a scale instead of inventing an absolute number out of nothing.
        {
            std::vector<envsettings::Setting> settings = envsettings::Enumerate(document);

            // SHALLOW FIRST, and the reason is not tidiness. A section like `ocean.render`
            // carries dozens of tuning knobs that nobody addresses in a sentence, and in
            // document order they crowded out `wind.strength` and `cameraExposure.*` --
            // the ones a phrase actually names -- while doubling the prompt and with it the
            // prefill every request pays. The resolver still knows the deep ones by name;
            // they simply do not need to be recited here.
            std::stable_sort(settings.begin(), settings.end(),
                [](const envsettings::Setting& a, const envsettings::Setting& b)
                {
                    const auto depth = [](const std::string& path)
                    {
                        return std::count(path.begin(), path.end(), '.');
                    };
                    const auto da = depth(a.path);
                    const auto db = depth(b.path);
                    return da != db ? da < db : a.path < b.path;
                });

            p += "\nTARGET.SETTING names one environment knob, for the setEnvironment action.\n";
            p += "These are the ones this level has, with their CURRENT values:\n";
            constexpr std::size_t kListed = 55;
            std::size_t shown = 0;
            for (const envsettings::Setting& setting : settings)
            {
                if (shown >= kListed)
                {
                    break;
                }
                p += "  " + setting.path + " = " + envsettings::ToText(setting.current) +
                    " (" + envsettings::KindName(setting.kind) + ")\n";
                ++shown;
            }
            if (settings.size() > shown)
            {
                p += "  (plus " + std::to_string(settings.size() - shown) +
                    " deeper tuning settings such as ocean.render.* -- name one exactly if\n";
                p += "   the designer asks for it by name, otherwise stay with the list above)\n";
            }
        }

        p += "\nSCOPE: \"all\" means everything in the level that matches the filter;\n";
        p += "\"selected\" means only what the designer has selected right now.\n";
        p += "The editor currently has " + std::to_string(document.Objects().size()) + " objects.\n\n";

        // The fields below are the ones a model skips unless it has SEEN them used. Left
        // undemonstrated, "hide everything except the palms" came back as a filter on the
        // palms with enabled=true -- the exact opposite of the request, and valid JSON.
        p += "TARGET FIELDS -- use the right one, they are not interchangeable:\n";
        p += "  filter  : act ON these. \"bury the palms\" -> filter lists the palm assets.\n";
        p += "  exclude : act on everything EXCEPT these. A phrase with \"except\", \"apart from\",\n";
        p += "            \"everything but\" or \"krome\" needs exclude, and usually an EMPTY filter.\n";
        p += "  asset   : the thing to create (spawn) or to switch to (replace). `replace` needs\n";
        p += "            BOTH -- filter for what changes, asset for what it becomes.\n";
        p += "  where   : a distance limit. \"within 50 m of the camera\" -> "
             "{\"anchor\":\"camera\",\"radius\":50}.\n";
        p += "            Or a named region: \"in zone Beach\" -> {\"zone\":\"Beach\"}.\n";
        p += "            Leaving it out silently widens the command to the whole level.\n\n";

        p += "EXAMPLES\n";
        p += "  \"zaroy vse palmy\" (bury all palms)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"bury\","
             "\"target\":{\"filter\":[\"models/coconut_palm.mesh.json\"],\"scope\":\"all\"}}\n";
        p += "  \"rassadi 10 novyh palm\" (plant 10 new palms)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"spawn\","
             "\"target\":{\"asset\":\"models/coconut_palm.mesh.json\"},\"params\":{\"count\":10}}\n";
        // The multi-asset example earns its place the hard way: `assets` shipped working and
        // went UNUSED -- across a whole battery the model wrote `asset` every time, because
        // every example it could see wrote `asset`. Described but never demonstrated is the
        // same as absent. Note the count too: it is the TOTAL, shared between the kinds, not
        // a count per kind, and a run without this said 10 when asked for 30.
        p += "  \"posadi 20 palm raznogo tipa\" (plant 20 palms of different types)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"spawn\","
             "\"target\":{\"assets\":[\"models/coconut_palm.mesh.json\","
             "\"models/curly_palm.mesh.json\",\"models/date_palm.mesh.json\"]},"
             "\"params\":{\"count\":20}}\n";
        p += "  \"spryach' vsyo krome palm\" (hide everything except the palms)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"setEnabled\","
             "\"target\":{\"exclude\":[\"models/coconut_palm.mesh.json\"],\"scope\":\"all\"},"
             "\"params\":{\"enabled\":false}}\n";
        p += "  \"zameni finikovye palmy na kokosovye\" (replace the date palms with coconut ones)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"replace\","
             "\"target\":{\"filter\":[\"models/date_palm.mesh.json\"],"
             "\"asset\":\"models/coconut_palm.mesh.json\",\"scope\":\"all\"}}\n";
        p += "  \"uberi kamni v radiuse 50 metrov ot kamery\" (remove the rocks within 50 m of the camera)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"delete\","
             "\"target\":{\"filter\":[\"models/beach_rock.mesh.json\"],\"scope\":\"all\","
             "\"where\":{\"anchor\":\"camera\",\"radius\":50}}}\n";
        p += "  \"skolko palm na urovne?\" (how many palms are in the level?)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"count\","
             "\"target\":{\"filter\":[\"models/coconut_palm.mesh.json\"],\"scope\":\"all\"}}\n";
        p += "  \"sdelay veter silnee\" (make the wind stronger)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"setEnvironment\","
             "\"target\":{\"setting\":\"wind.strength\"},\"params\":{\"scale\":1.5}}\n";
        p += "  \"postav' gtao intensivnost 0.8\" (set the GTAO intensity to 0.8)\n";
        p += "  -> {\"kind\":\"command\",\"action\":\"setEnvironment\","
             "\"target\":{\"setting\":\"gtao.intensity\"},\"params\":{\"value\":0.8}}\n";
        p += "  \"pokras' palmy v krasnyy\" (paint the palms red)\n";
        p += "  -> {\"kind\":\"needs_api\",\"requested\":\"paint objects red\","
             "\"proposed\":\"setBaseColor(objects, rgba)\","
             "\"why_existing_dont_fit\":\"replace swaps the whole mesh and there is no "
             "action that changes only a colour\"}\n";
        p += "  \"udali derevya\" (delete the trees) when several tree assets exist\n";
        p += "  -> {\"kind\":\"unclear\",\"question\":\"Which trees -- coconut, date or curly palms?\"}\n";
        return p;
    }

    std::string ApplyChatTemplate(const std::string& systemPrompt,
        const std::string& userPhrase,
        const std::string& templateName,
        const std::vector<IntentTurn>& history)
    {
        if (templateName == "plain")
        {
            std::string text = systemPrompt;
            for (const IntentTurn& turn : history)
            {
                text += "\n\nInstruction: " + turn.user + "\nJSON: " + turn.assistant;
            }
            return text + "\n\nInstruction: " + userPhrase + "\nJSON: ";
        }
        // ChatML, which Qwen and most current instruct models speak. Applying it here
        // rather than going through the server's chat endpoint keeps the prefix BYTE
        // IDENTICAL between requests, which is what the prefill cache keys on. History is
        // appended AFTER the system block for the same reason -- the cached prefix must
        // not move when a follow-up answer arrives.
        std::string text = "<|im_start|>system\n" + systemPrompt + "<|im_end|>\n";
        for (const IntentTurn& turn : history)
        {
            text += "<|im_start|>user\n" + turn.user + "<|im_end|>\n";
            text += "<|im_start|>assistant\n" + turn.assistant + "<|im_end|>\n";
        }
        text += "<|im_start|>user\n" + userPhrase + "<|im_end|>\n";
        text += "<|im_start|>assistant\n";

        // QWEN3 AND ITS RELATIVES THINK BEFORE THEY ANSWER, and a grammar that demands JSON
        // from the first token does not let them: the reasoning block is the first thing
        // they want to emit, the grammar forbids it, and the two fight over every token.
        // Pre-filling an EMPTY think block is the documented way to say "you have finished
        // thinking" -- it is part of the prompt, so the grammar never sees it, and
        // generation starts where the answer belongs.
        //
        // Costs nothing on a model that does not think: it reads as a stray tag it ignores.
        // `chatml-think` is here for the day someone wants the reasoning back.
        if (templateName != "chatml-think")
        {
            text += "<think>\n\n</think>\n\n";
        }
        return text;
    }
}

#endif // WITH_EDITOR
