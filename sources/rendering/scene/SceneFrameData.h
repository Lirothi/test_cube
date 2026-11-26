#pragma once

#include <string>
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/math/Math.h"
#include "rendering/scene/SceneRenderConfig.h" // S8: CascadeShadowConfig, read by the lighting/glass CBs
#include "rendering/lighting/SsrSettings.h"
#include "rendering/lighting/HeightFogSettings.h"
#include "rendering/lighting/GtaoSettings.h"
#include "rendering/post/BloomSettings.h"
#include "rendering/core/PhotographicSettings.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/SkyAtmosphereSettings.h"
#include "rendering/lighting/TimeOfDaySettings.h" // N1: SkyMoonFrame
#include "rendering/lighting/VolumetricCloudSettings.h"
#include "rendering/shadows/VirtualShadowMap.h" // vsm::kNumClipmapLevels (Step 24d)
#include "core/threading/Knob.h" // game thread plan G5: knobs with a game twin

struct SceneView; // app/scene/SceneView.h: the views are reached through pointers here
class Camera;
class DirectionalLight;
namespace render { class LocalShadowCache; }
class OceanRenderable;
class RenderableObjectBase;
class Skybox;
class ShadowGpuData;
class SkyOcclusionMap;
class VirtualShadowMap;
class SdsmShadows;
namespace vfx { struct WindState; } // W3: global wind, read when building the gbuffer per-view CB
namespace vis { struct OcclusionQueryPlan; class OcclusionQueryHeap; class HzbOcclusionTester; } // occlusion plan S3a / S3b

// The sky's irradiance cube for the lit particles and the depth with the water for their soft fade: R4.4 moved both
// into render::DirectPassInputs (Renderer::DirectInputs). particles.softWater 0 = the opaque copy, the A/B (--set,
// transient).
inline bool g_particleSoftWater = true;
inline bool g_particleSoftWater_game = true;
inline const bool g_particleSoftWater_knob = ::knob::Register(g_particleSoftWater_game, g_particleSoftWater, "g_particleSoftWater");

// Where surface reflections come from (S8). None disables both traced/screen
// reflections and the skybox fallback; SkyOnly keeps only the skybox fallback;
// SSR is screen-space; RT is hardware ray-traced (Tier-1). RT is only honored when
// Renderer::IsRaytracingSupported() — otherwise the renderer falls back to SSR.
enum class ReflectionSource : uint32_t
{
    None = 0,
    SkyOnly = 1,
    SSR = 2,
    RT = 3,
    Count
};

