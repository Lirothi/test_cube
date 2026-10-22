#!/usr/bin/env python3
"""Memory profiling plan M4: reads a memory trace (traces/memtrace_*.mtrace, written by --memtrace).

    python tools/memtrace.py summary [file]   what is in it: run, events, frames, live by tag at the end,
                                              the biggest live allocations with their stacks
    python tools/memtrace.py verify [file]    replay to the end == what the process itself counted alive
                                              when the trace stopped (the End record); exit 1 if not

Without a file: the newest traces/memtrace_*.mtrace. The format is in sources/core/memory/MemTrace.cpp.
M5 adds UE's query rules (aAf, AaBf, AaBCf, ...) on top of the same replay.
"""

import glob
import os
import struct
import sys
import time

EV_RESET, EV_ALLOC, EV_FREE, EV_MARKER, EV_STACK = 1, 2, 3, 4, 5
HEAP_CPU = 0
KIND_NAMES = {0: "cpu", 1: "buffer", 2: "texture", 3: "descheap", 4: "queryheap"}
SYNTHETIC = 0x80


class Trace:
    def __init__(self):
        self.header = {}
        self.tags = []          # (name, parent)
        self.heaps = {}
        self.modules = []
        self.end = None
        self.symbols = {}       # addr -> (func, file, line)
        self.stacks = {}        # id -> [addr...]
        self.allocs = []        # (serial, key, size, tag, heap, kind, stack, name)
        self.frees = []         # (serial, key, heap)
        self.markers = []       # (serial, qpc, frame or None)
        self.threads = set()
        self.chunks = 0
        self.truncated = False


def str16(data, pos):
    n = struct.unpack_from("<H", data, pos)[0]
    pos += 2
    return data[pos:pos + n].decode("utf-8", "replace"), pos + n


