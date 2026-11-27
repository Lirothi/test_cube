#pragma once
#if WITH_EDITOR

#include <string>
#include <string_view>

#include "core/math/Math.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObject, EditorObjectId, nlohmann::json
#include "imgui.h"                            // ImGuiSliderFlags

struct EditorContext;
class EditorCommandStack;
class AssetRegistry;

// Inspector for the top-level environment sections (Step 24). Edits write the
// entity's `properties` (round-tripped on save via the entity-driven
// serializer) and patch the live runtime; lights/camera update instantly,
// skybox texture edits rebuild the live skybox, and wind edits update the shared WindState.
//
// R10.3: this was InspectorPanel.cpp's DrawEnvironmentInspector, one 4280-line function whose lambdas shared its
// locals. The locals are this class's members and the lambdas its methods, UNDER THEIR OLD NAMES (no trailing
// underscore, lower case), so each domain's widgets moved verbatim into a file of their own:
//   EnvironmentInspector.cpp            the shared widgets, the type dispatch, the camera, the folded Post Process;
//   EnvironmentInspectorLights.cpp      point and spot lights;
//   EnvironmentInspectorTimeOfDay.cpp   sun, moon, stars, lamps, the night;
//   EnvironmentInspectorExposure.cpp    camera exposure, colour pipeline;
//   EnvironmentInspectorPostEffects.cpp ambient occlusion, height fog, bloom;
//   EnvironmentInspectorSky.cpp         sky atmosphere, volumetric clouds, skybox;
//   EnvironmentInspectorOcean.cpp       the ocean;
//   EnvironmentInspectorWind.cpp        the wind.
// One lives for one object's draw: InspectorPanel constructs it and calls Draw().
class EnvironmentInspector
{
public:
    EnvironmentInspector(EditorContext& ctx, EditorCommandStack& commandStack, const AssetRegistry& registry,
        EditorObject& env, bool showEnabled, EditorObjectId& activeEditObject, nlohmann::json& propertiesBeforeEdit,
        std::string& scrollToGroup);
    EnvironmentInspector(const EnvironmentInspector&) = delete;
    EnvironmentInspector& operator=(const EnvironmentInspector&) = delete;

    // One frame's widgets for the object; an edit goes through the command stack (undo).
    void Draw();

private:
    // The shared widgets (EnvironmentInspector.cpp).
    nlohmann::json& tgt();
    void scrollAnchor();
    nlohmann::json withField(const char* key, nlohmann::json value);
    void executeChange(nlohmann::json after, const std::string& label);
    void beginContinuousEdit(bool changed);
    void trackContinuousEdit(bool changed);
    void colorEdit();
    void dragF(const char* label, const char* key, float def, float speed, float lo, float hi,
        const char* fmt = "%.3f", ImGuiSliderFlags flags = 0);
    void dragF3(const char* label, const char* key, const Math::float3& def, float speed);
    void checkB(const char* label, const char* key, bool def);
    void onAtCombo();
    static bool EndsWithSuffix(std::string_view value, std::string_view suffix);
    static float JsonFloat(const nlohmann::json& p, const char* key, float def);
    static Math::float3 JsonFloat3(const nlohmann::json& p, const char* key, const Math::float3& def);
    static std::string NormalizePath(std::string path);
    static void InspectorHelp(const char* text);

    // P8B: the five level-wide look groups, lifted out of the type chain so they can be
    // drawn EITHER as their own legacy object or as a section of the folded "postProcess"
    // one. `tgt()` is what makes that work: the same widget code writes to the object's own
    // properties or to a named sub-object, decided by `groupKey` at the call site.
    void DrawCameraExposure();  // Exposure
    void DrawColorPipeline();   // Exposure
    void DrawGtao();            // PostEffects
    void DrawHeightFog();       // PostEffects
    void DrawBloom();           // PostEffects
    void DrawPostProcess();     // EnvironmentInspector.cpp: the five above as sections of one object
    void DrawPointLight();      // Lights
    void DrawSpotLight();       // Lights
    void DrawTimeOfDay();
    void DrawSkyAtmosphere();   // Sky
    void DrawVolumetricCloud(); // Sky
    void DrawSkybox();          // Sky
    void DrawOcean();
    void DrawWind();

    EditorContext& ctx;
    EditorCommandStack& commandStack;
    const AssetRegistry& registry;
    EditorObject& env;
    bool showEnabled;
    EditorObjectId& activeEditObject;     // InspectorPanel's: the object a continuous edit runs on
    nlohmann::json& propertiesBeforeEdit; // ... and its properties when it began
    std::string& scrollToGroup;           // InspectorPanel::ScrollToGroup's request
    nlohmann::json& props;                // env.properties
    // P8B: WHICH sub-object the widgets below write to. Empty = the object's own properties,
    // which is every environment type that owns its settings directly. The folded "postProcess"
    // object points this at each group in turn, so the five settings blocks are reused as they
    // are instead of being rewritten against a nested path.
    //
    // Undo snapshots and the command payload stay on `props` REGARDLESS: EditEnvironmentCommand
    // replaces an object's whole property set, so handing it a group would delete its siblings.
    std::string groupKey;
    nlohmann::json missingGroup = nlohmann::json::object();
    std::string historyLabel; // the undo entry's name, by the object's type
};

#endif // WITH_EDITOR
