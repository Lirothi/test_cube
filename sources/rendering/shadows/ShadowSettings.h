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

// The CSM atlas edge, in texels. 2x2 tiles, so a tile is half this and its CONTENT is the tile
// minus twice the gutter above. Shared by Legacy and SDSM -- they render into the same atlas with
// the same layout, which is what makes the two comparable at all.
//
// A KNOB because it is the single biggest quality/cost lever the directional shadow has, and the
// two modes want opposite answers from it: Legacy fits a cascade to a frustum-slice SPHERE and
// spends most of a tile on empty air, so it needs the texels; SDSM fits the tile to the geometry
// and reaches Legacy's density at half the edge (measured: SDSM's texel is 0.24-0.32x of Legacy's
// at the same 4096, so 2048 would still be ~0.5-0.65x). S16's whole bet is that 1K tiles beat
// Legacy's 2K, and without this there is nothing to test it with.
//
// Changing it REALLOCATES the atlas, so it is reconciled at GPU idle next to the shadow-mode
// residency switch (Scene::ReconcileShadowMode) rather than applied where it is set.
// Memory, R16 depth: 4096 = 33.5 MB, 2048 = 8.4 MB, 1024 = 2.1 MB.
inline constexpr unsigned kCsmAtlasResMin = 512u;
inline constexpr unsigned kCsmAtlasResMax = 8192u;
inline unsigned g_csmAtlasRes = 4096u;

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

// Occlusion plan S4: the camera's opaque G-buffer through the same registry -- the cull's camera
// row, one ExecuteIndirect per (mesh submesh, LOD) with the group's material bound by the CPU.
// Objects the registry does not take (non-casters, GI clouds, material overrides, a non-PNTUV
// layout, a per-object texture override on a shared mesh) keep the CPU path. ON since
// 2026-09-04: pixel-identical to the CPU path on the wall / shadow-camera / widest-crossfade
// shots, camera row of the cull validator PASS, K=4 GPU flat, worker record 0.51 -> 0.04 ms for
// a 0.08 ms serial record. `--set=gbuffer.indirect:0` is the A/B (both paths in one binary).
inline bool g_indirectGBufferEnabled = true;

// Occlusion plan S5: the camera's two-pass HZB occlusion INSIDE that indirect G-buffer (Nanite's
// main/post split): the cull defers every candidate last frame's depth pyramid hid, pass A draws
// the rest, the pyramid of pass A's depth retests the deferred with this frame's matrices, and
// pass B draws the survivors into the same G-buffer. Zero latency, no holes by construction; the
// price is a second pyramid build + the mostly-empty pass-B ExecuteIndirects. Needs
// `g_indirectGBufferEnabled`. `--set=gbuffer.hzb:0|1` is the A/B.
inline bool g_gbufferHzbCullEnabled = true;

