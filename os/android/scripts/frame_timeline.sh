#!/bin/bash
# Which stage makes frames miss the 33.3 ms vblank: turns on
# gta4_native_frame_timeline for a few seconds, reads SurfaceFlinger's
# per-frame times for the game layer, and joins both in frame_timeline.py.
#   frame_timeline.sh [seconds]   (default 4; SurfaceFlinger keeps 128 frames)
set -euo pipefail
. "$(dirname "$0")/env.sh"
SECONDS_TO_RECORD="${1:-4}"
OUT="$(mktemp -d)"
LAYER=$(adbs shell dumpsys SurfaceFlinger --list | tr -d '\r' | grep -E "^SurfaceView\[$PKG/.*\(BLAST\)" | head -1)
echo "--gta4_native_frame_timeline=true" | adbs shell "cat > $FILES/live_cvars.txt"
sleep 2
adbs logcat -c
adbs shell "dumpsys SurfaceFlinger --latency-clear '$LAYER'" >/dev/null
sleep "$SECONDS_TO_RECORD"
adbs shell "dumpsys SurfaceFlinger --latency '$LAYER'" | tr -d '\r' > "$OUT/sf.txt"
adbs logcat -d -s LibertyRecomp:W | tr -d '\r' | grep "frame-timeline:" > "$OUT/timeline.txt" || true
echo "--gta4_native_frame_timeline=false" | adbs shell "cat > $FILES/live_cvars.txt"
sleep 2
adbs shell "rm -f $FILES/live_cvars.txt"
"$PYTHON" "$(dirname "$0")/frame_timeline.py" "$OUT/sf.txt" "$OUT/timeline.txt"
