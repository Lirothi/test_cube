#pragma once
#if WITH_EDITOR

#include <string>
#include <vector>

struct EditorContext;
struct EditorObject;

// Live-patch the running scene from an environment entity's `properties`, so
// inspector edits and viewport gizmo drags both take effect immediately:
//   pointLight / spotLight  -> LightManager light SetDesc
//   directionalLight        -> Scene::SetDirectionalLight
//   camera                  -> Scene camera (fov / near / far)
// No-op for skybox / ocean (those apply on the next level load). The renderer
// re-reads these each frame, so no dirty flag is needed for the runtime.
namespace EnvironmentRuntime
{
    void Apply(EditorContext& ctx, const EditorObject& env);

    // Rebuild the LightManager's spot/point light lists from the document's
    // environment entities, keeping only enabled ones (mirrors JsonLevel's
    // load-time skip). Call after toggling a light's `enabled` so it appears or
    // disappears live. CPU-only: it never frees GPU light buffers or SRVs.
    void RebuildLights(EditorContext& ctx);

    // Set an environment entity's `enabled` flag and apply it live: rebuild the
    // light lists (spot/point), re-apply the directional light with a zeroed
    // contribution, or show/hide the ocean. Writes properties["enabled"] and marks
    // the document dirty. Shared by the inspector + outliner enable checkboxes.
    void SetEnabled(EditorContext& ctx, EditorObject& env, bool enabled);

    // Ocean preset files under data/ocean (forward-slash paths), for the ocean
    // inspector's preset combo.
    std::vector<std::string> OceanPresets();
}

#endif // WITH_EDITOR
