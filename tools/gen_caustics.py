"""Generate a seamless caustics flipbook atlas (BC4 DDS with per-frame wrapped mips).

Sunlight refracting through the surface lands at p + A*grad(H); the DENSITY of landing points is
the caustic. That density is computed by splatting a dense deterministic grid of rays, not from
1/|det(Jacobian)|: the closed-form determinant only describes a single sheet of the refracted map,
so it draws isolated fold arcs. The real pool/lagoon pattern -- a tessellation of dark cells walled
by bright cords meeting at cusp nodes -- only appears once several rays land on the same spot, and
that needs the actual sum over branches.

Two things set the look:
  * a RING spectrum (all wavelengths similar, directions spread evenly) gives cells of even size;
    a broadband spectrum gives squiggly multi-scale worms instead.
  * DEFLECT just past the first fold. Push it further and the sheets pile up into a chaotic
    interference wash, which is a deep-water look, not a sunlit shallow bottom.

The height field tiles exactly (integer wave vectors) and loops exactly (integer temporal
frequencies), so the atlas is seamless in space and in time.
"""
import numpy as np, struct, sys, os
from scipy import ndimage

FRAME    = 256     # texels per frame
GRID     = 4       # frames per atlas row/column -> GRID*GRID frames
SUPER    = 14      # rays per texel per axis (density noise ~ 1/SUPER)
WAVES    = 48      # directions around the ring
KRING    = 16.0    # ring radius in cycles per tile ~= cells across one tile
KJITTER  = 6.0     # ring width. A narrow ring makes every cell the same size and the tile reads
                   # as a lattice; this is the main knob against that.
DEFLECT  = 0.0016  # ray deflection; just past the first fold. Scales like 1/KRING^2.
GUST     = 0.45    # 0..1 low-frequency modulation of the ripple strength: calm patches grow big
                   # faint cells, choppy patches dense ones, which is what real water does.
# Long waves under the ripples. They barely curve the surface, but their large gradient drags the
# whole cell pattern around, which is what breaks the lattice. Both these and the gust field live at
# |k| 1..3, where the INTEGER lattice has almost no angular resolution -- any chosen direction
# rounds onto one of a handful of axes, and a few low-k waves carrying most of the amplitude then
# print a diagonal across the whole atlas. So don't sample directions at all down here: enumerate
# every lattice vector in the annulus and use them all. Isotropy by construction.
SWELL_KMIN = 1
SWELL_KMAX = 3
SWELL_AMP = 1.4    # per-wave, relative to the mean ripple amplitude
GUST_KMIN = 1
GUST_KMAX = 3
SUNBLUR  = 1.2     # texels. The sun is not a point (~0.5 deg), and that is what gives the cords
                   # their width instead of leaving them one texel wide and aliasing.
# Cycles per loop. With only GRID*GRID frames the fastest wave must still be sampled well above
# Nyquist. A caustic net reshapes far faster than the wave phase it comes from, so this is
# well under what dispersion would ask for: every wave runs at 1 cycle per loop = 16 frames.
FREQ_SCALE = 0.25
SEED     = 5


def build_waves():
    """Integer wave vectors on a ring, walked evenly in angle so no lattice direction dominates,
    plus a few long swell waves underneath."""
    rng = np.random.default_rng(SEED)
    ks, phases, freqs, amps = [], [], [], []
    seen = set()

    def add(k, amp):
        if (k[0] == 0 and k[1] == 0) or tuple(k) in seen:
            return False
        seen.add(tuple(k))
        ks.append(k)
        amps.append(amp)
        phases.append(rng.random()*2.0*np.pi)
        # deep-water dispersion omega ~ sqrt(k), rounded to an integer number of loop cycles
        freqs.append(max(1, int(round(FREQ_SCALE*np.sqrt(np.hypot(*k))))))
        return True

    for i in range(WAVES):
        angle = (i + rng.random()*0.5) * 2.0*np.pi / WAVES
        mag = KRING + KJITTER*(rng.random()*2.0 - 1.0)
        k = np.round([mag*np.cos(angle), mag*np.sin(angle)]).astype(int)
        add(k, 1.0/max(np.hypot(*k), 1.0))
    amps = list(np.array(amps)/np.sum(amps))
    ripple = np.mean(amps)
    for k in lattice_band(SWELL_KMIN, SWELL_KMAX):
        add(k, ripple*SWELL_AMP/max(np.hypot(*k), 1.0))
    amps = np.array(amps)
    mags = np.hypot(ks_arr := np.array(ks, float)[:, 0], np.array(ks, float)[:, 1])
    print('  swell carries %.0f%% of the amplitude' %
          (100.0*amps[mags < 5.0].sum()/amps.sum()))
    return (np.array(ks, float), amps, np.array(phases), np.array(freqs, float))