// Debug/render toggles owned by the app layer (AppController maps input actions
// to these) and snapshotted into SceneFrameData each frame.
struct SceneRenderSettings
{
    GtaoSettings gtao{};
    HeightFogSettings heightFog{};
    SkyAtmosphereSettings skyAtmosphere{};
    VolumetricCloudSettings volumetricCloud{}; // plan part C; level-authored like the sky
    BloomSettings bloom{};
    // STAYS LogMarch. The UE march is finished and correct after P13, and it is the cheaper search,
    // but on WATER the log march's dense mask is markedly the better picture -- and water is the
    // largest reflective surface in this project's scenes. The UE march is selectable everywhere
    // (`ssr.technique`), including the ocean plane; it is not the default. Do not flip this back on
    // the strength of the cost numbers alone: the comparison that decided it was the image.
    SsrTechnique ssrTechnique = SsrTechnique::LogMarch;
    UeSsrSettings ssrUe{};
    // RT foliage alpha test: on a FAILED alpha test the hit is still kept with this probability
    // (a FROZEN per-pixel dither -- static patterns measured strictly calmer than per-frame
    // re-rolls, see the frameSeed fill site). A 1-ray/px trace at reflection res undersamples
    // thin fronds and the reflected crown reads smaller than the real one; this inflates
    // coverage back. 0 = honest cutout, 1 = the old solid cards.
    float rtAlphaMissKeep = 0.15f;
    // RT foliage alpha mode. 0 = OFF: FORCE_OPAQUE traversal, foliage = solid cards, cheapest.
    // 1 = FIRST HIT: opaque traversal, then the committed hit is alpha-tested with the albedo
    //     sample shading fetches anyway -- transparent texels become misses (holes show the sky
    //     fallback, not what is truly behind); costs the same as OFF.
    // 2 = FULL: exact per-candidate alpha testing during traversal, the expensive one.
    uint32_t rtAlphaMode = 2u;
    // Interior lighting plan S: the RT sky visibility (UE DFAO's bent normal, traced on the TLAS at
    // half resolution, rt_sky_visibility_cs.hlsl) occluding the sky's diffuse and specular. Needs RT
    // hardware; off = the open sky everywhere, exactly the frame before it. `skyVisRays` per
    // half-resolution pixel per frame, accumulated with `skyVisHistory` (the weight of the
    // reprojected past); `skyVisMaxDistance` metres of ray -- an occluder farther than this does not
    // count (UE's SkyLight OcclusionMaxDistance, 10 m by default; ours reaches over a palm crown).
    // `skyVisDiffuse` / `skyVisSpecular` scale the two effects (0 off .. 1 UE).
    // Measured 2026-10-23 (wind_test grove, half of 2560x1440, GPU): 4 rays 0.28 / 0.31 / 0.34 ms at
    // 10 / 15 / 30 m, 8 rays 0.63 ms, 2 rays 0.18 ms -- the cost is the ray count, not the reach.
    // 8 rays and 0.98 since 2026-10-26 (owner: "what makes the noise here", a tunnel he built of boxes to watch the
    // light inside): a floor that sees the sky only through the tunnel's mouth gets it from a ray in a few, frame to
    // frame median 8.6 % (p90 12.3) at 4 rays / 0.95; 8 rays 5.6 %, 0.98 5.2 %, both 3.5 % (p90 5.1); SkyVis off
    // 0.2 % but the floor then takes the whole sky. 0.14 -> 0.25 ms there. The longer history costs the newly
    // uncovered surfaces of a moving camera a slower settle.
    bool skyVisEnabled = true;
    uint32_t skyVisRays = 8u;
    float skyVisMaxDistance = 15.0f;
    // With the spatial filter below (2026-10-24): wind_test's second room at 24:00, a wall lit by its window's
    // moonlit sky, per-pixel temporal noise over 12 shots median 9.4 % -> 3.2 % at 0.95 (p90 18 -> 5.7 %); 0.97
    // 2.8 %, 0.97 with 8 rays 2.5 % (+0.075 ms); SkyVis off 0.35 %. scratchpad noise_map.py.
    // 0.995 since 2026-10-27 (owner: "can you remove SkyVis's dancing" -- in his tent, cam 71.17 2.11 41.16, 15:00):
    // with the frame-count blend a still texel's estimate drifts by 1 / (1 + frames) of a few rays a frame, and the
    // 9x9 filter turns that into crawling blotches. The visibility alone (skyVis.debug:2), wind on, 60 consecutive
    // frames, two runs each: the fabric's frame-to-frame std median 1.5 / 0.67 / 0.40 % at 0.98 / 0.99 / 0.995 (p90
    // 3.1 / 1.45 / 0.98 %), the floor 1.4 / 0.64 / 0.33 %; 16 rays at 0.98 only 0.94-1.18 % for +0.1-0.27 ms. Free; a
    // moving camera is unaffected (rejected texels count from zero), but a surface whose sky a MOVED object changes
    // follows it over up to 199 frames instead of 49 (0.7 s at 280 fps, 3.3 s at 60).
    float skyVisHistory = 0.995f;
    float skyVisDiffuse = 1.0f;
    float skyVisSpecular = 1.0f;
    // The spatial half of the denoise (rt_sky_visibility_cs.hlsl CSAccumulate, UE DFAO's spatial filter): a 9x9
    // half-resolution gather of EACH FRAME'S rays before they enter the history, a tap counting while its relative
    // depth step is under `skyVisFilterDepth` and by its normal's agreement to the `skyVisFilterNormalPower` (also the
    // seed's weights). Off = the rays as they are, accumulated.
    bool skyVisFilter = true;
    // ...on each frame's rays BEFORE the history (UE DFAO's order: UpdateHistoryDepthRejectionPS runs its
    // GeometryAwareUpsample, then blends). false = the gather over the accumulation, after it -- the order before
    // 2026-10-27, whose weights followed the DLSS jitter unaveraged (the tent's hem trembled), kept for the A/B
    // (`--set skyVis.filterFirst:0`).
    bool skyVisFilterFirst = true;
    float skyVisFilterDepth = 0.05f;
    float skyVisFilterNormalPower = 8.0f;
    // 1 = paint the texels that had no history this frame as the open sky (bright wherever the sky is shut out):
    // what the temporal accumulation rejects -- a disocclusion, the screen's edge, a failed same-surface test.
    // 2 = the lit frame is the sky visibility alone, a gray (four times it: a quarter of the sky is white): the estimator without textures.
    uint32_t skyVisDebug = 0u;
    // How a texel's rays are rotated: 0 = white noise per texel and frame, 1 = UE's interleaved gradient noise
    // (rt_sky_visibility_cs.hlsl says what it measured).
    uint32_t skyVisSampling = 1u;
    // The spatial filter's reach in half-resolution texels (1-4; 2 = the 5x5 UE DFAO gathers), its Gaussian sigma
    // 0.75 of it. 4 since 2026-10-26 (rt_sky_visibility_cs.hlsl CSAccumulate says what it measured).
    uint32_t skyVisFilterRadius = 4u;
    // A texel without history is seeded from the neighbours that kept theirs (UE DFAO FilterHistoryPS). Off = it
    // starts from its own few rays, the behaviour before 2026-10-26 -- kept for the A/B (`--set skyVis.seed:0`).
    bool skyVisSeed = true;
    // ...searched near first (rt_sky_visibility_cs.hlsl kSkyVisSeedNearRadius): the 5x5, the ring out to 9x9 only when
    // that holds nothing. `--set skyVis.seedNearFirst:0` = the one 9x9 search, the A/B (transient).
    bool skyVisSeedNearFirst = true;
    // ...and the AIR's: the volumetric fog's sky term times the share of UE's 45-degree cone straight up that a cell
    // sees (fog_scatter_cs.hlsl FogSkyVisibility, UE VolumetricFog.usf:713-735), `skyVisFogRays` per cell sample per
    // frame out to skyVisMaxDistance, averaged by the fog's own history. 0 = the open sky in the air, the frame before.
    uint32_t skyVisFogRays = 2u;
    // WITHOUT the RT trace (skyVis off, or no RT hardware): the same bent normal from the world seen from above
    // (SkyOcclusionMap, sky_occlusion_cs.hlsl) -- crowns, roofs, rocks as slabs of air over the ground, out to
    // skyVisMaxDistance. Off = the open sky everywhere, the frame before 2026-11-09 (owner, the island at 18:00 with
    // skyVis off: "why is everything so pale"). With the RT trace on it never runs, so that frame does not move.
    bool skyVisFallback = true;
    // A slab's coverage -> its opacity: straight down through it passes 1 - coverage x this (--set only).
    float skyVisFallbackOpacity = 1.0f;
    // Nearer than this (m) a direction meets no slab: there the receiver's own texel is a slab around it (a trunk's
    // column, a tent's), which GTAO's contact radius answers instead (--set only). Against the RT frame on the owner's
    // island view at 18:00 (pixels over 8 levels from it): 1 m 12.8 %, 0.5 m 8.8 %, 0 9.0 / 7.9 % -- 0.5 the nearest
    // that keeps a margin over the 1 m texel's own column.
    float skyVisFallbackStart = 0.5f;
    // The map's size: its occlusion level's texels a side (a power of two, 64-1024; the depth renders are 4x that, the far
    // level a quarter) and the world size of one of those texels (m). It covers resolution x texel metres around the
    // camera: 512 x 1 m = 512 m, 26 MB (3 x 8 MB of depth). 256 m (the first cut) left the back of wind_test's grove
    // outside it from the owner's view at 18:00 -- a pale band behind the palms. A coarser texel reaches farther and
    // spreads a roof's or a crown's edge (2 m: the bathroom by day +89 of 255 over the RT frame, 1 m +52); a finer one
    // keeps it and reaches less (0.5 m: the grove 34 % of pixels off the RT frame, 1 m 17 %). A resolution change re-creates the textures (Scene::ReconcileSkyOcclusion, GPU idle).
    uint32_t skyVisFallbackResolution = 512u;
    float skyVisFallbackTexel = 1.0f;
    // Its edge: the occlusion fades out with the horizontal distance from the camera over this share of the radius the
    // map traces in full (half its width, less half a snap step and the reach), the open sky beyond -- a gradient around
    // the camera instead of the map's square in the world (owner, 2026-11-09: "at the texture's edge the transition shows
    // strongly"). 0 = the hard edge. At the default 512 m map 0.25 fades from 175 to 233 m, past wind_test's island
    // from anywhere on it; 0.5 (from 116 m) brought a paler band back to the back of the grove from the owner's view.
    float skyVisFallbackFade = 0.25f;
    // The visibility rays -- the sky visibility, the GI's probe, shadow and sky rays, the fog's sky and sun -- take a
    // cutout card (an alpha-tested slot: a leaf, a frond) at its MESH's opacity (mesh.json "rtCutoutOpacity", the palms
    // 0.8; absent = solid): a ray that hits one carries 1 - opacity on and goes no further (rt_geometry.hlsli
    // RtVisibilityTransmittance). 0 = every card solid, the frame before 2026-10-27 (`--set rt.meshOpacity:0`, the A/B);
    // 1 = anything solid in the way blocks, then the transmissive meshes' first surface found (two any-hit queries, 2026-10-28);
    // 2 = the nearest surface of the whole scene in one query (2026-10-27, the A/B of 1). The reflections' alpha modes
    // (rtAlphaMode) are theirs alone.
    uint32_t rtMeshOpacity = 1u;
    // `rt.cutoutOpacity` (--set / MCP only, transient; < 0 = off): every mesh whose asset says rtCutoutOpacity < 1 takes
    // this one instead -- a live A/B of the trees without editing their mesh.json. A mesh that says nothing stays solid.
    float rtCutoutOpacityOverride = -1.0f;
    // The volumetric fog's samples less than a slice behind the surface their pixel sees are moved this many slices in
    // front of it (fog_scatter_cs.hlsl FogApplyDepthConstraint, UE r.VolumetricFog.GridCenterOffsetFromDepthBuffer 0.5);
    // < 0 = off, the cells straddling a wall then light the air in front of it with the air behind (`--set fog.depthOffset:-1`).
    float fogDepthOffset = 0.5f;
    // ...and how many slices behind the surface a sample may be and still be moved (UE
    // r.VolumetricFog.OffsetThresholdToAcceptDepthBufferOffset, 1: "only the first layer of froxels intersecting depth").
    // Ours moves EVERY sample behind its pixel's surface (128 = the grid's depth and more): at UE's 1 a wall seen at a
    // grazing angle still leaked -- the next column's cell at this pixel's depth lies several slices behind ITS wall,
    // out of reach, and the trilinear read took its outside light (wind_test's tunnel, 15:00: one ring per slice on
    // both walls at 1, none at 64). UE's worry, the edges of meshes, measured on three open views (toward the sun over
    // the palms and the sea, away from it, low in the grove): against the constraint off, 1 and 128 both move the mean
    // by <= 0.15 % and the changed pixels sit at the noise floor of the same shot twice (foliage under DLSS).
    float fogDepthThreshold = 128.0f;
    // Under SDSM the air outside every partition box (in front of the nearest surface, beyond the farthest) traces the
    // sun on the TLAS the fog's sky visibility binds (fog_scatter_cs.hlsl FogSunShadow) instead of falling through to
    // "lit": SDSM's boxes hold the receivers, never the air -- a closed tent's air glowed with the sun through its roof.
    // VSM's air in no resident page the same since 2026-11-06 (a closed room's air may have none).
    // false = no trace, the A/B (`--set fog.sunRtFallback:0`).
    bool fogSunRtFallback = true;
    // Interior lighting plan D: the GI probe volume (DDGI over RayQuery, rendering/lighting/DdgiVolume.h):
    // 32x16x32 probes `giSpacing` m apart around the camera, 1 / `giUpdateDivisor` of them tracing 64
    // rays a frame and blending them in with `giHysteresis`. They hold the BOUNCE only (a miss brings
    // nothing; the sky is plan S's), which lighting_cs adds to the indirect diffuse x `giIntensity`
    // (1 = physical). `giRayDistance` m of ray; `giInfiniteBounce` feeds the volume back into its own
    // hits; `giDebug` 1 = the lit frame shows the bounce alone, 2 = the probes around each point (red moved by
    // their last update, green relocated off their grid point, blue inside geometry; ddgi_common.hlsli
    // DdgiProbeStates). Needs RT hardware; off = no bounce.
    bool giEnabled = true;
    float giSpacing = 0.5f;
    uint32_t giUpdateDivisor = 4u;
    // 0.98, not RTXGI's 0.97 at its 256 rays: 64 rays an update left the bathroom's walls and ceiling
    // breathing in metre-sized blotches (frame-to-frame std 0.68 sRGB levels at 0.9, 0.37 at 0.97, 0.29
    // at 0.98 -- 0.17 without GI; owner 2026-10-24 "dark spots dancing"). The divisor does not change the
    // noise (0.61 at 1/1), only how fast it decorrelates: 4 / (1 - 0.98) = 200 frames to settle.
    float giHysteresis = 0.98f;
    float giIntensity = 1.0f;
    // 200 m, UE's r.Lumen.MaxTraceDistance (2026-11-03, the UE A/B). At 30 m a probe more than 30 m over the ground
    // never saw it: a 4 m grey box 45 m over the island took 0.69-0.71 of UE's light on its faces turned from the sun
    // and bluer (the sunlit sand's warm bounce missing), 0.87-0.95 and UE's colour at 120 m; the trace +0.15 ms seen
    // from 35 m up, +0.02-0.04 on the grove (its rays end on geometry anyway).
    float giRayDistance = 200.0f;
    bool giInfiniteBounce = true;
    // The trace's price (ddgi_trace_cs.hlsl; Developer Controls > GI "Trace cost", graphics_settings.json gi.*). The
    // blended rays a probe traces an update (16-64, a multiple of 16): fewer = a noisier estimate the hysteresis
    // averages longer. A probe's 32 FIXED rays (its classification and relocation; their directions never change) every
    // `giFixedRayPeriod`-th update -- in between their last answers in its row hold, and a probe new to its cell or
    // moved traces them at once. A hit's sky through a ray (`giSkyRay`; off = the open sky at every hit), the lamps
    // (`giLights`), and the infinite bounce's Chebyshev visibility (`giBounceVisibility`; off = the probes round the hit
    // blend without their depth moments: 8 reads a hit less, a bounce of a bounce that may leak through a wall).
    uint32_t giRays = 64u;
    uint32_t giFixedRayPeriod = 4u;
    bool giSkyRay = true;
    bool giLights = true;
    bool giBounceVisibility = true;
    // A probe its last update found inside geometry traces only its fixed rays and blends nothing (RTXGI; nothing reads
    // it). `--set gi.skipInactive:0` = it traces and blends as any other, the A/B (transient).
    bool giSkipInactive = true;
    // A probe no pixel has read for 32 frames (ddgi_usage_cs.hlsl stamps the 8 round every 4th pixel each way) -- behind
    // the camera, under the ground, up in the air -- updates one turn in this many; 1 = every turn (before). The steady
    // picture does not move, only how fast a probe the camera turns to has followed a change of the light.
    uint32_t giSleepPeriod = 4u;
    // A probe ray that meets a two-sided material (MaterialData::doubleSided) from behind met its other face, not the
    // inside of a solid (ddgi_trace_cs.hlsl). 0 = every opaque back face is a solid's inside, the behaviour before.
    bool giTwoSidedSheets = true;
    // The FAR cascade: a second volume of the same 32x16x32 probes, `giFarSpacing` m apart, around the
    // near one, so the bounce reaches past +-8 m (64 x 32 x 64 m at 2 m). Lighting takes the near cascade
    // where it is whole and blends to the far one over the near's outer 2 m. It updates 1 / giFarDivisor
    // of its probes a frame, its own infinite bounce. Off = the near cascade alone, as before.
    bool giFarCascade = true;
    float giFarSpacing = 2.0f;
    uint32_t giFarDivisor = 4u;
    uint32_t giDebug = 0u;
    // Glass refraction (owner 2026-10-24: "the glasses pass the background straight through"): a
    // thinTranslucent slot bends what is behind it as a glass shell of the surface's own curvatures,
    // this thick under its surface (m), refracted in and out onto the opaque scene behind (glass.hlsl
    // GlassRefractedUv). 0 = straight through, the flat transmission before it; a few mm = a hollow
    // tumbler's wall (the_bathroom_free's are modelled 4.3 mm); at least the shape's radius = SOLID glass,
    // the default since the owner picked Sketchfab's look ("сделай чтобы как в скечфабе", 2026-10-24) --
    // its screen-space refraction bends every cup like a solid block. A pane is flat: no bend either way.
    float glassWallThickness = 0.5f;
    // RW: wind-deformed per-instance BLASes for near casters, so foliage sway reaches RT
    // reflections (and later RT shadows). Off = every caster reflects its rest pose.
    bool rtWindBlas = true;
    // Casters inside this radius get a deformed BLAS (nearest-first, capped by the slot pool);
    // beyond it the sway is sub-pixel in a reflection and the shared static BLAS stands -- the
    // same near-only rule the VSM applies by keeping wind rigid beyond clipmap L2.
    float rtWindBlasRadius = 40.0f;
    // Reflection temporal resolve, BOTH sources. A screen-space ray is violently sensitive to its
    // own start, so under DLSS's per-frame jitter the raw buffer boils even with a still camera;
    // RT at half reflection res boils the same way once reflected foliage is subpixel. Unreal
    // never show theirs unfiltered either. Defaults ON -- see ssr_temporal_cs.hlsl.
    bool ssrTemporal = true;
    // UE's AA_LERP 8 for ETAAPassConfig::ScreenSpaceReflections: this frame is worth 1/8.
    float ssrTemporalBlendWeight = 0.125f;
    // How much the neighbourhood clamp box may widen when the camera is still (0 = never).
    float ssrTemporalClampExpand = 0.8f;
    // The WATER reflection's own temporal resolve (Main_WaterReflection), separate from the one above.
    // Off by default (owner, 2026-10-10: "дико все мажет"): the resolve decides stillness from
    // gbVelocity, which under water is the SURFACE's motion -- ~0 with a still camera -- so the still
    // inertia relaxes the clamp and stretches the history ~4x while the waves change the reflection
    // every frame. Off = the trace's raw output; DLSS still accumulates the final image.
    // On, it runs only while no temporal upscaler accumulates the frame -- UE's rule
    // (ScreenSpaceRayTracing.cpp:213-222 IsSSRTemporalPassRequired, which SingleLayerWaterRendering.cpp:1748 asks for
    // the water too): under DLSS the two accumulators in a row turned the trace's grain into crawling blotches
    // (2026-11-02, the owner's lagoon view: the reflection's coarse detail +40 %, its fine grain not lower).
    bool waterReflectionTemporal = false;
    // UE's r.SSR.Temporal 1: the water resolve under DLSS too (`--set ssr.waterTemporalUnderDlss:1`, transient; the A/B).
    bool waterReflectionTemporalUnderDlss = false;
    // Its own blend and still inertia (the deferred resolve's knobs above stay the deferred one's).
    // Inertia defaults to 0 for the reason above: the surface's own motion reads as "still", and the
    // inertia's relaxed clamp + longer history is exactly what smeared the waves.
    float waterReflectionTemporalBlend = 0.125f;
    float waterReflectionTemporalStillInertia = 0.0f;
    // EXPERIMENT (2026-10-12, UE's way): trace the water from its own reflection normal (GB1) and read
    // the result at the pixel itself, instead of a flat mirror broken up by the planar UV offset.
    // `--set=ssr.waterSurfaceNormal:1`, or `--sweep=ssr.waterExperiment:...` for the whole variant.
    bool waterReflectionSurfaceNormal = false;
    // How many FFT cascades (largest first) the water's reflection normal GB1 takes -- what the trace
    // above casts its rays about, and what the UV break-up reads when it is off. 4 = all of them, the
    // shading normal; 2 = the swell only (the owner's idea, 2026-10-12: the far ripples a pixel cannot
    // resolve sent neighbouring rays apart). The sky in the reflection keeps the full normal.
    // Handed to the ocean every frame (OceanRenderable::SetReflectionNormalCascades).
    std::uint32_t waterReflectionNormalCascades = 4u;
    // How many mips coarser than the shading normal the water reflection's DIRECTION is taken (the sky's and GB1's; the
    // surf-sim surface), its dropped slope variance going to the sky's blur -- AGENTS.md "The Water Reflection". 0 = the
    // shading normal. Developer Controls > Reflections, graphics_settings.json reflections.waterNormalFilter, `--set
    // ocean.reflectionNormalFilter`; handed to the ocean every frame (OceanRenderable::SetReflectionNormalFilter).
    float waterReflectionNormalFilter = 0.0f;
    // The water reflection's prefilter (water_composite_cs, AGENTS.md "The Water Reflection"): the Gaussian sigma across
    // the screen's columns in render pixels, half that along them; 0 = off. Developer Controls > Reflections,
    // graphics_settings.json reflections.waterPrefilter, `--set ocean.reflectionPrefilter`.
    float waterReflectionPrefilter = 1.0f;
    bool doFxaa = false;
    bool debugTexMode = false;
    // Which target the fullscreen debug blit shows. It used to be hardwired to the cascade shadow
    // atlas, which meant a half-res intermediate could only ever be judged by opening the texture
    // viewer in the GUI -- no headless capture, so no A/B and no gate. The blit shows .rrr, which
    // is what every one of these is.
    //   0 = cascade shadow atlas (what it always showed)
    //   1 = GTAO raw   2 = GTAO denoised   3 = GTAO temporal   4 = GTAO upsampled (render res)
    //   5 = HZB furthest (mip selected by debugTexMip)   6 = scene depth
    //   7 = HZB closest (debug/P9; UE SSR reads target 5, the furthest chain)
    //   8 = the reflection buffer's ALPHA, i.e. which pixels' rays found a hit
    int debugTexTarget = 0;
    // Mip shown for a target that has a chain (P6C HZB). Ignored by single-level targets.
    int debugTexMip = 0;
    // THE PIXEL PROBE (editor MCP get_readouts; `--set=debug.probeU/probeV/probeRadius`): the
    // brightest pre-exposed HDR pixel inside a window of `debugProbeRadiusPx` around (u, v) of the
    // frame the tonemap reads, the window's mean, a 4x4 box at the brightest pixel (what one texel of
    // the bloom's FFT grid at 25 % averages) and the brightest pixel after the local exposure the FFT
    // input gets -- read back with the exposure record (bloom_conv_cs.hlsl stage 11). It measures
    // what a screenshot cannot: the tone curve has already clipped a glint to white there.
    // Radius 0 = off, nothing is dispatched.
    float debugProbeU = 0.5f;
    float debugProbeV = 0.5f;
    float debugProbeRadiusPx = 0.0f;
    // Which frame the probe reads: 0 the one the tonemap and the bloom read (the upscaler's output
    // when DLSS runs), 1 the scene BEFORE the upscaler (render resolution; the same image without DLSS),
    // 2 the bloom target the tone curve adds (pre-exposed, its own resolution).
    std::uint32_t debugProbeSource = 0u;
    // The SUN probe (bloom_conv_cs.hlsl stage 10) reads the scene before the upscaler; true = the upscaler's output, the
    // frame before 2026-10-28 (`--set debug.sunProbeAfterDlss:1`, the A/B; DLSS rings on the disc).
    bool sunProbeAfterDlss = false;
    // The sun's direct light gated by the visible side's GEOMETRIC normal (lighting_cs.hlsl SunGeometricTerminator):
    // a normal map may tilt a pixel toward a sun that lies behind its triangle -- the back of a tent's cloth lit through
    // itself, where no shadow map helps (the receiver is the caster). 1 = only where the visible side's VERTEX normal
    // faces away from the light too (G-buffer GB1.a), 2 = on every facet that does, the frame 2026-10-28 .. 11-02 (its
    // dark triangles on a low-poly dune's lee slope), 0 = the frame before 2026-10-28 (`--set light.geometricTerminator`).
    std::uint32_t sunGeometricTerminator = 1u;
    // Hanika's shadow terminator offset (lighting_cs.hlsl, gbuffer_common.hlsli GBufferTerminatorHeight): the sun's
    // shadow looked up this many times the height of the smooth surface the vertex normals describe above the flat
    // triangle. 1 = that surface; 0 = the triangle, the frame before 2026-11-02 (`--set light.terminatorOffset`, the A/B).
    float terminatorOffset = 1.0f;
    // A two-sided foliage leaf passes the sky and the ground's bounce behind it (lighting_cs.hlsl, UE's backface sky
    // lighting); false = only the sun through it, the frame before 2026-10-31 (`--set light.foliageBackSky:0`, the A/B).
    bool foliageBackSky = true;
    // The sun through a two-sided foliage leaf takes UE's TwoSidedBxDF lobe (utils.hlsli EvalFoliageBRDF: a wrap of the light
    // behind the face times GGX alpha 0.6 around the line to the light -- ~2x the old lobe looking straight into the sun);
    // false = the softer lobe before 2026-11-03 (`--set light.foliageTransmissionUe:0`, the A/B).
    bool foliageTransmissionUe = true;
    // The sun's direct diffuse is UE's rough diffuse (utils.hlsli DiffuseEON: EON at roughness x 0.4, what the owner's UE
    // renders -- BaseWindowsEngine.ini r.Substrate.RoughDiffuse=1); false = Lambert, the A/B (`--set light.roughDiffuse:0`).
    // The local lights, glass and the RT hits take it either way.
    bool roughDiffuse = true;
    // The direct sun takes the bump terminator (utils.hlsli SunBumpShadowing, GBAux.b's high bits): it fades as the smooth
    // surface turns from the sun whatever the normal map says -- a soft edge where the island's own shadow cut a lit,
    // normal-mapped beach; false = before (`--set light.bumpShadowing:0`, the A/B).
    bool bumpShadowing = true;
    bool showProfiler = false;
    // S8: the reflection source. Default RT (today's behavior). RT runs the
    // Tier-1 ray-traced pass instead of SSR; SkyOnly clears the reflection buffer
    // but keeps skybox specular; None disables both. RT auto-falls back to SSR on
    // non-RT hardware.
    ReflectionSource reflectionSource = ReflectionSource::RT;
    // S6: RT hit/visibility debug viz (dev tool). Traces a reflection ray per
    // pixel and writes a hit-distance/miss image into the reflection target (view via
    // the texture inspector -> Reflection). Builds the AS regardless of source.
    bool rtDebugView = false;
    // S16: glossy reflections. The reflection blur radius scales with the reflector's
    // roughness by this factor (in reflection-res pixels at full roughness); 0 = sharp
    // mirror reflections. The SSR's only, since the RT reflection spreads over its GGX lobe
    // (reflectionLobeBlur). Tunable in the developer window.
    float reflectionGlossyScale = 1.0f;
    // The RT reflection's glossy filter (reflection_glossy_cs.hlsl): each pixel's sharp mirror reflection spread
    // over its GGX lobe as seen on the screen -- the hit distance against the reflector's, the roughness's lobe
    // width, the projection. Off = S16's roughness-scaled separable blur, as before 2026-10-26 (the A/B; --set
    // reflections.lobe:0).
    bool reflectionLobeBlur = true;
    // The reflections' temporal resolve also reprojects its history by the RT reflection HIT (Lumen's
    // LightingHistoryFromHit, ssr_temporal_cs.hlsl), for narrow lobes: a mirror's image moves with the reflected scene,
    // not with the mirror. Off = the reflector's motion alone, as before 2026-10-26 (`--set reflections.hitReprojection:0`).
    bool reflectionHitReprojection = true;
    // The RT mirror ray is kept above, and clears, the reflector's GEOMETRIC plane rebuilt from the depth
    // (rt_reflect_common.hlsli RtSetupReflectionRay): a normal map turned grazing rays into the surface, and they left
    // the room through its wall. Off = the shading normal alone, as before 2026-10-28 (`--set reflections.geometricNormal:0`).
    bool reflectionGeometricNormal = true;
    // Analytic sun specular boost on metals. The sun's specular lobe is scaled by
    // (1 + metal*sunMetalSpecInfluence) in the lighting pass so a smooth metal shows a
    // distinct sun highlight (which otherwise merges into the environment reflection,
    // since the sun disk is not painted into the skybox). 0 = pure physical. Dev-tunable.
    float sunMetalSpecInfluence = 0.0f;
    // The sun's angular size is the SUN'S (DirectionalLight::GetSunHalfApexRadians, UE's
    // LightSourceAngle): it floors the analytic specular lobe AND draws the procedural disc, so it
    // lives on the level's light rather than here (part-B review, 2026-09-11).
};

