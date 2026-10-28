#!/bin/bash
# Unattended GPU benchmark at the save point: relaunches the game with the
# player's args.txt plus the profiler, waits for gameplay (the game loads the
# last save by itself), lets streaming settle, then records a screenshot,
# uncapped frame rate, GPU load and one native GPU profile capture.
#   gpu_bench.sh <label> [--cvar=value ...]   (extra lines go into args.txt)
# Results: $OUT_ROOT/<label>/{shot.png,summary.txt,profile.log}. The player's
# args.txt is restored afterwards.
set -uo pipefail
. "$(dirname "$0")/env.sh"
export ADB_TIMEOUT="${ADB_TIMEOUT:-60}"
LABEL="$1"; shift
OUT_ROOT="${OUT_ROOT:-$(dirname "$0")/../../../../_bench}"
OUT="$OUT_ROOT/$LABEL"
mkdir -p "$OUT"
DIAG="$FILES/LibertyRecomp/Diagnostics"

adbs shell "cp $FILES/args.txt $FILES/args_bench_backup.txt"
adbs shell am force-stop "$PKG"
adbs shell "rm -f $FILES/live_cvars.txt $DIAG/native-performance-latest.log"
{
  adbs shell "cat $FILES/args_bench_backup.txt" | tr -d '\r' |
    grep -v -e '^--gta4_profile_native_detailed' -e '^--gta4_frame_limit' -e '^--diagnostics' -e '^--gta4_profile_native_autostart'
  echo "--diagnostics=true"
  echo "--diagnostics_categories=logging,native-profiler"
  echo "--gta4_profile_native_detailed_gpu=true"
  echo "--gta4_profile_native_autostart=false"
  echo "--gta4_profile_native_samples=240"
  echo "--gta4_frame_limit=0"
  for line in "$@"; do echo "$line"; done
} > "$OUT/args.txt"
adbs push "$(winpath "$OUT/args.txt")" "$FILES/args.txt" >/dev/null
adbs logcat -c
adbs shell am start -n "$ACTIVITY" --ez play true >/dev/null

restore() { adbs shell "cp $FILES/args_bench_backup.txt $FILES/args.txt"; }
trap restore EXIT

# Gameplay: the busiest guest thread is >= 30% busy (ms per frame x fps).
ready=0
for _ in $(seq 1 80); do
  o=$(bash "$(dirname "$0")/cpu_frame.sh" 3 2>&1)
  f=$(echo "$o" | grep '^fps' | awk '{print $2}')
  g=$(echo "$o" | grep XThread | head -1 | awk '{print $2}')
  busy=$("$PYTHON" -c "print(int(float('${g:-0}') * float('${f:-0}') / 10))" 2>/dev/null || echo 0)
  if [ "${busy:-0}" -ge 30 ] 2>/dev/null; then ready=1; break; fi
  adbs shell pidof "$GAME_PROC" >/dev/null || { echo "[$LABEL] game exited" | tee "$OUT/summary.txt"; exit 1; }
done
[ "$ready" = 1 ] || { echo "[$LABEL] no gameplay" | tee "$OUT/summary.txt"; exit 1; }
sleep 25

adbs exec-out screencap -p > "$OUT/shot.png"
fps=$(bash "$(dirname "$0")/fps.sh" 10 2>&1 | head -1)
busy=$(adbs shell 'for i in 1 2 3 4 5; do cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage; sleep 1; done' |
       tr -d '% \r' | awk '{s+=$1} END {print s/NR}')
cpu=$(bash "$(dirname "$0")/cpu_frame.sh" 10 2>&1)
echo "--gta4_profile_start=1" | adbs shell "cat > $FILES/live_cvars.txt"
for _ in $(seq 1 60); do
  sleep 3
  adbs shell "test -s $DIAG/native-performance-latest.log" && break
done
sleep 3
adbs pull "$DIAG/native-performance-latest.log" "$(winpath "$OUT/profile.log")" >/dev/null 2>&1
alive=$(adbs shell pidof "$GAME_PROC" | tr -d '\r')
adbs logcat -d | grep -E "skips/frame|vertex-cache|stale-object|thermal|pipeline: 120|vk: barriers| F |FATAL|hang" |
  grep -v surface-address-alias | tail -40 > "$OUT/log.txt"
{
  echo "[$LABEL] $fps  gpu=${busy}%  alive=${alive:+yes}"
  echo "$cpu"
  [ -s "$OUT/profile.log" ] && python && python  "$PYTHON" "$(winpath "$(dirname "$0")/gpu_profile_summary.py")" "$(winpath "$OUT/profile.log")"
} | tee "$OUT/summary.txt"
