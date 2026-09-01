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

} // namespace render