// Per-frame inputs for the render passes. Scene::PrepareViews fills this once per
// frame; pass bodies read from it instead of reaching back into Scene members.
// Scene keeps ownership of objects, lights, and views — this struct only caches
// derived per-frame data (cascade matrices) and points at the rest.
struct SceneFrameData
{
    static constexpr int kCascades = 4;

    struct CascadeData
    {
        mat4 lightView[kCascades];
        mat4 lightProj[kCascades];
        float2 atlasScale[kCascades];
        float2 atlasBias[kCascades];
        float splitsVS[kCascades + 1] = {}; // near..far in view space
        float cascadeTexelWS[kCascades] = {}; // world size of one cascade texel (see Scene.cpp)
        float depthBiasNDC[kCascades] = {};

        // S0.1: per-cascade diagnostics for the developer window. Written by Scene::UpdateCascades
        // straight from the values the cascade was actually built with (NOT recomputed by the UI —
        // a recomputed estimate would hide exactly the fit bugs this readout exists to catch), and
        // read only by DeveloperWindow. Nothing here reaches the GPU.
        float sphereRadiusDbg[kCascades] = {};   // fitted bounding-sphere radius, BEFORE padding
        float radiusDbg[kCascades] = {};         // radius after padding — drives unitsPerTexel
        float unitsPerTexelDbg[kCascades] = {};  // world units per shadow texel (the density metric)
        float nearLsDbg[kCascades] = {};         // light-space ortho near plane
        float farLsDbg[kCascades] = {};          // light-space ortho far plane
        std::uint32_t tileSizeDbg[kCascades] = {}; // atlas tile edge in texels

