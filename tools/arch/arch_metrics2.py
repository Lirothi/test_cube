import os, re, sys, collections

ROOT = sys.argv[1]
SRC = os.path.join(ROOT, 'sources')

def read(rel):
    with open(os.path.join(SRC, rel), 'r', encoding='utf-8', errors='replace') as f:
        return f.read()

def lines(rel):
    return read(rel).split('\n')

# 1. --set name table in App.cpp: count string-compare branches
app = read('app/App.cpp')
print('=== App.cpp settings dispatch ===')
print('name == "..." comparisons:', len(re.findall(r'==\s*"[a-zA-Z]+\.[A-Za-z0-9_.]+"', app)))
print('distinct "ns.key" literals:', len(set(re.findall(r'"([a-zA-Z]+\.[A-Za-z0-9_]+)"', app))))
ns = collections.Counter(m.split('.')[0] for m in set(re.findall(r'"([a-zA-Z]+\.[A-Za-z0-9_]+)"', app)))
print('namespaces:', ', '.join(f'{k}={v}' for k, v in ns.most_common(60)))
for fn in ('ApplySweepValue', 'ReadSweepValue', 'ApplyFixedSettings'):
    for m in re.finditer(r'^[^\n]*\b' + fn + r'\s*\(', app, re.M):
        ln = app[:m.start()].count('\n') + 1
        print(f'  {fn} at App.cpp:{ln}: {m.group(0).strip()[:120]}')

# 2. main.cpp: command-line switches
mainc = read('app/main.cpp')
sw = set(re.findall(r'"(--[a-z0-9-]+)', mainc))
print('\n=== main.cpp ===')
print('distinct --switches in main.cpp:', len(sw))
allsw = set()
for dp, dn, fn in os.walk(SRC):
    for f in fn:
        if f.endswith(('.cpp', '.h')):
            p = os.path.join(dp, f)
            try:
                t = open(p, 'r', encoding='utf-8', errors='replace').read()
            except Exception:
                continue
            for s in re.findall(r'L?"(--[a-z0-9-]+)', t):
                allsw.add((s, os.path.relpath(p, SRC).replace('\\', '/')))
by_file = collections.Counter(f for s, f in allsw)
print('files that parse --switches:', ', '.join(f'{k}({v})' for k, v in by_file.most_common(15)))
print('distinct switches overall:', len(set(s for s, f in allsw)))

# 3. Settings touchpoints: how many files mention a sample knob
print('\n=== A knob\'s touchpoints (files mentioning it) ===')
samples = ['skyVis.fallback', 'g_skyVisFallback', 'tessy.lodPixelError', 'tessyLodPixelError', 'wakeBowFoam', 'convTemporalWeight', 'waterReflectionPrefilter', 'mieNightScale']
for s in samples:
    hits = []
    for base in (SRC, os.path.join(ROOT, 'data', 'config'), os.path.join(ROOT, 'tools', 'dist')):
        for dp, dn, fn in os.walk(base):
            for f in fn:
                if f.endswith(('.cpp', '.h', '.json', '.hlsl', '.hlsli')):
                    p = os.path.join(dp, f)
                    try:
                        t = open(p, 'r', encoding='utf-8', errors='replace').read()
                    except Exception:
                        continue
                    c = t.count(s)
                    if c:
                        hits.append(f'{os.path.basename(p)}({c})')
    print(f'{s:28s}: {len(hits)} files: ' + ', '.join(hits))

# 4. Systems:: usage per module
print('\n=== Systems::Get* usage (service locator) per subfolder ===')
c = collections.Counter(); cf = collections.Counter()
for dp, dn, fn in os.walk(SRC):
    for f in fn:
        if f.endswith(('.cpp', '.h')):
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, SRC).replace('\\', '/')
            t = open(p, 'r', encoding='utf-8', errors='replace').read()
            n = len(re.findall(r'Systems::Get\w*\(', t))
            if n:
                parts = rel.split('/')
                c['/'.join(parts[:2]) if len(parts) > 2 else parts[0]] += n
                cf[rel] += n
for k, v in c.most_common():
    print(f'  {v:4d} {k}')
print('  top files: ' + ', '.join(f'{os.path.basename(k)}({v})' for k, v in cf.most_common(15)))

# 5. Renderer / Scene / SceneRenderer public surface
print('\n=== Class surface (rough: lines with "(" ending ";" or "{" inside class body, and member fields ending "_;" / "_{" / "_ =") ===')
def surface(rel, cls):
    L = lines(rel)
    start = None
    for i, l in enumerate(L):
        if re.match(r'^class\s+' + cls + r'\b', l) and ';' not in l:
            start = i; break
    if start is None:
        print(' ?', cls); return
    end = len(L)
    for i in range(start + 1, len(L)):
        if L[i].startswith('};'):
            end = i; break
    body = L[start:end]
    methods = sum(1 for l in body if re.match(r'^\s{4}(virtual\s+|static\s+|inline\s+|const\s+|\[\[nodiscard\]\]\s+)*[\w:<>,\*&\s]+\s+\w+\s*\(', l) and not l.strip().startswith(('//', 'return', 'if', 'for', 'while')))
    fields = sum(1 for l in body if re.match(r'^\s{4}[\w:<>,\*&\s]+\s+\w+_(\s*=\s*[^;]+|\{[^;]*\}|\[[^\]]*\](\s*=\s*\{[^;]*\}|\{[^;]*\})?)?;', l))
    friends = sum(1 for l in body if 'friend ' in l)
    print(f'  {cls:26s} body={len(body):5d} lines  ~methods={methods:4d}  ~fields={fields:4d}  friends={friends}')
