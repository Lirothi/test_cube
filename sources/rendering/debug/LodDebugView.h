#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "core/math/Math.h"
#include "core/threading/Knob.h" // game thread plan G5: knobs with a game twin

class Camera;
class Renderer;
class RenderableObjectBase;

namespace render
{
// --- LOD selection debug view -------------------------------------------------------------------
//
// Answers one question on screen: "why is THIS piece of geometry at THAT LOD?". It draws the
// decision (the tier each object/chunk ended up at) next to the INPUTS the decision was made from
// (the measured distance, the tier boundaries, the closest AABB point the distance is taken to),
// so a wrong-looking LOD can be traced to either the curve or the geometry without a rebuild.
//
// It reads state only -- every value shown is what SelectLod already stored this frame
// (RenderableObject::chunkLods_ / GetCameraLod()), never a re-derivation. If the view disagrees
// with the picture, the picture is right and one of them is reading the wrong array.
enum class LodDebugMode : int
{
    Off = 0,
    Tier,     // colour = the LOD tier that was selected
    Density,  // colour = APPARENT triangle size (mrad) at the selected tier
};

inline LodDebugMode g_lodDebugMode = LodDebugMode::Off;
inline LodDebugMode g_lodDebugMode_game = LodDebugMode::Off;
inline const bool g_lodDebugMode_knob = ::knob::Register(g_lodDebugMode_game, g_lodDebugMode, "render::g_lodDebugMode");

// What the view reports on. A populated level answers with hundreds of meshes at once, which is the
// wrong shape of answer for "why is THIS thing at THAT LOD" -- Selected narrows it to the editor
// selection (all chunks of a selected chunked mesh included). It never silently widens back to All
// when nothing is selected; the readout says the selection is empty instead, because a filter that
// quietly stops filtering is worse than an empty view.
enum class LodDebugFilter : int
{
    All = 0,
    Selected,
};

#if WITH_EDITOR
inline LodDebugFilter g_lodDebugFilter = LodDebugFilter::Selected;
#else
inline LodDebugFilter g_lodDebugFilter = LodDebugFilter::All; // no selection exists in this build
#endif
inline LodDebugFilter g_lodDebugFilter_game = g_lodDebugFilter; // game thread plan G5: the game's copy
inline const bool g_lodDebugFilter_knob = ::knob::Register(g_lodDebugFilter_game, g_lodDebugFilter, "render::g_lodDebugFilter");

// Draw the per-chunk / per-object boxes. Off leaves just the labels and the HUD block.
inline bool g_lodDebugBoxes = true;
inline bool g_lodDebugBoxes_game = true;
inline const bool g_lodDebugBoxes_knob = ::knob::Register(g_lodDebugBoxes_game, g_lodDebugBoxes, "render::g_lodDebugBoxes");
// Draw the per-chunk / per-object text labels ("tier | distance | apparent size").
inline bool g_lodDebugLabels = true;
inline bool g_lodDebugLabels_game = true;
inline const bool g_lodDebugLabels_knob = ::knob::Register(g_lodDebugLabels_game, g_lodDebugLabels, "render::g_lodDebugLabels");
// Draw the SELECTION CRITERIA: the tier-boundary spheres around the camera (as their intersection
// with sea level, which is the readable slice of them) and, for the chunk under the crosshair, the
// exact closest-point segment the distance was measured along.
inline bool g_lodDebugCriteria = true;
inline bool g_lodDebugCriteria_game = true;
inline const bool g_lodDebugCriteria_knob = ::knob::Register(g_lodDebugCriteria_game, g_lodDebugCriteria, "render::g_lodDebugCriteria");
// Include non-chunked meshes (palms, props). They select on a DIFFERENT criterion -- distance from
// the bounds CENTRE divided by the instance radius, against the ratio boundaries -- and the view
// reports each family against its own curve rather than forcing one set of numbers on both.
inline bool g_lodDebugRegularMeshes = true;
inline bool g_lodDebugRegularMeshes_game = true;
inline const bool g_lodDebugRegularMeshes_knob = ::knob::Register(g_lodDebugRegularMeshes_game, g_lodDebugRegularMeshes, "render::g_lodDebugRegularMeshes");
// Metres. Anything farther is skipped entirely -- labels past a few hundred metres are noise.
inline float g_lodDebugRange = 320.0f;
inline float g_lodDebugRange_game = 320.0f;
inline const bool g_lodDebugRange_knob = ::knob::Register(g_lodDebugRange_game, g_lodDebugRange, "render::g_lodDebugRange");
// Budget for drawn boxes. A populated level has hundreds of regular meshes, and every box is 12
// lines: drawing them all buries the terrain the view is usually there to judge. Chunks are drawn
// first (they ARE the terrain), then the nearest regular meshes fill what is left. The READOUT
// always counts everything in range, so the budget hides geometry, never numbers.
inline int g_lodDebugMaxBoxes = 140;
inline int g_lodDebugMaxBoxes_game = 140;
inline const bool g_lodDebugMaxBoxes_knob = ::knob::Register(g_lodDebugMaxBoxes_game, g_lodDebugMaxBoxes, "render::g_lodDebugMaxBoxes");

// Emit this frame's LOD debug geometry and text. Call on the main thread AFTER LOD selection
// (Scene::PrepareViews) and BEFORE TextManager::Build; a no-op when the mode is Off.
//
// `selected` is the editor selection (may be null/empty, and always is in a non-editor build); the
// Selected filter reports on exactly these objects.
void DrawLodDebug(Renderer* renderer,
                  const Camera& camera,
                  const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
                  const RenderableObjectBase* const* selected = nullptr,
                  std::size_t selectedCount = 0);
} // namespace render
