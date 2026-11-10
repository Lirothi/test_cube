"""gen_wake_foam.py [out.png] [--preview dir]: the boat wake's foam texture (textures/ocean/WakeFoam.png), ours.

One seamless 1024^2 RGBA8 image, linear data, read by ocean_surface_surf_sim.hlsli WakeFoamCoverage over wakeFoamTile (3) m of water:

  R  the LACE: where the foam goes first as it thins. A network of filaments at three scales (cells of ~0.8, 0.35 and
     0.17 m) with rafts of small bubbles between them, as a rank: a pixel is foam while the wake's foam value (the share
     of the water it covers) is over its R -- so the texture's values are equalised to a flat histogram (a value v covers
     the share v, exactly), and then stored GAUSSIANISED, 0.5 + z / 6: the shader blends three windows of it (hex tiling,
     no repeat) keeping the variance and maps the blend back through the normal CDF, which leaves the histogram flat in
     the blends too (Heitz & Neyret 2018, "High-Performance By-Example Noise using a Histogram-Preserving Blending
     Operator").
  G  the churned white water's HEIGHT, 0..1: boils (a fifth to a twelfth of the tile) with lumps and creases on them. Read
     at its own scale (wakeChurnSize, 4 m a tile), not the lace's.
  B  its slope along u, A along v: 0.5 + slope / (2 SLOPE_MAX), slope in heights per texture width.

Deterministic (a fixed seed). Needs numpy and Pillow."""
import os
import sys

import numpy as np
from PIL import Image

N = 1024
SEED = 20261110
SLOPE_MAX = 160.0  # heights per texture width at the encoding's ends -- MUST MATCH kWakeFoamSlopeMax in the shader


def spectral_noise(rng, beta, lo, hi):
    """A seamless noise: white noise filtered to 1 / f^beta between `lo` and `hi` cycles a tile; zero mean, unit variance."""
    f = np.fft.fftfreq(N) * N
    fx, fy = np.meshgrid(f, f)
    r = np.sqrt(fx * fx + fy * fy)
    envelope = np.where((r >= lo) & (r <= hi), 1.0 / np.maximum(r, 1.0) ** beta, 0.0)
    field = np.fft.ifft2(np.fft.fft2(rng.standard_normal((N, N))) * envelope).real
    field -= field.mean()
    return field / field.std()


def worley_edges(rng, cells, warp_x, warp_y):
    """Distance to the nearest cell wall of a seamless jittered Voronoi diagram with `cells` cells a side, in cell
    sizes (F2 - F1), at coordinates warped by (warp_x, warp_y) tiles."""
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
    return (f2 - f1) * cells


def rank_uniform(field):
    """The field's ranks, 0..1: a flat histogram."""
    order = np.argsort(field, axis=None, kind="stable")
    ranks = np.empty(field.size)
    ranks[order] = (np.arange(field.size) + 0.5) / field.size
    return ranks.reshape(field.shape)


def inverse_normal_cdf(p):
    """Acklam's rational approximation (relative error under 1.2e-9)."""
    a = [-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00]
    b = [-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01]
    c = [-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00]
    d = [7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00]
    p = np.clip(p, 1.0e-6, 1.0 - 1.0e-6)
    out = np.empty_like(p)
    low = p < 0.02425
    high = p > 1.0 - 0.02425
    mid = ~(low | high)
    q = np.sqrt(-2.0 * np.log(p[low]))
    out[low] = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0)
    q = np.sqrt(-2.0 * np.log(1.0 - p[high]))
    out[high] = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0)
    q = p[mid] - 0.5
    r = q * q
    out[mid] = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0)
    return out


