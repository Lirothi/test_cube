"""Compile the engine's compute shaders offline, so a syntax error cannot reach a run.

MSBuild does NOT compile HLSL in this project -- shaders are compiled at runtime, which means a
broken shader shows up as a *silently missing feature* in Release (the material is null, its pass
early-outs) and as an assert on a constant-buffer field name in Debug. Neither points at the actual
error. This does.

    python tools/check_shaders.py                 # every entry point listed below
    python tools/check_shaders.py exposure        # only shaders whose path contains "exposure"

Exit code is non-zero if anything failed, so it can gate a change set.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SHADERS = PROJECT_ROOT / "shaders"

# (file, entry point). Compute shaders, which need no permutation to build.
COMPUTE_ENTRIES = [
    # Occlusion plan S2/S3b: the two compute shaders that include hzb_cull.hlsli -- a library
    # edit gets its compile check here (S5's G-buffer cull will be the third).
    ("hzb_cull_selftest_cs.hlsl", "CSMain"),
    ("vis_test_cs.hlsl", "CSMain"),
    ("exposure_histogram_cs.hlsl", "CSClear"),
    ("exposure_histogram_cs.hlsl", "CSBuild"),
    ("exposure_solve_cs.hlsl", "CSMain"),
    ("exposure_baselum_cs.hlsl", "CSMain"),
    ("tonemap_cs.hlsl", "CSMain"),
    ("fxaa_cs.hlsl", "CSMain"),
    ("compose_cs.hlsl", "CSMain"),
    ("lighting_cs.hlsl", "CSMain"),
    ("ssr_cs.hlsl", "CSMain"),
    # Traces the same two SSR searches as ssr_cs, so a change to either tracer has to compile here
    # too. (It was missing from this list until P13 put the UE march into it.)
    ("ocean_reflection_cs.hlsl", "CSMain"),
    ("gtao_cs.hlsl", "CSMain"),
    ("gtao_filter_cs.hlsl", "CSMain"),
    ("gtao_temporal_cs.hlsl", "CSMain"),
    ("ssr_temporal_cs.hlsl", "CSMain"),
    ("gtao_upsample_cs.hlsl", "CSMain"),
    ("hzb_build_cs.hlsl", "CSMain"),
    ("debug_preview_cs.hlsl", "CSMain"),
    ("bloom_cs.hlsl", "CSMain"),
    ("bloom_fft_cs.hlsl", "CSMain"),
    ("bloom_conv_cs.hlsl", "CSMain"),
    # P16.5 moved the local-light falloff into utils.hlsli; these are the passes that call it, and
    # none of them was checked while it was being changed under them.
    ("pointlight_cs.hlsl", "CSMain"),
    ("spotlight_cs.hlsl", "CSMain"),
    # The GPU-driven shadow path. Every one of these is plain compute with no permutation, and the
    # whole set was missing here while the terrain-chunking work was editing the setup CS's constant
    # buffer -- exactly the change class this tool exists to catch, since a CB that no longer matches
    # its CPU mirror produces a silently missing shadow pass in Release.
    ("vsm_page_setup_cs.hlsl", "CSMain"),
    ("vsm_page_scatter_cs.hlsl", "CSMain"),
    ("vsm_page_scatter_clear_cs.hlsl", "CSMain"),
    ("vsm_page_request_cs.hlsl", "CSMain"),
    ("vsm_page_request_clear_cs.hlsl", "CSMain"),
    ("vsm_page_alloc_init_cs.hlsl", "CSMain"),
    ("vsm_page_alloc_map_cs.hlsl", "CSMain"),
    ("vsm_page_alloc_touch_cs.hlsl", "CSMain"),
    ("vsm_page_alloc_freelist_cs.hlsl", "CSMain"),
    ("shadow_cull_cs.hlsl", "CSMain"),
    ("shadow_cull_clear_cs.hlsl", "CSMain"),
    ("shadow_gi_scatter_cs.hlsl", "CSMain"),
    # Occlusion plan S5b: the cascade HZB post cull (includes hzb_cull.hlsli, like the two above it).
    ("shadow_cull_post_cs.hlsl", "CSMain"),
]

# Shaders needing a target above the 6_0 default. Kept separate rather than widening every entry to
# a (file, entry, target) triple: exactly one shader needs it, and the reason is specific.
COMPUTE_ENTRIES_SM66 = [
    # ResourceDescriptorHeap (bindless) is SM 6.6, and inline RayQuery is 6.5.
    ("rt_reflections_cs.hlsl", "CSMain"),
]


# (file, target, entry, defines, label). Graphics shaders were left out of this tool on the belief
# that their material permutations "are not reproducible from the command line". For the ocean that
# was not true -- its two whole surfaces are chosen by ONE define -- and the cost of believing it
# was that ocean_surface.hlsl's modern variant stopped compiling (a kSkyRoughMaxMip redefinition,
# left behind when F8 moved that constant into ibl_common.hlsli) and NOBODY NOTICED. That variant
# is off by default, so the only symptom was `--ocean-runup-shore` drawing no water at all.
#
# Only permutations a single flag selects belong here. A shader whose defines really do come from
# runtime material state still cannot be checked this way, and listing it would make this tool lie
# about its own coverage.
GRAPHICS_ENTRIES = [
    ("ocean_surface.hlsl", "vs_6_0", "VSMain", [], "runup"),
    ("ocean_surface.hlsl", "ps_6_0", "PSMain", [], "runup"),
    ("ocean_surface.hlsl", "vs_6_0", "VSMain", ["OCEAN_SHORE_RUNUP=0"], "legacy"),
    ("ocean_surface.hlsl", "ps_6_0", "PSMain", ["OCEAN_SHORE_RUNUP=0"], "legacy"),
    # The other two transparent surfaces. Both were absent while their per-view constant buffer was
    # being extended, and both meet the rule above: one define picks each permutation.
    ("glass.hlsl", "vs_6_0", "VSMain", [], ""),
    ("glass.hlsl", "ps_6_0", "PSMain", [], ""),
    ("glass.hlsl", "ps_6_0", "PSMain", ["EDITOR_OBJECT_ID=1"], "editor-id"),
    ("particles.hlsl", "vs_6_0", "VSMain", [], ""),
    ("particles.hlsl", "vs_6_0", "VSMain", ["PARTICLE_SORTED=1"], "sorted"),
    ("particles.hlsl", "ps_6_0", "PSMain", [], ""),
    # P8C-2: the lens-flare bokeh scatter.
    ("lens_flare.hlsl", "vs_6_0", "VSMain", [], ""),
    ("lens_flare.hlsl", "ps_6_0", "PSMain", [], ""),
    # Occlusion plan S3a: the box draw of the hardware occlusion queries (no permutations).
    ("occlusion_query.hlsl", "vs_6_0", "VSMain", [], ""),
    ("occlusion_query.hlsl", "ps_6_0", "PSMain", [], ""),
    # Occlusion plan S4: the indirect G-buffer (plus the editor object-id permutation Debug builds).
    ("gbuffer_indirect.hlsl", "vs_6_0", "VSMain", [], ""),
    ("gbuffer_indirect.hlsl", "ps_6_0", "PSMain", [], ""),
    ("gbuffer_indirect.hlsl", "ps_6_0", "PSMain", ["EDITOR_OBJECT_ID=1"], "editor-id"),
    # Occlusion plan S5b: the cascade-tile permutation of the depth pyramid build (one define
    # selects it -- the atlas rect source and the 1 - z store). A compute shader in this list
    # because only this list carries defines.
    ("hzb_build_cs.hlsl", "cs_6_0", "CSMain", ["HZB_LIGHT=1"], "light"),
]


def find_dxc() -> Path | None:
    roots = [
        Path(r"C:\Program Files (x86)\Windows Kits\10\bin"),
        Path(r"C:\Program Files\Windows Kits\10\bin"),
    ]
    found = []
    for root in roots:
        if root.is_dir():
            found.extend(root.glob("*/x64/dxc.exe"))
            found.extend(root.glob("*/x86/dxc.exe"))
    return sorted(found)[-1] if found else None


def main() -> int:
    dxc = find_dxc()
    if dxc is None:
        print("dxc.exe not found in the Windows SDK; cannot check shaders")
        return 2

    needle = sys.argv[1].lower() if len(sys.argv) > 1 else ""
    failures = 0
    checked = 0
    for name, entry in COMPUTE_ENTRIES:
        if needle and needle not in name.lower():
            continue
        path = SHADERS / name
        if not path.exists():
            print(f"SKIP  {name} (missing)")
            continue
        checked += 1
        result = subprocess.run(
            [str(dxc), "-T", "cs_6_0", "-E", entry, str(path), "-Fo", "NUL"],
            capture_output=True, text=True, cwd=SHADERS,
        )
        if result.returncode == 0:
            # COMPILING IS NOT ENOUGH. dxc accepts a compute entry with no [RootSignature]
            # attribute; the engine does not -- Material::CreateCompute fails to build the PSO,
            # keeps the Material object, and leaves its pipeline state null. The pass then
            # dispatches with no PSO. P8 lost a debug session to exactly that with this tool
            # reporting a clean 22/22, so the attribute is checked here rather than trusted.
            src = path.read_text(encoding="utf-8", errors="ignore")
            if f"void {entry}(" in src and "[RootSignature(" not in src:
                failures += 1
                print(f"FAIL  {name}:{entry} -- compiles, but has no [RootSignature] attribute;"
                      f" the engine cannot build a PSO from it")
            else:
                print(f"ok    {name}:{entry}")
        else:
            failures += 1
            print(f"FAIL  {name}:{entry}")
            for line in (result.stderr or result.stdout).splitlines()[:12]:
                print(f"      {line}")

    for name, entry in COMPUTE_ENTRIES_SM66:
        if needle and needle not in name.lower():
            continue
        path = SHADERS / name
        if not path.exists():
            print(f"SKIP  {name} (missing)")
            continue
        checked += 1
        result = subprocess.run(
            [str(dxc), "-T", "cs_6_6", "-E", entry, str(path), "-Fo", "NUL"],
            capture_output=True, text=True, cwd=SHADERS,
        )
        if result.returncode == 0:
            src = path.read_text(encoding="utf-8", errors="ignore")
            if f"void {entry}(" in src and "[RootSignature(" not in src:
                failures += 1
                print(f"FAIL  {name}:{entry} -- compiles, but has no [RootSignature] attribute;"
                      f" the engine cannot build a PSO from it")
            else:
                print(f"ok    {name}:{entry} [sm6.6]")
        else:
            failures += 1
            print(f"FAIL  {name}:{entry} [sm6.6]")
            for line in (result.stderr or result.stdout).splitlines()[:12]:
                print(f"      {line}")

    for name, target, entry, defines, label in GRAPHICS_ENTRIES:
        if needle and needle not in name.lower():
            continue
        path = SHADERS / name
        if not path.exists():
            print(f"SKIP  {name} (missing)")
            continue
        checked += 1
        cmd = [str(dxc), "-T", target, "-E", entry]
        for d in defines:
            cmd += ["-D", d]
        cmd += [str(path), "-Fo", "NUL"]
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=SHADERS)
        tag = f"{name}:{entry}[{label}]"
        if result.returncode == 0:
            print(f"ok    {tag}")
        else:
            failures += 1
            print(f"FAIL  {tag}")
            for line in (result.stderr or result.stdout).splitlines()[:12]:
                print(f"      {line}")

    print(f"\n{checked - failures}/{checked} compiled")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