surface('rendering/core/Renderer.h', 'Renderer')
surface('app/scene/Scene.h', 'Scene')
surface('app/scene/SceneRenderer.h', 'SceneRenderer')
surface('rendering/renderables/RenderableObjectBase.h', 'RenderableObjectBase')
surface('rendering/renderables/RenderableObject.h', 'RenderableObject')
surface('rendering/renderables/GBufferRenderable.h', 'GBufferRenderable')
surface('rendering/shadows/ShadowGpuData.h', 'ShadowGpuData')
surface('rendering/shadows/VirtualShadowMap.h', 'VirtualShadowMap')
surface('ocean/OceanRenderable.h', 'OceanRenderable')
surface('editor/EditorController.h', 'EditorController')
surface('app/AppController.h', 'AppController')
surface('physics/PhysicsWorld.h', 'World')
surface('materials/Material.h', 'Material')

# 6. shaders
SH = os.path.join(ROOT, 'shaders')
n = 0; tot = 0; big = []
for dp, dn, fn in os.walk(SH):
    for f in fn:
        if f.endswith(('.hlsl', '.hlsli')):
            p = os.path.join(dp, f)
            k = sum(1 for _ in open(p, 'r', encoding='utf-8', errors='replace'))
            n += 1; tot += k; big.append((k, f))
print(f'\n=== shaders: {n} files, {tot} lines; top: ' + ', '.join(f'{f}({k})' for k, f in sorted(big, reverse=True)[:14]))

# 7. C++ <-> HLSL shared struct candidates: structs named *CB / *Constants in sources
cb = collections.Counter()
for dp, dn, fn in os.walk(SRC):
    for f in fn:
        if f.endswith(('.cpp', '.h')):
            t = open(os.path.join(dp, f), 'r', encoding='utf-8', errors='replace').read()
            for m in re.findall(r'struct\s+(\w*(?:CB|Constants|CBData|Params))\b', t):
                cb[m] += 1
print(f'C++ structs named *CB/*Constants/*Params: {len(cb)}; static_assert(sizeof...) in sources: ', end='')
sa = 0
for dp, dn, fn in os.walk(SRC):
    for f in fn:
        if f.endswith(('.cpp', '.h')):
            t = open(os.path.join(dp, f), 'r', encoding='utf-8', errors='replace').read()
            sa += len(re.findall(r'static_assert\s*\(\s*sizeof', t))
print(sa)

# 8. tools
print('\n=== tools/*.py checks ===')
for f in sorted(os.listdir(os.path.join(ROOT, 'tools'))):
    if f.startswith('check_') or f.endswith('.vcxproj'):
        print('  ', f)

# 9. level IO: who reads/writes level json keys
print('\n=== level section readers: files mentioning both "heightFog" and json ===')
for key in ('"heightFog"', '"timeOfDay"', '"raceGate"', '"castShadow"', '"buoyant"'):
    hits = []
    for dp, dn, fn in os.walk(SRC):
        for f in fn:
            if f.endswith(('.cpp', '.h')):
                t = open(os.path.join(dp, f), 'r', encoding='utf-8', errors='replace').read()
                if key in t:
                    hits.append(f'{f}({t.count(key)})')
    print(f'  {key:14s}: ' + ', '.join(hits))

# 10. dynamic type queries
print('\n=== type-switch smells ===')
for pat, name in ((r'\bAs(RenderableObject|GBufferRenderable|TransparentStaticMesh|OceanRenderable|ParticleEmitter|InstancedDrawBatch|Instanceable)\s*\(', 'As*() downcasts'),
                  (r'\bdynamic_cast\s*<', 'dynamic_cast'),
                  (r'\bfriend\s+(class|struct)\b', 'friend class'),
                  (r'\bconst_cast\s*<', 'const_cast'),
                  (r'WaitForPreviousFrame\s*\(', 'WaitForPreviousFrame calls'),
                  (r'WaitRenderForGame\s*\(', 'WaitRenderForGame calls'),
                  (r'\bstatic\s+[\w:<>\s\*]+\s+s_\w+|^\s*static\s+(?!const|constexpr|inline|void|bool|int|float|std::string\s+\w+\()[\w:<>\s\*&]+\s+\w+\s*(=|;|\{)', 'static locals/globals (rough)')):
    c = collections.Counter()
    for dp, dn, fn in os.walk(SRC):
        for f in fn:
            if f.endswith(('.cpp', '.h')):
                p = os.path.join(dp, f)
                rel = os.path.relpath(p, SRC).replace('\\', '/')
                t = open(p, 'r', encoding='utf-8', errors='replace').read()
                k = len(re.findall(pat, t, re.M))
                if k:
                    c[rel.split('/')[0]] += k
    print(f'  {name:32s} total={sum(c.values()):5d}  ' + ', '.join(f'{k}={v}' for k, v in c.most_common()))
