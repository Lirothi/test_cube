# Live probe of the model watchdog after it was changed to HOLD its server (review 10.15, fix of 2026-11-24).
# Two stand-in "servers" (python processes of this script), never a real llama-server; every process started here is
# ended by its own pid. The watchdog shows a tray icon for the seconds it lives.
#   python tools/reaper_probe.py <repo root> <scratch folder>   (needs x64/ModelReaper/model_reaper.exe built)
import pathlib
import subprocess
import sys
import time

repo = pathlib.Path(sys.argv[1]).resolve()
scratch = pathlib.Path(sys.argv[2]).resolve()
scratch.mkdir(parents=True, exist_ok=True)
reaper_exe = repo / "x64" / "ModelReaper" / "model_reaper.exe"
failures = []
started = []


def check(ok, what):
    print(("PASS  " if ok else "FAIL  ") + what)
    if not ok:
        failures.append(what)


def log_text(path):
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def start_reaper(port, pid, name):
    log = scratch / f"{name}.log"
    if log.exists():
        log.unlink()
    p = subprocess.Popen([str(reaper_exe), f"--reap-model=127.0.0.1:{port}", f"--reap-model-pid={pid}", "--reap-idle=30",
                          f"--log-file={log}"], cwd=scratch)
    started.append(p)
    return p, log


SLEEPER = "import time\ntime.sleep(300)\n"
# Answers /metrics for `sys.argv[2]` seconds, then stops listening and stays ALIVE: "it answered before and does not now".
SERVER = r"""
import http.server, sys, threading, time
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"llamacpp:requests_total 1\n"
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
srv = http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
print("listening", flush=True)
time.sleep(float(sys.argv[2]))
srv.shutdown(); srv.server_close()
print("silent", flush=True)
time.sleep(300)
"""

try:
    # ---- A. the server goes away by itself: the watch ends, and ends nothing ----
    sleeper = subprocess.Popen([sys.executable, "-c", SLEEPER])
    started.append(sleeper)
    reaper, log = start_reaper(59123, sleeper.pid, "reaper_a")
    time.sleep(3.0)
    check(reaper.poll() is None, "A: the watch runs while its server lives")
    sleeper.kill()
    sleeper.wait()
    try:
        code = reaper.wait(timeout=8)
    except subprocess.TimeoutExpired:
        code = None
    check(code == 0, f"A: the watch ends within seconds of its server's death (exit {code})")
    check("server is already gone" in log_text(log), "A: ... saying the server is already gone")

    # ---- B. the server stops answering: the HELD process is ended ----
    server = subprocess.Popen([sys.executable, "-c", SERVER, "59124", "4"], stdout=subprocess.PIPE, text=True)
    started.append(server)
    server.stdout.readline()
    reaper, log = start_reaper(59124, server.pid, "reaper_b")
    try:
        code = reaper.wait(timeout=30)
    except subprocess.TimeoutExpired:
        code = None
    text = log_text(log)
    check(code == 0 and "server stopped answering; retiring it" in text, f"B: a server that stops answering is retired (exit {code})")
    try:
        server_code = server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server_code = None
    check(server_code is not None, f"B: ... and its process is ended (exit {server_code})")
    check("could not stop llama-server" not in text, "B: ... without a failed kill")

    # ---- C. a server that was gone before the watch began: nothing to hold, nothing ended ----
    gone = subprocess.Popen([sys.executable, "-c", "pass"])
    gone.wait()
    bystander = subprocess.Popen([sys.executable, "-c", SLEEPER])  # stands for whoever gets a freed number next
    started.append(bystander)
    reaper, log = start_reaper(59125, gone.pid, "reaper_c")
    try:
        code = reaper.wait(timeout=8)
    except subprocess.TimeoutExpired:
        code = None
    check(code == 0 and "server is already gone" in log_text(log), f"C: a server gone before the watch began ends the watch (exit {code})")
    check(bystander.poll() is None, "C: ... and no other process is touched")
finally:
    for p in started:
        if p.poll() is None:
            p.kill()
            p.wait()

print()
print("reaper probe:", "PASS" if not failures else f"FAIL ({len(failures)})")
for name in ("reaper_a", "reaper_b", "reaper_c"):
    for line in log_text(scratch / f"{name}.log").splitlines():
        if "model reaper" in line:
            print(f"  [{name}] {line[:170]}")
sys.exit(len(failures))
