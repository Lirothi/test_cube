#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "core/math/Math.h"

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

// Draw the per-chunk / per-object boxes. Off leaves just the labels and the HUD block.
inline bool g_lodDebugBoxes = true;
// Draw the per-chunk / per-object text labels ("tier | distance | apparent size").
inline bool g_lodDebugLabels = true;
// Draw the SELECTION CRITERIA: the tier-boundary spheres around the camera (as their intersection
// with sea level, which is the readable slice of them) and, for the chunk under the crosshair, the
// exact closest-point segment the distance was measured along.
inline bool g_lodDebugCriteria = true;
// Include non-chunked meshes (palms, props). They select on a DIFFERENT criterion -- distance from
// the bounds CENTRE divided by the instance radius, against the ratio boundaries -- and the view
// reports each family against its own curve rather than forcing one set of numbers on both.
inline bool g_lodDebugRegularMeshes = true;
// Metres. Anything farther is skipped entirely -- labels past a few hundred metres are noise.
inline float g_lodDebugRange = 320.0f;
// Budget for drawn boxes. A populated level has hundreds of regular meshes, and every box is 12
// lines: drawing them all buries the terrain the view is usually there to judge. Chunks are drawn
// first (they ARE the terrain), then the nearest regular meshes fill what is left. The READOUT
// always counts everything in range, so the budget hides geometry, never numbers.
inline int g_lodDebugMaxBoxes = 140;

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
