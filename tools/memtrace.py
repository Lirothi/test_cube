#!/usr/bin/env python3
"""Memory profiling plan M4 + M5: reads a memory trace (traces/memtrace_*.mtrace, written by --memtrace).

    python tools/memtrace.py summary [file]             the run, events, frames, live by tag at the end,
                                                        the biggest live allocations with their stacks
    python tools/memtrace.py verify [file]              replay to the end == what the process counted alive
                                                        when the trace stopped (the End record); exit 1 if not
    python tools/memtrace.py query RULE A [B [C [D]]]   UE Memory Insights' allocation query (see RULES)
    python tools/memtrace.py leaks A B C                = query AaBCf A B C --by stack
    python tools/memtrace.py growth A B                 = query AaBf A B --by stack
    python tools/memtrace.py churn A B                  = query AafB A B --by stack --sort count
    python tools/memtrace.py decline A B                = query aAfB A B --by stack
    python tools/memtrace.py live A                     = query aAf A --by tag
    python tools/memtrace.py tags A B                   per tag: size at A, at B, B - A, min/max/avg over the frames
    python tools/memtrace.py timeline [--out x.json]    per-tag MB per frame as Chrome-trace counters (Perfetto)
    python tools/memtrace.py selftest                   the rules and the replay on a synthetic trace
    python tools/memtrace.py view [file] [--port N] [--no-open]
                                                        the browser viewer (tools/memtrace_viewer.html) on the
                                                        trace: timeline, markers, the same rules and groupings

Time markers A..D: a frame (`600`, `f600` -- the end of that frame), seconds since the trace started
(`12.5s`), `start` or `end`. The rules are UE's (TraceServices AllocationsProvider.h:25-52,
AllocationsQuery.cpp:196-397), every bound inclusive, `f` = infinity for what is never freed:
    aAf live at A          AaBf growth            aAfB decline         AafB short-lived (churn)
    aABf long-living       AaBCf leaks            AaBfC limited life   aABfC decline of long-living
    AaBCfD specific life   AaB alloc events       AfB free events      afA freed before  Aaf allocated after
    aAfaBf decline + growth
Options: --by tag|stack|tree|inverted|fstack|ftree|finverted|name|size|heap  --sort bytes|count  --top N
         --heap cpu|gpu|all   (fstack / ftree / finverted: by the stack of whoever FREED it -- format 2 traces)
         --depth N (tree/inverted)  --file PATH (or the file as the last argument)
Groupings follow Insights (SMemAllocTableTreeView.cpp:201-222): by size (power-of-two buckets, as
MemAllocGroupingBySize), by tag, by callstack top-down (`tree`) and inverted, by function, by heap; `name`
is the GPU resource. Without a file: the newest traces/memtrace_*.mtrace. The format is in
sources/core/memory/MemTrace.cpp.
"""

import glob
import json
import os
import struct
import sys
import time
from array import array
from operator import itemgetter

EV_RESET, EV_ALLOC, EV_FREE, EV_MARKER, EV_STACK = 1, 2, 3, 4, 5
HEAP_CPU = 0
HEAP_NAMES = {0: "cpu", 1: "gpu.local", 2: "gpu.nonlocal"}
KIND_NAMES = {0: "cpu", 1: "buffer", 2: "texture", 3: "descheap", 4: "queryheap"}
SYNTHETIC = 0x80
INF = (1 << 64) - 1

RULES = {
    # name: (markers, condition over a/f and A..D, meaning) -- AllocationsQuery.cpp:196-397
    "aAf": (1, "a <= A <= f", "live at A"),
    "afA": (1, "f <= A", "freed by A"),
    "Aaf": (1, "a >= A", "allocated from A on"),
    "aAfB": (2, "a <= A and A <= f <= B", "decline: alive at A, freed by B"),
    "AaBf": (2, "A <= a <= B and f >= B", "growth: allocated in [A, B], alive at B"),
    "aAfaBf": (2, "(a <= A and A <= f <= B) or (A <= a <= B and f >= B)", "decline + growth"),
    "AfB": (2, "A <= f <= B", "free events in [A, B]"),
    "AaB": (2, "A <= a <= B", "alloc events in [A, B]"),
    "AafB": (2, "a >= A and f <= B", "short-living: allocated and freed inside [A, B] (churn)"),
    "aABf": (2, "a <= A and f >= B", "long-living: alive through [A, B]"),
    "AaBCf": (3, "A <= a <= B and f >= C", "leaks: allocated in [A, B], still alive at C"),
    "AaBfC": (3, "A <= a <= B and B <= f <= C", "limited lifetime: allocated in [A, B], freed in [B, C]"),
    "aABfC": (3, "a <= A and B <= f <= C", "decline of long-living: alive at A, freed in [B, C]"),
    "AaBCfD": (4, "A <= a <= B and C <= f <= D", "specific lifetime: allocated in [A, B], freed in [C, D]"),
}
# Rules that pick allocations by their free: their sizes are negative (Model.size_fn).
FREE_RULES = {"afA", "aAfB", "AfB", "aABfC"}
ALIASES = {
    "leaks": ("AaBCf", "stack", "bytes"),
    "growth": ("AaBf", "stack", "bytes"),
    "churn": ("AafB", "stack", "count"),
    "decline": ("aAfB", "stack", "bytes"),
    "live": ("aAf", "tag", "bytes"),
}
# Leaf frames that only say "it allocated": the standard library (its containers growing), the
# allocator and the census wrappers. A stack is shown and grouped from the first frame past them -- the
# code that made the container grow. (Release names of such frames are unreliable anyway: identical
# COMDAT folding gives one body many types -- a vector of pointers shows up as nlohmann's.)
NOISE_PREFIXES = ("std::", "operator new", "operator delete", "gpu::`anonymous namespace'::", "gpu::Create",
                  "gpu::TrackExternalResource", "mi_", "_malloc", "malloc", "_free", "free")


