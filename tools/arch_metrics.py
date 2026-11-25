#!/usr/bin/env python3
"""Architecture plan R0.1: the numbers of docs/architecture_review_plan.md in one command.

Sections 1 and 3 of the plan (the map, the include graph, header reach, big files and functions) and the counters
that section 11 measures its steps by (Systems::Get outside app/, the extern of App.h, the setting-name chains of
App.cpp, static_assert(sizeof), As*() downcasts, string action names, test* fields, ...). Each counter names the step
that drives it to zero or watches it. Read-only; prints to stdout.

    python tools/arch_metrics.py                       # everything (a few seconds)
    python tools/arch_metrics.py --mechanic <commit>.. # what a commit touched, by subfolder (section 9.4)
    python tools/arch_metrics.py --root <tree>         # another tree, e.g. `git archive <commit> | tar -x -C <tree>`

The include graph and the layer table are tools/check_layers.py's.
"""

import collections
import pathlib
import re
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_layers  # noqa: E402

REPO = check_layers.REPO


def mod1(rel):
    return rel.split("/")[0] if "/" in rel else "(root)"


def mod2(rel):
    parts = rel.split("/")
    if len(parts) > 2:
        return parts[0] + "/" + parts[1]
    return parts[0] if len(parts) > 1 else "(root)"


def strip_comments(lines):
    """Lines with // and /* */ comment text removed (string literals are not parsed: good enough for counting)."""
    out = []
    in_block = False
    for line in lines:
        text = ""
        i = 0
        while i < len(line):
            if in_block:
                end = line.find("*/", i)
                if end < 0:
                    i = len(line)
                else:
                    in_block = False
                    i = end + 2
                continue
            start_block = line.find("/*", i)
            start_line = line.find("//", i)
            if start_line >= 0 and (start_block < 0 or start_line < start_block):
                text += line[i:start_line]
                break
            if start_block >= 0:
                text += line[i:start_block]
                in_block = True
                i = start_block + 2
                continue
            text += line[i:]
            break
        out.append(text)
    return out


class Tree:
    def __init__(self, root=REPO):
        self.root = root
        self.files = check_layers.load_sources(root / "sources")
        self.code = {rel: strip_comments(lines) for rel, lines in self.files.items()}
        self.edges = check_layers.include_edges(self.files)
        self.out = collections.defaultdict(set)
        for source, target, _ in self.edges:
            self.out[source].add(target)

    def count(self, pattern, scope=None, comments=False, flags=0):
        """(total matches, {file: matches}) of a regex over code (comments stripped unless comments=True)."""
        rx = re.compile(pattern, flags)
        per = {}
        for rel in self.files:
            if scope and not scope(rel):
                continue
            lines = self.files[rel] if comments else self.code[rel]
            n = sum(len(rx.findall(line)) for line in lines)
            if n:
                per[rel] = n
        return sum(per.values()), per


def section(title):
    print()
    print(f"=== {title} ===")


def top(per, n=6):
    return ", ".join(f"{rel.split('/')[-1]}({k})" for rel, k in sorted(per.items(), key=lambda x: (-x[1], x[0]))[:n])


