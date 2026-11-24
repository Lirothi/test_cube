import subprocess, sys, collections

ROOT = sys.argv[1]
commits = sys.argv[2:]

def mod2(p):
    parts = p.split('/')
    if parts[0] != 'sources':
        return parts[0] + '/'
    parts = parts[1:]
    if len(parts) > 2:
        return parts[0] + '/' + parts[1]
    return parts[0] if len(parts) > 1 else '(root)'

for c in commits:
    subj = subprocess.run(['git', '-C', ROOT, 'log', '-1', '--format=%h %ad %s', '--date=short', c],
                          capture_output=True, text=True, encoding='utf-8', errors='replace').stdout.strip()
    out = subprocess.run(['git', '-C', ROOT, 'show', '--numstat', '--format=', c],
                         capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
    by = collections.defaultdict(lambda: [0, 0, 0])
    files = []
    for line in out.splitlines():
        parts = line.split('\t')
        if len(parts) != 3:
            continue
        a, d, p = parts
        if a == '-':
            continue
        a, d = int(a), int(d)
        if not (p.startswith('sources/') or p.startswith('shaders/')):
            m = '(other: ' + p.split('/')[0] + ')'
        else:
            m = mod2(p)
        by[m][0] += 1; by[m][1] += a; by[m][2] += d
        if p.startswith('sources/'):
            files.append((a + d, p[8:]))
    src = {k: v for k, v in by.items() if not k.startswith('(other')}
    print('=' * 100)
    print(subj[:160])
    print(f'  sources+shaders: {sum(v[0] for v in src.values())} files in {len(src)} modules, +{sum(v[1] for v in src.values())} -{sum(v[2] for v in src.values())}')
    for k, v in sorted(src.items(), key=lambda x: -(x[1][1] + x[1][2])):
        print(f'    {k:28s} files={v[0]:3d}  +{v[1]:5d} -{v[2]:4d}')
    central = [(n, f) for n, f in files if any(f.endswith('/' + x) for x in (
        'Scene.cpp', 'Scene.h', 'App.cpp', 'AppController.cpp', 'PhysicsWorld.cpp', 'PhysicsWorld.h', 'PlaySession.cpp',
        'RenderableObject.h', 'RenderableObject.cpp', 'EditorController.cpp', 'EditorActionRegistry.cpp', 'InspectorPanel.cpp',
        'EditorMcpServer.cpp', 'JsonLevel.cpp', 'SceneObjectFactory.cpp', 'DeveloperWindow.cpp', 'intent_regression.cpp'))]
    print('  central files touched: ' + ', '.join(f'{f.split("/")[-1]}({n})' for n, f in sorted(central, reverse=True)))
