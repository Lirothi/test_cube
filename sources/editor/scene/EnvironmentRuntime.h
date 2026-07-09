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
//   skybox                  -> rebuild Scene skybox texture
//   ocean                   -> Simulation wind overrides
// The renderer re-reads these each frame, so no dirty flag is needed for the
// runtime.
namespace EnvironmentRuntime
{
    void Apply(EditorContext& ctx, const EditorObject& env);
    void ApplyChange(EditorContext& ctx, const EditorObject& env, const EditorObject& previous);
    void Remove(EditorContext& ctx, const EditorObject& env);

    // Rebuild the LightManager's spot/point light lists from the document's
    // environment entities, keeping only enabled ones (mirrors JsonLevel's
    // load-time skip). Call after toggling a light's `enabled` so it appears or
    // disappears live. CPU-only: it never frees GPU light buffers or SRVs.
    void RebuildLights(EditorContext& ctx);

    // Ocean preset files under data/ocean (forward-slash paths), for the ocean
    // inspector's preset combo.
    std::vector<std::string> OceanPresets();
}

#endif // WITH_EDITOR