def print_map(tree):
    section("1. Map (plan section 1)")
    groups = collections.OrderedDict([
        ("rendering/", ["rendering"]), ("editor/", ["editor"]), ("app/", ["app"]), ("core/", ["core"]),
        ("ocean/", ["ocean"]), ("physics/", ["physics"]),
        ("materials, text, vfx, assets, input, ui", ["materials", "text", "vfx", "assets", "input", "ui"]),
        ("(root)", ["(root)"]),
    ])
    for name, mods in groups.items():
        rels = [rel for rel in tree.files if mod1(rel) in mods]
        lines = sum(sum(1 for l in tree.files[rel] if l.strip()) for rel in rels)
        print(f"  {name:42s} files={len(rels):4d}  non-blank lines={lines:7d}")
    cpp = [rel for rel in tree.files if rel.endswith(".cpp")]
    total = sum(sum(1 for l in lines if l.strip()) for lines in tree.files.values())
    print(f"  total: {len(tree.files)} files, {total} non-blank lines; {len(cpp)} .cpp, {len(tree.files) - len(cpp)} headers")
    shaders = [p for p in (tree.root / "shaders").rglob("*") if p.suffix in (".hlsl", ".hlsli", ".h") and p.is_file()]
    shader_lines = sum(len(p.read_text(encoding="utf-8", errors="replace").split("\n")) for p in shaders)
    print(f"  shaders/: {len(shaders)} files, {shader_lines} lines")
    shader_list = tree.root / "tools" / "dist" / "shader_list.txt"
    if shader_list.exists():
        entries = [l for l in shader_list.read_text(encoding="utf-8").split("\n") if l.startswith("shaders/")]
        print(f"  tools/dist/shader_list.txt: {len(entries)} permutations (R5.2)")
    vcxproj = tree.root / "test_cube.vcxproj"
    if vcxproj.exists():
        configs = re.findall(r'ProjectConfiguration Include="([^"]+)"',
                             vcxproj.read_text(encoding="utf-8", errors="replace"))
        print(f"  test_cube.vcxproj: {len(configs)} configurations ({', '.join(configs)})")
    passes = render_pass_count(tree)
    if passes is not None:
        print(f"  RenderPass entries (rendering/core/RenderPass.h): {passes}")


def render_pass_count(tree):
    code = tree.code.get("rendering/core/RenderPass.h")
    if not code:
        return None
    inside = False
    n = 0
    for line in code:
        if re.match(r"\s*enum class RenderPass\b", line):
            inside = True
            continue
        if inside:
            if line.strip().startswith("}"):
                break
            n += len(re.findall(r"\b(Main_\w+|[A-Z]\w*)\s*(?:=\s*[^,]+)?,", line))
    return n


def sccs(nodes, graph):
    index, low, on_stack, stack, result = {}, {}, set(), [], []
    counter = [0]
    sys.setrecursionlimit(10000)

    def visit(v):
        index[v] = low[v] = counter[0]
        counter[0] += 1
        stack.append(v)
        on_stack.add(v)
        for w in graph.get(v, ()):
            if w not in index:
                visit(w)
                low[v] = min(low[v], low[w])
            elif w in on_stack:
                low[v] = min(low[v], index[w])
        if low[v] == index[v]:
            comp = []
            while True:
                w = stack.pop()
                on_stack.discard(w)
                comp.append(w)
                if w == v:
                    break
            result.append(sorted(comp))

    for v in sorted(nodes):
        if v not in index:
            visit(v)
    return result


def print_layers(tree):
    section("2. Layers (plan F1, section 4; tools/check_layers.py)")
    graph = collections.defaultdict(set)
    module_edges = collections.Counter()
    for source, target, _ in tree.edges:
        a, b = mod2(source), mod2(target)
        if a != b:
            graph[a].add(b)
        if mod1(source) != mod1(target):
            module_edges[(mod1(source), mod1(target))] += 1
    nodes = set(mod2(rel) for rel in tree.files)
    comps = sorted((c for c in sccs(nodes, graph) if len(c) > 1), key=len, reverse=True)
    print(f"  subfolders: {len(nodes)}; strongly connected components larger than one: "
          + ", ".join(str(len(c)) for c in comps))
    if comps:
        print(f"  the largest ({len(comps[0])}): " + ", ".join(comps[0]))
    print("  module -> module include edges (file to file) of the F1 table:")
    for a, b in (("rendering", "app"), ("materials", "rendering"), ("rendering", "materials"), ("core", None),
                 ("rendering", "ocean"), ("rendering", "physics"), ("rendering", "vfx"), ("physics", "ocean"),
                 ("ocean", "app"), ("vfx", "app"), ("text", "app"), ("app", "editor")):
        if b is None:
            n = sum(k for (s, t), k in module_edges.items() if s == a and t != a)
            print(f"    {a} -> anything above: {n}")
        else:
            print(f"    {a} -> {b}: {module_edges.get((a, b), 0)}")
    check_layers.summary(tree.files, check_layers.read_allowance() if tree.root == REPO else [])