// Rung 2 / Step 24a — active shadow method. Legacy = the CSM directional + spot/point/glass ATLAS
// path; VSM = the virtual page pool (spot/point/glass today; directional after Step 24). Drives both
// whether the VSM pipeline passes run AND which sampler the light/glass shaders use (VsmActive() →
// useVsm). Toggle Legacy<->VSM with Ctrl+V ("ToggleVsmPageRequest"). Step 24b makes the switch free
// the inactive mode's GPU resources (only one mode ever resident).
//
// S15 adds a THIRD mode, Sdsm: the same CSM atlas and the same indirect caster path, but the four
// cascades are replaced by four PARTITIONS whose depth interval and light-space box are computed
// on the GPU from THIS frame's depth buffer (docs/csm_improvement_plan.md S15-S18). It is not a
// variant of Legacy: Legacy's cascade fit, its splits and its sphere are all bypassed, and the
// shadow passes move from the head of the frame to after the G-buffer because the analysis needs
// the depth of the frame being shaded.
//
// `VsmActive()` deliberately stays "the mode IS VSM": every existing `!VsmActive()` site means
// "the LEGACY ATLAS machinery runs" (spot/point atlases at full size, the CSM atlas allocated,
// glass sampling the atlas), and all of that is true in Sdsm too. Sites that must tell Sdsm from
// Legacy ask SdsmActive() explicitly.
enum class ShadowMode : std::uint32_t { Legacy = 0, VSM = 1, Sdsm = 2 };
inline ShadowMode g_shadowMode = ShadowMode::VSM;
inline bool VsmActive() { return g_shadowMode == ShadowMode::VSM; }
inline bool SdsmActive() { return g_shadowMode == ShadowMode::Sdsm; }
// The directional sun comes from the CSM ATLAS (Legacy fit or SDSM partitions) rather than from
// the clipmap. The atlas, its gutter, the 2x2 tile grid and the indirect draw path are shared.
inline bool AtlasDirectional() { return g_shadowMode != ShadowMode::VSM; }
// `--shadow-mode=` on the command line is a BOOT OVERRIDE: it wins over graphics_settings.json
// for the session and is never written back into it. Before 2026-09-04 the settings file was
// applied after the flag and silently replaced it, so a headless `--shadow-mode=vsm` run
// measured Legacy whenever the saved mode was Legacy -- a control that lied.
inline bool g_shadowModeFromCli = false;
inline ShadowMode g_shadowModePersisted = ShadowMode::VSM; // what the settings file holds (saved back unchanged under the override)
// ...and it lied a SECOND way, found 2026-09-14: the flag's parser mapped everything it did not
// recognise onto VSM, so `--shadow-mode=csm` (the obvious spelling for the Legacy CSM mode) ran a
// whole measurement session against VSM while every log line and every `csm.*` knob said Legacy.
// A boot override that silently selects a mode nobody asked for is worse than no override, so an
// unrecognised token now LEAVES THE MODE ALONE and raises this flag, which the first reconcile
// turns into a WARN next to the mode it actually settled on.
inline bool g_shadowModeCliUnknown = false;
inline const char* ShadowModeLabel(ShadowMode m)
{
    return (m == ShadowMode::Legacy) ? "Legacy CSM" : (m == ShadowMode::Sdsm) ? "SDSM" : "VSM";
}

