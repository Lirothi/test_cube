"""Generate a SCALAR SPATIOTEMPORAL BLUE NOISE mask (STBN) -- the texture UE's volumetric clouds take their
ray-march start offset from (VolumetricCloud.usf:803, BlueNoiseScalar(FullResPixelCoord, FrameIndexMod8); UE
ship a 128x128x64 one). Ours is generated, not copied: Wolfe, Morrical, Ramamoorthi, "Scalar Spatiotemporal
Blue Noise Masks" (arXiv 2112.09629 / EGSR 2022), section 4.1:

  void and cluster (Ulichney 1993) with the energy of eq. 3 --
      E(p, q) = exp(-|p_xy - q_xy|^2 / (2 s^2))   if p_z == q_z          (blue in each 2D slice)
              = exp(-(p_z - q_z)^2  / (2 s^2))    if p_xy == q_xy        (blue along time, per pixel)
              = 0                                 otherwise
  sigma 1.9 on all axes, every distance toroidal, an initial binary pattern of 10 % of the pixels.

Each slice is then a blue noise mask, and each pixel's values over the slices are a 1D blue noise sequence:
an accumulation over frames converges faster and flickers less than with white or golden-ratio noise.

Output: an uncompressed DX10 DDS, DXGI_FORMAT_R8_UNORM, TEXTURE3D, W x H x D, value = floor(rank * 256 / N).
The engine reads it in VolumetricCloud::LoadVolumeDds.

usage: python tools/gen_stbn.py [out.dds] [--size 128] [--depth 64] [--seed 1]
Takes a few minutes for 128x128x64 (about a million void-and-cluster steps, each touching ~380 texels).
"""
import argparse
import struct
import sys
import time

import numpy as np

SIGMA = 1.9
RADIUS = 9           # the kernels are truncated at ~4.7 sigma (exp(-81/7.22) = 1.3e-5)
INITIAL_DENSITY = 0.10


class Field:
    """The energy field F and the on/off pattern, with the per-slice minimum over OFF pixels (the
    largest void) and maximum over ON pixels (the tightest cluster) cached: one toggle changes a 19x19
    window of its own slice (recomputed) and ONE texel in each of 18 other slices (patched in place)."""

    def __init__(self, depth, size, kernel_xy, kernel_z):
        self.Z, self.N = depth, size
        self.kxy, self.kz = kernel_xy, kernel_z
        self.F = np.zeros((depth, size, size), dtype=np.float64)
        self.on = np.zeros((depth, size, size), dtype=bool)
        self.minv = np.full(depth, np.inf); self.mini = np.zeros(depth, dtype=np.int64)
        self.maxv = np.full(depth, -np.inf); self.maxi = np.zeros(depth, dtype=np.int64)
        d = np.arange(-RADIUS, RADIUS + 1)
        self.d = d
        self.dz = np.array([v for v in d if v != 0])
        self.count_on = 0

    def rebuild(self):
        """F from scratch (FFT convolution with the same truncated kernels the toggles use)."""
        Z, N = self.Z, self.N
        kxy = np.zeros((N, N)); kz = np.zeros(Z)
        for i, dy in enumerate(self.d):
            for j, dx in enumerate(self.d):
                kxy[dy % N, dx % N] += self.kxy[i, j]
        for i, dz in enumerate(self.dz):
            kz[dz % Z] += self.kz[i]
        on = self.on.astype(np.float64)
        spatial = np.real(np.fft.ifft2(np.fft.fft2(on, axes=(1, 2)) * np.fft.fft2(kxy)[None, :, :], axes=(1, 2)))
        temporal = np.real(np.fft.ifft(np.fft.fft(on, axis=0) * np.fft.fft(kz)[:, None, None], axis=0))
        self.F = spatial + temporal
        self.count_on = int(self.on.sum())
        for z in range(Z):
            self.recompute(z)

    def recompute(self, z):
        Fz = self.F[z].ravel(); onz = self.on[z].ravel()
        off = np.where(onz, np.inf, Fz); i = int(off.argmin()); self.mini[z] = i; self.minv[z] = off[i]
        onv = np.where(onz, Fz, -np.inf); j = int(onv.argmax()); self.maxi[z] = j; self.maxv[z] = onv[j]

    def toggle(self, z, y, x):
        """Turn the pixel on if it is off and off if it is on, moving its energy accordingly."""
        Z, N = self.Z, self.N
        turning_on = not self.on[z, y, x]
        sign = 1.0 if turning_on else -1.0
        self.on[z, y, x] = turning_on
        self.count_on += 1 if turning_on else -1
        rows = (y + self.d) % N; cols = (x + self.d) % N
        self.F[z, rows[:, None], cols[None, :]] += sign * self.kxy
        zs = (z + self.dz) % Z
        self.F[zs, y, x] += sign * self.kz
        self.recompute(z)
        t = y * N + x
        for z2 in zs:
            v = self.F[z2, y, x]
            if turning_on:
                # Energy rose at t: the void minimum moves only if t WAS it; the cluster max may become t.
                if self.mini[z2] == t:
                    self.recompute(z2)
                elif self.on[z2, y, x] and (v > self.maxv[z2] or self.maxi[z2] == t):
                    self.maxv[z2] = v; self.maxi[z2] = t
            else:
                if self.maxi[z2] == t:
                    self.recompute(z2)
                elif not self.on[z2, y, x] and (v < self.minv[z2] or self.mini[z2] == t):
                    self.minv[z2] = v; self.mini[z2] = t

    def largest_void(self):
        z = int(self.minv.argmin()); i = int(self.mini[z])
        return z, i // self.N, i % self.N

    def tightest_cluster(self):
        z = int(self.maxv.argmax()); i = int(self.maxi[z])
        return z, i // self.N, i % self.N


