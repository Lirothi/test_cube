#pragma once

#include <cstdint>

// Process-wide shadow-system state: the things that select WHICH shadow machinery runs, and the
// debug view over it. Everything here spans BOTH algorithms (or is a debug view), which is exactly
// why it is not in CascadeShadowConfig — that struct is per-scene CSM tuning, and a CSM-only home
// cannot own the Legacy/VSM switch.
//
// These were in `rendering/renderables/InstanceTypes.h`, a header about INSTANCE and draw-submission
// types, purely because that is where the first shadow toggle happened to be added. Nothing about a
// shadow mode belongs beside an instance vertex layout.
//
// Deliberately process globals, not scene state: they survive a level switch the way the dev-window
// toggles do, and `--shadow-mode=` / `--csm-tint` are parsed in main.cpp before any Scene exists.
// A per-SCENE shadow setting belongs in CascadeShadowConfig (app/scene/SceneRenderConfig.h) instead;
// `csmFilterMode` moved there for that reason, next to the three filter knobs it is tuned with.
namespace render
{

// S5 — GUTTER. Texels reserved on every side of a Legacy CSM tile that the depth pass never draws
// into, so they keep the atlas clear value (1.0 = far = LIT). The cascade's world square is rendered
// into the inner `tile - 2*border` texels and sampled from exactly that rect.
//
// Why a filter tap must never leave it: `Gather()` picks its 2x2 quad by hardware UV rounding, so a
// tap sitting exactly on the content edge can pull in the NEIGHBOURING TILE -- another cascade,
// showing shadow from a different part of the scene. Clamping the UV is not a proof on its own
// (the rounding happens after the clamp); a ring of cleared texels makes the miss HARMLESS BY
// CONSTRUCTION rather than by margin.
// 4 is UE's SHADOW_BORDER (ShadowSetup.cpp:831), and like theirs it is cut OUT of the tile
// (`MaxShadowResolution = ... - SHADOW_BORDER * 2`), not added around it.
// 0 disables the whole mechanism: content == tile, and every UV clamp collapses to a no-op.
inline constexpr unsigned kCascadeAtlasBorder = 4u;

// Rung 0 runtime toggle (default ON): the shadow passes draw via GPU cull + ExecuteIndirect
// (ShadowGpuData) instead of the per-object CPU RenderShadow loop — the CPU-submission win.
// Toggle OFF (Ctrl+I, "ToggleIndirectShadows") for the CPU-path A/B. If the cull PSOs fail to
// build, IndirectDrawReady() returns false and the passes fall back to the CPU path anyway.
inline bool g_indirectShadowsEnabled = true;

// GI→VSM runtime toggle (default ON): fold GPU-instanced casters' instances into the consolidated
// ShadowGpuData caster set (GPU scatter → cull → indirect), so they cast in VSM and via the indirect
// path in Legacy (dropping their per-view CPU RenderShadow tail). Toggle OFF (Ctrl+G,
// "ToggleGiIndirectShadows") for the A/B: GI reverts to the Legacy CPU tail only (nothing in VSM) —
// exactly today's behavior. Also the safety fallback: if the scatter PSO fails or an object is over
// the group cap, GI keeps drawing through the retained CPU tail. Requires g_indirectShadowsEnabled.
inline bool g_giIndirectShadowsEnabled = true;

// Rung 2 / Step 24a — active shadow method. Legacy = the CSM directional + spot/point/glass ATLAS
// path; VSM = the virtual page pool (spot/point/glass today; directional after Step 24). Drives both
// whether the VSM pipeline passes run AND which sampler the light/glass shaders use (VsmActive() →
// useVsm). Toggle Legacy<->VSM with Ctrl+V ("ToggleVsmPageRequest"). Step 24b makes the switch free
// the inactive mode's GPU resources (only one mode ever resident).
enum class ShadowMode : std::uint32_t { Legacy = 0, VSM = 1 };
inline ShadowMode g_shadowMode = ShadowMode::VSM;
inline bool VsmActive() { return g_shadowMode == ShadowMode::VSM; }

// S0.3 — Legacy CSM debug visualization, forwarded to lighting_cs.hlsl as `csmDebugMode`.
// 0 = off (the shader's only cost is one uint compare). 1 = tint each pixel by the cascade the
// sample RESOLVED to (not the one the split picked): that difference is the point, because it is
// what makes the tile-border fallback ring visible. Legacy-only; the VSM branch ignores it.
// A DEBUG VIEW, so it stays a process global like render::g_lodDebugMode — and `--csm-tint` is
// parsed before a Scene exists, which a per-scene field could not serve.
enum class CsmDebugMode : std::uint32_t { Off = 0, CascadeTint = 1 };
inline CsmDebugMode g_csmDebugMode = CsmDebugMode::Off;

// Set by `--csm-readout`: dump the cascade fit table (slice, texel, near/far, zRange, D16 step)
// to logs/csm_readout.log on the next UpdateCascades, then clear itself. The dev window has the
// same table, but a headless --shot/--profdump run cannot open a GUI -- and zRange / D16 step
// are exactly what S7 pancaking is judged on.
inline bool g_csmDumpReadout = false;

// ---- CONTACT SHADOWS (docs/csm_improvement_plan.md S12) ------------------------------------
// A short march through the CAMERA depth buffer toward the light, recovering the scale a shadow
// map texel cannot resolve. Lives HERE rather than beside the VSM tunables because it is
// SHADOW-MODE INDEPENDENT: it reads no shadow map, so Legacy CSM and VSM get the identical term.
// That is also the reason it exists -- a far cascade covering hundreds of metres has nothing to
// say about a blade of grass touching the ground, and neither has a coarse clipmap level.
//
// Transcribed from UE's `CastScreenSpaceShadowRay` (ScreenSpaceShadowRayCast.ush) and its use in
// DeferredLightingCommon.ush.
namespace contact
{
    // MASTER SWITCH, default OFF -- and that matches UE, whose per-light ContactShadowLength
    // defaults to 0. Contact shadows are an opt-in artist tool there, not a global on. Off means
    // not a single depth sample is taken.
    inline bool          g_enabled = false;