// Local-light shadow plan S1 (docs/local_light_shadows_plan.md): the atlas spot / point shadows biased as UE does --
// a spot's in its DEPTH PASS (world metres, constant + slope: ShadowRendering.cpp:2037-2056), a point's at projection
// as a fraction of the distance (:1962-1983). false = the old receiver biases (a spot's NDC shadowDepthBias, a point's
// 0.10 m + 0.05 m normal), for the A/B in one binary: `--set shadow.localBiasUe:0`.
inline bool g_localShadowBiasUe = true;
// Plan S2: UE's filters on the atlases -- a spot's Manual5x5PCF (6x6 tent, soft occlusion ramp), a point's 29-tap
// CubemapHardwarePCF, both sharpened and squared. false = the old 3x3 boxes: `--set shadow.localFilterUe:0`.
inline bool g_localShadowFilterUe = true;
// Plan S3: UE's shadow fade by the resolution a light wants on screen (CalculateShadowFadeAlpha: full above 64 texels,
// none at 32) -- a distant lamp's shadow thins out instead of switching off when it loses its slot. false = every
// shadowed light at full strength (the slots are still ranked by that resolution): `--set shadow.localFadeUe:0`.
inline bool g_localShadowFadeUe = true;
// The peter-panning fix (2026-10-20, owner: "I see it"): the TEXEL bias (shaders/local_shadow_bias.hlsli
// LocalShadowTexelOffset) -- our VSM local-light model on the atlases: the receiver pushed `g_localShadowNormalTexels`
// shadow texels along its normal and `g_localShadowPushTexels` x slope (1 / NoL, 1..6) toward the light, no depth-pass
// bias, no fraction-of-distance compare; the spot tent's ramp 2 texels instead of UE's (f - n) / 60 m. UE's projection
// bias needs a distance-sized constant for grazing receivers and detaches every shadow from its caster's foot.
// The normal push must cover the FILTER's reach, not one texel: a tap r texels off the centre meets a plane tilted by
// theta r x tan(theta) texels deeper, the push buys n / cos(theta) -- n >= r. The spot's 6x6 tent reaches 3 texels, the
// point's disc 1.25 + each tap's bilinear compare + the face's perspective about the same: 3. Measured on
// data/levels/local_shadow_test.json, exposure pinned below the clip (a clipped white floor hides the acne -- wind_test's
// did): acne on 0.3-1.8 % of the lit floor with 3.0 (UE's own bias 0.4-1.8 %, VSM's shapes), 36-54 % with 1.5, the foot
// of every pillar touching like the VSM's where UE's leaves a gap. false = UE's / the old bias (shadow.localBiasUe).
// `--set shadow.localBiasTexel:0|1`, `shadow.localNormalTexels`, `shadow.localPushTexels`.
inline bool g_localShadowBiasTexel = true;
inline float g_localShadowNormalTexels = 3.0f;
inline float g_localShadowPushTexels = 0.5f;
// Plan S8: a light's OWN Shadow Bias / Shadow Slope Bias (UE's per-light knobs, 0.5 each) scale the texel bias, so the
// Inspector fields keep meaning something with it on: 0.5 = the global texels above, 1.0 = twice, 0 = none. (They drove
// only UE's bias before, which the texel bias replaced -- two knobs that moved nothing.) The normal push stays above 0:
// it is what marks the texel mode to the shaders.
inline float LocalShadowNormalTexels(float shadowBias)
{
    const float t = g_localShadowNormalTexels * (shadowBias > 0.0f ? shadowBias : 0.0f) * 2.0f;
    return t > 1.0e-3f ? t : 1.0e-3f;
}
inline float LocalShadowPushTexels(float shadowSlopeBias)
{
    return g_localShadowPushTexels * (shadowSlopeBias > 0.0f ? shadowSlopeBias : 0.0f) * 2.0f;
}
// Local-light plan S6a: the atlas local views draw with ONE ExecuteIndirect each -- the cull's non-empty records,
// compacted on the GPU (shadow_args_compact_cs) and pointed at the consolidated mega VB/IB -- instead of one per mesh
// group. Needs the mega buffer (uniform vertex stride, R32 indices; dropped by a mid-game Rebuild until the next
// load). false = one ExecuteIndirect per group: `--set shadow.localCompact:0|1`.
inline bool g_localShadowCompact = true;
// Local-light plan S7: UE's whole-scene shadow cache for the atlas lights (render::LocalShadowCache) -- the static
// casters' depth kept per light in a pool and copied in every frame, only the movable ones drawn on top. Needs S6a's
// compaction (the static rows are drawn the same way). `--set shadow.localCache:0|1`; the pool grows to what the lights
// want up to `shadow.localCacheMaxPages` pages of localMaxRes^2 D16 (UE r.Shadow.WholeSceneShadowCacheMb 150).
inline bool g_localShadowCache = true;
inline unsigned g_localShadowCacheMaxPages = 8u;
// UE r.Shadow.CacheWPOPrimitives (0): a caster that sways in the wind is MOVABLE for the cache, drawn every frame. 1 caches
// it with the statics -- right only while the wind stands still (wind.freeze), the case UE's switch exists for.
// `--set shadow.localCacheWind:0|1`.
inline bool g_localShadowCacheWind = false;
constexpr unsigned kLocalShadowCacheMaxPagesLimit = 16u;
// Local-light plan S6b: how many atlas views (a spot, a cube face) one command list of the spot / point shadow passes
// records. A list of one small view costs the GPU ~8 us by itself (its boundary, descriptor heaps, profiler timestamps);
// UE draws a whole atlas in one pass. 1 = one list per view, the old fan-out. `--set shadow.localViewsPerList:<n>`.
// Measured on demo (9 spots + 30 faces, editor closed): GPU frame 3.94 ms at 1, 3.84 at 2, 3.76 at 4, 3.74 at 8, 3.71
// at 16, 3.72 at 64; the shadow lists' CPU recording 1.05 -> 0.22 ms in total.
inline unsigned g_localShadowViewsPerList = 16u;
// Local-light plan S9e: the GPU cull's tests for a spot / point-face view beyond its own frustum, UE's. Bounds: the
// caster's box against the light's reach sphere and a spot's bounding sphere against its cone (UE AffectsBounds, the
// light-primitive interaction; camera-independent, so the S7 cache and VSM take it too). Hull: UE
// BuildLightViewFrustumConvexHull -- the hull of the light and the camera frustum's four side planes; a caster outside it
// cannot shadow anything the camera sees (r.Shadow.LightViewConvexHullCull 1). Radius threshold: UE
// r.Shadow.RadiusThreshold (Epic 0.01) -- a caster whose bounding radius is at most that share of its distance to the
// camera (x the view's LODDistanceFactor, hfov / 90) casts no local shadow; GPU-instanced (GI) casters are exempt, UE
// tests an instanced component's bounds, never an instance's. The hull and the threshold depend on the camera, so --
// like UE (SDCM_StaticPrimitivesOnly, VSM) -- neither touches the S7 cache's static rows nor VSM.
// `--set shadow.localBoundsCull:0|1`, `shadow.localHullCull:0|1`, `shadow.localRadiusThreshold:<share>` (0 = off).
inline bool g_localShadowBoundsCull = true;
inline bool g_localShadowHullCull = true;
inline float g_localShadowRadiusThreshold = 0.01f;
// `--set shadow.cullValidate:1` (MCP set_settings too): the shadow cull's one-shot CPU validation runs again on the next
// frame at the current view; its verdict line also says how many local caster draws each S9e test removed there.
inline bool g_shadowCullRevalidate = false;
// The atlas modes' point-light cube faces are rendered this many texels wider each side (Scene.cpp, the face fov) and
// sampled in their inner 90 degrees -- MIRRORS kPointShadowGuardTexels in shaders/local_shadow_filter.hlsli.
constexpr float kPointShadowGuardTexels = 2.0f;
// The texel bias is the ATLAS modes' -- VSM keeps its own (vsm::g_local*Texels) and the old per-light values.
inline bool LocalShadowTexelBias() { return g_localShadowBiasTexel && !VsmActive(); }
// Local-light plan S4: a spot's atlas holds a LINEAR depth, UE's (TShadowDepthPS<PerspectiveCorrect>,
// ShadowDepthPixelShader.usf:80-86): the pixel shader writes SV_Depth = (w - n) / (f - n) + bias, so a D16 step is
// (f - n) / 65535 at any distance (0.2 mm on a 14 m spot) where the perspective depth's grew with w^2 (3 cm at 14 m from
// a 0.1 m near). That is what lets the near plane be UE's: a caster 1 mm from the light shadows. The cost is a pixel
// shader on every caster of a spot view (no early depth there) -- a SHADOW_LINEAR_DEPTH twin of each shadow PSO
// (Material::LinearDepthVariant). The atlas modes only: VSM's pool keeps its own perspective depth.
// false = the old perspective depth and a 0.1 m near: `--set shadow.spotLinearDepth:0|1`.
inline bool g_spotShadowLinearDepth = true;
inline bool SpotShadowLinearDepth() { return g_spotShadowLinearDepth && !VsmActive(); }
// UE MinLightW 0.1 (cm: SpotLightSceneProxy.cpp:101) -- the floor of any spot's near, authored or not.
constexpr float kSpotShadowMinNear = 0.001f;
// A spot's shadow near plane, metres. `authored` > 0 is the level's "shadowNear" (SpotLightDesc::nearPlane) and wins;
// otherwise UE's 1 mm on the linear depth, the old 0.1 m on a perspective one (1 mm there would leave steps of
// centimetres at the far end -- of the D16 atlas, and of the VSM pool's direct-Z D32 as well).
inline float SpotShadowNear(float authored)
{
    if (authored > 0.0f) { return authored > kSpotShadowMinNear ? authored : kSpotShadowMinNear; }
    return SpotShadowLinearDepth() ? kSpotShadowMinNear : 0.1f;
}
// The same for a POINT light's cube faces. UE's near is 1 cm (ShadowSetup.cpp:1169, FReversedZPerspectiveMatrix(PI / 4,
// 1, 1, MinZ 1, Radius)) on a PERSPECTIVE depth, and UE's projection bias is a share of the distance (2 % x 512 / Res),
// which grows with the perspective step. Ours is the texel bias (a few texels, the peter-panning fix), which does not:
// at 1 cm a D16 step is ~w^2 / n x 1.5e-5 -- 12 cm at 9 m, 38 cm at the end of a 16 m light -- far past it. So the
// faces take the spot's LINEAR depth with UE's near (a deviation from UE's point, for the same reason UE's spot has it).
// Atlas modes only; VSM keeps the old near max(0.2, 2 % of the radius). `--set shadow.pointLinearDepth:0|1` (0 = the
// perspective depth), `shadow.pointNearUe:0|1` (0 = the old near; perspective + 1 cm is the A/B that shows the steps).
inline bool g_pointShadowLinearDepth = true;
inline bool g_pointShadowNearUe = true;
constexpr float kPointShadowUeNear = 0.01f;
inline bool PointShadowLinearDepth() { return g_pointShadowLinearDepth && !VsmActive(); }
inline float PointShadowNear(float radius)
{
    if (g_pointShadowNearUe && !VsmActive()) { return kPointShadowUeNear; }
    const float old = radius * 0.02f;
    return old > 0.2f ? old : 0.2f;
}
// Plan S4: UE r.Shadow.MaxResolution for the local lights (BaseScalability.ini: 1024 at High, 2048 at Epic). A spot's
// shadow is its screen radius x 2.546 rounded DOWN to a power of two up to this; a point's cube face is the first of
// max/2, /4, /8, /16 and 32 not above its radius x 1.273 (ShadowSetup.cpp:4301-4464, :771-797). Plan S9: the atlas
// pages are sized from it (a spot page max^2, a point page 1.5 max x max -- see g_localShadowMaxPages). Reallocated by
// Scene::ReconcileShadowMode; `--set shadow.localMaxRes:<n>`.
inline unsigned g_localShadowMaxRes = 2048u;
constexpr unsigned kLocalShadowMaxResMin = 256u;
constexpr unsigned kLocalShadowMaxResMax = 4096u;
constexpr unsigned kLocalShadowMinRes = 32u; // UE r.Shadow.MinResolution
// Plan S9: the atlas modes' local shadows live in PAGES -- spot pages of max^2, point pages of 1.5 max x max (one block
// of the largest cube) -- that grow, as UE opens another atlas, up to this many per pool. UE has no cap (its atlases
// come from the render-target pool); ours is the VRAM guard: at max 2048 a spot page is 8 MB and a point page 12 MB
// (R16), so 4 + 4 = 80 MB at most. A frame that needs more halves every light a step (UE's VSM global LOD bias).
// `--set shadow.localMaxPages:<n>`; grown (never shrunk below the need) in Scene::ReconcileShadowMode.
inline unsigned g_localShadowMaxPages = 4u; // 1 .. render::kLocalShadowMaxPagesLimit

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
    // TEMPORAL DITHER -- and this one IS UE's: their contact dither is
    // `InterleavedGradientNoise(PixelPos, View.StateFrameIndexMod8)`, i.e. it rotates over an
    // 8-frame cycle and TAA averages the binary per-pixel outcomes into a smooth value. That is
    // the cheapest "denoiser" there is: no extra pass, no extra buffer, the temporal pass we
    // already run does the work. Off = today's static IGN (UE's formula with FrameId 0 IS the
    // static one), for judging a single still frame.
    inline bool          g_temporalDither = true;
    // LOCAL LIGHTS (spot + point): which shadow source they use once contacts are enabled.
    //   0 = their shadow map; contacts stay off for locals.
    //   1 = contacts INSTEAD of the map -- the map is not sampled at all, which is also a cost
    //       lever: a shadowed spot costs 9 atlas taps per pixel, a contact trace 8 depth taps
    //       and no atlas render.
    //   2 = auto: contacts only where the light has no shadow slot; slotted lights keep their map.
    //   3 = both -- UE's own arrangement (a light with ContactShadowLength > 0 multiplies the trace into its
    //       shadow map), added for the A/B by local-light plan S8.
    // 0-2 never stack them: on a small-range light that darkens the same contact twice, and the user
    // asked for an either/or -- which is what they are. Sun is unaffected -- there the contact
    // term sits on top of CSM/VSM by design, recovering what a far cascade cannot resolve.
    inline std::uint32_t g_localMode = 1u;
    // What the local-light passes actually receive. The mode is meaningless with the master
    // switch off, and mode 1 with a zero-length trace would leave spot/point with NO shadow at
    // all -- so off always means "shadow map", whatever the combo says.
    inline std::uint32_t EffectiveLocalMode() { return g_enabled ? g_localMode : 0u; }
    // Distance window in METRES from the camera. Outside it the term is off. maxDistance 0 = no
    // far limit. BOTH ends fade over `g_fadeBandM` metres so neither pops -- the near end used to
    // cut hard, which only looked fine because this defaults to 0 and no geometry sits at zero
    // metres. Raising it to where there IS ground (the ask was 500 m: keep contacts only out
    // where the cascades are too coarse to resolve anything) needs the near ramp too.
    inline float         g_minDistanceM = 0.0f;
    // 0 = NO LIMIT, and it stays that way. A 150 m default was tried here and it was the wrong
    // answer to the wrong question: the ask was contacts that KEEP WORKING at distance, and
    // switching them off past 150 m is not a fix for the far-field speckle, it is deleting the
    // feature. The speckle at 350 m is still open -- see the note on the real conflict below.
    inline float         g_maxDistanceM = 0.0f;
    inline float         g_fadeBandM = 10.0f;

    // ONE writer for the contact fields of every light pass's constants (sun, spot, point). The
    // three CBs carry identically named members -- that is the contract that lets the same
    // shader function serve all three -- and this is what keeps their VALUES identical too. A
    // template rather than a struct copy because the three constants types are unrelated.
    template <class Constants, class Mat4>
    inline void FillConstants(Constants& c, const Mat4& viewProj, const Mat4& proj,
                              std::uint64_t frameNumber)
    {
        c.viewProj = viewProj;
        c.projMatrix = proj;
        // The master switch folds into the length: 0 means the shader takes no samples at all,
        // so "off" costs nothing rather than costing a branch per pixel.
        c.contactShadowLength = g_enabled ? g_length : 0.0f;
        c.contactShadowIntensity = g_intensity;
        c.contactShadowSteps = g_steps;
        c.contactShadowLengthInWS = g_lengthInWorldSpace ? 1u : 0u;
        c.contactShadowNormalOffset = g_normalOffsetFrac;
        c.contactShadowGrazingFade = g_grazingFadeNdotL;
        c.contactShadowMinDist = g_minDistanceM;
        c.contactShadowMaxDist = g_maxDistanceM;
        c.contactShadowFadeBand = g_fadeBandM;
        c.contactShadowThickness = g_maxThicknessFrac;
        // UE's StateFrameIndexMod8, +1 so that 0 stays the "static dither" sentinel.
        c.contactShadowFrameId = g_temporalDither
            ? static_cast<std::uint32_t>((frameNumber & 7ull) + 1ull)
            : 0u;
    }
}

