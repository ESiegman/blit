#!/usr/bin/env python3
"""One-line summary of an in-app frame benchmark.

    bench/frame.py SCENARIO FILE [blit args...]
"""
import json
import os
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
scenario, path, extra = sys.argv[1], sys.argv[2], sys.argv[3:]
# Benchmarks live in the optimized Debug build; see bench/run.sh.
build = os.environ.get("BLIT_BUILD", "build-bench")
out = subprocess.run([os.path.join(root, build, "blit"), *extra, "--bench", scenario, path],
                     capture_output=True, text=True)
try:
    r = json.loads(out.stdout)
except ValueError:
    sys.exit("no result: " + (out.stdout + out.stderr)[:300])

if r["bench"] == "pace":
    g, w = r["frame_gap_ms"], r["swapchain_wait_ms"]
    print(f"pace     {r['target']:22} {r['hz']:.1f} Hz, {r['frames']} frames, late {r['late_frames']}"
          f" ({100 * r['late_frames'] / r['frames']:.1f}%), early {r.get('early_frames', 0)}"
          f" | gap p50 {g['p50']:.2f} p99 {g['p99']:.2f} max {g['max']:.2f} ms"
          f" | wait p50 {w['p50']:.2f} ms | cpu p50 {r['frame_cpu_us']['p50']:.0f} us "
          + " ".join(f"{k} {v:.0f}" for k, v in r.get("parts_us", {}).items()))
    sys.stderr.write(out.stderr)
    sys.exit(0)
if "step_us" not in r:
    print(json.dumps(r))
    sys.exit(0)
c = r["frame_cpu_us"]
parts = " ".join(f"{k} {v:.1f}" for k, v in r.get("parts_us", {}).items())
print(f"{r['bench']:8} {r['target']:22} cpu mean {c['mean']:7.1f} p50 {c['p50']:6.1f} p99 {c['p99']:7.1f} us"
      f" | step {r['step_us']['mean']:5.2f} fill {r['fill_us']['mean']:5.2f} {parts}"
      + (f" | {r['fps_gpu_done']:.0f} fps" if "fps_gpu_done" in r else ""))