def decode_chunk(tr, data, pos, end, state):
    """Events of one chunk. `state` = [prevSerial, prevKey] of its buffer, carried across chunks."""
    allocs, frees, markers, stacks = tr.allocs, tr.frees, tr.markers, tr.stacks
    prev_serial, prev_key = state
    mask = 0xFFFFFFFFFFFFFFFF

    while pos < end:
        ev = data[pos]
        pos += 1
        if ev == EV_ALLOC or ev == EV_FREE or ev == EV_MARKER:
            # serial delta
            v = 0; s = 0
            while True:
                b = data[pos]; pos += 1
                v |= (b & 0x7F) << s
                if b < 0x80: break
                s += 7
            prev_serial += v
            serial = prev_serial
            if ev == EV_MARKER:
                v = 0; s = 0
                while True:
                    b = data[pos]; pos += 1
                    v |= (b & 0x7F) << s
                    if b < 0x80: break
                    s += 7
                qpc = v
                v = 0; s = 0
                while True:
                    b = data[pos]; pos += 1
                    v |= (b & 0x7F) << s
                    if b < 0x80: break
                    s += 7
                markers.append((serial, qpc, v - 1 if v else None))
                continue
            # key delta (zigzag)
            v = 0; s = 0
            while True:
                b = data[pos]; pos += 1
                v |= (b & 0x7F) << s
                if b < 0x80: break
                s += 7
            prev_key = (prev_key + ((v >> 1) ^ -(v & 1))) & mask
            key = prev_key
            if ev == EV_FREE:
                frees.append((serial, key, data[pos]))
                pos += 1
                continue
            v = 0; s = 0
            while True:
                b = data[pos]; pos += 1
                v |= (b & 0x7F) << s
                if b < 0x80: break
                s += 7
            size = v
            tag, heap, kind = data[pos], data[pos + 1], data[pos + 2]
            pos += 3
            v = 0; s = 0
            while True:
                b = data[pos]; pos += 1
                v |= (b & 0x7F) << s
                if b < 0x80: break
                s += 7
            stack = v
            name = None
            if heap != HEAP_CPU:
                v = 0; s = 0
                while True:
                    b = data[pos]; pos += 1
                    v |= (b & 0x7F) << s
                    if b < 0x80: break
                    s += 7
                name = data[pos:pos + v].decode("utf-8", "replace")
                pos += v
            allocs.append((serial, key, size, tag, heap, kind, stack, name))
        elif ev == EV_RESET:
            v = 0; s = 0
            while True:
                b = data[pos]; pos += 1
                v |= (b & 0x7F) << s
                if b < 0x80: break
                s += 7
            tr.threads.add(v)
            prev_serial, prev_key = 0, 0
        elif ev == EV_STACK:
            v = 0; s = 0
            while True:
                b = data[pos]; pos += 1
                v |= (b & 0x7F) << s
                if b < 0x80: break
                s += 7
            sid = v
            n = data[pos]
            pos += 1
            frames = []
            addr = 0
            for _ in range(n):
                v = 0; s = 0
                while True:
                    b = data[pos]; pos += 1
                    v |= (b & 0x7F) << s
                    if b < 0x80: break
                    s += 7
                addr = (addr + ((v >> 1) ^ -(v & 1))) & mask
                frames.append(addr)
            stacks[sid] = frames
        else:
            raise ValueError(f"unknown event {ev} at byte {pos - 1}")
    state[0], state[1] = prev_serial, prev_key


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    tr = Trace()
    states = {}
    pos, n = 0, len(data)
    while pos + 5 <= n:
        rec = data[pos]
        length = struct.unpack_from("<I", data, pos + 1)[0]
        body = pos + 5
        if body + length > n:
            tr.truncated = True
            break
        t = chr(rec)
        if t == "H":
            if data[body:body + 4] != b"MTRC":
                raise ValueError("not a memory trace")
            (ver, qpc_freq, start_qpc, pid, cpu, gpu, stacks, _pad, sample, start_frame, marker_period,
             tag_count) = struct.unpack_from("<IQQIBBBBIQII", data, body + 4)
            exe, _ = str16(data, body + 4 + struct.calcsize("<IQQIBBBBIQII"))
            tr.header = dict(version=ver, qpc_freq=qpc_freq, start_qpc=start_qpc, pid=pid, cpu=cpu, gpu=gpu,
                             stacks=["off", "big", "all", f"1/{sample}"][stacks], sample=sample, start_frame=start_frame,
                             marker_period=marker_period, tag_count=tag_count, exe=exe)
        elif t == "T":
            count = struct.unpack_from("<I", data, body)[0]
            p = body + 4
            for _ in range(count):
                parent = data[p]
                name, p = str16(data, p + 1)
                tr.tags.append((name, parent))
        elif t == "P":
            count = struct.unpack_from("<I", data, body)[0]
            p = body + 4
            for _ in range(count):
                hid = data[p]
                name, p = str16(data, p + 1)
                tr.heaps[hid] = name
        elif t == "D":
            count = struct.unpack_from("<I", data, body)[0]
            p = body + 4
            mods = []
            for _ in range(count):
                base, size = struct.unpack_from("<QI", data, p)
                guid = data[p + 12:p + 28]
                age = struct.unpack_from("<I", data, p + 28)[0]
                path_, p = str16(data, p + 32)
                mods.append((base, size, guid.hex(), age, path_))
            tr.modules = mods  # the last table wins (the one written at the stop has the late DLLs)
        elif t == "C":
            buf = struct.unpack_from("<I", data, body)[0]
            state = states.setdefault(buf, [0, 0])
            decode_chunk(tr, data, body + 4, body + length, state)
            tr.chunks += 1
        elif t == "E":
            drops, last_serial, stacks, buffers, cpu_on, gpu_on, tag_count = struct.unpack_from("<QQIIBBI", data, body)
            p = body + struct.calcsize("<QQIIBBI")
            rows = []
            for _ in range(tag_count):
                rows.append(struct.unpack_from("<qqQQQII", data, p))
                p += struct.calcsize("<qqQQQII")
            tr.end = dict(drops=drops, last_serial=last_serial, stacks=stacks, buffers=buffers,
                          cpu_on=bool(cpu_on), gpu_on=bool(gpu_on), rows=rows)
        elif t == "Y":
            count = struct.unpack_from("<I", data, body)[0]
            p = body + 4
            for _ in range(count):
                addr = struct.unpack_from("<Q", data, p)[0]
                func, p = str16(data, p + 8)
                file_, p = str16(data, p)
                line = struct.unpack_from("<I", data, p)[0]
                p += 4
                tr.symbols[addr] = (func, file_, line)
        pos = body + length
    if pos < n:
        tr.truncated = True
    return tr