// ---- SDSM (docs/csm_improvement_plan.md S15) ------------------------------------------------
// Sample Distribution Shadow Maps: the partition intervals AND their light-space boxes come from
// a compute reduction over this frame's depth buffer, so a cascade covers the depth range and the
// screen area that is actually THERE instead of a static split and a frustum-slice sphere.
//
// Transcribed from the Intel sample (D:\Programming\sdsm\sdsm_dx11): LogPartitions.hlsl
// (ReduceZBoundsFromGBuffer, LogPartitionFromRange, ComputeLogPartitionsFromZBounds),
// CustomPartitions.hlsl (ReduceBoundsFromGBuffer) and SDSMPartitions.hlsl
// (ComputePartitionDataFromBounds). Our deviations are listed in shaders/sdsm_partitions.hlsli.
//
// PROCESS globals, like the mode switch itself and for the same reason: `--set=sdsm.*` is applied
// before any Scene exists and they must survive a level switch. The per-SCENE CSM tuning that
// SDSM still uses (the depth bias in texels, the filter knobs, the blend fraction) stays in
// CascadeShadowConfig -- SDSM changes WHERE a cascade looks, not how it is filtered.
namespace sdsm
{
    // How many partitions are computed and rendered. The atlas is the Legacy 2x2 grid, so 4 is
    // both the default and the cap until S17 makes the count dynamic.
    inline constexpr std::uint32_t kMaxPartitions = 4u;
    inline std::uint32_t g_partitions = 4u;

