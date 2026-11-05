#pragma once
#if WITH_EDITOR

#include "app/game/PlayerStart.h"
#include "editor/scene/EditorSceneDocument.h"

class DebugDrawSystem;
class Scene;

// The Player Start in the editor: a document object with no renderable, as freeCameraStart and the zones are (docs/play_mode_plan.md).
namespace editorplayerstart
{
    // A new one, where the camera looks and facing as it faces: the ground it hits, else the ground under the camera, else the camera's own place.
    EditorObject BuildObject(Scene& scene);
    // The first one that is on: what Play starts from. Null = none, Play starts from the camera.
    const EditorObject* Find(const EditorSceneDocument& document);
    void FromObject(const EditorObject& object, game::PlayerStart& out);
    // Always on screen, as a zone is: it has nothing else to be seen by. A capsule of the player's size and an arrow from the eye.
    void Draw(DebugDrawSystem& draw, const EditorObject& object, bool selected);
}

#endif // WITH_EDITOR