        // S11: the cascade's view-cone scissor in ATLAS texels (Scene::ComputeCascadeScissor).
        // Always computed, applied by Pass_CSM only while CascadeShadowConfig::scissorOptim is on,
        // so the readout can show what the optimisation WOULD cut before it is switched on.
        struct ScissorRect { std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
        ScissorRect scissor[kCascades] = {};
        float scissorAreaDbg[kCascades] = {};    // rect area / content-rect area, 1 = nothing cut

        // S14: the cascade's caster-cull volume. Plane count (6 = the ortho box, 7..11 = the
        // accurate convex volume) and how many objects the CPU cull let through LAST frame
        // (the queue is filled after UpdateCascades runs), so a headless run can show the cut.
        std::uint32_t cullPlanesDbg[kCascades] = {};
        std::uint32_t cullCastersDbg[kCascades] = {};
        // Objects in LAST frame's queue that the OTHER volume OF THAT FRAME rejects: with the
        // accurate volume applied, how many of its survivors the plain box would reject (must be
        // 0 -- the volume carries every box plane); with the box applied, how many of its
        // survivors the accurate volume would have cut (the saving). A cross-check the image
        // cannot give. Same-frame pair on purpose (Scene::cascadeBoxPrev_/AccPrev_): against this
        // frame's box a camera turn counts as a leak. Dynamic/moved casters are not counted.
        std::uint32_t cullLeakDbg[kCascades] = {};
    };