    // `mLightSpaceBorder` (SDSMPartitions.hlsl:35). Expressed in TEXELS of the partition's own
    // tile, not in the sample's normalized light-space units: our bounds are metres, so a
    // normalized border would mean a different world distance per partition. It reserves room for
    // the filter kernel inside the partition's own box -- it is NOT the S5 atlas gutter, which
    // still exists and still sits outside the content rect.
    inline float g_borderTexels = 4.0f;

    // `mDilationFactor` (SDSMPartitions.hlsl:38). A fraction of the box's own extent added on
    // every side. Covers what the per-sample reduction cannot see: a caster whose shadow lands
    // just outside the visible samples' bounds, and the anisotropic reach of a filter kernel.
    inline float g_dilation = 0.01f;

    // `mMaxScale` (SDSMPartitions.hlsl:36), expressed as a MULTIPLE OF THE LEGACY SPHERE. The
    // sample clamp the zoom so a partition holding a handful of samples cannot magnify without
    // bound; our ceiling is the S1 bounding sphere of the same interval, which is also the
    // FALLBACK when a partition has no samples at all. 1 = never tighter than Legacy (the A/B
    // control), 0.05 = at most 20x tighter.
    inline float g_minScaleOverSphere = 0.02f;

    // Metres added to the light-space Z range on both sides of the sampled bounds, so a caster
    // between the sun and the visible geometry is still inside the projection. The Legacy twin is
    // `casterReachWS` + `zPadding`; here the near side is handled by the same pancake clamp S7
    // uses, so this only has to cover the far cap and the depth-precision margin.
    inline float g_zMargin = 25.0f;

