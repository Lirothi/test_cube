"""Game thread plan G5 (docs/game_thread_plan.md): the game files must reach a twinned render knob through its game copy.

A knob registered with ::knob::Register(g_x_game, g_x, "ns::g_x") (core/threading/Knob.h) has two copies: g_x is the
render half's, read by the render code; g_x_game is the game's. The hand-over (Scene::Sync, a parked render half)
exchanges them. A game file that reads or writes ns::g_x directly races the render half once it has its own thread.

    python tools/check_render_knobs.py            # exit code = findings

The game files are the ones that set knobs: the command line, --set / --sweep / MCP, graphics_settings.json,
Developer Controls, the hotkeys, the Inspector, the ocean controls. A new one goes in GAME_FILES."""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'sources')
GAME_FILES = ['app/App.cpp', 'app/GraphicsSettings.cpp', 'app/ui/DeveloperWindow.cpp', 'app/AppController.cpp',
              'app/main.cpp', 'editor/ui/InspectorPanel.cpp', 'ocean/OceanControlsWindow.cpp', 'app/ui/MemoryTab.cpp']
REG = re.compile(r'::knob::Register\(\s*\w+_game\s*,\s*\w+\s*,\s*"([\w:]+)"\s*\)')


def main():
    knobs = set()
    for dp, _, fns in os.walk(ROOT):
        for fn in fns:
            if fn.endswith('.h'):
                with open(os.path.join(dp, fn), encoding='utf-8', errors='replace') as f:
                    knobs.update(REG.findall(f.read()))
    patterns = []
    for q in sorted(knobs):
        name = q.split('::')[-1]
        if '::' in q:
            patterns.append((q, re.compile(r'(?<![\w:])' + re.escape(q) + r'(?!\w)')))
        else:  # a knob in the global namespace, reached unqualified
            patterns.append((q, re.compile(r'(?<![\w:.>])' + re.escape(name) + r'(?!\w)')))
    findings = 0
    for rel in GAME_FILES:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            continue
        with open(path, encoding='utf-8', errors='replace') as f:
            for ln, line in enumerate(f, 1):
                code = line.split('//', 1)[0]
                for q, rx in patterns:
                    if rx.search(code):
                        findings += 1
                        print(f'{rel}:{ln}: {q} is the render half\'s copy -- use {q}_game')
    print(f'{len(knobs)} twinned knobs, {findings} finding(s)')
    return findings


if __name__ == '__main__':
    sys.exit(min(main(), 255))
