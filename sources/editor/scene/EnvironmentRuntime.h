#pragma once
#if WITH_EDITOR

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
}

#endif // WITH_EDITOR