# ---------------------------------------------------------------------------------------------- file

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
        self.frees = []         # (serial, key, heap, stack of the free -- 0 before format 2)
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
    free_stacks = tr.header.get("version", 1) >= 2
    prev_serial, prev_key = state
    mask = 0xFFFFFFFFFFFFFFFF

    while pos < end:
        ev = data[pos]
        pos += 1
        if ev == EV_ALLOC or ev == EV_FREE or ev == EV_MARKER:
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
            v = 0; s = 0
            while True:
                b = data[pos]; pos += 1
                v |= (b & 0x7F) << s
                if b < 0x80: break
                s += 7
            prev_key = (prev_key + ((v >> 1) ^ -(v & 1))) & mask
            key = prev_key
            if ev == EV_FREE:
                heap = data[pos]
                pos += 1
                fstack = 0
                if free_stacks:  # format 2: who freed it
                    v = 0; s = 0
                    while True:
                        b = data[pos]; pos += 1
                        v |= (b & 0x7F) << s
                        if b < 0x80: break
                        s += 7
                    fstack = v
                frees.append((serial, key, heap, fstack))
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


# -------------------------------------------------------------------------------------------- replay

class Model:
    """Every allocation as one row (UE FAllocationItem): alloc serial a, free serial f (INF = never), size,
    tag, heap, kind, stack; plus the per-tag live bytes at the end of every frame."""

    def __init__(self, tr):
        self.tr = tr
        allocs = sorted(tr.allocs, key=itemgetter(0))
        frees = sorted(tr.frees, key=itemgetter(0))
        frames = sorted((m[0], m[2], m[1]) for m in tr.markers if m[2] is not None)  # (serial, frame, qpc)
        n, nf, nk = len(allocs), len(frees), len(frames)
        self.a = array("Q", map(itemgetter(0), allocs))
        self.size = array("Q", map(itemgetter(2), allocs))
        self.tag = array("B", map(itemgetter(3), allocs))
        self.heap = array("B", map(itemgetter(4), allocs))
        self.kind = array("B", map(itemgetter(5), allocs))
        self.stack = array("I", map(itemgetter(6), allocs))
        self.names = {i: x[7] for i, x in enumerate(allocs) if x[7] is not None}
        self.f = array("Q", [INF]) * n
        self.fstack = array("I", [0]) * n  # the stack of whoever freed it (format 2; 0 = never freed / not captured)
        fstack = self.fstack
        ntags = max(len(tr.tags), 1)
        self.ntags = ntags
        cpu = [0] * ntags
        gpu = [0] * ntags
        self.frames = []  # (frame, serial, qpc, cpu bytes per tag, gpu bytes per tag) at the end of each frame
        live = {}
        double_alloc = unknown_free = 0
        f = self.f
        i = j = k = 0
        while True:
            sa = allocs[i][0] if i < n else INF
            sf = frees[j][0] if j < nf else INF
            s = sa if sa < sf else sf
            while k < nk and frames[k][0] < s:
                fs, fr, fq = frames[k]
                self.frames.append((fr, fs, fq, cpu[:], gpu[:]))
                k += 1
            if s == INF:
                break
            if sa < sf:
                x = allocs[i]
                key = (x[4] != HEAP_CPU, x[1])
                if key in live:
                    double_alloc += 1
                live[key] = i
                if x[4] == HEAP_CPU:
                    cpu[x[3]] += x[2]
                else:
                    gpu[x[3]] += x[2]
                i += 1
            else:
                x = frees[j]
                idx = live.pop((x[2] != HEAP_CPU, x[1]), None)
                if idx is None:
                    unknown_free += 1
                else:
                    f[idx] = x[0]
                    fstack[idx] = x[3] if len(x) > 3 else 0
                    if allocs[idx][4] == HEAP_CPU:
                        cpu[allocs[idx][3]] -= allocs[idx][2]
                    else:
                        gpu[allocs[idx][3]] -= allocs[idx][2]
                j += 1
        self.live = live
        self.anomalies = dict(double_alloc=double_alloc, unknown_free=unknown_free)
        self.last_serial = max(allocs[-1][0] if n else 0, frees[-1][0] if nf else 0, frames[-1][0] if nk else 0)
        hdr = tr.header
        self.freq = hdr.get("qpc_freq", 1) or 1
        self.start_qpc = hdr.get("start_qpc", 0)

    def __len__(self):
        return len(self.a)

    # ---- time markers ----
    def resolve(self, spec):
        """'600' / 'f600' (end of that frame), '12.5s' (since the trace started), 'start', 'end' -> (serial, label)."""
        s = spec.strip().lower()
        if s in ("start", "begin"):
            return 0, "start"
        if s == "end":
            return self.last_serial + 1, "end"
        if s.endswith("s"):
            secs = float(s[:-1])
            target = self.start_qpc + secs * self.freq
            for fr, fs, fq, _c, _g in self.frames:
                if fq >= target:
                    return fs, f"{secs:g}s (frame {fr})"
            return self.last_serial + 1, f"{secs:g}s (past the end)"
        frame = int(s.lstrip("f"))
        for fr, fs, _fq, _c, _g in self.frames:
            if fr >= frame:
                return fs, f"frame {fr}"
        return self.last_serial + 1, f"frame {frame} (past the end)"

    def seconds(self, serial):
        for fr, fs, fq, _c, _g in self.frames:
            if fs >= serial:
                return (fq - self.start_qpc) / self.freq
        return (self.frames[-1][2] - self.start_qpc) / self.freq if self.frames else 0.0

    # ---- queries ----
    def size_fn(self, rule, marks):
        """The size an allocation adds to a result: a free negative, an allocation positive (owner, 2026-10-23:
        frees with a minus everywhere). The rules that pick allocations by their free inside a window (decline,
        free events, decline of long-living: FREE_RULES) are negative; under aAfaBf the decline part only
        (a <= A, f <= B) -- all UE flips (SMemAllocTableTreeView.cpp:682-688). State, allocation and lifetime
        rules are positive. The viewer's sizeFn is the same rule."""
        size = self.size
        if rule in FREE_RULES:
            return lambda i: -size[i]
        if rule != "aAfaBf":
            return lambda i: size[i]
        A, B = marks[0], marks[1]
        a, f = self.a, self.f
        return lambda i: -size[i] if a[i] <= A and f[i] <= B else size[i]

    def select(self, rule, marks, heap="all"):
        """Indices of the allocations the rule picks (UE QueryLiveAllocs). `marks` = serials A..D."""
        need, cond, _meaning = RULES[rule]
        A, B, C, D = (list(marks) + [0, 0, 0, 0])[:4]
        code = compile(f"[i for i, (a, f) in enumerate(zip(ma, mf)) if {cond}]", f"<{rule}>", "eval")
        picked = eval(code, {}, dict(ma=self.a, mf=self.f, A=A, B=B, C=C, D=D))
        if heap == "cpu":
            hp = self.heap
            picked = [i for i in picked if hp[i] == HEAP_CPU]
        elif heap == "gpu":
            hp = self.heap
            picked = [i for i in picked if hp[i] != HEAP_CPU]
        return picked

    def live_at_end(self):
        return list(self.live.values())


