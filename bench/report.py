#!/usr/bin/env python3
"""Prints benchmark results as a table, or compares two or more runs.

    bench/report.py [--all] bench/results/A.jsonl [bench/results/B.jsonl ...]

Repeated measurements of the same thing are reduced to their median. Without
--all only the headline numbers are shown. The last column compares the last
run with the first: above 1x is better.
"""
import json
import statistics
import sys
from collections import OrderedDict

# Higher is better for these; everything else is "lower is better".
HIGHER = ("gb_per_s", "mb_per_ms", "fps", ".hz")
# Bookkeeping fields, not results.
SKIP = ("bytes", "lines", "frames", "seconds", "pieces_after_random_insert")

HEADLINE = (
    "load_ms", "index_mb", "scan_all_gb_per_s", "line_start_random_ns", "line_of_random_ns",
    "fill_frame_sequential_us", "fill_frame_sequential_us_ligatures", "type_char_ns", "backspace_ns", "buffer_insert_random_ns",
    "undo_redo_step_ns", "paste_mb_per_ms", "save_ms",
    "syntax_setup_ms", "syntax_first_parse_ms", "syntax_window_query_ms", "syntax_key_main_thread_us",
    "syntax_key_reparse_ms", "syntax_key_to_visible_colours_ms", "syntax_key_to_all_colours_ms",
    "fill_frame_plain_same_rows_us", "fill_frame_highlighted_us",
    "scroll.frame_cpu_us.p50", "scroll.frame_cpu_us.p99", "scroll.fps_gpu_done", "scroll_4k.fps_gpu_done",
    "type.step_us.mean", "type.frame_cpu_us.p50", "hscroll_far.fill_us.mean", "jump.fill_us.mean",
    "startup@window.ms.sdl_init", "startup@window.ms.window", "startup@window.ms.first_frame_done",
    "startup@window.exec_to_first_frame_ms", "startup_floor@window.exec_to_first_frame_ms",
    "keypress@window.event_to_submit_us.p50", "keypress@window.event_to_submit_us.p99",
    "keypress@window.event_to_gpu_done_us.p50",
    "scroll@window.frame_cpu_us.p50", "scroll@window.frame_cpu_us.p99",
    "pace@window.hz", "pace@window.late_frames", "pace@window.frame_gap_ms.p99", "pace@window.frame_gap_ms.max",
    "pace@window.frame_cpu_us.p50", "idle@window.frames_drawn", "idle@window.cpu_percent",
)


def flatten(prefix, obj, out):
    for k, v in obj.items():
        if isinstance(v, dict):
            flatten(prefix + k + ".", v, out)
        elif isinstance(v, (int, float)) and not isinstance(v, bool):
            out[prefix + k] = v


def load(path):
    rows = OrderedDict()
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        r = json.loads(line)
        f = r.get("file", "")
        if "value" in r:  # headless: one metric per line
            rows.setdefault((f, r["bench"], r.get("unit", "")), []).append(r["value"])
            continue
        where = r["bench"] + ("@window" if r.get("target", "").startswith("window") else "")
        flat = {}
        flatten("", {k: v for k, v in r.items() if k not in ("file", "bench", "driver", "target")}, flat)
        for k, v in flat.items():
            if k in SKIP:
                continue
            rows.setdefault((f, where + "." + k, ""), []).append(v)
    return OrderedDict((k, statistics.median(v)) for k, v in rows.items() if k[1] not in SKIP)


def fmt(v):
    if v == 0:
        return "0"
    if abs(v) >= 1000:
        return f"{v:,.0f}"
    if abs(v) >= 10:
        return f"{v:.1f}"
    return f"{v:.2f}"


def main():
    args = sys.argv[1:]
    everything = "--all" in args
    paths = [a for a in args if a != "--all"]
    if not paths:
        sys.exit(__doc__)
    runs = [load(p) for p in paths]
    names = [p.rsplit("/", 1)[-1].removesuffix(".jsonl") for p in paths]
    keys = list(OrderedDict.fromkeys(k for r in runs for k in r))
    if not everything:
        keys = [k for k in keys if k[1] in HEADLINE]
    head = ["file", "metric"] + names + (["change"] if len(runs) > 1 else [])
    print("| " + " | ".join(head) + " |")
    print("|" + "|".join(["---"] * 2 + ["---:"] * (len(head) - 2)) + "|")
    for k in keys:
        f, metric, unit = k
        cells = [fmt(r[k]) if k in r else "" for r in runs]
        row = [f, metric + (f" ({unit})" if unit and unit != "count" else "")] + cells
        if len(runs) > 1:
            a, b = runs[0].get(k), runs[-1].get(k)
            if a and b:
                higher = any(h in metric.lower() for h in HIGHER)
                speedup = b / a if higher else a / b
                row.append(f"{speedup:.2f}x" if abs(speedup - 1) >= 0.03 else "~")
            else:
                row.append("")
        print("| " + " | ".join(row) + " |")


main()
