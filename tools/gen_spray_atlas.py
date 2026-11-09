r"""Generates the spray's sprite atlases (white RGB, coverage in alpha), our own: no licence to track.
- spray_drops.png: 4x4 cells of 256 px, each a clump of drops and torn filaments elongated along the cell's vertical
  (the quad's long axis follows the velocity, particles.hlsl), dense in the middle and breaking up toward the ends.
- spray_mist.png: 2x2 cells of 256 px, soft fbm puffs with ragged edges.
python tools/gen_spray_atlas.py textures/particles  (seeded: the same files every run)"""
import os
import sys

import numpy as np
from PIL import Image

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rng = np.random.default_rng(20261108)
C = 256
yy, xx = np.mgrid[0:C, 0:C].astype(np.float32)


def blob(img, cx, cy, rx, ry, a):
    # A soft drop: an anisotropic gaussian, its peak `a`, combined by "over" (1 - (1-a)(1-b)).
    x0, x1 = int(max(cx - 3 * rx, 0)), int(min(cx + 3 * rx + 1, C))
    y0, y1 = int(max(cy - 3 * ry, 0)), int(min(cy + 3 * ry + 1, C))
    if x0 >= x1 or y0 >= y1:
        return
    gx = (xx[y0:y1, x0:x1] - cx) / rx
    gy = (yy[y0:y1, x0:x1] - cy) / ry
    g = a * np.exp(-0.5 * (gx * gx + gy * gy))
    img[y0:y1, x0:x1] = 1.0 - (1.0 - img[y0:y1, x0:x1]) * (1.0 - g)


def drops_cell():
    img = np.zeros((C, C), np.float32)
    # A torn sheet: a few filaments along the vertical, each a chain of elongated blobs that thins and breaks.
    for _ in range(rng.integers(3, 7)):
        x = rng.normal(C * 0.5, C * 0.12)
        y = rng.uniform(C * 0.15, C * 0.4)
        length = rng.uniform(C * 0.35, C * 0.7)
        steps = int(length / 4)
        width = rng.uniform(2.0, 5.0)
        drift = rng.normal(0.0, 0.15)
        for s in range(steps):
            t = s / max(steps - 1, 1)
            if rng.random() < 0.15 + 0.5 * t:  # it tears more toward its end
                continue
            blob(img, x + drift * s * 4 + rng.normal(0, 0.8), y + s * 4, width * (1.0 - 0.6 * t), 4.5, 0.55 * (1.0 - 0.5 * t))
    # Drops round it: more near the middle, smaller farther out.
    for _ in range(rng.integers(40, 90)):
        cx = rng.normal(C * 0.5, C * 0.16)
        cy = rng.normal(C * 0.5, C * 0.22)
        r = rng.uniform(1.2, 4.5)
        blob(img, cx, cy, r, r * rng.uniform(1.0, 2.2), rng.uniform(0.5, 0.95))
    # The cell's edge fades to nothing (no square cut by the quad).
    edge = np.clip(np.minimum(np.minimum(xx, C - 1 - xx), np.minimum(yy, C - 1 - yy)) / 14.0, 0.0, 1.0)
    return np.clip(img * edge, 0.0, 1.0)


def fbm(scale, octaves=5):
    acc = np.zeros((C, C), np.float32)
    amp = 1.0
    total = 0.0
    for o in range(octaves):
        n = C // scale * (2 ** o)
        n = max(n, 2)
        grid = rng.random((n + 1, n + 1)).astype(np.float32)
        img = Image.fromarray((grid * 255).astype(np.uint8)).resize((C, C), Image.BICUBIC)
        acc += amp * (np.asarray(img, np.float32) / 255.0)
        total += amp
        amp *= 0.5
    return acc / total


def mist_cell():
    r = np.sqrt((xx - C * 0.5) ** 2 + (yy - C * 0.5) ** 2) / (C * 0.5)
    noise = fbm(64)
    shape = np.clip(1.0 - r * (0.8 + 0.6 * noise), 0.0, 1.0)
    a = shape ** 1.5 * (0.55 + 0.45 * fbm(32))
    return np.clip(a, 0.0, 1.0)


def atlas(cells, cols, rows, name):
    a = np.zeros((rows * C, cols * C), np.float32)
    for i, cell in enumerate(cells):
        cx, cy = i % cols, i // cols
        a[cy * C:(cy + 1) * C, cx * C:(cx + 1) * C] = cell
    rgba = np.zeros((rows * C, cols * C, 4), np.uint8)
    rgba[..., 0:3] = 255
    rgba[..., 3] = (a * 255.0 + 0.5).astype(np.uint8)
    Image.fromarray(rgba, "RGBA").save(os.path.join(out, name))
    print(name, rgba.shape, "mean alpha %.3f" % (a.mean()))


atlas([drops_cell() for _ in range(16)], 4, 4, "spray_drops.png")
atlas([mist_cell() for _ in range(4)], 2, 2, "spray_mist.png")
