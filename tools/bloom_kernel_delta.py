"""Give a bloom kernel UE's centre/scatter balance by adding energy to its centre texel.

UE's FFT bloom splits a kernel into the light that never scattered (the CENTRE: the excess above
MaxScatterDispersion inside one view texel of the brightest texel) and the SCATTER (the whole kernel
clamped to that level), and BloomIntensity scales the scatter's share (BloomFinalizeApplyConstants.usf).
Their DefaultBloomKernel photograph is ~98 % centre, so BloomIntensity 0.1-1 is its range. The derived
kernels in textures/ (BloomKernelStar*, the anamorphic and sunburst variants) were built with the
delta clipped or left out, so UE's survey calls 8-80 % of them scatter and the same look needs an
intensity 5-45x lower. This adds the missing delta -- a single texel, so the rays, the glow and any
baked band keep their shape -- until the scatter share at the reference view-texel diameter equals
the target (default: what DefaultBloomKernel measures there), then scales the whole image so the
centre fits FP16. The survey is the engine's (BloomRenderer::SurveyKernel), transcribed.

usage: bloom_kernel_delta.py <kernel.dds> [--target 0.0175 | --like textures/DefaultBloomKernel.dds]
                             [--diameter 1.6] [--peak 60000] [--write]
Without --write it only reports. With it the original is first copied to
textures/kernel_backup/<name>.orig.dds (never overwritten once there).
"""
import argparse
import os
import shutil

import numpy as np

HEADER = 148  # 'DDS ' + DDS_HEADER (124) + DDS_HEADER_DXT10 (20)


def load(path):
    raw = open(path, 'rb').read()
    h, w = np.frombuffer(raw[12:20], dtype=np.uint32)
    if raw[84:88] != b'DX10' or int(np.frombuffer(raw[128:132], dtype=np.uint32)[0]) != 10:
        raise SystemExit(f'{path}: not an R16G16B16A16_FLOAT DX10 DDS')
    px = np.frombuffer(raw[HEADER:HEADER + int(w) * int(h) * 8], dtype=np.float16).reshape(int(h), int(w), 4)
    return raw[:HEADER], px.astype(np.float64), raw[HEADER + int(w) * int(h) * 8:]


def survey(k, diameter):
    """(centre (x, y), clamp level, centre energy rgb, scatter energy rgb) -- SurveyKernel's numbers."""
    dim = k.shape[0]
    R = 0.5 * diameter
    lum = k[..., :3] @ np.array([0.2126, 0.7152, 0.0722])
    cy, cx = np.unravel_index(np.argmax(lum), lum.shape)
    groups = 2 * ((int(np.ceil(R)) + 4 + 7) // 8)
    lo, hi = -4 * groups, 4 * groups - 1
    reach = int(np.ceil(2 * R + 4))
    clamp = 0.0
    for oy in range(max(-reach, lo), min(reach, hi) + 1):
        for ox in range(max(-reach, lo), min(reach, hi) + 1):
            cheb = max(abs(ox), abs(oy))
            zone = (1 - np.clip(2 * R + 1 - cheb, 0, 1)) * np.clip(2 * R + 4 - cheb, 0, 1)
            if zone > 0:
                clamp = max(clamp, k[(cy + oy) % dim, (cx + ox) % dim, :3].max() * zone)
    centre = np.zeros(3)
    r2 = int(np.ceil(R + 0.5))
    for oy in range(-r2, r2 + 1):
        for ox in range(-r2, r2 + 1):
            zone = np.clip(R + 0.5 - max(abs(ox), abs(oy)), 0, 1)
            if zone > 0:
                centre += np.maximum(k[(cy + oy) % dim, (cx + ox) % dim, :3] - clamp, 0) * zone
    scatter = np.clip(k[..., :3], 0, clamp).sum(axis=(0, 1))
    return (cx, cy), clamp, centre, scatter


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('kernel')
    ap.add_argument('--target', type=float, default=None)
    ap.add_argument('--like', default='textures/DefaultBloomKernel.dds')
    ap.add_argument('--diameter', type=float, default=1.6)  # convSize 0.5 at 25 % of 2560: 512 / 320
    ap.add_argument('--peak', type=float, default=60000.0)
    ap.add_argument('--write', action='store_true')
    a = ap.parse_args()

    head, k, tail = load(a.kernel)
    (cx, cy), clamp, centre, scatter = survey(k, a.diameter)
    frac = scatter / (centre + scatter)
    if a.target is None:
        _, kr, _ = load(a.like)
        _, _, rc, rs = survey(kr, a.diameter)
        a.target = float((rs / (rc + rs))[1])
    print(f'{a.kernel}: centre texel ({cx},{cy}) = {k[cy, cx, :3].round(1)}, clamp {clamp:.1f}, '
          f'scatter share {frac.round(4)} -> target {a.target:.4f} (green)')
    if frac[1] <= a.target:
        print('  already at or under the target: nothing to add')
        return
    # The centre texel is above the clamp, so whatever is added to it is centre energy one for one.
    delta = scatter[1] / a.target - scatter[1] - centre[1]
    k[cy, cx, :3] += delta
    scale = a.peak / k[..., :3].max()
    k *= scale
    (cx2, cy2), clamp2, centre2, scatter2 = survey(k, a.diameter)
    frac2 = scatter2 / (centre2 + scatter2)
    print(f'  + {delta:.4g} on the centre texel, whole image x {scale:.4g}: scatter share {frac2.round(4)}, '
          f'centre ({cx2},{cy2}) peak {k[..., :3].max():.1f}')
    for d in (1.0, 3.2):
        _, _, c3, s3 = survey(k, d)
        print(f'  at diameter {d}: scatter share {(s3 / (c3 + s3)).round(4)}')
    if a.write:
        backup_dir = os.path.join(os.path.dirname(a.kernel), 'kernel_backup')
        os.makedirs(backup_dir, exist_ok=True)
        backup = os.path.join(backup_dir, os.path.basename(a.kernel)[:-4] + '.orig.dds')
        if not os.path.exists(backup):
            shutil.copy2(a.kernel, backup)
            print(f'  original kept at {backup}')
        elif open(backup, 'rb').read() != open(a.kernel, 'rb').read():
            raise SystemExit(f'  {backup} exists and differs from the kernel: refusing to overwrite twice')
        out = k.astype(np.float16)
        if not np.all(np.isfinite(out)):
            raise SystemExit('  FP16 overflow: lower --peak')
        open(a.kernel, 'wb').write(head + out.tobytes() + tail)
        print(f'  written {a.kernel}')


if __name__ == '__main__':
    main()
