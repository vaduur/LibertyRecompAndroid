#!/bin/bash
# Unattended recorder benchmark at the save point: restarts the game (it
# loads the last save by itself), waits for gameplay and for streaming to
# settle, then measures the frame timeline twice and the recorder's busy
# share from /proc schedstat (exact, unlike tick-based cpu_frame.sh).
#   spawn_timeline.sh <label>     results in $OUT_ROOT/<label>/
set -uo pipefail
. "$(dirname "$0")/env.sh"
export ADB_TIMEOUT="${ADB_TIMEOUT:-60}"
LABEL="$1"
OUT_ROOT="${OUT_ROOT:-$(dirname "$0")/../../../../_bench}"
OUT="$OUT_ROOT/$LABEL"
mkdir -p "$OUT"
adbs shell am force-stop "$PKG"
adbs shell "rm -f $FILES/live_cvars.txt"
adbs logcat -c
adbs shell am start -n "$ACTIVITY" --ez play true >/dev/null
ready=0
for _ in $(seq 1 80); do
  o=$(bash "$(dirname "$0")/cpu_frame.sh" 3 2>&1)
  f=$(echo "$o" | grep '^fps' | awk '{print $2}')
  g=$(echo "$o" | grep XThread | head -1 | awk '{print $2}')
  busy=$("$PYTHON" -c "print(int(float('${g:-0}') * float('${f:-0}') / 10))" 2>/dev/null || echo 0)
  # Gameplay: the busiest guest thread works >= ~4 ms per frame at a steady
  # frame rate, twice in a row (it spun at ~32 ms before gta4_frame_wait_sleep).
  if [ "${busy:-0}" -ge 12 ] 2>/dev/null; then
    hits=$((${hits:-0} + 1)); [ "$hits" -ge 2 ] && { ready=1; break; }
  else
    hits=0
  fi
  adbs shell pidof "$GAME_PROC" >/dev/null || { echo "[$LABEL] game exited" | tee "$OUT/summary.txt"; exit 1; }
done
[ "$ready" = 1 ] || { echo "[$LABEL] no gameplay" | tee "$OUT/summary.txt"; exit 1; }
sleep 25
{
  echo "[$LABEL]"
  for _ in 1 2; do bash "$(dirname "$0")/frame_timeline.sh" 4 2>&1 | head -4; done
  P=$(adbs shell pidof "$GAME_PROC" | tr -d '\r')
  T=$(adbs shell "grep -l GtaRecorder /proc/$P/task/*/comm" | tr -d '\r' | head -1 | sed 's#/comm##')
  a=$(adbs shell "cat $T/schedstat" | awk '{print $1}')
  f0=$(adbs shell "dumpsys SurfaceFlinger --latency-clear" >/dev/null; date +%s%N)
  sleep 5
  b=$(adbs shell "cat $T/schedstat" | awk '{print $1}')
  echo "recorder busy: $(( (b - a) / 50000000 ))%"
  bash "$(dirname "$0")/fps.sh" 6 2>&1 | head -1
} | tee "$OUT/summary.txt"
adbs exec-out screencap -p > "$OUT/shot.png"