    // S16.7. How much of LAST frame's reduced depth range survives into this one, 0 = none (the
    // raw per-frame reduction, and the A/B). The partition boundaries are logarithmic over
    // [minZ, maxZ], so a frond swinging past the camera moves minZ and drags EVERY boundary with
    // it -- the cascade edge visibly walks across the ground under wind. Smoothing the RANGE
    // rather than the boundaries keeps the split's shape intact.
    //
    // Per FRAME, deliberately: the wind clock can be frozen, and a per-second decay stops
    // converging when dt is 0. The consequence is that the time constant follows the frame rate,
    // which is acceptable for a stabiliser.
    //
    // IN FRAMES OF HALF-LIFE, not in a retention factor. The factor is what the shader needs
    // (0.977, 0.9971 ...) and it is unreadable as a setting: everything useful hides between
    // 0.97 and 0.999. "How many frames until half the wobble is gone" is the actual question,
    // so that is what is stored, typed and saved; Scene converts once, at the CB.
    // 0 = off. Frames rather than seconds because the average IS per frame -- seconds would be
    // a lie at a different frame rate, and the wind clock can be frozen at dt = 0.
    inline float g_stabilityFrames = 30.0f;

    // S20 PARTITION CACHE ("кэширование дальних каскадов тени плюс обновление реже начиная с не
    // 0го каскада"). Partition 0 renders every frame; partition p >= 1 every min(N, 2^p) frames,
    // staggered so a frame re-renders at most one of them besides partition 0 (N = 4: 1 every 2nd
    // frame, 2 and 3 every 4th). Between renders a partition keeps its tile AND everything the
    // sampler reads about it (Finalize carries matrix, rect, biases from last frame; only its
    // depth interval is fresh, so the receivers' choice stays contiguous). The tile shadows the
    // swaying foliage at 1/period of the frame rate. 1 = off: every partition every frame.
    // A partition is re-rendered out of turn when the sun moves, the camera cuts, the atlas or
    // the partition count changes, or the camera has turned or moved further than its widened box
    // covers (Scene::UpdateSdsmFrameParams).
    inline std::uint32_t g_cacheFrames = 4u;
    // How much wider (fraction of the box's own extent, per side, in light-space XY) a partition
    // that will be kept is fitted, so the camera can turn and move a little before a receiver walks
    // off its tile -- off it, CsmSampleChain falls to the next partition, and past the last one to
    // lit. 0.05 covers about 5 degrees of turn at a 90-degree field of view; the camera guard
    // re-renders at half of that.
    inline float g_cacheDilation = 0.05f;

