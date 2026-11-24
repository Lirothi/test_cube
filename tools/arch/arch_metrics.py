import os, re, sys, collections

ROOT = sys.argv[1]
SRC = os.path.join(ROOT, 'sources')
EXT = ('.cpp', '.h', '.hpp', '.inl')

files = {}
for dp, dn, fn in os.walk(SRC):
    for f in fn:
        if f.endswith(EXT):
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, SRC).replace('\\', '/')
            try:
                with open(p, 'r', encoding='utf-8', errors='replace') as fh:
                    files[rel] = fh.read().split('\n')
            except Exception as e:
                print('ERR', rel, e)

def mod1(rel):
    parts = rel.split('/')
    return parts[0] if len(parts) > 1 else '(root)'

def mod2(rel):
    parts = rel.split('/')
    if len(parts) > 2:
        return parts[0] + '/' + parts[1]
    return parts[0] if len(parts) > 1 else '(root)'

# 1. lines per mod2
print('=== LINES PER SUBFOLDER ===')
agg = collections.Counter(); cnt = collections.Counter()
for rel, lines in files.items():
    agg[mod2(rel)] += len(lines); cnt[mod2(rel)] += 1
for k, v in sorted(agg.items(), key=lambda x: -x[1]):
    print(f'{k:28s} files={cnt[k]:4d} lines={v:7d}')

# 2. top files
print('\n=== TOP 60 FILES BY LINES ===')
for rel, lines in sorted(files.items(), key=lambda x: -len(x[1]))[:60]:
    print(f'{len(lines):7d}  {rel}')

# 3. include graph
inc_re = re.compile(r'^\s*#\s*include\s*"([^"]+)"')
basename_index = collections.defaultdict(list)
for rel in files:
    basename_index[os.path.basename(rel)].append(rel)

def resolve(src_rel, inc):
    inc = inc.replace('\\', '/')
    # relative to sources/
    cand = os.path.normpath(inc).replace('\\', '/')
    if cand in files:
        return cand
    # relative to file dir
    cand = os.path.normpath(os.path.join(os.path.dirname(src_rel), inc)).replace('\\', '/')
    if cand in files:
        return cand
    # by suffix
    hits = [r for r in files if r.endswith('/' + inc) or r == inc]
    if len(hits) == 1:
        return hits[0]
    b = os.path.basename(inc)
    if len(basename_index.get(b, [])) == 1:
        return basename_index[b][0]
    return None

edges = collections.defaultdict(set)  # file -> files
unresolved = collections.Counter()
for rel, lines in files.items():
    for ln in lines:
        m = inc_re.match(ln)
        if m:
            t = resolve(rel, m.group(1))
            if t:
                edges[rel].add(t)
            else:
                unresolved[m.group(1)] += 1

mods = sorted(set(mod1(r) for r in files))
mat = collections.defaultdict(int)
for s, ts in edges.items():
    for t in ts:
        if mod1(s) != mod1(t):
            mat[(mod1(s), mod1(t))] += 1
print('\n=== MODULE INCLUDE MATRIX (row includes col), count of file->file edges ===')
print(' ' * 12 + ''.join(f'{m[:9]:>10s}' for m in mods))
for a in mods:
    print(f'{a:12s}' + ''.join(f'{mat.get((a, b), 0):10d}' if a != b else f'{"-":>10s}' for b in mods))

# 4. sub-module matrix: list of edges from lower layers to upper
print('\n=== SUBMODULE EDGES (mod2 -> mod2), top 150 by count ===')
mat2 = collections.defaultdict(int)
for s, ts in edges.items():
    for t in ts:
        if mod2(s) != mod2(t):
            mat2[(mod2(s), mod2(t))] += 1
for (a, b), n in sorted(mat2.items(), key=lambda x: -x[1])[:150]:
    print(f'{n:5d}  {a:26s} -> {b}')

# 5. suspicious upward edges: explicit list
print('\n=== UPWARD EDGES (files) ===')
LAYER = {'core': 0, 'input': 1, 'assets': 1, 'text': 2, 'materials': 2, 'rendering': 3, 'ui': 3, 'physics': 3, 'ocean': 4, 'vfx': 4, 'app': 5, 'editor': 6, '(root)': -1}
up = collections.defaultdict(list)
for s, ts in edges.items():
    for t in ts:
        a, b = mod1(s), mod1(t)
        if a != b and LAYER.get(a, 9) < LAYER.get(b, 9):
            up[(a, b)].append((s, t))
for (a, b), lst in sorted(up.items(), key=lambda x: (LAYER[x[0][0]], -len(x[1]))):
    print(f'-- {a} -> {b}: {len(lst)} edges')
    tcount = collections.Counter(t for s, t in lst)
    for t, n in tcount.most_common(12):
        print(f'      {n:4d} x  {t}')
    scount = collections.Counter(s for s, t in lst)
    print('      from: ' + ', '.join(f'{os.path.basename(s)}({n})' for s, n in scount.most_common(14)))