    const Camera* camera = nullptr;
    SceneView* mainView = nullptr;
    std::array<SceneView, kCascades>* cascadeViews = nullptr;
    std::array<SceneView, vsm::kNumClipmapLevels>* clipmapViews = nullptr; // Step 24d: directional clipmap (VSM)
    // The same levels expressed in the SHARED light frame, for the clipmap LOD fallback chain
    // (shaders/vsm_page_propagate_cs.hlsl). Null when the level has no valid sun.
    const vsm::ClipmapSquares* clipmapSquares = nullptr;
    // SMRT temporal dither phase, 1..64; 0 = temporal rotation off. Lives here so the lighting
    // pass and the glass pass rotate their sample sets IDENTICALLY -- two different phases would
    // make a window shade against a differently-sampled shadow than the ground beneath it.
    std::uint32_t smrtFrameIndex = 0;
    // Local-light plan S9e: one view per shadow slot of the current capacity (render::ShadowSpotCapacity, 6 per
    // render::ShadowPointCapacity) -- the first GetShadowed*Count of them are this frame's.
    std::vector<SceneView>* spotShadowViews = nullptr;
    std::vector<SceneView>* pointShadowViews = nullptr;
    LightManager* lightManager = nullptr;
    Skybox* skybox = nullptr;
    const std::vector<std::unique_ptr<RenderableObjectBase>>* objects = nullptr;
    // Occlusion plan S3a: this frame's box queries (decided by the camera prepare) and the heap
    // that records them; null / empty batches = the pass declares nothing.
    const vis::OcclusionQueryPlan* occlusionPlan = nullptr;
    vis::OcclusionQueryHeap* occlusionQueries = nullptr;
    // S3b: the HZB tester that runs the plan's boxes when its method is Hzb (Main_VisTest).
    vis::HzbOcclusionTester* hzbTester = nullptr;
    // S3a.6: per light index, 1 = the light's influence volume was occluded last frame -- the
    // GPU light entry is zeroed (it can light no visible pixel) and its shadow views were built
    // with a reject-all frustum (their passes clear and draw nothing). Sized to the light lists.
    const std::vector<std::uint8_t>* spotLightOccluded = nullptr;
    const std::vector<std::uint8_t>* pointLightOccluded = nullptr;
    const DirectionalLight* dirLight = nullptr;
    // Time-of-day plan T2: the light the SKY is lit by -- its LUTs, the sky pixel and disc, the capture,
    // the distant light, the aerial perspective. The same object as dirLight unless the level's time of
    // day has handed dirLight to the moon; the sky keeps the sun, so the twilight outlives the hand-over.
    const DirectionalLight* skyLight = nullptr;
    // Time-of-day plan N1: the moon as the sky sees it -- the atmosphere's second light and the drawn
    // disc (SkyViewFrameData light 1). `present` false without a time of day: light 1 is skipped.
    SkyMoonFrame skyMoon{};
    NightSkyFrame nightSky{}; // time-of-day N2: stars, the Milky Way, the airglow
    UdsSkyFrameData udsSky{}; // Ultra Dynamic Sky's sky material over the atmosphere (TimeOfDaySettings::skyUdsMaterial)
    // Time-of-day N4: the light's glints on the water fade with the moon's phase (TimeOfDay::GlintScale; 1 without a
    // time of day). Taken at Sync so the ocean's constants never read the game's clock (game thread plan G2).
    float sunGlintScale = 1.0f;
    // S8: the CSM knobs the lighting pass needs (receiver bias / sharpen / over-blur). Same object
    // the developer window edits, so a slider move reaches the shader the very next frame.
    const CascadeShadowConfig* cascadeConfig = nullptr;
    ShadowGpuData* shadowGpu = nullptr; // Rung 0: GPU-driven shadow cull inputs/outputs
    render::LocalShadowCache* localShadowCache = nullptr; // local-light plan S7: the atlas lights' static-caster cache
    // Occlusion plan S4: this frame draws the registry's eligible objects through the indirect
    // G-buffer pass (Scene decided before the camera prepare; the CPU buckets hold the rest).
    bool gbufferIndirect = false;
    VirtualShadowMap* vsm = nullptr;    // Rung 2: page pool + page table (Step 18; unused yet)
    // S15: the SDSM analysis. Non-null only while the mode is Sdsm AND its resources built --
    // which is exactly the gate every SDSM pass builder uses, so a failed PSO degrades to
    // "the passes are not in the graph" rather than to a black shadow atlas.
    SdsmShadows* sdsm = nullptr;
    const vfx::WindState* wind = nullptr; // W3: global wind, folded into the gbuffer per-view CB
    // Water in the level, or null. The deferred lighting pass reads its caustics settings, clock
    // and flipbook; no ocean simply means no caustics.
    OceanRenderable* ocean = nullptr;
    // Plan S's fallback without RT (skyVis.fallback): the world from above, or null -- non-null only on a frame the
    // fallback is wanted (the setting on, the RT sky visibility not) and its resources were built.
    SkyOcclusionMap* skyOcclusion = nullptr;

    CascadeData cascades{};
    // The editor selection, SORTED ASCENDING, owned by Scene -- a pointer rather than a copied
    // fixed array. It used to be a std::array of 64 and everything past that was dropped without a
    // word, so selecting a few hundred meshes outlined the first 64 of them (reported 2026-09-15).
    // Sorted because the membership test runs per object per frame and has to be a binary search
    // once the selection is large; see IsSelectedEditorObject.
    const std::uint64_t* selectedEditorObjectIds = nullptr;
    std::uint32_t selectedEditorObjectCount = 0;
    std::uint32_t selectionOutlineRadius = 1;

    SceneRenderSettings settings{};
    // P2: the level's photographic camera settings, snapshotted with the rest of the frame so the
    // metering pass reads a value that cannot change under it mid-frame.
    render::CameraExposureSettings cameraExposure{};
    render::ColorPipelineSettings colorPipeline{};
};
