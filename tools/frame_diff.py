#!/usr/bin/env python3
"""Architecture plan R0.3: how different two frames are -- the share of pixels more than 8 levels apart.

A pixel differs when any of its R, G, B channels differs by more than the threshold (8 of 255 by default; alpha is
ignored). The noise floor of two runs of one binary on the plan's views is 0.05-0.4 % (section 11.3; the bathroom with
its candles is higher), so measure the floor first, then before / after.

    python tools/frame_diff.py a.png b.png                      # prints the share in percent
    python tools/frame_diff.py a.png b.png --mask m.png         # only where the mask is not black
    python tools/frame_diff.py a.png b.png --rect x0,y0,x1,y1   # only inside the rectangle (x1, y1 exclusive)
    python tools/frame_diff.py a.png b.png --threshold 4 --out diff.png --max 0.4

--out writes the differing pixels white on black; --max makes the exit code 1 above that percent (a gate). Two frames
of different sizes, or a mask of another size, are an error (exit code 2).
"""

import argparse
import sys

import numpy as np
from PIL import Image


def load_rgb(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def differing(a, b, threshold):
    """Boolean map of the pixels whose largest channel difference exceeds the threshold."""
    return np.abs(a - b).max(axis=2) > threshold


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--threshold", type=int, default=8, help="levels of 255 a channel may differ (default 8)")
    parser.add_argument("--mask", help="PNG of the frame's size: compare only where it is not black")
    parser.add_argument("--rect", help="x0,y0,x1,y1: compare only inside it")
    parser.add_argument("--out", help="write the differing pixels as a white-on-black PNG")
    parser.add_argument("--max", type=float, help="exit code 1 when the share in percent is above it")
    args = parser.parse_args(argv)

    a = load_rgb(args.a)
    b = load_rgb(args.b)
    if a.shape != b.shape:
        print(f"frame_diff: sizes differ: {args.a} {a.shape[1]}x{a.shape[0]}, {args.b} {b.shape[1]}x{b.shape[0]}")
        return 2
    region = np.ones(a.shape[:2], dtype=bool)
    if args.mask:
        mask = np.asarray(Image.open(args.mask).convert("L"))
        if mask.shape != region.shape:
            print(f"frame_diff: the mask is {mask.shape[1]}x{mask.shape[0]}, the frames {a.shape[1]}x{a.shape[0]}")
            return 2
        region &= mask > 0
    if args.rect:
        x0, y0, x1, y1 = (int(v) for v in args.rect.split(","))
        inside = np.zeros_like(region)
        inside[max(0, y0):max(0, y1), max(0, x0):max(0, x1)] = True
        region &= inside
    compared = int(region.sum())
    if compared == 0:
        print("frame_diff: the mask / rectangle leaves no pixel to compare")
        return 2
    diff = differing(a, b, args.threshold) & region
    count = int(diff.sum())
    share = 100.0 * count / compared
    print(f"frame_diff: {share:.4f} % ({count} of {compared} pixels differ by more than {args.threshold} levels)")
    if args.out:
        Image.fromarray((diff * 255).astype(np.uint8), mode="L").save(args.out)
    if args.max is not None and share > args.max:
        print(f"frame_diff: above --max {args.max} %")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