# 6. header fan-in (direct)
print('\n=== HEADER FAN-IN (direct includers), top 50 ===')
fanin = collections.Counter()
for s, ts in edges.items():
    for t in ts:
        fanin[t] += 1
for t, n in fanin.most_common(50):
    print(f'{n:5d}  {len(files[t]):6d} lines  {t}')

# 7. transitive include size of headers (how many headers a header drags)
print('\n=== HEADER TRANSITIVE FAN-OUT (project headers dragged), top 40 ===')
memo = {}
def closure(f, stack=()):
    if f in memo:
        return memo[f]
    seen = set()
    todo = [f]
    while todo:
        x = todo.pop()
        for t in edges.get(x, ()):
            if t not in seen:
                seen.add(t); todo.append(t)
    memo[f] = seen
    return seen
hdrs = [f for f in files if f.endswith('.h')]
tf = []
for h in hdrs:
    c = closure(h)
    tf.append((len(c), sum(len(files[x]) for x in c), h))
for n, l, h in sorted(tf, reverse=True)[:40]:
    print(f'{n:5d} headers {l:7d} lines  fanin={fanin[h]:4d}  {h}')

# 8. cycles between mod2 (SCC)
print('\n=== STRONGLY CONNECTED COMPONENTS between subfolders (mod2) ===')
g = collections.defaultdict(set)
for (a, b), n in mat2.items():
    g[a].add(b)
nodes = sorted(set(mod2(r) for r in files))
index = {}; low = {}; onst = set(); st = []; res = []; idx = [0]
sys.setrecursionlimit(10000)
def sc(v):
    index[v] = low[v] = idx[0]; idx[0] += 1; st.append(v); onst.add(v)
    for w in g[v]:
        if w not in index:
            sc(w); low[v] = min(low[v], low[w])
        elif w in onst:
            low[v] = min(low[v], index[w])
    if low[v] == index[v]:
        comp = []
        while True:
            w = st.pop(); onst.discard(w); comp.append(w)
            if w == v: break
        res.append(comp)
for v in nodes:
    if v not in index: sc(v)
for comp in res:
    if len(comp) > 1:
        print(f'SCC size {len(comp)}: ' + ', '.join(sorted(comp)))

# 9. globals
print('\n=== GLOBALS ===')
ext_re = re.compile(r'^\s*extern\s+(?!"C")[^;(]+\bg_\w+')
inline_g_re = re.compile(r'^\s*inline\s+(?!.*\()[^;=]*\bg_\w+')
gdef = collections.Counter(); gfile = collections.Counter()
for rel, lines in files.items():
    for ln in lines:
        if ext_re.match(ln) or (rel.endswith('.h') and inline_g_re.match(ln)):
            gdef[mod2(rel)] += 1; gfile[rel] += 1
print('extern/inline g_* declarations per subfolder:')
for k, v in gdef.most_common(40):
    print(f'  {v:5d}  {k}')
print('top files:')
for k, v in gfile.most_common(30):
    print(f'  {v:5d}  {k}')

# 10. preprocessor flavours
print('\n=== BUILD-FLAVOUR #if COUNTS per module ===')
for flag in ('WITH_EDITOR', 'TC_SHIPPING', 'TC_PACKAGED', '_DEBUG', 'TC_COOK'):
    c = collections.Counter()
    r = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif).*\b' + flag + r'\b')
    for rel, lines in files.items():
        for ln in lines:
            if r.match(ln):
                c[mod1(rel)] += 1
    print(f'{flag}: total={sum(c.values())}  ' + ', '.join(f'{k}={v}' for k, v in c.most_common()))

# 11. long functions (heuristic: top-level '{' at col 0 ... '}' at col 0)
print('\n=== LONGEST BRACE BLOCKS AT COLUMN 0 (functions / namespaces w/o indent), top 50 ===')
blocks = []
for rel, lines in files.items():
    if not rel.endswith('.cpp'):
        continue
    start = None
    for i, ln in enumerate(lines):
        if ln.startswith('{'):
            start = i
        elif ln.startswith('}') and start is not None:
            ln_len = i - start
            # header line(s) above
            j = start - 1
            hdr = ''
            while j >= 0 and not lines[j].strip():
                j -= 1
            k = j
            while k >= 0 and lines[k].strip() and not lines[k].startswith('}') and j - k < 6:
                k -= 1
            hdr = ' '.join(l.strip() for l in lines[k + 1:j + 1])
            if not hdr.startswith('namespace') and 'namespace' not in hdr[:20]:
                blocks.append((ln_len, rel, start + 1, hdr[:110]))
            start = None
for n, rel, ln, hdr in sorted(blocks, reverse=True)[:50]:
    print(f'{n:6d}  {rel}:{ln}  {hdr}')

print('\n=== UNRESOLVED INCLUDES (top 25, likely third-party) ===')
for k, v in unresolved.most_common(25):
    print(f'{v:4d} {k}')
