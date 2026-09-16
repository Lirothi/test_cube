#pragma once
#if WITH_EDITOR

#include "core/math/Math.h"
#include "editor/EditorSelection.h"
#include "editor/scene/EditorSceneDocument.h"

class Renderer;
class Scene;

// Pointing the camera at things. Lifted out of EditorController for the same reason the
// bulk actions were: the F key and the typed phrase "show me the palms" must move the
// camera the same way, and two copies of "where is this object and how big is it" would
// eventually disagree about instanced batches or environment entities.
//
// Nothing here is undoable. The camera is not in the document -- `freeCameraStart` is, but
// that is the level's stored start, not where the user happens to be looking.
namespace editorframing
{
    // Centre and radius of one object, runtime bounds first and the document's transform as
    // the fallback for entries with no live renderable. False when the id names nothing
    // that can be framed.
    bool TryGetFrameTarget(const Scene& scene,
        const EditorSceneDocument& document,
        EditorObjectId id,
        Math::float3& outCenter,
        float& outRadius);

    // The world AABB rather than the sphere around it. The camera does not care -- it needs
    // a distance -- but anything REPORTING an extent does: the bounding sphere of the atoll
    // is a 530 m cube around a 355 m island, and a model told that would place things two
    // hundred metres out to sea. Falls back to the sphere for entries with no runtime
    // renderable, where a sphere is all there is.
    bool TryGetWorldBounds(const Scene& scene,
        const EditorSceneDocument& document,
        EditorObjectId id,
        Math::float3& outMin,
        Math::float3& outMax);

    // Move the camera so the whole set fits. Keeps the current view DIRECTION and only
    // changes position: turning the camera as well would leave the user re-orienting
    // themselves after every jump. False when nothing in the set could be located.
    bool FrameObjects(Renderer& renderer,
        Scene& scene,
        const EditorSceneDocument& document,
        const EditorSelection& objects);

    // Everything enabled and visible in the level.
    bool FrameVisibleScene(Renderer& renderer, Scene& scene, const EditorSceneDocument& document);
}

#endif // WITH_EDITOR