    // S21 STABLE PARTITIONS. The analyze re-measures every box from the depth buffer each frame,
    // and that buffer carries the DLSS jitter, so with the camera and the scene standing still the
    // box edges still wander by millimetres to centimetres and each re-render rasterises the
    // casters on a slightly different texel grid -- every shadow edge shifts by up to a texel,
    // about a screen pixel. Rendered every frame that is noise DLSS averages away; with the S20
    // cache the shift is held for 2-4 frames and steps, and DLSS lets it through as trembling
    // (owner: "оно зависит от sdsm.cacheFrames, при 1 все норм").
    // On: per axis the texel is KEPT while the measured box fits into it with at most
    // `g_sizeHysteresis` to spare (a fresh texel is placed mid-band), the box origin is snapped to a
    // WORLD-anchored grid of that texel, and the depth range gets the same hysteresis (the atlas is
    // D16, a wandering near plane re-quantises every stored depth). A still camera then renders the
    // same tile bit for bit; a moving one slides it by whole texels. Off = the raw fit (the A/B).
    inline bool g_texelSnap = true;
    // The band, as a fraction of the measured size: how much larger than needed a kept texel (and a
    // kept depth range) may be before it is refitted. Wider = rarer refits while the camera moves,
    // at up to that much coarser shadows (half of it on average).
    inline float g_sizeHysteresis = 0.1f;