    // ---- the four knobs UE actually expose --------------------------------------------------
    // Trace length. UE support BOTH interpretations and encode the choice in the SIGN of their
    // value (`ContactShadowLengthInWS = ContactShadowLength < 0`); split into two fields here
    // because a sign-encoded mode is a lousy thing to put on a slider.
    //   world space OFF -> a MULTIPLE OF VIEW DEPTH (UE's screen-scale form). The trace then covers
    //                      the same number of SCREEN pixels near and far, which is what keeps it
    //                      alive at distance instead of shrinking below a pixel.
    //   world space ON  -> METRES, flat. Predictable, but at distance it shrinks to sub-pixel and
    //                      stops doing anything.
    inline float         g_length = 0.05f;
    inline bool          g_lengthInWorldSpace = false;
    // A CAP ON THE RAY LENGTH IN METRES WAS TRIED HERE AND REMOVED. It looked right on paper --
    // the screen-scaled ray is 17.5 m at 350 m, so shorten it -- and a median-based metric even
    // said it worked. The IMAGE said otherwise: a 0.5 m ray at 350 m is shorter than the depth
    // buffer can resolve there, the compare tolerance (built from the ray's own depth span)
    // collapses toward zero, and the test then fires almost everywhere -- the whole slope went
    // black. The distance window below is the fix that actually holds up.
    // How dark a hit makes the pixel. UE's ContactShadowCastingIntensity.
    inline float         g_intensity = 1.0f;
    // UE hardcode 8 at their call site, and that is NOT laziness: their compare tolerance is
    // `|rayDepthSpan| * (1/steps) * 2`, so the step count is baked into the acceptance window.
    // Raising it narrows the window per sample while adding samples along a ray that hugs the
    // surface, and the outcome per pixel becomes more sensitive to the dither phase -- i.e. MORE
    // speckle, not less. Measured added speckle: 4 steps +3.49 pp, 8 +4.00, 16 +4.05, 32 +5.77.
    // Capped at 16 in the UI for that reason; it is a cost/robustness knob, not a quality one.
    inline std::uint32_t g_steps = 8u;

    // ---- OURS, not UE's. Everything below is a departure and is here for one reason ----------
    // A screen-space march along a ray that runs nearly PARALLEL to the surface it started on
    // cannot tell "just below the surface" from "behind an occluder": depth quantisation alone
    // dips the ray under the ground, and the result is a field of dark speckles on flat, distant
    // terrain under a low sun. UE ship no denoiser and no distance fade for this -- their answer is
    // that an artist enables contact shadows per light, on content where it looks right.
    //
    // Since the sun here IS low and the terrain IS flat, these three exist to bound the damage.

    // Push the ray's start off the surface along the normal, as a FRACTION OF THE RAY LENGTH.
    // A ray that begins ON the surface is ambiguous at step one, and this is the cheapest guard
    // against that.
    //
    // Not metres, for the same reason the thickness is not: what it fights is the world footprint
    // of a SCREEN PIXEL plus depth-buffer precision, and both grow with distance. A fixed 0.02 m
    // is meaningful at 10 m and far below one pixel at 350 m, where it silently stops doing
    // anything. Tied to the ray -- which is itself a multiple of view depth -- it keeps its
    // meaning at any range, and this feature then has ONE scaling concept instead of three.
    inline float         g_normalOffsetFrac = 0.04f;
    // Below this NdotL the sun is grazing, the ray is nearly parallel to the surface, and the
    // march is measuring quantisation rather than geometry. Faded out, not cut, so no visible edge.
    inline float         g_grazingFadeNdotL = 0.15f;
    // OURS. A FRACTION OF THE RAY LENGTH -- deliberately not metres. A hit whose occluder sits
    // further behind the ray point than this is not a contact: without the test a hit can be the
    // far side of a dune, reported per pixel as binary occlusion, i.e. a speckle field.
    //
    // It was metres first, and that was wrong: the ray length is itself a multiple of view depth,
    // so it grows with distance, and a fixed metre threshold cannot track it. Near, any value big
    // enough to matter far is a no-op; far, any value tight enough to kill speckle also kills the
    // real contacts. As a fraction it rides the ray and stays meaningful at 10 m and at 3 km.
    // 0 = no thickness test (UE behaviour).
    inline float         g_maxThicknessFrac = 0.5f;
    // Distance window in METRES from the camera. Outside it the term is off. maxDistance 0 = no
    // far limit. The far end fades over the last `g_fadeBandM` metres so it does not pop.
    inline float         g_minDistanceM = 0.0f;
    // 0 = NO LIMIT, and it stays that way. A 150 m default was tried here and it was the wrong
    // answer to the wrong question: the ask was contacts that KEEP WORKING at distance, and
    // switching them off past 150 m is not a fix for the far-field speckle, it is deleting the
    // feature. The speckle at 350 m is still open -- see the note on the real conflict below.
    inline float         g_maxDistanceM = 0.0f;
    inline float         g_fadeBandM = 10.0f;
}

} // namespace render
