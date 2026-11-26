#include "app/BootSwitches.h"

#include "app/BootParse.h"

#include <algorithm>
#include <array>
#include <vector>

namespace boot
{
namespace
{
// Grouped by what the switches are for; Switches() hands them out sorted by name. Keep one line of help: the long story
// is at the switch's parse site and in AGENTS.md.
constexpr Switch kSwitches[] = {
    // --- help -----------------------------------------------------------------------------------------------------
    { "--help", "", "print this table (to the console the program was started from, and to the session log) and quit", false },

    // --- the level, the camera, the capture ---------------------------------------------------------------------------
    { "--level", "=<path>", "the level at boot instead of the default demo level", true, &parse::Level },
    { "--cam-pos", "=x,y,z", "the camera's position after the level's freeCameraStart (what the HUD prints)", true, &parse::CamPose },
    { "--cam-rot", "=x,y,z,w", "the camera's orientation quaternion, with --cam-pos (what the HUD prints)", true },
    { "--cam-fly", "=x,z", "drift the camera at a constant world velocity, m/s", false },
    { "--cam-fly-delay", "=<sec>", "hold the camera still this long before the --cam-fly drift starts", false },
    { "--cam-fly-yaw", "=<deg/s>", "a constant yaw rate on top of the drift (headless mouse look)", false },
    { "--cam-orbit", "=<radius m>,<deg/s>[,<sec per frame>]", "circle the start position looking along the tangent", false },
    { "--shot", "=<path>", "after the warmup (--shot-delay) save the presented frame as a PNG and quit", true, &parse::Shot },
    { "--shot-delay", "=<sec>", "the warmup before --shot / --profdump / --trace (default 7)", true, &parse::ShotDelay },
    { "--shot-count", "=<n>", "a series of n shots (path suffixed _NN), with --shot-step and --shot-interval", false, &parse::ShotCount },
    { "--shot-step", "=<sec>", "the frozen wind/ocean clock advanced between the shots of a series", false, &parse::ShotStep },
    { "--shot-interval", "=<sec>", "the settle time between the shots of a series (0 = consecutive frames)", false, &parse::ShotInterval },
    { "--sweep", "=<setting>:<v0>,<v1>[;<setting>:...]", "one shot per value of a setting (in step for several)", false, &parse::Sweep },
    { "--set", "=<name>:<value>[;<name>:<value>...]", "pin settings for the run, by the --sweep / MCP set_settings names", false },
    { "--profdump", "=<path>", "after the warmup, write the profiler overlay rows to a file and quit", false, &parse::ProfDump },
    { "--settings-dump", "=<path>", "after the warmup, write every readable setting as `name value` and quit (R2)", false, &parse::SettingsDump },
    { "--boot-dump", "=<path>", "write every boot option the command line set as `group.field=value` and quit before the app (R3.2b)", false, &parse::BootDump },
    { "--trace", "=<frames>", "after the warmup, capture a profiler trace of that many frames into traces/ and quit", false, &parse::Trace },
    { "--window", "=<w>x<h>", "a borderless window whose client area is exactly w x h", true, &parse::Window },
    { "--no-hud", "", "an empty HUD (two runs of one frozen frame then compare the image, not the counters)", false, &parse::NoHud },
    { "--dlss", "=<off|perf|balanced|quality|ultraperf|ultraquality|dlaa>", "the upscaler mode at boot", false, &parse::Dlss },
    { "--dlss-mvec", "=<engine|camera|legacy>", "which motion vectors the upscaler is told about", false },
    { "--scalability", "=<low|medium|high|ultra|off|auto>", "the graphics preset at boot, over what the player saved", true, &parse::Scalability },
    { "--time", "=HH:MM", "the time of day at boot (tod.time)", true },
    { "--date", "=YYYY-MM-DD", "the date at boot (the sun's and the moon's paths)", false },
    { "--time-speed", "=<x>", "the time of day's clock rate (tod.speed)", false },
    { "--time-freeze", "", "the time of day stands (tod.animate 0)", false },
    { "--tod-legacy-light", "", "a level's old directionalLight section lights the level directly, not through timeOfDay", false },
    { "--tod-selftest", "", "the sun and moon positions against reference values instead of the app; exit = failed checks", false },
    { "--wind-freeze", "[=<sec>]", "hold the wind and ocean clock (at that time): two runs then differ only by DLSS jitter", true },
    { "--shadow-mode", "=<legacy|vsm|sdsm>", "the directional shadow method at boot", false },
    { "--csm-tint", "", "tint each cascade (the cascade split debug view)", false },
    { "--async-pass", "=<names>", "narrow which passes may use the async compute queue (BuildAS, ObjectCompute, RTTrace, CloudTrace)", false },
    { "--no-async-compute", "", "keep every pass on the direct queue", false },

    // --- the editor and what opens at boot ----------------------------------------------------------------------------
    { "--editor", "", "open the Level Editor at boot (WITH_EDITOR builds)", false, &parse::Editor },
    { "--play", "", "boot into the play session", true, &parse::Play },
    { "--mcp-port", "=<n>", "serve the editor's MCP on that port instead of editor_state.json's (8128)", false, &parse::McpPort },
    { "--log-window", "", "open the session-log viewer at boot", false, &parse::LogWindow },
    { "--mem-window", "", "open Developer Controls on the Memory tab at boot", false, &parse::MemWindow },
    { "--dev-tab", "=<label>", "open Developer Controls on that tab at boot (gi, lod, ...)", false, &parse::DevTab },
    { "--edit-mesh", "=<models/x/x.mesh.json>[#<section>]", "open the Mesh Editor on that asset at boot (implies --editor)", false, &parse::EditMesh },
    { "--show-import", "[=<name>]", "open the Import Assets window at boot with that item selected (implies --editor)", false, &parse::ShowImport },
    { "--inspect", "=<environment type>[#<group>]", "select that environment object at boot and scroll the Inspector to the group", false, &parse::Inspect },
    { "--settings-menu", "[=display|graphics|image|controls|menu]", "the player's settings screen (or the Esc menu) open at boot", true, &parse::SettingsMenu },
    { "--build-dist", "[=<profile>[,override-licences][,delete=<build>]]", "the Build Distribution window open at boot (implies --editor)", false, &parse::BuildDist },
    { "--user-dir", "=<folder>", "a packaged build's user folder (log, settings, caches) instead of %LOCALAPPDATA%", true },

    // --- the local model's harnesses ----------------------------------------------------------------------------------
    { "--intent", "=<phrase>", "run one phrase through the editor's command pipeline, log the verdict, quit (needs the model)", false, &parse::Intent },
    { "--intent-run", "", "with --intent: execute the phrase, not only preview it", false, &parse::IntentRun },
    { "--intent-repeat", "=<n>", "with --intent: ask the same phrase n times in one process", false, &parse::IntentRepeat },
    { "--intent-timeout", "=<sec>", "with --intent: give up after this long (default 180)", false, &parse::IntentTimeout },
    { "--chat", "=<phrase>", "send one message through the Model Chat panel once the editor is warm", false, &parse::Chat },
    { "--then", "=<phrase>", "with --chat: a second phrase in the same session once the first has answered", false },

    // --- gates and self-tests ------------------------------------------------------------------------------------------
    { "--physics-gate", "[=<name>]", "the physics gates (no window); exit code = the failed ones", false },
    { "--scalability-check", "", "every graphics level applied and every knob read back in a running process; exit = failed reads", false, &parse::ScalabilityCheck },
    { "--scalability-selftest", "", "the scalability table's self-test", false },
    { "--scene-stress", "[=<iterations>]", "the level-switch churn gate on the real loop; verdict CLEAN or a fault", false },
    { "--scene-stress-gbv", "[=<iterations>]", "the churn gate under GPU-based validation, every pass (minutes: ask the owner)", false },
    { "--scene-stress-roughness", "", "the churn gate edits materials' roughness on wind_test", false },
    { "--scene-stress-sky", "", "the churn gate edits the sky on wind_test", false },
    { "--stress-level", "=<path>", "one more level in --scene-stress's rotation (boots into it)", false },
    { "--rt-audit", "[=<sec>]", "the render thread's audit: the frame in flight's state must not change under it", false },
    { "--rt-audit-selftest", "", "the audit's control: must FAIL naming object #0 model matrix", false },
    { "--retire-selftest", "[=control|upload|pass]", "the deferred-release service's proof", false },
    { "--thread-check-selftest", "", "proves the thread-role checks are compiled in (TcThreadChecks builds)", false },
    { "--throw-selftest", "=<texstream|texdecode|fsprobe|...>", "that detached task throws once; its owner must see it land", false },
    { "--crash-selftest", "=<arm>", "a crash of that kind, its minidump (and in Shipping the message box)", true },
    { "--texstream-selftest", "=layout|size", "the first texture stream-in is built wrong on purpose; the check must catch it", false },
    { "--water-mask-selftest", "=throw[:<path part>]", "the first water mask asked for throws; the frame must survive it", false },
    { "--upload-ring-check", "[=selftest]", "every frame's upload ring sealed and compared at its fence", false },
    { "--hzb-cull-selftest", "", "the box -> HZB visibility self-test instead of the app", false },
    { "--tasksystem-stress", "", "the task system's stress harness instead of the app", false },
    { "--stress-overflow", "", "with --tasksystem-stress: overflow a task's dependents (must end through the invariant, exit 3)", false },
    { "--renderer-submission-stress", "", "the CPU-only submission-timeline stress harness instead of the app", false },
    { "--stress-null-cl", "", "with --renderer-submission-stress: submit a null command list (control arm)", false },
    { "--stress-invalid-batch", "", "with --renderer-submission-stress: submit an invalid batch (control arm)", false },
    { "--stress-duplicate-cl", "", "with --renderer-submission-stress: submit a command list twice (control arm)", false },
    { "--stress-dup-order-direct", "", "with --renderer-submission-stress: a duplicate order on the direct queue (control arm)", false },
    { "--stress-dup-order-bundle", "", "with --renderer-submission-stress: a duplicate order in a bundle (control arm)", false },
    { "--log-stress", "", "the logging stress harness instead of the app", false },
    { "--log-stress-fatal-child", "", "a child process of --log-stress (the fatal path)", false },
    { "--log-stress-session-child", "", "a child process of --log-stress (two sessions at once)", false },
    { "--log-synchronous", "", "a misspelling --log-stress proves is NOT taken for --log-sync", false },
    { "--mem-readout", "=<s>[,warmup=<s>][,limit=<MB/s>]", "the leak gate: memory series over the run; verdict PASS or FAIL <series>", false },
    { "--mem-selftest", "=leak@<tag>", "the leak gate's control: leak into that tag on purpose", false },
    { "--mem-bench", "", "time the counted operator new/delete against raw mimalloc", false },
    { "--cull-benchmark", "", "time the per-object frustum test, legacy against the precomputed planes", false },
    { "--textmanager-benchmark", "", "the text manager's benchmark instead of the app", false },
    { "--gpu-check", "", "what this GPU has against what a shipped build needs; exit 0 or 1", false },

    // --- the log, memory, the GPU's diagnostics ---------------------------------------------------------------------
    { "--log-level", "=<trace|debug|info|warning|error|fatal>", "the session log's threshold", true },
    { "--log-category", "=<name>:<level>", "one category's threshold (repeatable)", false },
    { "--log-file", "=<path>", "the session log's file instead of logs/session_*.log", false },
    { "--log-no-file", "", "no session log file", false },
    { "--log-sync", "", "write every log line synchronously (for a crash that loses its last lines)", false },
    { "--boot-profile", "", "the boot profile's per-frame buckets on an ordinary run", false },
    { "--memreport", "[=<sec>]", "one memory report into the session log that long after the first frame", false },
    { "--memtrack", "", "count CPU allocations by owner (MEM_SCOPE)", false },
    { "--memtrace", "[=all|cpu|gpu][,stacks=big|all|off|1/N][,start=<frame>]", "every allocation and free into traces/memtrace_*.mtrace", false },
    { "--gbv", "", "GPU-based validation on an ordinary run (Debug)", false },
    { "--gbv-mode", "=<state|none|...>", "the GBV shader patching mode (state is falsely clean: never gate on it)", false },
    { "--gbv-passes", "=<Pass>,<Pass>,...", "validate only these passes (RenderPassToWString names)", false },
    { "--gbv-rest", "=<none|...>", "with --gbv-passes: how the passes outside the list are patched", false },
    { "--gbv-selftest", "=<n>@<Pass>", "inject a validation error into that pass's command list (Skybox is the proven target)", false },
    { "--no-d3d-debug", "", "a Debug run without the D3D12 debug layer", false },
    { "--dred", "", "DRED breadcrumbs and page-fault report on a normal run", false },
    { "--dr-check", "", "poll the device-removed reason every frame (leaves logs/device_removed.log)", false },
    { "--no-streamline", "", "no Streamline SDK at all (no slInit, no device proxy)", false },
    { "--rt-force-as-fail", "", "fail the ray-tracing acceleration structure build on purpose (the fallback's test)", false },
    { "--rt-smoke", "", "a trivial BLAS/TLAS build instead of the app; verdict in rt_smoke.txt", false },
    { "--csm-readout", "", "dump the cascade fit table to the log on the first frame", false },
    { "--sdsm-readout", "", "dump the SDSM partitions to the log", false },
    { "--vis-readout", "", "dump the per-view visibility counters", false },
    { "--cull-nofuse", "", "the split Bucketize + Cull path for self-culling views (an A/B)", false },
    { "--compute-lane-probe", "", "acquire one compute command list at boot and check it records (async compute step 1)", false },
    { "--async-empty-submit", "", "one empty compute command list on the async queue every frame", false },
    { "--async-order-probe", "", "log the order the queues' submissions land in", false },
    { "--dump-submit-order", "", "write the frame's submission order once", false },
    { "--dump-barriers", "", "write every compiled barrier once (a before/after diff)", false },
    { "--barrier-census", "", "count the barrier transitions the engine emits and dump them", false },
    { "--barrier-cmp", "", "compare the compiled barriers against the reference path", false },
    { "--barrier-compile-log", "", "the compiled barrier count per frame", false },
    { "--barrier-flip-trace", "", "log every emitted barrier point and every request that matched none", false },
    { "--barrier-msg-trace", "", "a stack per barrier-interop debug message", false },
    { "--barrier-sync-wide", "", "the conservative sync scopes the narrowing removed", false },
    { "--barrier-cache-verify", "", "recompile the barriers every frame and diff them against the cross-frame cache", false },
    { "--canonical-check", "", "check the barriers against the canonical layouts every frame", false },
    { "--enhanced-barriers", "", "a no-op kept for old scripts: enhanced barriers are the default (--legacy-barriers is the switch)", false },
    { "--legacy-barriers", "", "the legacy resource-state barriers instead of the enhanced ones", false },
    { "--no-bind-batching", "", "every Material::Bind issues its root bindings in full", false },

    // --- the virtual shadow map's A/B switches -------------------------------------------------------------------------
    { "--vsm-compactargs", "=0|1", "the compacted (page, group) argument records", false },
    { "--vsm-lodbias", "=<n>", "the shadow LOD bias", false },
    { "--vsm-lodstride", "=<n>", "the shadow LOD tier stride", false },
    { "--vsm-no-local-scatter", "", "local views back on the brute-force per-page cull (an A/B)", false },
    { "--vsm-normalbias", "=<x>", "the clipmap's normal bias", false },
    { "--vsm-page-compact", "", "compact the page draws", false },
    { "--vsm-page-nocompact", "", "no compaction of the page draws", false },
    { "--vsm-page-multidraw", "", "the per-page draw loop instead of the single draw", false },
    { "--vsm-singledraw", "=0|1", "the single draw for every page, or the per-page loop", false },
    { "--vsm-refdist", "=<m>", "the clipmap's reference distance", false },
    { "--vsm-resident", "", "the page loop iterates the resident pages only", false },

    // --- the ocean's A/B switches --------------------------------------------------------------------------------------
    { "--ocean-classic-shore", "", "the old name of --ocean-surf-sim-shore", false },
    { "--ocean-runup-shore", "", "the run-up shore stack instead of the surf-sim surface", false },
    { "--ocean-surf-sim", "", "force the surf sim on whatever the level says", false },
    { "--ocean-surf-sim-shore", "", "the surf-sim surface (the default) instead of the run-up stack", false },
    { "--ocean-surf-debug", "=<view>", "the surf sim's debug view", false },
    { "--ocean-surf-poke", "=<sec>", "poke the surf sim this often (headless wave captures)", false },
    { "--ocean-shore-sink", "", "sink the run-up sheet under the terrain in the VS instead of discarding it", false },
    { "--ocean-vs-depth-probe", "", "a screen-space depth probe instead of the shore SDF in the vertex shader", false },
    { "--ocean-foam-debug", "[=<view>]", "compile the contact-foam debug views into the ocean shader", false },
    { "--ocean-geomfade", "=<m>", "override shoreGeometryFadeDistance", false },
    { "--ocean-wind", "=<0..1>", "force the sea state", false },
    { "--cloud-profile", "=<dds>", "the clouds' profile LUT from a file instead of generated", false },
    { "--cloud-formation", "=<dds>", "the clouds' formation volume from a file", false },
    { "--cloud-cells", "=<dds>", "the clouds' erosion cells volume from a file", false },
    { "--cloud-dump", "=<folder>", "write the clouds' generated volumes to that folder", false },

    // --- content tools (loose tree, no window) ------------------------------------------------------------------------
    { "--import", "=<staging folder>", "the offline asset converter (textures, skyboxes) instead of the app", false },
    { "--skybox", "=<hdr>", "with --import: convert that HDR panorama into a cube with its IBL", false },
    { "--sky-face", "=<px>", "with --skybox: the cube face size", false },
    { "--sky-out", "=<textures root>", "with --skybox: where the cube and its IBL siblings go", false },
    { "--sky-target", "=<median luminance>", "with --skybox: the calibration target (0 = the source's own)", false },
    { "--sky-keep-sun", "", "with --skybox: keep the sun in the IBL", false },
    { "--sky-sun-radius", "=<deg>", "with --skybox: the sun disc removed from the IBL", false },
    { "--max-size", "=<px>", "with --import: the largest texture size", false },
    { "--high", "", "with --import: exhaustive BC7", false },
    { "--flip-green", "", "with --import: flip normal maps' green channel", false },
    { "--bc5-normal", "", "with --import: normal maps as BC5", false },
    { "--center-normals", "", "with --import: recentre normal maps", false },
    { "--cpu", "", "with --import: BC encode on the CPU (the GPU encodes by default)", false },
    { "--reimport", "", "re-bake a mesh headlessly (CPU only) with --reimport-src and --reimport-out", false },
    { "--reimport-src", "=<glTF>", "the source of --reimport", false },
    { "--reimport-out", "=<.mesh.bin>", "the output of --reimport", false },
    { "--reimport-manifest", "=<models/x/x.mesh.json>", "take every bake option from the manifest", false },
    { "--reimport-scale", "=<x>", "the bake scale (the import dialog's unit fix)", false },
    { "--reimport-chunk", "=<n>", "split LOD0 into an n x n grid of submeshes (mesh.json chunkGrid)", false },
    { "--reimport-drop-small", "=0|1", "drop small disconnected parts", false },
    { "--reimport-foliage", "=<0|1,...>", "which slots are foliage (mesh.json windFoliage)", false },
    { "--reimport-recompute", "=<slot,...>", "slots whose normals are recomputed (mesh.json recomputeNormalSlots)", false },
    { "--reimport-normal-weight", "=<x>", "the LOD simplifier's normal weight (mesh.json lodNormalWeight)", false },
    { "--reimport-prune", "=<fraction>", "LOD3 foliage prune keep fraction (0 = off)", false },
    { "--reimport-lod3", "=0|1", "the harsh LOD3 solid budget (mesh.json lod3Aggressive)", false },
    { "--reimport-lod3-drop", "=<slot,...>", "slots that vanish at LOD3 (mesh.json lod3DropSlots)", false },
    { "--reimport-lod3-ratio", "=<x>", "LOD3's ratio scale (mesh.json lod3RatioScale)", false },
    { "--reimport-lod3-error", "=<x>", "LOD3's error scale (mesh.json lod3ErrorScale)", false },
    { "--reimport-inner-ratio", "=<x>", "LOD3 leaf interior decimation ratio (mesh.json foliageInnerRatio)", false },
    { "--reimport-inner-error", "=<x>", "LOD3 leaf interior decimation error (mesh.json foliageInnerError)", false },
    { "--tessy-upgrade", "=all|<a.mesh.bin>,...", "rewrite older .mesh.bin files IN PLACE as the current version (models/: the owner's call)", false },
    { "--tessy-lod-build", "=all|<part>,...", "build the cluster DAG at load for every (or the matching) clustered asset", false },
    { "--tessy-lod-fold", "=0", "a double-sided sheet keeps both sides in the DAG's simplification (an A/B)", false },
    { "--tessy-lod-prune", "=0", "the DAG's leaf cards simplified by edge collapses alone (an A/B)", false },
    { "--lod-report", "[=<part of a path>]", "the Mesh Editor's LOD switching report for every (or the matching) mesh", false },
    { "--uv-density", "=<mesh.json>[,...]", "measure each manifest's per-slot UV density and write it as uvDensity", false },
    { "--seam-ratio", "=<geometry>[,<geometry>...]", "how open each submesh's surface is", false },
    { "--water-mask-lid", "=<geometry .mesh.bin>[,<out.obj>[,<glass slots>]]", "a hull's water mask lid, offline", false },

    // --- the distribution: cook, packages, the VFS ----------------------------------------------------------------------
    { "--cook", "=<profile.json>", "cook the content of a distribution profile (with --cook-out)", false },
    { "--cook-out", "=<folder>", "where --cook writes", false },
    { "--cook-check", "", "with --cook: only the dependency walk", false },
    { "--levels", "=<level,...>", "with --cook: the levels instead of the profile's", false },
    { "--boot-level", "=<level>", "with --cook: the level a shipped build boots", false },
    { "--version", "=<x.y.z>", "with --cook: the version written into the build", false },
    { "--flavour", "=<shipping|development>", "with --cook: the build flavour", false },
    { "--no-encrypt", "", "with --cook: packages without encryption", false },
    { "--override-licences", "", "with --cook: ship the assets strictLicences refuses, for one build", false },
    { "--shader-cook", "=<list>,<out.tcpak>", "compile a list of shader permutations into a shaders package", false },
    { "--shader-record", "=<file>", "list every shader compile request the run makes", false },
    { "--shader-model", "=<6_x>", "the shader model of every compile", false },
    { "--pak-build", "=<out.tcpak>", "build a package from a file list (--pak-files)", false },
    { "--pak-files", "=<list>", "with --pak-build: the files to pack", false },
    { "--pak-name", "=<name>", "with --pak-build: the package's name", false },
    { "--pak-level", "=<n>", "with --pak-build: the compression level", false },
    { "--pak-priority", "=<n>", "with --pak-build: the package's priority", false },
    { "--pak-encrypt", "", "with --pak-build: encrypt (the key from --pak-key or TC_PAK_KEY)", false },
    { "--pak-key", "=<key.json>", "the package key", false },
    { "--pak-keygen", "=<key.json>", "write a new package key", false },
    { "--pak-key-header", "=<key.json>,<header>", "write the key's built-in header", false },
    { "--pak-list", "=<file.tcpak>", "list a package's files", false },
    { "--pak-verify", "=<file.tcpak>", "verify a package", false },
    { "--pak-dir", "=<folder>", "the packages' folder (--vfs=packages)", false },
    { "--vfs", "=<loose|packages>", "where content is read from", false },
    { "--vfs-record", "=<file>", "record every file the run reads", false },
    { "--vfs-selftest", "", "the VFS's self-test", false },
};

std::vector<Switch> Sorted()
{
    std::vector<Switch> sorted(std::begin(kSwitches), std::end(kSwitches));
    std::sort(sorted.begin(), sorted.end(),
        [](const Switch& a, const Switch& b) { return std::string_view(a.name) < std::string_view(b.name); });
    return sorted;
}
}

const Switch* Switches(std::size_t& count)
{
    static const std::vector<Switch> sorted = Sorted();
    count = sorted.size();
    return sorted.data();
}

const Switch* FindSwitch(std::string_view name)
{
    for (const Switch& s : kSwitches)
    {
        if (name == s.name) { return &s; }
    }
    return nullptr;
}

void ParseSwitches(const char* cmdLine)
{
    for (const Switch& s : kSwitches)
    {
        if (s.parse) { s.parse(cmdLine); }
    }
}

std::string HelpText(bool shippingOnly)
{
    std::size_t count = 0;
    const Switch* table = Switches(count);
    std::size_t width = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (shippingOnly && !table[i].shipping) { continue; }
        width = std::max(width, std::string_view(table[i].name).size() + std::string_view(table[i].value).size());
    }
    std::string text;
    for (std::size_t i = 0; i < count; ++i)
    {
        const Switch& s = table[i];
        if (shippingOnly && !s.shipping) { continue; }
        std::string left = std::string(s.name) + s.value;
        if (left.size() < width) { left.append(width - left.size(), ' '); }
        text += "  " + left + "  " + s.help + (s.shipping ? "  [shipping]" : "") + "\n";
    }
    return text;
}
}
