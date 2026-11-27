#include "editor/intent/EditorActionBuilders.h"
#if WITH_EDITOR

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "core/JsonField.h" // R9.6d: Value for the document's fields
#include "app/scene/Scene.h"
#include "editor/commands/CreateEnvironmentCommand.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "editor/scene/EditorZone.h"
#include "editor/commands/SetLampSwitchCommand.h"
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/GBufferRenderable.h" // plan L: setOnAt offers a mesh's glow only when it has one
#include "editor/EditorContext.h"

// R10.3: the editor's actions -- lights: when a lamp comes on, a light's settings, adding lights to meshes.
// Cut out of EditorActionRegistry.cpp; the table that names them is still there (EditorActionBuilders.h).

namespace editoractions
{
    // ------------------------------------------------------------------ setOnAt

    // Plan L: lamps that come on at night. A light takes "onAt" in its environment properties (its runtime is rebuilt
    // from them); a mesh in the document plus its runtime switch, and only a mesh that glows -- on anything else the
    // key would be a control that moves nothing.
    std::unique_ptr<EditorCommand> BuildSetOnAt(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        const LampSwitch lampSwitch = LampSwitchFromName(StringOr(intent.params, "onAt", "night"));
        const EditorSceneDocument& document = actionCtx.editor.document;
        std::vector<std::unique_ptr<EditorCommand>> commands;
        std::size_t lights = 0, meshes = 0, skipped = 0;
        for (const EditorObjectId id : targets)
        {
            if (const EditorObject* environment = FindEnvironmentObject(document, id))
            {
                if (environment->type != "pointLight" && environment->type != "spotLight") { ++skipped; continue; }
                nlohmann::json after = environment->properties;
                if (lampSwitch != LampSwitch::Always) { after["onAt"] = LampSwitchName(lampSwitch); }
                else { after.erase("onAt"); }
                commands.push_back(std::make_unique<EditEnvironmentCommand>(id, environment->properties, std::move(after),
                                                                            "Set Light On At"));
                ++lights;
                continue;
            }
            const EditorObject* obj = document.Find(id);
            RenderableObjectBase* runtime = obj ? actionCtx.editor.scene.FindEditorObject(id.value) : nullptr;
            const GBufferRenderable* gb = runtime ? runtime->As<GBufferRenderable>() : nullptr;
            if (!obj || obj->type != "staticMesh" || !gb || !gb->HasEmissive())
            {
                ++skipped; // a mesh that does not glow, a zone, the ocean: nothing to switch
                continue;
            }
            commands.push_back(std::make_unique<SetLampSwitchCommand>(id, lampSwitch));
            ++meshes;
        }
        if (commands.empty())
        {
            outStatus = "Nothing to switch: onAt is for point and spot lights and for meshes that glow (emissive)";
            return nullptr;
        }
        const char* when = lampSwitch == LampSwitch::Night ? "at night" : lampSwitch == LampSwitch::Day ? "by day" : "always";
        outStatus = std::string("On ") + when + ": " + std::to_string(lights) + " light(s), " + std::to_string(meshes) +
                    " glowing mesh(es)";
        if (skipped > 0) { outStatus += " (" + CountedObjects(skipped) + " skipped: not lights, no glow)"; }
        const std::size_t count = commands.size();
        return FoldIntoOneEntry(std::move(commands), std::string("Set On At (") + when + ") " + std::to_string(count) + " Objects");
    }

    // ------------------------------------------------------------------ setLight