def print_headers(tree):
    section("3. Header reach (plan F5)")
    cpps = [rel for rel in tree.files if rel.endswith(".cpp")]
    closure_size = {}
    rebuild = collections.Counter()
    total_headers = 0
    total_lines = 0
    for cpp in cpps:
        seen = set()
        todo = [cpp]
        while todo:
            x = todo.pop()
            for t in tree.out.get(x, ()):
                if t not in seen:
                    seen.add(t)
                    todo.append(t)
        closure_size[cpp] = len(seen)
        total_headers += len(seen)
        total_lines += sum(len(tree.files[h]) for h in seen)
        for h in seen:
            rebuild[h] += 1
    print(f"  a .cpp includes on average {total_headers / len(cpps):.0f} project headers, "
          f"{total_lines / len(cpps):.0f} lines")
    print(f"  headers by the .cpp a change rebuilds (of {len(cpps)}):")
    for h, n in rebuild.most_common(14):
        print(f"    {n:4d} ({100 * n // len(cpps):2d} %)  {len(tree.files[h]):5d} lines  {h}")
    for h in ("rendering/core/Renderer.h", "app/camera/Camera.h", "rendering/view/Camera.h", "app/App.h",
              "core/task/TaskSystem.h", "app/scene/SceneFrameData.h", "rendering/scene/SceneFrameData.h"):
        if h in tree.files:
            seen = set()
            todo = [h]
            while todo:
                x = todo.pop()
                for t in tree.out.get(x, ()):
                    if t not in seen:
                        seen.add(t)
                        todo.append(t)
            print(f"    {h}: rebuilds {rebuild[h]} .cpp, drags {len(seen)} project headers")


def function_blocks(tree):
    """[(length, file, line, header)] of brace blocks opened and closed at column 0 in .cpp (functions)."""
    blocks = []
    for rel, lines in tree.files.items():
        if not rel.endswith(".cpp"):
            continue
        start = None
        for i, line in enumerate(lines):
            if line.startswith("{"):
                start = i
            elif line.startswith("}") and start is not None:
                j = start - 1
                while j >= 0 and not lines[j].strip():
                    j -= 1
                k = j
                while k >= 0 and lines[k].strip() and not lines[k].startswith("}") and j - k < 6:
                    k -= 1
                header = " ".join(l.strip() for l in lines[k + 1:j + 1])
                if "namespace" not in header[:20]:
                    blocks.append((i - start, rel, start + 1, header[:100]))
                start = None
    return sorted(blocks, reverse=True)


def print_sizes(tree):
    section("4. Big files and functions (plan F2, F12)")
    for rel, lines in sorted(tree.files.items(), key=lambda x: -len(x[1]))[:15]:
        print(f"  {len(lines):6d}  {rel}")
    blocks = function_blocks(tree)
    print(f"  functions longer than 300 lines: {sum(1 for b in blocks if b[0] > 300)}; "
          f"longer than 750: {sum(1 for b in blocks if b[0] > 750)}")
    for n, rel, line, header in blocks[:15]:
        print(f"    {n:5d}  {rel}:{line}  {header}")
    print("  class surface (regex, an order of magnitude; plan F2):")
    for rel, cls in (("rendering/core/Renderer.h", "Renderer"), ("rendering/shadows/ShadowGpuData.h", "ShadowGpuData"),
                     ("rendering/shadows/VirtualShadowMap.h", "VirtualShadowMap"), ("app/scene/Scene.h", "Scene"),
                     ("rendering/renderables/RenderableObject.h", "RenderableObject"),
                     ("app/scene/SceneRenderer.h", "SceneRenderer"), ("editor/EditorController.h", "EditorController")):
        surface = class_surface(tree.files.get(rel, []), cls)
        if surface:
            print(f"    {cls:20s} {surface[0]:5d} lines  ~{surface[1]:3d} methods  ~{surface[2]:3d} fields  ({rel})")


def class_surface(lines, cls):
    """(body lines, ~methods, ~fields) of a class declared at column 0."""
    start = next((i for i, l in enumerate(lines) if re.match(r"^class\s+" + cls + r"\b", l) and ";" not in l), None)
    if start is None:
        return None
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("};")), len(lines))
    body = lines[start:end]
    method = re.compile(r"^\s{4}(virtual\s+|static\s+|inline\s+|const\s+|\[\[nodiscard\]\]\s+)*[\w:<>,\*&\s]+\s+\w+\s*\(")
    field = re.compile(r"^\s{4}[\w:<>,\*&\s]+\s+\w+_(\s*=\s*[^;]+|\{[^;]*\}|\[[^\]]*\](\s*=\s*\{[^;]*\}|\{[^;]*\})?)?;")
    methods = sum(1 for l in body if method.match(l)
                  and not l.strip().startswith(("//", "return", "if", "for", "while")))
    fields = sum(1 for l in body if field.match(l))
    return len(body), methods, fields


