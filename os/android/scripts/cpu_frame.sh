#!/bin/bash
# CPU milliseconds per presented frame for the busiest game threads - a
# steadier optimisation metric than fps, which also moves with the scene.
#   cpu_frame.sh [seconds]   (default 10; stand still while it runs)
set -euo pipefail
. "$(dirname "$0")/env.sh"
SECS="${1:-10}"
PID=$(adbs shell pidof "$GAME_PROC" | tr -d '\r')
[ -n "$PID" ] || { echo "game not running"; exit 1; }
LAYER=$(adbs shell dumpsys SurfaceFlinger --list | tr -d '\r' | grep -E "^SurfaceView\[$PKG/.*\(BLAST\)" | head -1)
snap() {
  adbs shell "run-as $PKG sh -c 'for t in /proc/$PID/task/*; do echo \$(basename \$t) \$(cut -d\" \" -f14,15 \$t/stat) \$(cat \$t/comm | tr \" \" _); done'" | tr -d '\r'
}
frames() {
  adbs shell "dumpsys SurfaceFlinger --latency '$LAYER'" | tr -d '\r' | awk 'NF==3 && $2>0 && $2<4611686018427387904 {print $2}'
}
A="$(mktemp)"; B="$(mktemp)"; F="$(mktemp)"
adbs shell "dumpsys SurfaceFlinger --latency-clear '$LAYER'" >/dev/null
T0=$(date +%s.%N)
snap > "$A"
for _ in $(seq "$SECS"); do sleep 1; frames >> "$F"; done
snap > "$B"
T1=$(date +%s.%N)
"$PYTHON" - "$(winpath "$A")" "$(winpath "$B")" "$(winpath "$F")" "$("$PYTHON" -c "print($T1-$T0)")" <<'PY'
import sys
def load(p):
    d={}
    for l in open(p):
        x=l.split()
        if len(x)==4: d[x[0]]=(int(x[1])+int(x[2]),x[3])
    return d
a,b=load(sys.argv[1]),load(sys.argv[2])
t=sorted(set(int(x) for x in open(sys.argv[3]) if x.strip()))
secs=float(sys.argv[4])  # wall time between the two thread snapshots
if len(t)<3: sys.exit("not enough frames")
span=(t[-1]-t[0])/1e9; n=len(t)-1; fps=n/span
print(f"fps {fps:.1f}")
rows=[]
for tid,(tb,name) in b.items():
    if tid in a:
        ticks=tb-a[tid][0]          # 100 Hz jiffies
        ms=ticks*10.0/(secs*fps)    # CPU ms per frame
        if ms>1: rows.append((ms,tid,name))
for ms,tid,name in sorted(rows,reverse=True)[:6]:
    print(f"  {name:<18} {ms:5.1f} ms/frame")
PY
rm -f "$A" "$B" "$F"
