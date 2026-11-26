r"""Presses real keys in a Play run of test_cube and reads what they did from the session log (architecture plan R12.2 /
R12.3; docs/agents/boats_and_water.md, play_and_physics.md).

The game runs on a desktop of its own (CreateDesktopW: no window on the owner's screen, the keys reach no other
program); each key goes to the game's D3D12WindowClass window as WM_KEYDOWN / WM_KEYUP (PostMessage), the way App::WndProc
receives a real one. The run ends with WM_CLOSE, so the log closes with its footer.

    python tools/helm_keys.py [--exe x64/Release/test_cube.exe] [--keys Shift+F1,E] [--gap 1.0] [--expect helm|foot|none]
                              [--timeout 120] [-- <more game arguments>]

Default: wind_test, `--play` standing on small_yacht 001's deck (52.66, 115.45), then Shift+F1 (a scripted --play leaves
the mouse free, and with it the keys: Shift+F1 hands them to the game) and E. The log's `play:` lines are printed with
their frame numbers. --expect:
  helm  -- "play: at the helm" and no "play: left the helm" after it (one E takes the helm and stays: R12.2's test);
  foot  -- the last helm line is "left the helm" (or there is none);
  none  -- no helm line at all (a key the UI took never reached the player).
Exit code: 0 when the expectation holds and the session ended cleanly, 1 otherwise, 2 for a run that did not start.
A step of --keys is
  a key or chord     E, F1, Escape, Space, W, ... or Shift+F1: pressed together, let go together;
  hold:<key>:<s>     a key held that long (D held 1 s walks right);
  wait:<s>           a longer wait;
  set:<name>:<v>[;<name>:<v>]   with --mcp-port, MCP set_settings -- what a script says in place of the keys
                     (player.testInteract:1 = the action of E, through the keys' own channel since R12.3);
  read:player        with --mcp-port, MCP get_readouts: prints the player's feet.
  read:camera        with --mcp-port, MCP get_camera: prints the camera's position.
--mcp-port <n> needs a build with the editor (x64/Release_Editor); the run gets --editor (the editor's frame serves
MCP calls) and listens on <n> instead of the owner's 8128. Every
run puts imgui.ini and editor_state.json back as they were (a run of the game rewrites them).
"""
import argparse
import ctypes
import json
import pathlib
import re
import shutil
import socket
import sys
import time
import urllib.request
from ctypes import wintypes

REPO = pathlib.Path(__file__).resolve().parent.parent
user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

VK = {"Shift": 0x10, "Ctrl": 0x11, "Alt": 0x12, "Escape": 0x1B, "Space": 0x20, "Tab": 0x09, "Enter": 0x0D}
VK.update({f"F{i}": 0x6F + i for i in range(1, 13)})
VK.update({chr(c): c for c in range(ord("A"), ord("Z") + 1)})
VK.update({str(d): 0x30 + d for d in range(10)})


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR), ("lpDesktop", wintypes.LPWSTR), ("lpTitle", wintypes.LPWSTR),
                ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD), ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD),
                ("dwXCountChars", wintypes.DWORD), ("dwYCountChars", wintypes.DWORD), ("dwFillAttribute", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wintypes.HANDLE), ("hStdOutput", wintypes.HANDLE),
                ("hStdError", wintypes.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE), ("dwProcessId", wintypes.DWORD),
                ("dwThreadId", wintypes.DWORD)]


user32.CreateDesktopW.restype = wintypes.HANDLE
user32.CreateDesktopW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.MapVirtualKeyW.argtypes = [wintypes.UINT, wintypes.UINT]
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumDesktopWindows.argtypes = [wintypes.HANDLE, WNDENUMPROC, wintypes.LPARAM]
WM_KEYDOWN, WM_KEYUP, WM_CLOSE = 0x0100, 0x0101, 0x0010


def find_window(desktop, pid):
    found = []

    def on_window(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, name, 256)
        if owner.value == pid and name.value == "D3D12WindowClass":
            found.append(hwnd)
        return True

    user32.EnumDesktopWindows(desktop, WNDENUMPROC(on_window), 0)
    return found[0] if found else None


