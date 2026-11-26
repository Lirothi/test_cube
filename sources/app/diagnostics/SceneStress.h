#pragma once

#include <memory>

#include "app/harness/Harness.h"

struct HWND__;

namespace harness
{
// "--scene-stress[=<n>]", "--scene-stress-gbv", "--scene-stress-roughness", "--scene-stress-sky" (BootOptions::Harness
// sceneStress*): the level-churn stress -- reloads, switches, resizes, DLSS / render / reflection scale, an editor
// spawn and delete -- as a harness of App::Run's loop (architecture plan R3.3e), its ops at afterFrame. Its verdict is
// in logs/scene_stress.log and the session log; a fault terminates the process with its exit code.
std::unique_ptr<Harness> MakeSceneStressHarness(HWND__* window, Renderer& renderer, Scene& scene, LevelManager& levelManager);
}