# ------------------------------------------------------------------------------------------- display

def newest():
    files = glob.glob(os.path.join("traces", "memtrace_*.mtrace"))
    if not files:
        sys.exit("no traces/memtrace_*.mtrace -- run with --memtrace")
    return max(files, key=os.path.getmtime)


def tag_name(tr, t):
    return tr.tags[t][0] if t < len(tr.tags) else f"tag{t}"


def mb(x):
    return x / (1024.0 * 1024.0)


def human(x):
    if abs(x) >= 1 << 20:
        return f"{mb(x):.2f} MB"
    if abs(x) >= 1 << 10:
        return f"{x / 1024.0:.1f} KB"
    return f"{x} B"


def func_name(tr, addr):
    sym = tr.symbols.get(addr)
    return sym[0] if sym and sym[0] else f"0x{addr:x}"


def frame_text(tr, addr):
    sym = tr.symbols.get(addr)
    if not sym or not sym[0]:
        return f"0x{addr:x}"
    func, file_, line = sym
    return f"{func} ({os.path.basename(file_)}:{line})" if file_ else func


def system_ranges(tr):
    """[base, end) of the modules under Windows\\ (d3d12, dxgi, kernel32, ntdll): a free's stack starts inside
    them -- the D3D object's destructor, HeapFree -- before it reaches the code that let go."""
    if not hasattr(tr, "_sys"):
        tr._sys = [(b, b + s) for (b, s, _g, _a, p) in tr.modules if "\\windows\\" in p.lower()]
    return tr._sys


