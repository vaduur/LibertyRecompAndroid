#!/bin/bash
# Frame rate of the game surface, measured by SurfaceFlinger.
#   fps.sh [seconds]   (default 10; SurfaceFlinger keeps only the last 128
#                       frames, so it is sampled once a second and merged)
set -euo pipefail
. "$(dirname "$0")/env.sh"
SECONDS_TOTAL="${1:-10}"
LAYER=$(adbs shell dumpsys SurfaceFlinger --list | tr -d '\r' | grep -E "^SurfaceView\[$PKG/.*\(BLAST\)" | head -1)
[ -n "$LAYER" ] || { echo "game surface not found - is the game in the foreground?"; exit 1; }
OUT="$(mktemp)"
adbs shell "dumpsys SurfaceFlinger --latency-clear '$LAYER'" >/dev/null
for _ in $(seq "$SECONDS_TOTAL"); do
  sleep 1
  adbs shell "dumpsys SurfaceFlinger --latency '$LAYER'" | tr -d '\r' >> "$OUT"
done
"$PYTHON" - "$(winpath "$OUT")" <<'EOF'
import sys
present = set()
for line in open(sys.argv[1]):
    parts = line.split()
    if len(parts) == 3:
        t = int(parts[1])
        if 0 < t < 2**62:
            present.add(t)
t = sorted(present)
if len(t) < 3:
    sys.exit("not enough frames")
span = (t[-1] - t[0]) / 1e9
d = sorted((b - a) / 1e6 for a, b in zip(t, t[1:]))
print(f"fps {(len(t) - 1) / span:.1f} over {span:.1f}s  frame ms: "
      f"median {d[len(d) // 2]:.1f}  p95 {d[int(len(d) * .95)]:.1f}  max {d[-1]:.1f}")
# Display intervals in vsyncs: an even 30 fps is all "2"; a mix of 1/2/3 judders.
vs = [round((b - a) / 1e6 / 16.667) for a, b in zip(t, t[1:])]
hist = {k: vs.count(k) for k in sorted(set(vs))}
print("vsyncs per frame:", "  ".join(f"{k}x={v}" for k, v in hist.items()))
EOF
rm -f "$OUT"
