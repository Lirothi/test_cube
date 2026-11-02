"""Where a thread's frame goes: per CPU scope on one thread of a profiler trace (traces/trace_*.json, Chrome format),
the mean time per frame and the scope it sits in, averaged over EVERY frame of the capture.

    python tools/trace_thread_breakdown.py traces/trace_<...>.json [--thread MainThread] [--min-ms 0.02] [--depth 3]

Frames are the thread's 'Frame N' markers. The last line sums the worker threads' spans (nested spans counted
twice) -- how busy the pool is, not a critical path. Written for docs/game_thread_plan.md (section 1.2)."""
import argparse
import collections
import json
import re


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--thread', default='MainThread', help="thread_name of the track to break down")
    ap.add_argument('--min-ms', type=float, default=0.02, help="hide scopes cheaper than this per frame")
    ap.add_argument('--depth', type=int, default=3, help="deepest nesting level printed (0 = roots)")
    args = ap.parse_args()

    with open(args.trace, 'r', encoding='utf-8') as f:
        data = json.load(f)
    events = data['traceEvents'] if isinstance(data, dict) else data

    names = {e['tid']: e['args']['name'] for e in events
             if e.get('ph') == 'M' and e.get('name') == 'thread_name'}
    tids = [t for t, n in names.items() if n == args.thread]
    if not tids:
        raise SystemExit(f"no thread named {args.thread!r}; threads: {sorted(set(names.values()))}")
    tid = tids[0]

    spans = [e for e in events if e.get('ph') == 'X']
    is_marker = lambda e: re.match(r'Frame \d+', e['name']) is not None
    frames = sum(1 for e in spans if e['tid'] == tid and is_marker(e))
    track = sorted((e for e in spans if e['tid'] == tid and not is_marker(e)), key=lambda e: (e['ts'], -e['dur']))
    if not track or frames == 0:
        raise SystemExit("the thread has no scopes or no frame markers")

    # Nesting by a stack of open scopes (end time, name); a child starts before its parent ends.
    totals = collections.defaultdict(float)
    counts = collections.Counter()
    stack = []
    for e in track:
        while stack and e['ts'] >= stack[-1][0] - 1e-3:
            stack.pop()
        key = (len(stack), stack[-1][1] if stack else '-', e['name'])
        totals[key] += e['dur']
        counts[key] += 1
        stack.append((e['ts'] + e['dur'], e['name']))

    t0 = min(e['ts'] for e in track)
    t1 = max(e['ts'] + e['dur'] for e in track)
    print(f"{args.thread}: {frames} frames, span {(t1 - t0) / 1000:.1f} ms, period {(t1 - t0) / 1000 / frames:.3f} ms")
    for (depth, parent, name), us in sorted(totals.items(), key=lambda kv: (kv[0][0], -kv[1])):
        per_frame = us / frames / 1000.0
        if per_frame >= args.min_ms and depth <= args.depth:
            print(f"{'  ' * depth}{name:48s} {per_frame:7.3f} ms/frame  x{counts[(depth, parent, name)] / frames:.1f}"
                  f"   (in {parent})")

    busy = collections.defaultdict(float)
    for e in spans:
        if e['tid'] != tid and names.get(e['tid'], '').startswith('Thread'):
            busy[e['tid']] += e['dur']
    print(f"worker threads: {len(busy)}, sum of spans {sum(busy.values()) / frames / 1000.0:.3f} ms/frame")


if __name__ == '__main__':
    main()
