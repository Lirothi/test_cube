"""Tessy baseline (docs/tessy_plan.md N0): the plan's three views in a RUNNING editor, tessy.vsm 0 / 1 alternated
twice per view -- Pass_VsmPageRender (GPU EMA), the GPU frame and the tessy counters of get_readouts.

    python tools/tessy_baseline.py [--port 8128] [grove|bathroom|tunnel ...]

The editor must be open with data/levels/wind_test.json, its panels closed (F2). Wind runs, VSM is forced, the
exposure is pinned after the first metering. The tunnel view needs the level's `tunnel` group enabled. set_settings
is sticky: every arm names its whole base."""
import argparse
import json
import time
import urllib.request

VIEWS = [
    ("grove", 15.0, {"position": [34, 3.5, 68], "yawDeg": 80, "pitchDeg": 8}),
    ("bathroom", 23.0, {"position": [-22.19, 1.53, 86.70], "yawDeg": -16.06, "pitchDeg": 7.49}),
    ("tunnel", 15.0, {"position": [-57.89, 2.27, 65.68], "yawDeg": 57.8, "pitchDeg": 0}),
]


def make_call(port):
    counter = [0]

    def call(name, args=None):
        counter[0] += 1
        body = json.dumps({"jsonrpc": "2.0", "id": counter[0], "method": "tools/call",
                           "params": {"name": name, "arguments": args or {}}}).encode("utf-8")
        req = urllib.request.Request(f"http://127.0.0.1:{port}/mcp", data=body, method="POST",
                                     headers={"Content-Type": "application/json",
                                              "Accept": "application/json, text/event-stream", "Connection": "close"})
        with urllib.request.urlopen(req, timeout=120) as r:
            n = r.headers.get("Content-Length")
            raw = (r.read(int(n)) if n else r.read()).decode("utf-8")
        if raw.startswith("event:") or raw.startswith("data:"):
            raw = "\n".join(line[5:].strip() for line in raw.splitlines() if line.startswith("data:"))
        msg = json.loads(raw)
        if "error" in msg:
            raise RuntimeError(f"{name}: {msg['error']}")
        return "\n".join(c.get("text", "") for c in msg.get("result", {}).get("content", []) if c.get("type") == "text")

    return call


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8128)
    ap.add_argument("--settle", type=float, default=6.0)
    ap.add_argument("views", nargs="*")
    a = ap.parse_args()
    call = make_call(a.port)

    def readouts():
        t = call("get_readouts", {})
        i = t.find("{")
        return json.loads(t[i:]) if i >= 0 else {}

    def gpu_rows(flt):
        return {r[0]: r[1] for r in json.loads(call("get_profile", {"filter": flt})).get("gpu", [])}

    for name, tod, cam in VIEWS:
        if a.views and name not in a.views:
            continue
        base = f"shadow.mode:1;tod.time:{tod};tessy.windPad:1;wind.freeze:0"
        call("reset_settings", {})
        call("set_settings", {"set": base + ";tessy.vsm:1"})
        call("set_camera", cam)
        time.sleep(4)
        ev = readouts().get("adaptedEv100")
        call("set_settings", {"set": f"exposure.autoExposure:0;exposure.manualEv100:{ev}"})
        for rnd in range(2):
            for v in (0, 1):
                call("set_settings", {"set": base + f";tessy.vsm:{v}", "resetExposure": False})
                time.sleep(a.settle)
                t = readouts().get("tessy", {})
                page = gpu_rows("VsmPage").get("Pass_VsmPageRender", 0.0)
                frame = gpu_rows("GPU.Frame").get("GPU.Frame", 0.0)
                print(f"{name} [{rnd}] tessy.vsm {v}: Pass_VsmPageRender {page:.3f} ms, GPU.Frame {frame:.2f} ms | "
                      f"pairs whole {t.get('wholePairs')} ({t.get('wholeTris', 0) / 1e6:.2f} M tris), to clusters "
                      f"{t.get('tessyPairs')} -> {t.get('clusters')} of {t.get('clustersTested')} clusters "
                      f"({t.get('clusterTris', 0) / 1e6:.2f} M tris), lost {t.get('clustersLost')}", flush=True)


if __name__ == "__main__":
    main()