def caller_frames(tr, stack_id):
    """The stack leaf first, past the allocator/census frames and the system DLLs under them."""
    frames = tr.stacks.get(stack_id, [])
    sys = system_ranges(tr)
    start = 0
    while start < len(frames) and (func_name(tr, frames[start]).startswith(NOISE_PREFIXES) or
                                   any(lo <= frames[start] < hi for lo, hi in sys)):
        start += 1
    return frames[start:] if start < len(frames) else frames


def group(model, picked, by, size_of=None):
    """{key: [bytes, count, example index]} and a label function. `size_of(i)`: Model.size_fn's signed size."""
    tr = model.tr
    out = {}
    size, tag, heap, kind, stack = model.size, model.tag, model.heap, model.kind, model.stack
    size_of = size_of or (lambda i: size[i])
    if by == "tag":
        keyf = lambda i: tag[i]
        label = lambda k: tag_name(tr, k)
    elif by == "heap":
        keyf = lambda i: heap[i]
        label = lambda k: HEAP_NAMES.get(k, f"heap{k}")
    elif by == "size":
        keyf = lambda i: size[i].bit_length()
        label = lambda k: f"[{human(1 << (k - 1)) if k else '0 B'}, {human(1 << k)})"
    elif by == "name":
        names = model.names
        keyf = lambda i: names.get(i) or f"(cpu, {tag_name(tr, tag[i])})"
        label = lambda k: k
    elif by == "stack":
        keyf = lambda i: stack[i]
        label = lambda k: "(no stack)" if k == 0 else f"stack {k}"
    elif by == "fstack":  # who freed it (UE's grouping by free callstack)
        f, fstack = model.f, model.fstack
        keyf = lambda i: -1 if f[i] == INF else fstack[i]
        label = lambda k: "(never freed)" if k == -1 else "(freed, no stack captured)" if k == 0 else f"free stack {k}"
    else:
        raise ValueError(by)
    for i in picked:
        k = keyf(i)
        row = out.get(k)
        if row is None:
            out[k] = [size_of(i), 1, i]
        else:
            row[0] += size_of(i)
            row[1] += 1
    return out, label


def print_rows(model, rows, label, sort, top, total_b, total_n, show_stack=False):
    tr = model.tr
    # Bytes by magnitude: a free is negative, and the biggest one is what --top has to keep.
    order = sorted(rows.items(), key=lambda kv: -(kv[1][1] if sort == "count" else abs(kv[1][0])))
    print(f"  {'bytes':>12} {'%':>6} {'count':>9}  group")
    for k, (b, c, ex) in order[:top]:
        pct = 100.0 * (c / total_n if sort == "count" else b / total_b) if (total_n if sort == "count" else total_b) else 0.0
        extra = ""
        if show_stack and k <= 0:
            extra = f"  e.g. {tag_name(tr, model.tag[ex])}"
        print(f"  {human(b):>12} {pct:5.1f}% {c:9d}  {label(k)}{extra}")
        if show_stack and k > 0:
            print(f"      {tag_name(tr, model.tag[ex])}, {HEAP_NAMES.get(model.heap[ex])}"
                  + (f", {model.names[ex]}" if ex in model.names else ""))
            for addr in caller_frames(tr, k)[:8]:
                print(f"        {frame_text(tr, addr)}")
    if len(order) > top:
        rest_b = sum(v[0] for _k, v in order[top:])
        rest_c = sum(v[1] for _k, v in order[top:])
        print(f"  {human(rest_b):>12}        {rest_c:9d}  ... {len(order) - top} more groups")


def print_tree(model, picked, inverted, depth, top, sort, size_of=None, free=False):
    """Callstack grouping by function: top-down from the root (`tree`) or from the allocation site
    (`inverted`), each node with the bytes/count of the allocations under it. `free`: by the stack of the
    free instead (UE's free-callstack groupings)."""
    tr = model.tr
    stack = model.fstack if free else model.stack
    size_of = size_of or (lambda i: model.size[i])
    root = {}
    for i in picked:
        frames = caller_frames(tr, stack[i]) if stack[i] else []
        if free and model.f[i] == INF:
            names = ["(never freed)"]
        else:
            names = [func_name(tr, a) for a in frames] or ["(no stack)"]
        if not inverted:
            names = names[::-1]
        node = root
        for nm in names[:depth]:
            child = node.get(nm)
            if child is None:
                child = node[nm] = [0, 0, {}]
            child[0] += size_of(i)
            child[1] += 1
            node = child[2]
    total = sum(abs(size_of(i)) for i in picked) or 1  # absolute: under aAfaBf the net may be ~0

    def walk(node, indent):
        items = sorted(node.items(), key=lambda kv: -(kv[1][1] if sort == "count" else abs(kv[1][0])))
        for nm, (b, c, kids) in items[:top]:
            if abs(b) * 200 < total and indent > 0:  # below 0.5 % of the query: not worth a line
                continue
            print(f"  {human(b):>12} {100.0 * b / total:5.1f}% {c:9d}  {'  ' * indent}{nm}")
            walk(kids, indent + 1)

    site = "free site" if free else "allocation site"
    print(f"  {'bytes':>12} {'%':>6} {'count':>9}  {site + ' -> callers' if inverted else 'root -> ' + site}")
    walk(root, 0)


