#!/bin/sh
# Median startup timeline over N launches: bench/startup.sh [N] [file] [extra blit args]
cd "$(dirname "$0")/.."
n=${1:-9}; file=${2:-bench/data/code-1m.txt}; shift 2 2>/dev/null || true
build=${BLIT_BUILD:-build-bench}  # an optimized Debug build, see run.sh
for i in $(seq "$n"); do ./$build/blit_bench launch ./$build/blit --bench startup "$file" "$@"; done | python3 -c '
import json,sys,statistics as st
rows=[json.loads(l) for l in sys.stdin if l.strip()]
keys=list(rows[0]["ms"].keys())
med=lambda f:[st.median(f(r) for r in rows)][0]
prev=0
for k in keys:
    v=med(lambda r:r["ms"][k]); print(f"  {k:20} {v:8.2f} ms  (+{v-prev:6.2f})"); prev=v
e=med(lambda r:r.get("exec_to_first_frame_ms",0)); d=med(lambda r:r["ms"]["first_frame_done"])
print(f"  exec->first frame    {e:8.2f} ms  (before main: {e-d:5.2f})   n={len(rows)}")'