def build():
    rng = np.random.default_rng(SEED)
    # The warp: cells drawn out and bent, no two alike (a tenth of a tile at the large scale, finer wobble on top).
    warp_x = 0.045 * spectral_noise(rng, 1.6, 1, 6) + 0.006 * spectral_noise(rng, 1.2, 6, 40)
    warp_y = 0.045 * spectral_noise(rng, 1.6, 1, 6) + 0.006 * spectral_noise(rng, 1.2, 6, 40)

    # Filaments at three scales. `wall` is how wide a filament is in cell sizes; the strength of a scale says how long
    # its filaments outlast the thinning (the large network is the last lace left).
    foaminess = np.zeros((N, N))
    for cells, wall, strength in ((5, 0.16, 1.0), (11, 0.20, 0.78), (24, 0.26, 0.56)):
        e = worley_edges(rng, cells, warp_x, warp_y)
        # Filaments are not of one width: thick where three meet, thin and broken along a wall.
        width = wall * (0.55 + 0.9 * np.clip(0.5 + 0.35 * spectral_noise(rng, 1.0, cells * 0.5, cells * 3.0), 0.0, 1.0))
        foaminess = np.maximum(foaminess, strength * np.clip(1.0 - e / width, 0.0, 1.0) ** 0.8)
    # Rafts: patches where the bubbles between the filaments linger (and patches of clear water where they do not).
    rafts = np.clip(0.5 + 0.30 * spectral_noise(rng, 1.8, 2, 14), 0.0, 1.0)
    bubbles = np.clip(0.5 + 0.28 * spectral_noise(rng, 0.6, 30, 220), 0.0, 1.0)
    foaminess = np.maximum(foaminess, 0.62 * rafts * (0.35 + 0.65 * bubbles))
    # A grain of the finest bubbles so the thinning edge is frayed, not a clean contour.
    foaminess += 0.05 * spectral_noise(rng, 0.3, 100, 400)
    lace = rank_uniform(-foaminess)  # 0 on the stoutest filaments, 1 in the clearest water
    lace_gauss = np.clip(0.5 + inverse_normal_cdf(lace) / 6.0, 0.0, 1.0)

    # The churn: billows -- |noise| folded into creases, their bottoms rounded -- at two scales, the finer one riding
    # the coarser one's lumps.
    def billow(noise, soft):
        return np.sqrt(np.clip(noise * 0.55, -1.0, 1.0) ** 2 + soft * soft) - soft

    # Three scales: the boils (a fifth to a twelfth of the tile: 0.3-0.8 m at the relief's 4 m), the lumps on them and a
    # little grain. Mostly the boils: with the small scales alone the white water read as plaster.
    boils = billow(spectral_noise(rng, 1.8, 5, 12), 0.10)
    coarse = billow(spectral_noise(rng, 1.5, 14, 50), 0.08)
    fine = billow(spectral_noise(rng, 1.2, 50, 160), 0.12)
    height = 0.60 * boils + (0.30 * coarse + 0.10 * fine * (0.4 + 0.6 * coarse)) * (0.5 + 0.5 * boils / boils.max())
    height = (height - height.min()) / (height.max() - height.min())
    # Slopes in heights per texture width (central differences round the seam): axis 0 is u here.
    slope_u = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * (N * 0.5)
    slope_v = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * (N * 0.5)
    enc_u = np.clip(0.5 + slope_u / (2.0 * SLOPE_MAX), 0.0, 1.0)
    enc_v = np.clip(0.5 + slope_v / (2.0 * SLOPE_MAX), 0.0, 1.0)
    clipped = float(np.mean((np.abs(slope_u) > SLOPE_MAX) | (np.abs(slope_v) > SLOPE_MAX)))

    # Image rows are v, columns u: transpose from [u, v].
    rgba = np.stack([lace_gauss.T, height.T, enc_u.T, enc_v.T], -1)
    return (np.clip(rgba, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8), lace, height, clipped


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "textures", "ocean", "WakeFoam.png")
    rgba, lace, height, clipped = build()
    Image.fromarray(rgba, "RGBA").save(out)
    print(f"wrote {os.path.normpath(out)}: lace mean {lace.mean():.3f}, height mean {height.mean():.3f}, slopes clipped {clipped * 100.0:.2f} %")
    if "--preview" in sys.argv:
        folder = sys.argv[sys.argv.index("--preview") + 1]
        # What the water shows at five foam values (white = foam), tiled 2 x 2 so the seam is in view, and the height.
        for value in (0.15, 0.3, 0.5, 0.7, 0.9):
            cover = np.clip((value - lace) / 0.16 + 0.5, 0.0, 1.0).T
            Image.fromarray((np.tile(cover, (2, 2)) * 255.0).astype(np.uint8), "L").resize((1024, 1024), Image.LANCZOS).save(
                os.path.join(folder, f"wakefoam_cover_{int(value * 100):02d}.png"))
        Image.fromarray((height.T * 255.0).astype(np.uint8), "L").save(os.path.join(folder, "wakefoam_height.png"))


if __name__ == "__main__":
    main()