def lattice_band(kmin, kmax):
    """Every integer wave vector with kmin <= |k| <= kmax, one per +/- pair (k and -k are the same
    wave up to phase). Using the whole annulus is the only way to be isotropic this close to the
    origin, where rounding a direction to the lattice is hopelessly coarse."""
    out = []
    for y in range(-kmax, kmax + 1):
        for x in range(-kmax, kmax + 1):
            if x < 0 or (x == 0 and y <= 0):        # keep one of each +/- pair
                continue
            if kmin <= np.hypot(x, y) <= kmax:
                out.append(np.array([x, y]))
    return out


def gust_waves():
    rng = np.random.default_rng(SEED + 977)
    return [(k, rng.random()*2.0*np.pi) for k in lattice_band(GUST_KMIN, GUST_KMAX)]


def gust_field(waves, t, n):
    """Smooth periodic 0..1 field scaling the local ripple strength. It ANIMATES: held static it
    bakes the same bright and dark patches into every frame, and a stationary macro feature in a
    scrolling texture is exactly what the eye picks out as 'the atlas'."""
    u = (np.arange(n) + 0.5)/n
    field = np.zeros((n, n))
    for k, p in waves:
        a = 2.0*np.pi*k[0]*u
        b = 2.0*np.pi*(k[1]*u + t) + p          # one cycle per loop, so the patches drift
        field += np.cos(a)[None, :]*np.cos(b)[:, None] - np.sin(a)[None, :]*np.sin(b)[:, None]
    lo = field.min()
    return (field - lo)/max(field.max() - lo, 1e-6)


def surface_gradient(ks, amps, phases, freqs, t, n):
    """grad H on an n x n periodic grid. sin(a+b) separates into outer products, which turns an
    n^2-per-wave transcendental evaluation into n."""
    u = (np.arange(n) + 0.5)/n
    gx = np.zeros((n, n)); gy = np.zeros((n, n))
    for i in range(len(amps)):
        a = 2.0*np.pi*ks[i, 0]*u                          # varies along x
        b = 2.0*np.pi*(ks[i, 1]*u + freqs[i]*t) + phases[i]  # varies along y
        cosphase = (np.cos(a)[None, :]*np.cos(b)[:, None] -
                    np.sin(a)[None, :]*np.sin(b)[:, None])
        gx += amps[i]*2.0*np.pi*ks[i, 0]*cosphase
        gy += amps[i]*2.0*np.pi*ks[i, 1]*cosphase
    return gx, gy


def build_frames():
    waves = build_waves()
    ks = waves[0]
    print('  %d waves, |k| %.1f..%.1f' % (
        len(ks), np.hypot(ks[:,0], ks[:,1]).min(), np.hypot(ks[:,0], ks[:,1]).max()))

    nframes = GRID*GRID
    n = FRAME*SUPER
    u = (np.arange(n) + 0.5)/n
    U, V = np.meshgrid(u, u)
    out = np.zeros((nframes, FRAME, FRAME), np.float32)
    gusts = gust_waves() if GUST > 0.0 else None

    for f in range(nframes):
        t = f/nframes
        gx, gy = surface_gradient(*waves, t, n)
        deflect = (DEFLECT*(1.0 - GUST + 2.0*GUST*gust_field(gusts, t, n))
                   if GUST > 0.0 else DEFLECT)
        px = (U + deflect*gx) % 1.0
        py = (V + deflect*gy) % 1.0

        fx = px*FRAME - 0.5; fy = py*FRAME - 0.5
        x0 = np.floor(fx).astype(np.int64); y0 = np.floor(fy).astype(np.int64)
        tx = (fx - x0).ravel(); ty = (fy - y0).ravel()
        x0 = x0.ravel() % FRAME; y0 = y0.ravel() % FRAME
        x1 = (x0 + 1) % FRAME;   y1 = (y0 + 1) % FRAME
        acc = np.zeros(FRAME*FRAME)
        for xs, ys, w in ((x0, y0, (1-tx)*(1-ty)), (x1, y0, tx*(1-ty)),
                          (x0, y1, (1-tx)*ty),     (x1, y1, tx*ty)):
            acc += np.bincount(ys*FRAME + xs, weights=w, minlength=FRAME*FRAME)
        density = acc.reshape(FRAME, FRAME)/(SUPER*SUPER)   # 1.0 = unfocused sunlight
        if SUNBLUR > 0.0:
            density = ndimage.gaussian_filter(density, SUNBLUR, mode='wrap')
        out[f] = density.astype(np.float32)
        sys.stdout.write('\r  frame %d/%d' % (f+1, nframes)); sys.stdout.flush()
    print()
    return out