# ------------------------------------------------------------------------------------------ commands

def parse_args(argv):
    opts = dict(by=None, sort=None, top=15, heap="all", depth=8, file=None, out=None, port=0, open=True)
    pos = []
    it = iter(argv)
    for x in it:
        if x in ("--by", "--sort", "--top", "--heap", "--depth", "--file", "--out", "--port"):
            v = next(it)
            opts[x[2:]] = int(v) if x in ("--top", "--depth", "--port") else v
        elif x == "--no-open":
            opts["open"] = False
        elif x.startswith("--"):
            raise SystemExit(f"unknown option {x}")
        else:
            pos.append(x)
    if opts["file"] is None and pos and (pos[-1].endswith(".mtrace") or os.sep in pos[-1] or "/" in pos[-1]):
        opts["file"] = pos.pop()
    return pos, opts


def open_model(path):
    t0 = time.perf_counter()
    tr = load(path)
    t1 = time.perf_counter()
    model = Model(tr)
    t2 = time.perf_counter()
    events = len(tr.allocs) + len(tr.frees)
    print(f"{path}: {len(tr.allocs)} allocs, {len(tr.frees)} frees ({events / 1e6:.2f} M events): "
          f"read {t1 - t0:.1f} s, replay {t2 - t1:.1f} s")
    return model


def cmd_verify(path):
    model = open_model(path)
    tr = model.tr
    if tr.end is None:
        print("memtrace verify: FAIL -- no End record (the process did not stop the trace)")
        return 1
    rows = [[0] * 7 for _ in range(model.ntags)]
    for i in model.live_at_end():
        r = rows[model.tag[i]]
        if model.heap[i] == HEAP_CPU:
            r[0] += model.size[i]
            r[1] += 1
        elif model.kind[i] & 0x7F in (3, 4):
            r[4] += model.size[i]
            r[6] += 1
        else:
            r[2 if model.heap[i] == 1 else 3] += model.size[i]
            r[5] += 1
    end = tr.end
    failures = []
    if end["drops"]:
        failures.append(f"{end['drops']} events dropped")
    if tr.truncated:
        failures.append("file truncated")
    an = model.anomalies
    if an["double_alloc"] or an["unknown_free"]:
        failures.append(f"{an['double_alloc']} allocs of a live key, {an['unknown_free']} frees of an unknown key")
    cols = ["cpu bytes", "cpu count", "gpu local", "gpu nonlocal", "gpu descriptor", "gpu resources", "gpu heaps"]
    use = ([0, 1] if end["cpu_on"] else []) + ([2, 3, 4, 5, 6] if end["gpu_on"] else [])
    for t, want in enumerate(end["rows"]):
        have = rows[t] if t < len(rows) else [0] * 7
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
    model = open_model(path)
    tr = model.tr
    h = tr.header
    synth = sum(1 for k in model.kind if k & SYNTHETIC)
    span = (model.frames[-1][2] - model.start_qpc) / model.freq if model.frames else 0.0
    print(f"  file {mb(os.path.getsize(path)):.1f} MB{'  TRUNCATED' if tr.truncated else ''}; run: pid {h.get('pid')}, "
          f"{os.path.basename(h.get('exe', ''))}, cpu {'on' if h.get('cpu') else 'off'}, gpu {'on' if h.get('gpu') else 'off'}, "
          f"stacks {h.get('stacks')}, started at frame {h.get('start_frame')}")
    if model.frames:
        print(f"  frames {model.frames[0][0]}..{model.frames[-1][0]} ({len(model.frames)} markers), {span:.1f} s")
    print(f"  {synth} alive at the start, {len(tr.markers)} markers, {len(tr.stacks)} stacks, {len(tr.symbols)} symbols, "
          f"{len(tr.threads)} threads, {tr.chunks} chunks")
    if tr.end:
        print(f"  end: drops {tr.end['drops']}, buffers {tr.end['buffers']}, anomalies {model.anomalies}")
    else:
        print("  end: none (the process did not stop the trace)")
    live = model.live_at_end()
    rows = [[0, 0, 0, 0, 0] for _ in range(model.ntags)]  # cpu, cpu count, gpu local, gpu nonlocal, gpu resources
    for i in live:
        r = rows[model.tag[i]]
        if model.heap[i] == HEAP_CPU:
            r[0] += model.size[i]
            r[1] += 1
        else:
            r[2 if model.heap[i] == 1 else 3] += model.size[i]
            r[4] += 1
    order = sorted(range(len(rows)), key=lambda t: -(rows[t][0] + rows[t][2] + rows[t][3]))
    print("  alive at the end, by tag:      cpu MB    count   gpu local MB  nonlocal MB  resources")
    for t in order[:20]:
        r = rows[t]
        if r[0] + r[2] + r[3] == 0:
            continue
        print(f"    {tag_name(tr, t):<24} {mb(r[0]):9.2f} {r[1]:8d} {mb(r[2]):14.2f} {mb(r[3]):12.2f} {r[4]:10d}")
    for title, pick in (("gpu", lambda i: model.heap[i] != HEAP_CPU), ("cpu", lambda i: model.heap[i] == HEAP_CPU and model.stack[i])):
        biggest = sorted((i for i in live if pick(i)), key=lambda i: -model.size[i])[:5]
        if not biggest:
            continue
        print(f"  biggest live {title} allocations{' with a stack' if title == 'cpu' else ''}:")
        for i in biggest:
            label = model.names.get(i) or KIND_NAMES.get(model.kind[i] & 0x7F, "?")
            print(f"    {mb(model.size[i]):8.2f} MB  {tag_name(tr, model.tag[i])}  {label}"
                  f"{'  (alive at start)' if model.kind[i] & SYNTHETIC else ''}")
            for addr in caller_frames(tr, model.stack[i])[:6]:
                print(f"        {frame_text(tr, addr)}")
    return 0


