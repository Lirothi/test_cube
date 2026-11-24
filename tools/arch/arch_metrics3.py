import os, re, sys, collections

ROOT = sys.argv[1]
SRC = os.path.join(ROOT, 'sources')
files = {}
for dp, dn, fn in os.walk(SRC):
    for f in fn:
        if f.endswith(('.cpp', '.h', '.hpp', '.inl')):
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, SRC).replace('\\', '/')
            files[rel] = open(p, 'r', encoding='utf-8', errors='replace').read().split('\n')
inc_re = re.compile(r'^\s*#\s*include\s*"([^"]+)"')
base = collections.defaultdict(list)
for r in files:
    base[os.path.basename(r)].append(r)

def resolve(src, inc):
    inc = inc.replace('\\', '/')
    c = os.path.normpath(inc).replace('\\', '/')
    if c in files: return c
    c = os.path.normpath(os.path.join(os.path.dirname(src), inc)).replace('\\', '/')
    if c in files: return c
    b = os.path.basename(inc)
    return base[b][0] if len(base.get(b, [])) == 1 else None

edges = collections.defaultdict(set)
for r, L in files.items():
    for l in L:
        m = inc_re.match(l)
        if m:
            t = resolve(r, m.group(1))
            if t: edges[r].add(t)

cpps = [r for r in files if r.endswith('.cpp')]
closure = {}
for c in cpps:
    seen = set(); todo = [c]
    while todo:
        x = todo.pop()
        for t in edges.get(x, ()):
            if t not in seen:
                seen.add(t); todo.append(t)
    closure[c] = seen
print('total .cpp:', len(cpps), ' total .h:', len(files) - len(cpps))
rebuild = collections.Counter()
for c, s in closure.items():
    for h in s:
        rebuild[h] += 1
print('=== headers by number of .cpp they force to rebuild (top 45) ===')
for h, n in rebuild.most_common(45):
    print(f'{n:4d} ({100 * n // len(cpps):3d}%)  {len(files[h]):5d} lines  {h}')
avg = sum(len(s) for s in closure.values()) / len(cpps)
avgl = sum(sum(len(files[h]) for h in s) for s in closure.values()) / len(cpps)
print(f'average project headers per .cpp: {avg:.0f}; average project header lines per .cpp: {avgl:.0f}')
# non-knob transport globals
print('\n=== inline/extern g_ in headers that are NOT KNOB twins, by file (top 25) ===')
c = collections.Counter()
for r, L in files.items():
    if not r.endswith('.h'): continue
    text = '\n'.join(L)
    names = set(re.findall(r'^\s*(?:inline|extern)\s+(?!const bool)[^;(\n]*?\b(g_\w+)\b', text, re.M))
    twins = {n for n in names if n.endswith('_game')}
    knob = {n[:-5] for n in twins} | twins | {n for n in names if n.endswith('_knob')}
    knobmacro = set(re.findall(r'KNOB\(\s*[^,]+,\s*(g_\w+)', text))
    plain = names - knob - knobmacro
    if plain:
        c[r] = len(plain)
print('total plain globals in headers:', sum(c.values()))
for r, n in c.most_common(25):
    print(f'  {n:4d}  {r}')
kn = 0
for r, L in files.items():
    text = '\n'.join(L)
    kn += len(re.findall(r'KNOB\(', text)) + len(re.findall(r'knob::Register\(', text))
print('KNOB registrations (macro + manual):', kn)
