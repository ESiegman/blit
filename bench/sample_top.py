#!/usr/bin/env python3
"""Self time by function for the main thread of a macOS `sample` report.

    bench/sample_top.py REPORT [HOW_MANY]
"""
import collections
import re
import sys


def main(path, top=16):
    lines = open(path, errors="replace").read().split("\n")
    try:
        start = next(i for i, l in enumerate(lines) if l.startswith("Call graph:"))
    except StopIteration:
        sys.exit("no call graph in " + path)
    # The first thread in the report is the main one. Each line of its call
    # tree is: indentation and tree marks, a sample count, a function.
    rows, total, started = [], 0, False
    for line in lines[start + 1:]:
        m = re.match(r"^(\s+[+!:| ]*?)(\d+) (.*)$", line)
        if not m:
            if started and not line.strip():
                break
            continue
        depth, count, name = len(m.group(1)), int(m.group(2)), m.group(3)
        if "Thread_" in name:
            if started:
                break
            started, total = True, count
            continue
        if started:
            rows.append((depth, count, re.sub(r"\s+\(in ([^)]*)\).*$", r"  [\1]", name)))
    # Time in a function itself is its count less its callees' counts: the
    # rows one level deeper, up to the next row that is not deeper.
    own = collections.Counter()
    for i, (depth, count, name) in enumerate(rows):
        callees, level = 0, None
        for d, c, _ in rows[i + 1:]:
            if d <= depth:
                break
            if level is None:
                level = d
            if d == level:
                callees += c
        own[name] += count - callees
    print(f"main thread: {total} samples")
    for name, count in own.most_common(top):
        if count > 0:
            print(f"  {100.0 * count / total:5.1f}%  {name[:120]}")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 16)