def run_query(model, rule, specs, by, sort, top, heap, depth):
    need = RULES[rule][0]
    if len(specs) < need:
        raise SystemExit(f"{rule} needs {need} time marker(s): {' '.join('ABCD'[:need])}")
    marks = []
    labels = []
    for s in specs[:need]:
        serial, lab = model.resolve(s)
        marks.append(serial)
        labels.append(lab)
    t0 = time.perf_counter()
    picked = model.select(rule, marks, heap)
    size_of = model.size_fn(rule, marks)
    sizes = [size_of(i) for i in picked]
    growth = sum(s for s in sizes if s > 0)
    decline = -sum(s for s in sizes if s < 0)
    if decline and growth:
        amount = f"net {'+' if growth >= decline else ''}{human(growth - decline)} (growth {human(growth)}, decline -{human(decline)})"
    elif decline:
        amount = f"-{human(decline)} freed"
    else:
        amount = human(growth)
    print(f"{rule} -- {RULES[rule][2]}; " + ", ".join(f"{'ABCD'[n]} = {labels[n]}" for n in range(need))
          + f"; heap {heap}: {len(picked)} allocations, {amount} ({time.perf_counter() - t0:.1f} s)")
    if not picked:
        return 0
    if by in ("tree", "inverted", "ftree", "finverted"):
        print_tree(model, picked, by in ("inverted", "finverted"), depth, top, sort, size_of, free=by.startswith("f"))
        return 0
    rows, label = group(model, picked, by, size_of)
    print_rows(model, rows, label, sort, top, (growth + decline) or 1, len(picked), show_stack=by in ("stack", "fstack"))
    return 0


def cmd_query(model, rule, specs, opts):
    if rule not in RULES:
        raise SystemExit(f"unknown rule {rule}; rules: {' '.join(RULES)}")
    return run_query(model, rule, specs, opts["by"] or "tag", opts["sort"] or "bytes", opts["top"], opts["heap"], opts["depth"])


def cmd_tags(model, specs, opts):
    """UE's LLM tag table (MemTagTable.cpp:289-497): size at A, at B, B - A, and min/max/avg over the frames
    between them."""
    if len(specs) < 2:
        raise SystemExit("tags needs A B")
    (sa, la), (sb, lb) = model.resolve(specs[0]), model.resolve(specs[1])
    heap = opts["heap"]
    tr = model.tr
    nt = model.ntags

    def at(serial):
        v = [0] * nt
        for i in model.select("aAf", [serial], heap):
            v[model.tag[i]] += model.size[i]
        return v

    va, vb = at(sa), at(sb)
    window = [fr for fr in model.frames if sa <= fr[1] <= sb]
    print(f"tags, heap {heap}: A = {la}, B = {lb}, {len(window)} frames between")
    print(f"  {'tag':<24} {'size A':>12} {'size B':>12} {'B - A':>12} {'min':>12} {'max':>12} {'avg':>12}")
    order = sorted(range(nt), key=lambda t: -max(va[t], vb[t]))
    for t in order:
        series = [(fr[3][t] if heap != "gpu" else 0) + (fr[4][t] if heap != "cpu" else 0) for fr in window]
        if va[t] == 0 and vb[t] == 0 and not any(series):
            continue
        lo, hi = (min(series), max(series)) if series else (0, 0)
        avg = sum(series) / len(series) if series else 0
        d = vb[t] - va[t]
        sign = "+" if d > 0 else ("-" if d < 0 else "")
        print(f"  {tag_name(tr, t):<24} {human(va[t]):>12} {human(vb[t]):>12} {sign + human(abs(d)):>12} "
              f"{human(lo):>12} {human(hi):>12} {human(int(avg)):>12}")
    return 0


