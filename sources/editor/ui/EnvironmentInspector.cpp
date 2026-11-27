#include "editor/ui/EnvironmentInspector.h"
#if WITH_EDITOR

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>

#include "core/JsonField.h" // R9.6: jsonfield::Value -- value()'s conversions, no throw outside the loader
#include "app/scene/Scene.h"
#include "core/math/Math.h"
#include "editor/EditorContext.h"
#include "editor/scene/EnvironmentRuntime.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "editor/commands/EditorCommandStack.h"
#include "imgui.h"

// R10.3: the shared widgets, the type dispatch, the camera and the folded Post Process of the Inspector's
// environment objects; each domain's widgets are a file of their own (EnvironmentInspector.h).

// Case-insensitive suffix test, for hiding the IBL sibling cubes from the sky picker.
bool EnvironmentInspector::EndsWithSuffix(std::string_view value, std::string_view suffix)
{
    if (value.size() < suffix.size()) { return false; }
    const size_t off = value.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i)
    {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(value[off + i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) { return false; }
    }
    return true;
}

float EnvironmentInspector::JsonFloat(const nlohmann::json& p, const char* key, float def)
{
    const auto it = p.find(key);
    return (it != p.end() && it->is_number()) ? it->get<float>() : def;
}

Math::float3 EnvironmentInspector::JsonFloat3(const nlohmann::json& p, const char* key, const Math::float3& def)
{
    const auto it = p.find(key);
    if (it != p.end() && it->is_array() && it->size() >= 3)
    {
        return Math::float3(jsonfield::As((*it)[0], def.x), jsonfield::As((*it)[1], def.y), jsonfield::As((*it)[2], def.z));
    }
    return def;
}

std::string EnvironmentInspector::NormalizePath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