def post_chord(hwnd, chord):
    vks = [VK[k] for k in chord.split("+")]
    for vk in vks:
        scan = user32.MapVirtualKeyW(vk, 0)
        user32.PostMessageW(hwnd, WM_KEYDOWN, vk, 1 | (scan << 16))
    time.sleep(0.15)  # several frames down: the press is latched by the frame after it arrives
    for vk in reversed(vks):
        scan = user32.MapVirtualKeyW(vk, 0)
        user32.PostMessageW(hwnd, WM_KEYUP, vk, 1 | (scan << 16) | (1 << 30) | (1 << 31))


def post_key(hwnd, vk, up):
    scan = user32.MapVirtualKeyW(vk, 0)
    user32.PostMessageW(hwnd, WM_KEYUP if up else WM_KEYDOWN, vk, 1 | (scan << 16) | ((3 << 30) if up else 0))


def mcp(port, name, args, timeout=30):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": name, "arguments": args}}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}/mcp", data=body, method="POST",
                                 headers={"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                                          "Connection": "close"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw = r.read().decode("utf-8")
    if raw.startswith("event:") or raw.startswith("data:"):
        raw = "\n".join(line[5:].strip() for line in raw.splitlines() if line.startswith("data:"))
    msg = json.loads(raw)
    if "error" in msg:
        raise RuntimeError(f"{name}: {msg['error']}")
    return "\n".join(c.get("text", "") for c in msg.get("result", {}).get("content", []) if c.get("type") == "text")


def log_lines(log):
    return log.read_text(encoding="utf-8", errors="replace").splitlines() if log.exists() else []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(REPO / "x64" / "Release" / "test_cube.exe"))
    ap.add_argument("--keys", default="Shift+F1,E")
    ap.add_argument("--gap", type=float, default=1.0, help="seconds between steps")
    ap.add_argument("--expect", choices=["helm", "foot", "none"], default=None)
    ap.add_argument("--mcp-port", type=int, default=0, help="an editor build's MCP port for set: / read: steps")
    ap.add_argument("--cam-pos", default="52.66,3.2,115.45", help="where Play starts (default: small_yacht 001's deck)")
    ap.add_argument("--cam-rot", default="0,0,0,1", help="the camera's quaternion there (yaw 90 deg = 0,0.7071068,0,0.7071068)")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("game", nargs="*", help="more game arguments (after --)")
    a = ap.parse_args()
    a.exe = str(pathlib.Path(a.exe) if pathlib.Path(a.exe).is_absolute() else (REPO / a.exe).resolve())
    steps = a.keys.split(",")
    for step in steps:
        kind = step.split(":", 1)[0]
        if kind in ("set", "read") and not a.mcp_port:
            print(f"helm_keys: {step!r} needs --mcp-port (and an editor build)")
            return 2
        if kind == "hold":
            step = step.split(":")[1]
        elif kind in ("wait", "set", "read"):
            continue
        unknown = [k for k in step.split("+") if k not in VK]
        if unknown:
            print(f"helm_keys: unknown key {unknown} in {step!r}")
            return 2
    if a.mcp_port:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            if s.connect_ex(("127.0.0.1", a.mcp_port)) == 0:
                print(f"helm_keys: port {a.mcp_port} is taken")
                return 2

    game = ["--level=data/levels/wind_test.json", "--play", f"--cam-pos={a.cam_pos}", f"--cam-rot={a.cam_rot}",
            "--wind-freeze=3", "--time=15:00", "--time-freeze"] + (["--editor", f"--mcp-port={a.mcp_port}"] if a.mcp_port else []) + a.game
    saved = {}
    for name in ("imgui.ini", "editor_state.json"):
        if (REPO / name).exists():
            saved[name] = (REPO / name).read_bytes()
    desktop = user32.CreateDesktopW("claude_helm_keys", None, None, 0, 0x10000000, None)
    if not desktop:
        print("helm_keys: CreateDesktop failed", ctypes.get_last_error())
        return 2
    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(si)
    si.lpDesktop = "claude_helm_keys"
    pi = PROCESS_INFORMATION()
    line = ctypes.create_unicode_buffer('"%s" %s' % (a.exe, " ".join(game)))
    latest = REPO / "logs" / "latest.txt"
    before = latest.read_text().strip() if latest.exists() else ""
    if not kernel32.CreateProcessW(None, line, None, None, False, 0, None, str(REPO), ctypes.byref(si), ctypes.byref(pi)):
        print("helm_keys: CreateProcess failed", ctypes.get_last_error())
        return 2
    t0 = time.time()
    try:
        # The run's own log (logs/latest.txt names it once the session opened), then its "play: the player at" line.
        log = None
        while time.time() - t0 < a.timeout:
            now = latest.read_text().strip() if latest.exists() else ""
            if now and now != before and f"_{pi.dwProcessId}_" in now:
                log = REPO / now
                if any("play: the player at" in l for l in log_lines(log)):
                    break
            if kernel32.WaitForSingleObject(wintypes.HANDLE(pi.hProcess), 0) == 0:
                print("helm_keys: the game ended before Play started")
                return 2
            time.sleep(0.25)
        else:
            print("helm_keys: no Play within the timeout")
            return 2
        time.sleep(a.gap)
        hwnd = find_window(desktop, pi.dwProcessId)
        if not hwnd:
            print("helm_keys: no D3D12WindowClass window of the game")
            return 2
        for step in steps:
            kind, _, rest = step.partition(":")
            if kind == "wait":
                time.sleep(float(rest))
                continue
            if kind == "set":
                answer = mcp(a.mcp_port, "set_settings", {"set": rest, "resetExposure": False})
                print(f"helm_keys: {time.time() - t0:6.2f} s  set {rest}: {answer[:120]}")
            elif kind == "read" and rest == "camera":
                text = mcp(a.mcp_port, "get_camera", {})
                m = re.search(r'"position":\s*\[([^\]]*)\]', text)
                print(f"helm_keys: {time.time() - t0:6.2f} s  read camera position {m.group(1) if m else text[:120]}")
                continue
            elif kind == "read":
                text = mcp(a.mcp_port, "get_readouts", {})
                m = re.search(r'"player":\{[^}]*"feet":\[([^\]]*)\]', text)
                print(f"helm_keys: {time.time() - t0:6.2f} s  read player feet {m.group(1) if m else '(none)'}")
                continue
            elif kind == "hold":
                key, seconds = rest.split(":")
                post_key(hwnd, VK[key], False)
                time.sleep(float(seconds))
                post_key(hwnd, VK[key], True)
                print(f"helm_keys: {time.time() - t0:6.2f} s  held {key} {seconds} s")
            else:
                post_chord(hwnd, step)
                print(f"helm_keys: {time.time() - t0:6.2f} s  {step}")
            time.sleep(a.gap)
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        if kernel32.WaitForSingleObject(wintypes.HANDLE(pi.hProcess), 60000) != 0:
            print("helm_keys: the game did not close; killed")
            kernel32.TerminateProcess(wintypes.HANDLE(pi.hProcess), 99)
        exit_code = wintypes.DWORD()
        kernel32.GetExitCodeProcess(wintypes.HANDLE(pi.hProcess), ctypes.byref(exit_code))
        print(f"helm_keys: the game exited with 0x{exit_code.value:08X}")
    finally:
        if kernel32.WaitForSingleObject(wintypes.HANDLE(pi.hProcess), 0) != 0:
            kernel32.TerminateProcess(wintypes.HANDLE(pi.hProcess), 99)
            kernel32.WaitForSingleObject(wintypes.HANDLE(pi.hProcess), 5000)
        user32.CloseDesktop(wintypes.HANDLE(desktop))
        for name, data in saved.items():
            (REPO / name).write_bytes(data)

    lines = log_lines(log)
    play = [l for l in lines if "][app][" in l and "play:" in l]
    for l in play:
        m = re.match(r"\[([^\]]*)\]\[[^\]]*\]\[[^\]]*\]\[([^\]]*)\]\s*(.*)$", l)
        print(f"  frame {m.group(2):>6}  {m.group(3)[:110]}" if m else "  " + l[:140])
    clean = any("session end: clean shutdown" in l for l in lines)
    helm = [l for l in play if "at the helm" in l or "left the helm" in l]
    if a.expect == "helm":
        ok = bool(helm) and "at the helm" in helm[-1] and not any("left the helm" in l for l in helm)
    elif a.expect == "foot":
        ok = not helm or "left the helm" in helm[-1]
    elif a.expect == "none":
        ok = not helm
    else:
        ok = True
    print(f"helm_keys: {'PASS' if ok and clean else 'FAIL'} (expect {a.expect or 'nothing'}; "
          f"{'clean shutdown' if clean else 'NO clean shutdown'}) -- {log.relative_to(REPO).as_posix()}")
    return 0 if ok and clean else 1


if __name__ == "__main__":
    sys.exit(main())