    // NO `analyzeFullRes` KNOB, and that is a decision rather than an omission. The plan listed
    // one; it was written, wired to --set, and read by NOBODY -- a control that lies. The
    // reduction always walks EVERY depth pixel, because a min/max over a strided grid loses a
    // thin object entirely and a thin object is exactly what pulls a partition's near plane in.
    // Measured, the whole analysis is 0.037 ms; a half-res mode would save ~0.02 ms and cost
    // correctness, so there is nothing here worth choosing between.

    // ---- S16: EVSM4 (transcription of the sample's RenderingEVSM.hlsl) -----------------------
    // The depth tile is converted to four exponentially-warped MOMENTS (pos, neg, pos^2, neg^2)
    // and sampled through a Chebyshev upper bound instead of a percentage-closer filter. The point
    // is that moments are PREFILTERABLE: hardware bilinear, a box blur and mips all operate on the
    // shadow ESTIMATE, where PCF can only ever average binary compares taken before the filter.
    //
    // Master switch, default OFF: it costs a second atlas (RGBA32F) and a conversion pass, and the
    // PCF arm stays the A/B and the fallback.
    inline bool g_evsm = false;

    // The warp exponents, applied to the partition's OWN depth rescaled to [-1, 1].
    //
    // DEVIATION, and it is a real one. The sample write them as light-space constants (800 / 100)
    // divided by `partition.scale.z`, which keeps the warp consistent across partitions of ONE
    // global light projection. We have no global light projection -- each partition builds its own
    // ortho from measured bounds -- and at our metre scales that formula saturates at the fp32
    // clamp (42) for every partition we ever build. The clamp IS the operating point, so it is
    // exposed directly rather than behind an indirection that always saturates. The 8:1 ratio
    // between them is the sample's, and it is what the pair is for: the positive warp is steep
    // (sharp contact, crushes light leaking), the negative one shallow (catches what the positive
    // one lets through).
    //
    // 42 is not a taste: exp(42) ~ 1.7e18 and its SQUARE ~ 2.9e36, against fp32's 3.4e38 ceiling.
    // Past it the second moment is Inf and every shadow test returns garbage.
    inline constexpr float kEvsmMaxExponent = 42.0f;
    inline float g_evsmPos = 42.0f;
    inline float g_evsmNeg = 5.25f;

    // Edge softening: a separable box over the moments, its width a FRACTION OF THE PARTITION
    // (the sample's edgeSofteningAmount, 0.02). Because a partition's box is fitted to the
    // geometry, a fraction of it is an authored WORLD size that keeps its meaning as the
    // partition moves -- which a texel count would not. 0 = no blur pass at all.
    //
    // DEFAULT 0.002, NOT THE SAMPLE'S 0.02, and the difference is our world scale rather than
    // taste. The fraction is of the PARTITION, and ours are metres wide: 0.02 of p3's 163 m box is
    // a 3.3 m penumbra, which on the atoll washes the whole far field to a flat grey (shot and
    // looked at, 2026-09-14). 0.002 gives ~1 cm on the near partition and ~33 cm on the far one --
    // softness that grows with distance, which is what a penumbra does, instead of erasing the
    // shadow. The sample's scene is simply not measured in metres.
    inline float g_evsmBlur = 0.002f;
    // ...capped, because the kernel must not reach out of its own tile: the S5 gutter is 4
    // texels and past it lies ANOTHER partition. The blur clamps its taps to the tile, so the
    // cap is about cost and about the edge going flat, not about correctness.
    // (the sample's maxEdgeSofteningFilter, 16 texels.)
    inline float g_evsmBlurMaxTexels = 16.0f;

    // Dump the partition readout (intervals, box extents, texel size, sample counts, the
    // CPU-caster tail) to the session log on the next analyze, then clear itself. `--sdsm-readout`.
    inline bool g_dumpReadout = false;
}

} // namespace render