LITERAL_RE =re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])+\'')


def function_range(code, name):
    """(first, last) line index of the body of the first definition of `name` (comment-stripped lines; braces
    counted outside string and character literals)."""
    header = re.compile(r"^\s*[\w:<>,\*&\s]*\b" + re.escape(name) + r"\s*\(")
    for i, line in enumerate(code):
        if not header.match(line):
            continue
        depth = 0
        opened = None
        for k in range(i, len(code)):
            text = LITERAL_RE.sub('""', code[k])
            if opened is None and ";" in text and "{" not in text:
                break  # a declaration
            for ch in text:
                if ch == "{":
                    depth += 1
                    if opened is None:
                        opened = k
                elif ch == "}":
                    depth -= 1
                    if opened is not None and depth == 0:
                        return opened, k
    return None


def print_counters(tree):
    section("5. Counters of section 11 (the step that moves each)")
    rows = []

    def add(step, label, total, per, note=""):
        rows.append((step, label, total, per, note))

    not_app = lambda rel: not rel.startswith(("app/", "editor/"))
    t, per = tree.count(r"Systems::Get\w*\(", not_app)
    add("R1.3", "Systems::Get* outside app/ and editor/ (code)", t, per)
    t, per = tree.count(r"Systems::Get\w*\(", not_app, comments=True)
    add("R1.3", "  ... with commented-out lines", t, per)
    t, per = tree.count(r'^\s*#\s*include\s*"app/', lambda rel: rel.startswith("rendering/"), comments=True)
    add("R1.4-5", "#include \"app/...\" in rendering/", t, per)
    t, per = tree.count(r"^\s*extern\b", lambda rel: rel == "app/App.h")
    add("R3.1", "extern in app/App.h", t, per)
    t, per = tree.count(r'"(--[a-z0-9][a-z0-9-]*)', lambda rel: rel == "app/main.cpp", comments=True)
    switches_main = set()
    switches_all = set()
    rx = re.compile(r'L?"(--[a-z0-9][a-z0-9-]*)')
    for rel, lines in tree.files.items():
        for line in lines:
            for s in rx.findall(line):
                switches_all.add(s)
                if rel == "app/main.cpp":
                    switches_main.add(s)
    add("R3.2", "distinct --switches in main.cpp / in sources", len(switches_main), {}, f"{len(switches_all)} overall")
    main_lines = tree.code.get("app/main.cpp", [])
    winmain = function_range(main_lines, "WinMain")
    if winmain:
        add("R3.2", "WinMain length (lines)", winmain[1] - winmain[0], {})
    app_lines = tree.code.get("app/App.cpp", [])
    for fn, rx_name in (("ApplySweepValue", r'setting\s*==\s*"'), ("ReadSweepValue", r'name\s*==\s*"')):
        rng = function_range(app_lines, fn)
        if rng:
            body = tree.code["app/App.cpp"][rng[0]:rng[1]]
            n = sum(len(re.findall(rx_name, line)) for line in body)
            add("R2.0", f"{fn} name comparisons (App.cpp:{rng[0] + 1}-{rng[1] + 1})", n, {})
    names = set()
    for line in tree.code.get("app/App.cpp", []):
        names.update(re.findall(r'"([a-zA-Z]+\.[A-Za-z0-9_.]+)"', line))
    add("R2.0", "distinct \"ns.key\" literals in App.cpp", len(names), {})
    t, per = tree.count(r"::knob::Register\(", lambda rel: rel != "core/threading/Knob.h")
    add("R2.1", "knob registrations (KNOB twins)", t, per)
    t, per = tree.count(r"static_assert\s*\(\s*sizeof", comments=True)
    add("R5.3", "static_assert(sizeof ...)", t, per)
    t, per = tree.count(r"\bstruct\s+\w*(?:CB|Constants|CBData|Params)\b")
    add("R5.3", "C++ structs *CB / *Constants / *Params", t, per)
    t, per = tree.count(r"\bRaceRef\(\)")
    add("R6.2", "RaceRef() calls", t, per)
    as_names = r"As(?:RenderableObject|GBufferRenderable|TransparentStaticMesh|OceanRenderable|ParticleEmitter|" \
               r"InstancedDrawBatch|Instanceable)\(\)"
    t, per = tree.count(r"(?:->|\.)\s*" + as_names)
    t2, per2 = tree.count(r"\b" + as_names)
    add("R7.3", "As*() downcast calls (through -> or .)", t, per, f"{t2} occurrences with declarations")
    t, per = tree.count(r"\bWaitForPreviousFrame\s*\(")
    add("R16.10", "WaitForPreviousFrame( (code, with the definition)", t, per)
    t, per = tree.count(r"(?:\.|->)value\(")
    add("R9.6", "json value( calls", t, per)
    t, per = tree.count(r"\bjsonfield::Get\b")
    add("R9.6", "jsonfield::Get", t, per)
    t, per = tree.count(r"\bdrawPanel\s*\(", lambda rel: rel == "editor/EditorController.cpp")
    add("R10.1", "drawPanel( in EditorController.cpp", t, per)
    t, per = tree.count(r"^\s*#\s*(?:if|ifdef|ifndef|elif).*\bWITH_EDITOR\b", comments=True)
    by_mod = collections.Counter()
    for rel, k in per.items():
        by_mod[mod1(rel)] += k
    add("R11", "#if WITH_EDITOR", t, per, ", ".join(f"{m}={k}" for m, k in by_mod.most_common()))
    t, per = tree.count(r'(?:WasActionPressed|IsActionDown|GetActionAxis)\("')
    add("R12.1", "action names as string literals", t, per)
    t, per = tree.count(r"\btest[A-Z]\w*", comments=True)
    add("R12.3", "test* mentions", t, per)
    for step, label, total, per, note in rows:
        extra = f"  in {len(per)} files: {top(per)}" if per else ""
        note = f"  [{note}]" if note else ""
        print(f"  {step:7s} {label:58s} {total:5d}{note}{extra}")