// Mirrors the developer window's marker so the two surfaces explain a control identically --
// the inspector is where a look is made permanent, so it is the surface that most needs the
// explanation, not the least.
void EnvironmentInspector::InspectorHelp(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

EnvironmentInspector::EnvironmentInspector(EditorContext& ctx, EditorCommandStack& commandStack, const AssetRegistry& registry,
    EditorObject& env, bool showEnabled, EditorObjectId& activeEditObject, nlohmann::json& propertiesBeforeEdit,
    std::string& scrollToGroup)
    : ctx(ctx), commandStack(commandStack), registry(registry), env(env), showEnabled(showEnabled),
      activeEditObject(activeEditObject), propertiesBeforeEdit(propertiesBeforeEdit), scrollToGroup(scrollToGroup),
      props(env.properties)
{
    historyLabel =
        env.type == "pointLight" ? "Edit Point Light" :
        env.type == "spotLight" ? "Edit Spot Light" :
        env.type == "timeOfDay" ? "Edit Time of Day" :
        env.type == "camera" ? "Edit Camera" :
        env.type == "skyAtmosphere" ? "Edit Sky Atmosphere" :
        env.type == "volumetricCloud" ? "Edit Volumetric Cloud" :
        env.type == "skybox" ? "Edit Skybox" :
        env.type == "ocean" ? "Edit Ocean" :
        env.type == "wind" ? "Edit Wind" :
        env.type == "cameraExposure" ? "Edit Camera Exposure" :
        env.type == "colorPipeline" ? "Edit Color Pipeline" :
        env.type == "gtao" ? "Edit Ambient Occlusion" :
        env.type == "heightFog" ? "Edit Height Fog" :
        env.type == "bloom" ? "Edit Bloom" :
        env.type == "postProcess" ? "Edit Post Process" :
        "Edit Environment";
}

nlohmann::json& EnvironmentInspector::tgt()
{
    if (groupKey.empty()) { return props; }
    const auto it = props.find(groupKey);
    return it != props.end() && it->is_object() ? *it : missingGroup;
}

// InspectorPanel::ScrollToGroup (--inspect=<type>#<group>): called where a group starts, scrolls there once.
void EnvironmentInspector::scrollAnchor()
{
    if (!scrollToGroup.empty() && scrollToGroup == groupKey)
    {
        ImGui::SetScrollHereY(0.0f);
        scrollToGroup.clear();
    }
}

// `after` for the commands that build one by copy-then-mutate.
nlohmann::json EnvironmentInspector::withField(const char* key, nlohmann::json value)
{
    nlohmann::json after = props;
    if (groupKey.empty()) { after[key] = std::move(value); }
    else { after[groupKey][key] = std::move(value); }
    return after;
}

void EnvironmentInspector::executeChange(nlohmann::json after, const std::string& label)
{
    commandStack.Execute(ctx, std::make_unique<EditEnvironmentCommand>(
        env.id,
        props,
        std::move(after),
        label));
}

// Capture once on activation, BEFORE writing the widget's new value.
// Copying the entire environment JSON per visible control made expanded
// Post Process particularly expensive in unoptimized Debug builds.
void EnvironmentInspector::beginContinuousEdit(bool changed)
{
    if (ImGui::IsItemActivated() ||
        (changed && activeEditObject.value != env.id.value))
    {
        activeEditObject = env.id;
        propertiesBeforeEdit = props;
    }
}

void EnvironmentInspector::trackContinuousEdit(bool changed)
{
    if (changed)
    {
        if (!groupKey.empty() &&
            (!props.contains(groupKey) || !props[groupKey].is_object()))
        {
            props[groupKey] = std::move(missingGroup);
            missingGroup = nlohmann::json::object();
        }
        EnvironmentRuntime::Apply(ctx, env);
        ctx.document.SetDirty(true);
    }
    // Checkboxes/combos can finish in the same frame as activation.
    if (activeEditObject.value == env.id.value &&
        (ImGui::IsItemDeactivatedAfterEdit() ||
            (changed && !ImGui::IsItemActive())))
    {
        commandStack.Execute(ctx, std::make_unique<EditEnvironmentCommand>(
            env.id,
            propertiesBeforeEdit,
            props,
            historyLabel));
        activeEditObject = EditorObjectId{};
    }
}

void EnvironmentInspector::colorEdit()
{
    const Math::float3 c = JsonFloat3(tgt(), "color", Math::float3(1.0f, 1.0f, 1.0f));
    float cv[3] = { c.x, c.y, c.z };
    const bool changed = ImGui::ColorEdit3("Color", cv);
    beginContinuousEdit(changed);
    if (changed) { tgt()["color"] = { cv[0], cv[1], cv[2] }; }
    trackContinuousEdit(changed);
}

// `flags` is here for P16.2's lux row: a quantity whose useful range spans five decades
// cannot be dragged linearly, and a logarithmic drag is a property of THAT row, not a reason
// for a second helper.
void EnvironmentInspector::dragF(const char* label, const char* key, float def, float speed, float lo, float hi,
    const char* fmt, ImGuiSliderFlags flags)
{
    // ImGui's logarithmic drag moves its 0..1 PARAMETER by speed x pixels / (max - min)
    // (imgui_widgets.cpp DragBehaviorT): a speed in value units -- 1 lm a pixel over 0-200000 -- crawled 5e-6 of
    // the range a pixel, and "%.0f" rounded the value back every frame, so the flux jumped between 0 and 1
    // (owner, 2026-10-27). A logarithmic row crosses its whole range in kLogDragPixels (Shift x10, Alt x0.1)
    // and keeps what it is dragged to; `speed` is the linear rows'.
    constexpr float kLogDragPixels = 600.0f;
    if (flags & ImGuiSliderFlags_Logarithmic)
    {
        speed = (hi - lo) / kLogDragPixels;
        flags |= ImGuiSliderFlags_NoRoundToFormat;
    }
    float v = JsonFloat(tgt(), key, def);
    const bool changed = ImGui::DragFloat(label, &v, speed, lo, hi, fmt, flags);
    beginContinuousEdit(changed);
    if (changed) { tgt()[key] = v; }
    trackContinuousEdit(changed);
}

void EnvironmentInspector::dragF3(const char* label, const char* key, const Math::float3& def, float speed)
{
    const Math::float3 d3 = JsonFloat3(tgt(), key, def);
    float v[3] = { d3.x, d3.y, d3.z };
    const bool changed = ImGui::DragFloat3(label, v, speed);
    beginContinuousEdit(changed);
    if (changed) { tgt()[key] = { v[0], v[1], v[2] }; }
    trackContinuousEdit(changed);
}

void EnvironmentInspector::checkB(const char* label, const char* key, bool def)
{
    bool v = jsonfield::Value(tgt(), key, def);
    if (ImGui::Checkbox(label, &v))
    {
        nlohmann::json after = withField(key, v);
        executeChange(std::move(after), historyLabel);
    }
}

// Plan L: when a light comes on (rendering/lighting/NightLamps.h); the key is absent while it is "always".
void EnvironmentInspector::onAtCombo()
{
    const LampSwitch current = LampSwitchFromName(jsonfield::Value(tgt(), "onAt", std::string()));
    static const char* const kLabels[] = { "Always", "At night", "By day" };
    int index = static_cast<int>(current);
    if (ImGui::Combo("On At", &index, kLabels, 3) && index != static_cast<int>(current))
    {
        nlohmann::json after = props;
        if (index == 0) { after.erase("onAt"); }
        else { after["onAt"] = LampSwitchName(static_cast<LampSwitch>(index)); }
        executeChange(std::move(after), "Set Light On At");
    }
    InspectorHelp("A lamp that comes on at night: it switches when the sun goes down past Time of Day > "
                  "Lamps > Switch At (0 = sunset, as Ultra Dynamic Sky's Sunset event), after a delay of "
                  "up to Random Delay, warming up over Turn On with a flicker; it goes out at once at "
                  "sunrise. By day = the opposite. Setting the clock by hand switches it at once. Off, "
                  "it costs no shading and takes no shadow slot. A night lamp is night light: Time of Day > "
                  "Night Brightness multiplies it as it does the moon, so the lumens stay a bulb's.");
    if (current != LampSwitch::Always)
    {
        const NightLamps& lamps = ctx.scene.GetNightLamps();
        const float level = lamps.Level(current, JsonFloat3(tgt(), "position", Math::float3(0.0f, 0.0f, 0.0f)));
        ImGui::TextDisabled("Now: %s, at %.0f %%", lamps.Night() ? "night" : "day", level * 100.0f);
    }
}

void EnvironmentInspector::Draw()
{
    // Cap the width of every control in this inspector. ImGui defaults to roughly 65% of the
    // panel, which on a wide docked Inspector drags each slider halfway across the screen and
    // pushes its label out of eyeshot. A fixed cap keeps label and value together; the panel can
    // still be narrower, hence the min().
    const float controlWidth = std::min(220.0f, std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.5f));
    ImGui::PushItemWidth(controlWidth);
    struct ItemWidthScope
    {
        ~ItemWidthScope() { ImGui::PopItemWidth(); }
    } itemWidthScope;

    const bool supportsEnable =
        env.type == "pointLight" || env.type == "spotLight" || env.type == "ocean";
    if (showEnabled && supportsEnable)
    {
        bool enabled = jsonfield::Value(tgt(), "enabled", true);
        if (ImGui::Checkbox("Enabled", &enabled))
        {
            nlohmann::json after = withField("enabled", enabled);
            executeChange(
                std::move(after),
                enabled ? "Enable Environment" : "Disable Environment");
        }
        ImGui::Separator();
    }

    if (env.type == "pointLight")
    {
        DrawPointLight();
    }
    else if (env.type == "spotLight")
    {
        DrawSpotLight();
    }
    else if (env.type == "timeOfDay")
    {
        DrawTimeOfDay();
    }
    else if (env.type == "camera")
    {
        dragF("H FOV (deg)", "hfovDeg", 90.0f, 0.5f, 1.0f, 179.0f);
        dragF("Z Near", "zNear", 0.01f, 0.001f, 0.0001f, 100.0f);
        dragF("Z Far", "zFar", 10000.0f, 1.0f, 0.1f, 1000000.0f);
    }
    else if (env.type == "cameraExposure")
    {
        DrawCameraExposure();
    }
    else if (env.type == "gtao")
    {
        DrawGtao();
    }
    else if (env.type == "heightFog")
    {
        DrawHeightFog();
    }
    else if (env.type == "bloom")
    {
        DrawBloom();
    }
    else if (env.type == "colorPipeline")
    {
        DrawColorPipeline();
    }
    else if (env.type == "postProcess")
    {
        DrawPostProcess();
    }
    else if (env.type == "skyAtmosphere")
    {
        DrawSkyAtmosphere();
    }
    else if (env.type == "volumetricCloud")
    {
        DrawVolumetricCloud();
    }
    else if (env.type == "skybox")
    {
        DrawSkybox();
    }
    else if (env.type == "ocean")
    {
        DrawOcean();
    }
    else if (env.type == "wind")
    {
        DrawWind();
    }
    else
    {
        ImGui::TextDisabled("No editable properties.");
    }
}