def write_dds(path, volume):
    D, H, W = volume.shape
    header = bytearray(148)
    header[0:4] = b'DDS '
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x800000  # CAPS HEIGHT WIDTH PITCH PIXELFORMAT DEPTH
    struct.pack_into('<7I', header, 4, 124, flags, H, W, W, D, 1)
    struct.pack_into('<2I4s', header, 76, 32, 0x4, b'DX10')             # pixel format: FOURCC DX10
    struct.pack_into('<2I', header, 108, 0x1000 | 0x8, 0x200000)         # caps TEXTURE|COMPLEX, caps2 VOLUME
    struct.pack_into('<5I', header, 128, 61, 4, 0, 1, 0)                 # R8_UNORM, TEXTURE3D, misc, array 1
    with open(path, 'wb') as f:
        f.write(header)
        f.write(np.ascontiguousarray(volume, dtype=np.uint8).tobytes())


def spectrum_check(volume):
    """Low-frequency share of the power, spatial (per slice) and temporal (per pixel): blue noise has
    little; white noise has the share of the band's area (~0.2 for the |k| < 0.2*Nyquist band here)."""
    v = volume.astype(np.float64) - volume.mean()
    D, H, W = v.shape
    p_xy = np.mean(np.abs(np.fft.fft2(v, axes=(1, 2))) ** 2, axis=0)
    ky = np.fft.fftfreq(H)[:, None]; kx = np.fft.fftfreq(W)[None, :]
    low_xy = (np.sqrt(kx ** 2 + ky ** 2) < 0.1) & ((kx != 0) | (ky != 0))
    p_z = np.mean(np.abs(np.fft.fft(v, axis=0)) ** 2, axis=(1, 2))
    kz = np.abs(np.fft.fftfreq(D))
    low_z = (kz < 0.1) & (kz > 0)
    white_xy = low_xy.sum() / (H * W - 1); white_z = low_z.sum() / (D - 1)
    return (p_xy[low_xy].sum() / p_xy.sum() / white_xy, p_z[low_z].sum() / p_z.sum() / white_z)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out', nargs='?', default='textures/noise/stbn_scalar_128x128x64.dds')
    ap.add_argument('--size', type=int, default=128)
    ap.add_argument('--depth', type=int, default=64)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    N, Z = a.size, a.depth
    total = N * N * Z
    d = np.arange(-RADIUS, RADIUS + 1)
    kxy = np.exp(-(d[:, None] ** 2 + d[None, :] ** 2) / (2.0 * SIGMA * SIGMA))
    kz = np.exp(-np.array([v * v for v in d if v != 0], dtype=np.float64) / (2.0 * SIGMA * SIGMA))
    rng = np.random.default_rng(a.seed)
    field = Field(Z, N, kxy, kz)
    rank = np.full((Z, N, N), -1, dtype=np.int64)
    t0 = time.time()

    # Initial binary pattern: 10 % white noise, made blue by moving the tightest cluster to the largest
    # void until the two are the same pixel.
    n0 = int(total * INITIAL_DENSITY)
    flat = rng.choice(total, size=n0, replace=False)
    field.on.ravel()[flat] = True
    field.rebuild()
    swaps = 0
    while True:
        c = field.tightest_cluster(); field.toggle(*c)
        v = field.largest_void()
        if v == c:
            field.toggle(*c)
            break
        field.toggle(*v)
        swaps += 1
        if swaps % 20000 == 0:
            print('  initial pattern: %d swaps (%.0f s)' % (swaps, time.time() - t0), flush=True)
    print('initial pattern: %d points, %d swaps, %.0f s' % (n0, swaps, time.time() - t0), flush=True)
    initial = field.on.copy()

    # Phase I: rank the initial points by removing the tightest cluster each time.
    for k in range(n0 - 1, -1, -1):
        c = field.tightest_cluster(); field.toggle(*c); rank[c] = k
    print('phase I done, %.0f s' % (time.time() - t0), flush=True)

    # Phase II: back to the initial pattern; fill the largest void until half the pixels are on.
    field.on = initial.copy(); field.rebuild()
    for k in range(n0, total // 2):
        v = field.largest_void(); field.toggle(*v); rank[v] = k
        if (k - n0) % 100000 == 0:
            print('  phase II %d / %d (%.0f s)' % (k, total // 2, time.time() - t0), flush=True)
    print('phase II done, %.0f s' % (time.time() - t0), flush=True)

    # Phase III: invert -- the unranked pixels are now the "on" ones -- and rank by removing their
    # tightest cluster each time.
    field.on = rank < 0; field.rebuild()
    for k in range(total // 2, total):
        c = field.tightest_cluster(); field.toggle(*c); rank[c] = k
        if (k - total // 2) % 100000 == 0:
            print('  phase III %d / %d (%.0f s)' % (k, total, time.time() - t0), flush=True)
    assert (rank >= 0).all() and len(np.unique(rank)) == total
    volume = (rank * 256 // total).astype(np.uint8)
    sxy, sz = spectrum_check(volume)
    print('low-frequency power vs white noise: spatial %.3f, temporal %.3f (white = 1)' % (sxy, sz))
    import os
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    write_dds(a.out, volume)
    print('wrote %s (%dx%dx%d R8), %.0f s' % (a.out, N, N, Z, time.time() - t0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