def mechanic(commits):
    def subfolder(path):
        parts = path.split("/")
        if parts[0] != "sources":
            return parts[0] + "/"
        return mod2("/".join(parts[1:]))

    for commit in commits:
        subject = subprocess.run(["git", "-C", str(REPO), "log", "-1", "--format=%h %ad %s", "--date=short", commit],
                                 capture_output=True, text=True, encoding="utf-8", errors="replace").stdout.strip()
        numstat = subprocess.run(["git", "-C", str(REPO), "show", "--numstat", "--format=", commit],
                                 capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
        by = collections.defaultdict(lambda: [0, 0, 0])
        for line in numstat.splitlines():
            parts = line.split("\t")
            if len(parts) != 3 or parts[0] == "-":
                continue
            added, deleted, path = int(parts[0]), int(parts[1]), parts[2]
            if not path.startswith(("sources/", "shaders/")):
                continue
            row = by[subfolder(path)]
            row[0] += 1
            row[1] += added
            row[2] += deleted
        print(subject[:150])
        print(f"  sources+shaders: {sum(v[0] for v in by.values())} files in {len(by)} subfolders, "
              f"+{sum(v[1] for v in by.values())} -{sum(v[2] for v in by.values())}")
        for name, (n, a, d) in sorted(by.items(), key=lambda x: -(x[1][1] + x[1][2])):
            print(f"    {name:28s} files={n:3d}  +{a:5d} -{d:4d}")


def main(argv):
    if argv and argv[0] == "--mechanic":
        mechanic(argv[1:])
        return 0
    started = time.perf_counter()
    root = REPO
    if "--root" in argv:
        root = pathlib.Path(argv[argv.index("--root") + 1]).resolve()
    tree = Tree(root)
    print_map(tree)
    print_layers(tree)
    print_headers(tree)
    print_sizes(tree)
    print_counters(tree)
    print(f"\narch_metrics: {time.perf_counter() - started:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