def cmd_timeline(model, path, opts):
    """Per-tag live MB at the end of every frame as Chrome-trace counter events -- two stacked tracks,
    cpu and gpu (chrome://tracing, ui.perfetto.dev)."""
    tr = model.tr
    out = opts["out"] or os.path.splitext(path)[0] + ".json"
    nt = model.ntags
    used_cpu = [t for t in range(nt) if any(fr[3][t] for fr in model.frames)]
    used_gpu = [t for t in range(nt) if any(fr[4][t] for fr in model.frames)]
    events = [{"name": "process_name", "ph": "M", "pid": 1, "args": {"name": f"memtrace {os.path.basename(path)}"}}]
    for fr, _fs, fq, cpu, gpu in model.frames:
        ts = (fq - model.start_qpc) * 1e6 / model.freq
        events.append({"name": "cpu MB by tag", "ph": "C", "ts": ts, "pid": 1, "tid": 1,
                       "args": {tag_name(tr, t): round(mb(cpu[t]), 3) for t in used_cpu}})
        events.append({"name": "gpu MB by tag", "ph": "C", "ts": ts, "pid": 1, "tid": 1,
                       "args": {tag_name(tr, t): round(mb(gpu[t]), 3) for t in used_gpu}})
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"traceEvents": events, "displayTimeUnit": "ms"}, f)
    print(f"timeline: {len(model.frames)} frames, {len(used_cpu)} cpu tags, {len(used_gpu)} gpu tags -> {out}")
    return 0


# ----------------------------------------------------------------------------------------- self-test