def replay(tr):
    """Every alloc and free in serial order. Returns (live, anomalies): live = {(ns, key): alloc tuple}."""
    events = [(a[0], 1, a) for a in tr.allocs]
    events += [(f[0], 0, f) for f in tr.frees]
    events.sort(key=lambda e: e[0])
    live = {}
    double_alloc = unknown_free = 0
    for _serial, is_alloc, e in events:
        if is_alloc:
            k = (0 if e[4] == HEAP_CPU else 1, e[1])
            if k in live:
                double_alloc += 1
            live[k] = e
        else:
            k = (0 if e[2] == HEAP_CPU else 1, e[1])
            if live.pop(k, None) is None:
                unknown_free += 1
    return live, dict(double_alloc=double_alloc, unknown_free=unknown_free)


def tally(tr, live):
    """Per tag: [cpuBytes, cpuCount, gpuLocal, gpuNonLocal, gpuDescriptor, gpuResources, gpuHeaps]."""
    rows = [[0] * 7 for _ in range(max(len(tr.tags), 1))]
    for (ns, _key), a in live.items():
        _s, _k, size, tag, heap, kind, _st, _n = a
        r = rows[tag]
        if ns == 0:
            r[0] += size
            r[1] += 1
            continue
        k = kind & 0x7F
        if k in (3, 4):
            r[4] += size
            r[6] += 1
        else:
            r[2 if heap == 1 else 3] += size
            r[5] += 1
    return rows


def newest():
    files = glob.glob(os.path.join("traces", "memtrace_*.mtrace"))
    if not files:
        sys.exit("no traces/memtrace_*.mtrace -- run with --memtrace")
    return max(files, key=os.path.getmtime)


def tag_name(tr, t):
    return tr.tags[t][0] if t < len(tr.tags) else f"tag{t}"


def mb(x):
    return x / (1024.0 * 1024.0)


def frame_text(tr, addr):
    sym = tr.symbols.get(addr)
    if not sym or not sym[0]:
        return f"0x{addr:x}"
    func, file_, line = sym
    return f"{func} ({os.path.basename(file_)}:{line})" if file_ else func


def cmd_verify(path):
    t0 = time.perf_counter()
    tr = load(path)
    live, anomalies = replay(tr)
    print(f"memtrace verify: {path} ({len(tr.allocs)} allocs, {len(tr.frees)} frees, read in {time.perf_counter() - t0:.1f} s)")
    if tr.end is None:
        print("memtrace verify: FAIL -- no End record (the process did not stop the trace)")
        return 1
    got = tally(tr, live)
    end = tr.end
    failures = []
    if end["drops"]:
        failures.append(f"{end['drops']} events dropped")
    if tr.truncated:
        failures.append("file truncated")
    if anomalies["double_alloc"] or anomalies["unknown_free"]:
        failures.append(f"{anomalies['double_alloc']} allocs of a live key, {anomalies['unknown_free']} frees of an unknown key")
    cols = ["cpu bytes", "cpu count", "gpu local", "gpu nonlocal", "gpu descriptor", "gpu resources", "gpu heaps"]
    use = ([0, 1] if end["cpu_on"] else []) + ([2, 3, 4, 5, 6] if end["gpu_on"] else [])
    for t, want in enumerate(end["rows"]):
        have = got[t] if t < len(got) else [0] * 7
        for c in use:
            if have[c] != want[c]:
                failures.append(f"{tag_name(tr, t)} {cols[c]}: replay {have[c]} != process {want[c]}")
    cpu_b = sum(r[0] for r in end["rows"])
    gpu_b = sum(r[2] + r[3] for r in end["rows"])
    what = ", ".join(x for x in (f"cpu {mb(cpu_b):.1f} MB" if end["cpu_on"] else "",
                                 f"gpu {mb(gpu_b):.1f} MB" if end["gpu_on"] else "") if x)
    if failures:
        for f in failures[:40]:
            print(f"  {f}")
        print(f"memtrace verify: FAIL ({len(failures)} mismatches)")
        return 1
    print(f"memtrace verify: PASS -- the replay matches the process byte for byte, every tag ({what})")
    return 0