void EnvironmentInspector::DrawPostProcess()
{
    // P8B. One object, five collapsing sections. Each section sets `groupKey` so the shared
    // widgets write into `properties[<group>]`, and clears it afterwards -- an escaped
    // groupKey would send the NEXT object's edits into a sub-object that does not exist.
    struct Section { const char* key; const char* label; };
    const Section sections[] = {
        { "cameraExposure", "Camera Exposure" },
        { "colorPipeline",  "Color Pipeline" },
        { "gtao",           "Ambient Occlusion (GTAO)" },
        { "heightFog",      "Exponential Height Fog" },
        { "bloom",          "Bloom" },
    };
    for (const Section& section : sections)
    {
        // Missing sections use defaults without authoring empty groups just by viewing.
        missingGroup = nlohmann::json::object();
        // --inspect=postProcess#<group>: open the section it names, or the scroll below has nowhere to go.
        if (!scrollToGroup.empty() && scrollToGroup == section.key) { ImGui::SetNextItemOpen(true); }
        if (ImGui::CollapsingHeader(section.label))
        {
            ImGui::PushID(section.key);
            groupKey = section.key;
            scrollAnchor();
            if (std::string_view(section.key) == "cameraExposure") { DrawCameraExposure(); }
            else if (std::string_view(section.key) == "colorPipeline") { DrawColorPipeline(); }
            else if (std::string_view(section.key) == "gtao") { DrawGtao(); }
            else if (std::string_view(section.key) == "heightFog") { DrawHeightFog(); }
            else { DrawBloom(); }
            groupKey.clear();
            ImGui::PopID();
        }
    }
}

#endif // WITH_EDITOR