def cmd_selftest():
    """Synthetic trace with known lifetimes: every rule and the per-frame series must give the answer
    worked out by hand from UE's conditions."""
    tr = Trace()
    tr.tags = [("t0", 255), ("t1", 255)]
    tr.header = dict(qpc_freq=1000, start_qpc=0)
    # frame markers: end of frame 1..4 at serials 10, 20, 30, 40 (qpc = serial ms)
    tr.markers = [(10, 10, 1), (20, 20, 2), (30, 30, 3), (40, 40, 4), (4096, 50, None)]
    #             name: (alloc serial, free serial or None, size, tag)
    items = {"X": (5, None, 100, 0), "W": (3, 18, 200, 0), "Y": (12, 25, 400, 1),
             "Z": (15, None, 800, 1), "V": (22, 35, 1600, 0), "G": (7, 33, 3200, 1)}
    by_serial = {}
    for n, (a, f, size, tag) in items.items():
        key = 0x1000 + 16 * len(by_serial)
        by_serial[a] = n
        heap = 1 if n == "G" else HEAP_CPU  # one GPU allocation: the namespaces must not collide
        tr.allocs.append((a, key, size, tag, heap, 1 if heap else 0, 0, "gpu-res" if heap else None))
        if f is not None:
            tr.frees.append((f, key, heap, 0))
    # a CPU key equal to the GPU one (G's), alive the whole time: the GPU free must not take it
    tr.allocs.append((1, 0x1000 + 16 * 5, 1, 0, HEAP_CPU, 0, 0, None))
    by_serial[1] = "K"
    model = Model(tr)

    def names(rule, *specs):
        marks = [model.resolve(s)[0] for s in specs]
        return {by_serial[model.a[i]] for i in model.select(rule, marks)}

    expect = [
        (("aAf", "f1"), {"X", "W", "G", "K"}),
        (("afA", "f2"), {"W"}),
        (("Aaf", "f2"), {"V"}),
        (("aAfB", "f1", "f2"), {"W"}),
        (("AaBf", "f1", "f2"), {"Y", "Z"}),
        (("aAfaBf", "f1", "f2"), {"W", "Y", "Z"}),
        (("AfB", "f1", "f2"), {"W"}),
        (("AaB", "f1", "f2"), {"Y", "Z"}),
        (("AafB", "f1", "f3"), {"Y"}),
        (("aABf", "f1", "f2"), {"X", "G", "K"}),
        (("AaBCf", "f1", "f2", "f3"), {"Z"}),
        (("AaBfC", "f1", "f2", "f3"), {"Y"}),
        (("aABfC", "f1", "f2", "f4"), {"G"}),
        (("AaBCfD", "f2", "f3", "f3", "f4"), {"V"}),
        (("aAf", "end"), {"X", "Z", "K"}),
        (("aAf", "0.02s"), {"X", "Y", "Z", "G", "K"}),  # 0.02 s = the end of frame 2 (serial 20): W freed at 18
    ]
    failures = 0
    for (rule, *specs), want in expect:
        got = names(rule, *specs)
        ok = got == want
        failures += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {rule:<7} {' '.join(specs):<16} {sorted(got)}{'' if ok else f' != {sorted(want)}'}")
    # the series at the end of frame 2 (serial 20): X 100 + Z 800 + Y 400 + K 1 on the cpu (W freed at 18), G 3200 on the gpu
    fr2 = next(fr for fr in model.frames if fr[0] == 2)
    series_ok = fr2[3] == [100 + 1, 400 + 800] and fr2[4] == [0, 3200]
    failures += not series_ok
    print(f"  {'ok  ' if series_ok else 'FAIL'} series at frame 2: cpu {fr2[3]} gpu {fr2[4]}")
    # aAfaBf on f1..f2: W declined (-200, freed at 18), Y and Z grew (+400 +800) -> net +1000, as UE sums it
    m1, m2 = model.resolve("f1")[0], model.resolve("f2")[0]
    size_of = model.size_fn("aAfaBf", [m1, m2])
    net = sum(size_of(i) for i in model.select("aAfaBf", [m1, m2]))
    net_ok = net == 1000
    failures += not net_ok
    print(f"  {'ok  ' if net_ok else 'FAIL'} aAfaBf f1 f2 net bytes {net} (decline counted negative, UE SMemAllocTableTreeView.cpp:682)")
    # frees are negative in every rule that picks by a free: AfB/aAfB f1 f2 = W (-200), aABfC f1 f2 f4 = G (-3200);
    # a lifetime rule stays positive: AafB f1 f3 = Y (+400)
    for (rule, specs, want) in (("afA", ("f2",), -200), ("AfB", ("f1", "f2"), -200), ("aAfB", ("f1", "f2"), -200),
                                ("aABfC", ("f1", "f2", "f4"), -3200), ("AafB", ("f1", "f3"), 400)):
        marks = [model.resolve(s)[0] for s in specs]
        got = sum(model.size_fn(rule, marks)(i) for i in model.select(rule, marks))
        ok = got == want
        failures += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {rule:<7} {' '.join(specs):<16} signed bytes {got}{'' if ok else f' != {want}'}")
    an_ok = model.anomalies == dict(double_alloc=0, unknown_free=0)
    failures += not an_ok
    print(f"  {'ok  ' if an_ok else 'FAIL'} anomalies {model.anomalies}")
    print(f"memtrace selftest: {'PASS' if failures == 0 else f'FAIL ({failures})'}")
    return 1 if failures else 0


def cmd_view(path, opts):
    """Serves tools/memtrace_viewer.html and the traces on 127.0.0.1 and opens the viewer on `path`. Only
    those two kinds of file are served: nothing else of the repository is reachable through the port."""
    import http.server
    import webbrowser

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    rel = os.path.relpath(os.path.abspath(path), root).replace(os.sep, "/")
    if rel.startswith("../") or not rel.startswith("traces/"):
        raise SystemExit(f"view serves traces/ only; open {path} by dropping it on tools/memtrace_viewer.html instead")

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=root, **kwargs)

        def do_GET(self):
            p = self.path.split("?", 1)[0]
            if p == "/tools/memtrace_viewer.html" or (p.startswith("/traces/") and p.endswith(".mtrace") and ".." not in p):
                return super().do_GET()
            self.send_error(404)

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", opts["port"]), Handler)
    url = f"http://127.0.0.1:{server.server_address[1]}/tools/memtrace_viewer.html?file=/{rel}"
    print(f"memtrace view: {url}\n  serving the viewer and traces/ on 127.0.0.1 only; Ctrl+C stops it")
    if opts["open"]:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help", "help"):
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "selftest":
        return cmd_selftest()
    pos, opts = parse_args(argv[2:])
    path = opts["file"] or newest()
    if cmd == "view":
        return cmd_view(path, opts)
    if cmd == "verify":
        return cmd_verify(path)
    if cmd == "summary":
        return cmd_summary(path)
    model = open_model(path)
    if cmd == "query":
        if not pos:
            raise SystemExit("query RULE A [B [C [D]]]")
        return cmd_query(model, pos[0], pos[1:], opts)
    if cmd in ALIASES:
        rule, by, sort = ALIASES[cmd]
        return run_query(model, rule, pos, opts["by"] or by, opts["sort"] or sort, opts["top"], opts["heap"], opts["depth"])
    if cmd == "tags":
        return cmd_tags(model, pos, opts)
    if cmd == "timeline":
        return cmd_timeline(model, path, opts)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
