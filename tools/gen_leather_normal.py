"""gen_leather_normal.py [out.png]: a leather grain normal map (textures/materials/leather_normal.png and its .dds), ours.

One seamless 1024^2 tile meant for about 8 cm of leather (a material's texOffsScale sets that: a slot whose unit UV square
is D m wide wants D / 0.08 tiles). The grain is what a pebbled upholstery leather shows: cells of ~2 mm, slightly domed,
parted by narrow creases, a finer grain of a third the size inside them, and a faint long wrinkle. Tangent-space normals in
glTF's convention (OpenGL: green toward the image's TOP, +v), linear, written as an 8-bit PNG and as an R8G8B8A8_UNORM DDS
with its whole mip chain (each mip the average of the four normals under it, renormalised), which the engine reads in
place of the PNG. A material uses it with "normal": "textures/materials/leather_normal.png", "normalIsRG": false.

Deterministic (a fixed seed). Needs numpy and Pillow."""
import os
import struct
import sys

import numpy as np
from PIL import Image

N = 1024
SEED = 20261111
CELLS = 40            # ~2 mm cells on an 8 cm tile
FINE_CELLS = 120      # the grain inside them
TILT_P95_DEG = 32.0   # the 95th percentile of the normals' tilt off the surface


def spectral_noise(rng, beta, lo, hi):
    """A seamless noise: white noise filtered to 1 / f^beta between `lo` and `hi` cycles a tile; zero mean, unit variance."""
    f = np.fft.fftfreq(N) * N
    fx, fy = np.meshgrid(f, f, indexing="ij")
    r = np.sqrt(fx * fx + fy * fy)
    envelope = np.where((r >= lo) & (r <= hi), 1.0 / np.maximum(r, 1.0) ** beta, 0.0)
    field = np.fft.ifft2(np.fft.fft2(rng.standard_normal((N, N))) * envelope).real
    field -= field.mean()
    return field / field.std()


def worley(rng, cells, warp_x, warp_y):
    """F1 and F2 of a seamless jittered Voronoi diagram with `cells` cells a side, in cell sizes, at coordinates warped by
    (warp_x, warp_y) tiles. Axis 0 is x (u), axis 1 is y (v down the image)."""
    points = (np.stack(np.meshgrid(np.arange(cells), np.arange(cells), indexing="ij"), -1) + rng.random((cells, cells, 2))) / cells
    u = (np.arange(N) + 0.5) / N
    px, py = np.meshgrid(u, u, indexing="ij")
    px = (px + warp_x) % 1.0
    py = (py + warp_y) % 1.0
    ci = np.floor(px * cells).astype(np.int64)
    cj = np.floor(py * cells).astype(np.int64)
    f1 = np.full((N, N), 1.0e9)
    f2 = np.full((N, N), 1.0e9)
    for di in (-2, -1, 0, 1, 2):
        for dj in (-2, -1, 0, 1, 2):
            ii = (ci + di) % cells
            jj = (cj + dj) % cells
            qx = points[ii, jj, 0] + np.floor_divide(ci + di, cells)
            qy = points[ii, jj, 1] + np.floor_divide(cj + dj, cells)
            d = np.hypot(px - qx, py - qy)
            nearer = d < f1
            f2 = np.where(nearer, f1, np.minimum(f2, d))
            f1 = np.where(nearer, d, f1)
    return f1 * cells, f2 * cells


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def grain(rng, cells, crease):
    """Domed cells parted by creases `crease` cell sizes wide, 0..1."""
    wx = spectral_noise(rng, 2.0, 2, 12) * (0.35 / cells)
    wy = spectral_noise(rng, 2.0, 2, 12) * (0.35 / cells)
    f1, f2 = worley(rng, cells, wx, wy)
    walls = smoothstep(0.0, crease, f2 - f1)
    dome = 1.0 - np.clip(f1 / 0.75, 0.0, 1.0) ** 2
    return walls * (0.75 + 0.25 * dome)


def height(rng):
    h = grain(rng, CELLS, 0.22)
    h += 0.30 * grain(rng, FINE_CELLS, 0.30)
    h += 0.06 * spectral_noise(rng, 1.0, 60, 300)   # the surface's own tooth
    h += 0.25 * spectral_noise(rng, 2.5, 1, 6)      # a faint long wrinkle
    return h


def normals(h, strength):
    """Tangent-space normals of the height field (axis 0 = u, axis 1 = v down the image), glTF convention: +v up."""
    dhdu = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5
    dhdy = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5   # down the image
    n = np.stack([-dhdu * strength, dhdy * strength, np.ones_like(h)], -1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def encode(n):
    """(x, y, z) in -1..1 -> RGBA8, rows = image rows (v down): the array is (u, v), so transpose."""
    rgb = np.clip(np.rint((n * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)
    a = np.full(rgb.shape[:2] + (1,), 255, np.uint8)
    return np.ascontiguousarray(np.concatenate([rgb, a], -1).transpose(1, 0, 2))


def mip_chain(n):
    chain = [n]
    while chain[-1].shape[0] > 1:
        m = chain[-1]
        m = (m[0::2, 0::2] + m[1::2, 0::2] + m[0::2, 1::2] + m[1::2, 1::2]) * 0.25
        m /= np.maximum(np.linalg.norm(m, axis=-1, keepdims=True), 1.0e-6)
        chain.append(m)
    return chain


def write_dds(path, levels):
    """R8G8B8A8_UNORM (DXGI 28), a DX10 header, the whole mip chain."""
    w = h = levels[0].shape[0]
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000           # caps, height, width, pitch, pixel format, mip count
    caps = 0x8 | 0x1000 | 0x400000                             # complex, texture, mipmap
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, flags, h, w, w * 4, 0, len(levels))
    pixel_format = struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    header += pixel_format + struct.pack("<IIIII", caps, 0, 0, 0, 0)
    dx10 = struct.pack("<IIIII", 28, 3, 0, 1, 0)
    with open(path, "wb") as f:
        f.write(header + dx10)
        for level in levels:
            f.write(encode(level).tobytes())


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join("textures", "materials", "leather_normal.png")
    rng = np.random.default_rng(SEED)
    h = height(rng)
    # The strength that puts the 95th percentile of the tilt at TILT_P95_DEG.
    g = np.hypot((np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5, (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5)
    strength = np.tan(np.radians(TILT_P95_DEG)) / max(np.percentile(g, 95), 1.0e-9)
    n = normals(h, strength)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    Image.fromarray(encode(n)[..., :3]).save(out)
    dds = os.path.splitext(out)[0] + ".dds"
    write_dds(dds, mip_chain(n))
    tilt = np.degrees(np.arccos(np.clip(n[..., 2], -1, 1)))
    print(f"{out} + {dds}: strength {strength:.1f}, tilt median {np.median(tilt):.1f} p95 {np.percentile(tilt, 95):.1f} max {tilt.max():.1f} deg")


if __name__ == "__main__":
    main()