    // THE LAMPS THAT EXIST, retuned: brightness, reach, colour, shadows. addLights could make a candle and nothing could
    // dim it afterwards -- the bathroom's candles were tuned by restarting the editor on level copies, fifteen seconds a
    // picture. The keys are the ones the Inspector writes (luminousFluxLm, radius / a spot's range, color,
    // shadowsEnabled), through its EditEnvironmentCommand, so the runtime rebuilds the light as it would from there.
    // `scale` multiplies each light's own flux, so a group of different lamps dims together and keeps its ratios.
    std::unique_ptr<EditorCommand> BuildSetLight(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        const auto number = [&](const char* key) -> const nlohmann::json* {
            const auto it = intent.params.find(key);
            return it != intent.params.end() && it->is_number() ? &*it : nullptr;
        };
        const nlohmann::json* lumens = number("lumens");
        const nlohmann::json* scale = number("scale");
        const nlohmann::json* radius = number("radius");
        const auto colorIt = intent.params.find("color");
        const bool hasColor = colorIt != intent.params.end() && colorIt->is_array() && colorIt->size() == 3 &&
            (*colorIt)[0].is_number() && (*colorIt)[1].is_number() && (*colorIt)[2].is_number();
        const auto shadowsIt = intent.params.find("shadows");
        const bool hasShadows = shadowsIt != intent.params.end() && shadowsIt->is_boolean();
        // A point light's flicker (PointLightFlickerDesc): +/- amplitude of its flux, around frequencyHz.
        const nlohmann::json* flicker = number("flicker");
        const nlohmann::json* flickerHz = number("flickerHz");
        // The light's own multiplier by day (Scene::TickNightLamps): 1 = none, 0 = dark by day.
        const nlohmann::json* dayBrightness = number("dayBrightness");
        // UE's Source Radius (m): how big the light is for VSM's rays.
        const nlohmann::json* sourceRadius = number("sourceRadius");
        if (!lumens && !scale && !radius && !hasColor && !hasShadows && !flicker && !flickerHz && !dayBrightness && !sourceRadius)
        {
            outStatus = "setLight needs lumens, scale, radius, color, shadows, flicker, dayBrightness and/or sourceRadius";
            return nullptr;
        }
        if (sourceRadius && sourceRadius->get<float>() < 0.0f)
        {
            outStatus = "setLight: sourceRadius is a size in metres, 0 or more";
            return nullptr;
        }
        if (dayBrightness && dayBrightness->get<float>() < 0.0f)
        {
            outStatus = "setLight: dayBrightness is a multiplier, 0 or more";
            return nullptr;
        }
        if (flicker && (flicker->get<float>() < 0.0f || flicker->get<float>() > 1.0f))
        {
            outStatus = "setLight: flicker is a share of the flux, 0..1";
            return nullptr;
        }
        if ((lumens && lumens->get<float>() < 0.0f) || (scale && scale->get<float>() < 0.0f) ||
            (radius && radius->get<float>() <= 0.0f))
        {
            outStatus = "setLight: lumens and scale must be 0 or more, radius more than 0";
            return nullptr;
        }

        const EditorSceneDocument& document = actionCtx.editor.document;
        std::vector<std::unique_ptr<EditorCommand>> commands;
        std::size_t skipped = 0, unchanged = 0;
        for (const EditorObjectId id : targets)
        {
            const EditorObject* light = FindEnvironmentObject(document, id);
            if (!light || (light->type != "pointLight" && light->type != "spotLight")) { ++skipped; continue; }
            nlohmann::json after = light->properties;
            if (lumens) { after["luminousFluxLm"] = lumens->get<float>(); }
            if (scale)
            {
                // A light written before P16.5 carries no flux yet: it starts from what the Inspector would show.
                const float now = jsonfield::Value(after, "luminousFluxLm", light->type == "spotLight" ? 12000.0f : 3000.0f);
                after["luminousFluxLm"] = now * scale->get<float>();
            }
            if (radius) { after[light->type == "spotLight" ? "range" : "radius"] = radius->get<float>(); }
            if (hasColor)
            {
                after["color"] = nlohmann::json::array(
                    { (*colorIt)[0].get<float>(), (*colorIt)[1].get<float>(), (*colorIt)[2].get<float>() });
            }
            if (hasShadows) { after["shadowsEnabled"] = shadowsIt->get<bool>(); }
            if (dayBrightness) { after["dayBrightness"] = dayBrightness->get<float>(); }
            if (sourceRadius) { after["sourceRadius"] = sourceRadius->get<float>(); }
            if ((flicker || flickerHz) && light->type == "pointLight")
            {
                nlohmann::json f = jsonfield::Value(after, "flicker", nlohmann::json::object());
                if (!f.is_object()) { f = nlohmann::json::object(); }
                if (flicker) { f["amplitude"] = flicker->get<float>(); }
                if (flickerHz) { f["frequencyHz"] = flickerHz->get<float>(); }
                // Each lamp its own phase: candles in step read as one machine. Kept once given.
                if (!f.contains("seed")) { f["seed"] = static_cast<std::int64_t>((id.value * 2654435761ull) & 0x7FFFFFFFull) + 1; }
                if (jsonfield::Value(f, "amplitude", 0.0f) > 0.0f && !f.contains("frequencyHz")) { f["frequencyHz"] = 3.0f; }
                after["flicker"] = std::move(f);
            }
            // A light already at these values: EditEnvironmentCommand refuses an edit that changes nothing, and the
            // whole phrase read "Changed 5 light(s) ... -- failed" when the candles were simply as asked.
            if (after == light->properties) { ++unchanged; continue; }
            commands.push_back(std::make_unique<EditEnvironmentCommand>(id, light->properties, std::move(after), "Set Light"));
        }
        if (commands.empty())
        {
            outStatus = unchanged > 0
                ? "Nothing to change: the " + std::to_string(unchanged) + " light(s) already have these values"
                : std::string("Nothing to change: setLight is for point and spot lights");
            return nullptr;
        }
        const auto text = [](const nlohmann::json& value)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%g", value.get<float>());
            return std::string(buffer);
        };
        const std::size_t count = commands.size();
        outStatus = "Changed " + std::to_string(count) + " light(s):";
        if (lumens) { outStatus += " " + text(*lumens) + " lm"; }
        if (scale) { outStatus += " flux x" + text(*scale); }
        if (radius) { outStatus += " reach " + text(*radius) + " m"; }
        if (hasColor) { outStatus += " colour"; }
        if (hasShadows) { outStatus += shadowsIt->get<bool>() ? " shadows on" : " shadows off"; }
        if (flicker) { outStatus += " flicker " + text(*flicker); }
        if (flickerHz) { outStatus += " at " + text(*flickerHz) + " Hz"; }
        if (dayBrightness) { outStatus += " by day x" + text(*dayBrightness); }
        if (sourceRadius) { outStatus += " source radius " + text(*sourceRadius) + " m"; }
        if (unchanged > 0) { outStatus += " (" + std::to_string(unchanged) + " already so)"; }
        if (skipped > 0) { outStatus += " (" + CountedObjects(skipped) + " skipped: not lights)"; }
        return FoldIntoOneEntry(std::move(commands), "Set " + std::to_string(count) + " Lights");
    }

    // ADD LIGHTS: point and spot lights at the positions the caller worked out -- a candle's flame,
    // a lamp's bulb. `move` and `setOnAt` reach lights that exist; nothing reached one that does not,
    // so a scene could be dressed with meshes over MCP and never lit. Each item starts from what the
    // Create > Light menu makes (EditorController's BuildPointLightObject / BuildSpotLightObject) and
    // takes the fields it names.
    std::unique_ptr<EditorCommand> BuildAddLights(const EditorActionContext&,
        const std::vector<EditorObjectId>&,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        const auto itemsIt = intent.params.find("items");
        if (itemsIt == intent.params.end() || !itemsIt->is_array() || itemsIt->empty())
        {
            outStatus = "addLights needs a list of lights";
            return nullptr;
        }
        const std::string group = StringOr(intent.params, "group", "");
        const auto vec3 = [](const nlohmann::json& v)
        {
            return nlohmann::json::array({ v[0].get<float>(), v[1].get<float>(), v[2].get<float>() });
        };

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(itemsIt->size());
        std::size_t points = 0, spots = 0, shadowed = 0;
        for (const nlohmann::json& item : *itemsIt)
        {
            const bool spot = item.value("type", std::string("point")) == "spot";
            EditorObject light;
            light.type = spot ? "spotLight" : "pointLight";
            light.name = spot ? "Spot Light" : "Point Light";
            light.properties = {
                { "enabled", true },
                { "position", vec3(item["position"]) },
                { "color", item.contains("color") ? vec3(item["color"]) : nlohmann::json::array({ 1.0f, 0.92f, 0.78f }) },
                { "luminousFluxLm", item.value("lumens", spot ? 12000.0f : 3000.0f) },
                { "shadowsEnabled", item.value("shadows", spot) },
            };
            if (spot)
            {
                light.properties["direction"] = item.contains("direction")
                    ? vec3(item["direction"]) : nlohmann::json::array({ 0.0f, -1.0f, 0.0f });
                light.properties["range"] = item.value("range", 14.0f);
                light.properties["innerAngleDeg"] = item.value("innerAngleDeg", 15.0f);
                light.properties["outerAngleDeg"] = item.value("outerAngleDeg", 28.0f);
                light.properties["shadowNormalBias"] = 0.05f;
                light.properties["shadowDepthBias"] = 0.0001f;
            }
            else
            {
                light.properties["radius"] = item.value("radius", 6.0f);
            }
            if (item.contains("name")) { light.properties["name"] = item["name"]; }
            // "always" is the absence of the key, as SetLampSwitchCommand writes it.
            const std::string onAt = item.value("onAt", std::string("always"));
            if (onAt != "always") { light.properties["onAt"] = onAt; }
            if (!group.empty()) { light.properties["group"] = group; }
            (spot ? spots : points) += 1;
            shadowed += light.properties["shadowsEnabled"].get<bool>() ? 1u : 0u;
            commands.push_back(std::make_unique<CreateEnvironmentCommand>(std::move(light)));
        }
        outStatus = "Added " + std::to_string(points) + " point and " + std::to_string(spots) +
            " spot light(s), " + std::to_string(shadowed) + " casting shadows" +
            (group.empty() ? std::string() : " into " + group);
        const std::size_t count = commands.size();
        return FoldIntoOneEntry(std::move(commands), "Add " + std::to_string(count) + " lights");
    }
}

#endif // WITH_EDITOR
