# What fp16 actually does to an EVSM moment pair, numerically.
#
# The Chebyshev bound is  p = var / (var + d^2),  var = E[x^2] - E[x]^2.
# That subtraction is CATASTROPHIC CANCELLATION: on a flat receiver the two terms are equal, so the
# result is pure rounding error. The question is how big that error is next to `minVariance`, which
# is the floor the shader already applies.
import numpy as np

def probe(c, fmt, label):
    d = np.linspace(-1.0, 1.0, 4001)            # warped depth domain
    pos = np.exp(c * d).astype(np.float64)
    m1 = pos.astype(fmt).astype(np.float64)     # stored first moment
    m2 = (pos * pos).astype(fmt).astype(np.float64)  # stored second moment
    var = m2 - m1 * m1                          # should be exactly 0 (single sample)
    # the shader's floor, from csm_sample.hlsli: minVariance = (1e-4 * c * warped)^2
    minvar = (1e-4 * c * pos) ** 2
    worst = np.max(np.abs(var) / np.maximum(minvar, 1e-30))
    top = float(np.max(pos * pos))
    print("%-28s c=%-5.2f  max|pos^2|=%10.3g  worst |var|/minVariance = %.3g"
          % (label, c, top, worst))

print("fp16 max finite value: 65504\n")
probe(42.0,  np.float32, "EVSM4 today   fp32")
probe(5.5,   np.float32, "exponent 5.5  fp32")
probe(5.5,   np.float16, "exponent 5.5  fp16")
probe(4.0,   np.float16, "exponent 4.0  fp16")
probe(3.0,   np.float16, "exponent 3.0  fp16")

print()
print("memory at atlas 2048 (moments + blur scratch):")
for name, bpp in (("RGBA32F  EVSM4 (today)", 16), ("RG32F    EVSM2", 8), ("RGBA16F  EVSM4", 8)):
    mom = 2048 * 2048 * bpp / 1e6
    scr = 1024 * 1024 * bpp / 1e6
    print("  %-24s %6.1f MB + %5.1f MB scratch = %6.1f MB" % (name, mom, scr, mom + scr))
print("  %-24s %6.1f MB" % ("legacy D16 4096", 4096 * 4096 * 2 / 1e6))