def cmd_summary(path):
    t0 = time.perf_counter()
    tr = load(path)
    live, anomalies = replay(tr)
    h = tr.header
    freq = h.get("qpc_freq", 1) or 1
    frames = [m for m in tr.markers if m[2] is not None]
    span = (max(m[1] for m in tr.markers) - h.get("start_qpc", 0)) / freq if tr.markers else 0.0
    synth = sum(1 for a in tr.allocs if a[5] & SYNTHETIC)
    print(f"{path}: {mb(os.path.getsize(path)):.1f} MB, read in {time.perf_counter() - t0:.1f} s{'  TRUNCATED' if tr.truncated else ''}")
    print(f"  run: pid {h.get('pid')}, {os.path.basename(h.get('exe', ''))}, cpu {'on' if h.get('cpu') else 'off'}, "
          f"gpu {'on' if h.get('gpu') else 'off'}, stacks {h.get('stacks')}, started at frame {h.get('start_frame')}")
    if frames:
        print(f"  frames {frames[0][2]}..{frames[-1][2]} ({len(frames)} markers), {span:.1f} s")
    cpu_a = sum(1 for a in tr.allocs if a[4] == HEAP_CPU)
    print(f"  events: {len(tr.allocs)} allocs ({synth} alive at the start, {cpu_a} cpu), {len(tr.frees)} frees, "
          f"{len(tr.markers)} markers, {len(tr.stacks)} stacks, {len(tr.symbols)} symbols, {len(tr.threads)} threads, {tr.chunks} chunks")
    if tr.end:
        print(f"  end: drops {tr.end['drops']}, buffers {tr.end['buffers']}, anomalies {anomalies}")
    else:
        print("  end: none (the process did not stop the trace)")
    rows = tally(tr, live)
    order = sorted(range(len(rows)), key=lambda t: -(rows[t][0] + rows[t][2] + rows[t][3]))
    print("  alive at the end, by tag:      cpu MB    count   gpu local MB  nonlocal MB  resources")
    for t in order[:20]:
        r = rows[t]
        if r[0] + r[2] + r[3] + r[4] == 0:
            continue
        print(f"    {tag_name(tr, t):<24} {mb(r[0]):9.2f} {r[1]:8d} {mb(r[2]):14.2f} {mb(r[3]):12.2f} {r[5]:10d}")
    for title, pick in (("gpu", lambda a: a[4] != HEAP_CPU), ("cpu", lambda a: a[4] == HEAP_CPU and a[6] != 0)):
        biggest = sorted((a for a in live.values() if pick(a)), key=lambda a: -a[2])[:5]
        if not biggest:
            continue
        print(f"  biggest live {title} allocations{' with a stack' if title == 'cpu' else ''}:")
        for a in biggest:
            label = a[7] or KIND_NAMES.get(a[5] & 0x7F, "?")
            print(f"    {mb(a[2]):8.2f} MB  {tag_name(tr, a[3])}  {label}{'  (alive at start)' if a[5] & SYNTHETIC else ''}")
            for addr in tr.stacks.get(a[6], [])[:6]:
                print(f"        {frame_text(tr, addr)}")
    return 0


def main(argv):
    if len(argv) < 2 or argv[1] not in ("summary", "verify"):
        print(__doc__)
        return 2
    path = argv[2] if len(argv) > 2 else newest()
    return cmd_verify(path) if argv[1] == "verify" else cmd_summary(path)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