def tone(frames):
    """Scale so a bright cord reaches 1.0. Returns the encoded value of UNFOCUSED light, which is
    the value the shader's causticsBias should sit at for the dark cells to read as neutral."""
    peak = np.percentile(frames, 99.7)
    return np.clip(frames/peak, 0.0, 1.0), 1.0/peak


# ---------- per-frame wrapped mip chain (never filters across frame borders) ----------
def frame_mips(frames):
    levels = [frames]
    cur = frames
    while cur.shape[-1] > 1:
        n = cur.shape[-1]//2
        cur = cur.reshape(cur.shape[0], n, 2, n, 2).mean((2, 4))
        levels.append(cur)
    return levels


def assemble(level):
    n, s, _ = level.shape
    atlas = np.zeros((GRID*s, GRID*s), np.float32)
    for i in range(n):
        r, c = divmod(i, GRID)
        atlas[r*s:(r+1)*s, c*s:(c+1)*s] = level[i]
    return atlas


# ---------- BC4 (single channel, 8 bytes / 4x4 block) ----------
def bc4_encode(img):
    h, w = img.shape
    q = np.clip(np.rint(img*255), 0, 255).astype(np.uint8)
    if h < 4 or w < 4:
        pad = np.zeros((4, 4), np.uint8); pad[:h, :w] = q
        q = pad; h = w = 4
    bh, bw = h//4, w//4
    blocks = q.reshape(bh, 4, bw, 4).transpose(0, 2, 1, 3).reshape(-1, 16)
    r0 = blocks.max(1).astype(np.int32)
    r1 = blocks.min(1).astype(np.int32)
    lut = np.stack([r0, r1] + [((7-i)*r0 + i*r1)//7 for i in range(1, 7)], 1)
    idx = np.abs(blocks[:, None, :].astype(np.int32) - lut[:, :, None]).argmin(1).astype(np.uint64)
    out = bytearray()
    for b in range(blocks.shape[0]):
        bits = 0
        for t in range(16):
            bits |= int(idx[b, t]) << (3*t)
        out += struct.pack('<BB', int(r0[b]), int(r1[b])) + bits.to_bytes(6, 'little')
    return bytes(out)


def write_dds(path, mips):
    w = h = mips[0].shape[0]
    hdr = bytearray(b'DDS ')
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000
    pitch = max(1, w//4)*max(1, h//4)*8
    hdr += struct.pack('<7I', 124, flags, h, w, pitch, 0, len(mips))
    hdr += b'\x00'*44
    hdr += struct.pack('<2I', 32, 0x4) + b'DX10' + struct.pack('<5I', 0, 0, 0, 0, 0)
    hdr += struct.pack('<5I', 0x1000 | 0x400008, 0, 0, 0, 0)
    hdr += struct.pack('<5I', 80, 3, 0, 1, 0)               # BC4_UNORM, TEXTURE2D, arraySize 1
    with open(path, 'wb') as fh:
        fh.write(bytes(hdr))
        for m in mips:
            fh.write(bc4_encode(m))


if __name__ == '__main__':
    out_path = sys.argv[1] if len(sys.argv) > 1 else 'caustics_flipbook.dds'
    print('generating %d frames of %dx%d ...' % (GRID*GRID, FRAME, FRAME))
    frames, neutral = tone(build_frames())
    print('  value stats: mean %.3f  p50 %.3f  p99 %.3f' %
          (frames.mean(), np.percentile(frames, 50), np.percentile(frames, 99)))
    print('  unfocused light encodes to %.3f  <-- set causticsBias to this' % neutral)
    atlases = [assemble(l) for l in frame_mips(frames)]
    print('  mip chain: ' + ', '.join('%dx%d' % (a.shape[1], a.shape[0]) for a in atlases))
    write_dds(out_path, atlases)
    print('  wrote %s (%.0f KB)' % (out_path, os.path.getsize(out_path)/1024))
