#!/bin/bash
# Regression run at the save point, for checking a build before a release:
# restarts the game (it loads the last save), waits for gameplay, measures the
# frame timeline and CPU/GPU load, takes a screenshot, and scans the log for
# crashes, crash-guard hits, renderer rejections and errors.
#   regress.sh <label> [--ref <label>] [--safe] [--min-similarity <0..1>]
# Every run checks its screenshot for a black, flat or single-coloured frame.
# --ref     also compare the frame's brightness and colour distribution with an
#           earlier run (shot_check.py; the save point is a live scene, so the
#           pixels themselves differ between any two runs); diff.png shows where
# --safe    launch with gta4_safe_mode (args.txt is restored afterwards)
# --extra   one more launch argument for this run, e.g. --extra --cvar=value
# Results in $OUT_ROOT/<label>/ (summary.txt, shot.png, diff.png, log.txt);
# the last line is PASS or FAIL with the reasons.
set -uo pipefail
. "$(dirname "$0")/env.sh"
export ADB_TIMEOUT="${ADB_TIMEOUT:-60}"
LABEL="$1"; shift
REF=""; SAFE=0; MIN_SIMILARITY=0.95; EXTRA=()
while [ $# -gt 0 ]; do
  case "$1" in
    --ref) REF="$2"; shift 2 ;;
    --safe) SAFE=1; shift ;;
    --extra) EXTRA+=("$2"); shift 2 ;;
    --min-similarity) MIN_SIMILARITY="$2"; shift 2 ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
done
OUT_ROOT="${OUT_ROOT:-$(dirname "$0")/../../../../_bench}"
OUT="$OUT_ROOT/$LABEL"
mkdir -p "$OUT"
TMP="$(mktemp -d)"

# The user's args.txt is copied off the device before safe mode replaces it
# and put back on exit (also on errors); it is never deleted unless it did
# not exist before.
ARGS_STATE=untouched
LOG_PID=""
cleanup() {
  [ -n "$LOG_PID" ] && kill "$LOG_PID" 2>/dev/null
  case "$ARGS_STATE" in
    saved) adbs push "$(winpath "$TMP/args.saved")" "$FILES/args.txt" >/dev/null \
             || echo "!! could not restore args.txt; a copy is in $OUT/args.saved" ;;
    absent) adbs shell rm -f "$FILES/args.txt" ;;
  esac
  rm -rf "$TMP"
}
trap cleanup EXIT
if [ "$SAFE" = 1 ] || [ ${#EXTRA[@]} -gt 0 ]; then
  if adbs pull "$FILES/args.txt" "$(winpath "$TMP/args.saved")" >/dev/null 2>&1; then
    ARGS_STATE=saved
    cp "$TMP/args.saved" "$OUT/args.saved"
  elif adbs shell "test -e $FILES/args.txt" ; then
    echo "!! args.txt exists but could not be copied; not touching it"; exit 1
  else
    ARGS_STATE=absent
    : > "$TMP/args.saved"
  fi
  { grep -v -- '--gta4_safe_mode' "$TMP/args.saved" | tr -d '\r'
    [ "$SAFE" = 1 ] && echo "--gta4_safe_mode=true"
    for a in "${EXTRA[@]}"; do echo "$a"; done; } > "$TMP/args.safe"
  adbs push "$(winpath "$TMP/args.safe")" "$FILES/args.txt" >/dev/null
fi

reasons=()
# The whole run's log, streamed: the measuring scripts clear the buffer, which
# would lose the startup lines.
adbs logcat -c
( exec adb ${SERIAL:+-s "$SERIAL"} logcat > "$OUT/log.txt" 2>/dev/null ) &  # exec: $! is adb itself
LOG_PID=$!
bash "$(dirname "$0")/spawn_timeline.sh" "$LABEL" > "$TMP/timeline.txt" 2>&1
grep -qE "game exited|no gameplay" "$OUT/summary.txt" && reasons+=("$(head -1 "$OUT/summary.txt")")
adbs exec-out screencap -p > "$OUT/shot.png"
bash "$(dirname "$0")/power_sample.sh" 8 "$LABEL" >> "$OUT/summary.txt" 2>&1
kill "$LOG_PID" 2>/dev/null; LOG_PID=""

alive=$(adbs shell pidof "$GAME_PROC" | tr -d '\r')
[ -n "$alive" ] || reasons+=("game not running at the end")
crashes=$(grep -cE "Fatal signal|SIGSEGV|SIGABRT|backtrace:" "$OUT/log.txt")
guards=$(grep -c "stale-object-guard:" "$OUT/log.txt")
rejected=$(grep -cE "rejected worker command|snapshot failed|gta4-native.*(error|failed)" "$OUT/log.txt")
errors=$(grep -E " E (LibertyRecomp|rex)" "$OUT/log.txt" | wc -l)
[ "$crashes" = 0 ] || reasons+=("$crashes crash lines")
[ "$rejected" = 0 ] || reasons+=("$rejected renderer rejections")
if [ "$SAFE" = 1 ] && ! grep -q "SAFE MODE" "$OUT/log.txt"; then
  reasons+=("safe mode not applied")
fi
echo "log: crashes=$crashes guard-hits=$guards renderer-rejections=$rejected errors=$errors" >> "$OUT/summary.txt"

check_args=("$OUT/shot.png")
[ -n "$REF" ] && check_args+=("$OUT_ROOT/$REF/shot.png")
check_line=$("$PYTHON" "$(dirname "$0")/shot_check.py" "${check_args[@]}" 2>&1 | tail -1)
echo "screenshot: $check_line${REF:+ (against $REF)}" >> "$OUT/summary.txt"
case "$check_line" in *broken:*) reasons+=("screenshot ${check_line#*broken: }") ;; esac
similarity=$(echo "$check_line" | sed -n 's/.*similarity \([0-9.]*\).*/\1/p')
if [ -n "$similarity" ] && "$PYTHON" -c "import sys; sys.exit(0 if $similarity < $MIN_SIMILARITY else 1)"; then
  reasons+=("screenshot similarity $similarity (< $MIN_SIMILARITY) against $REF")
fi
[ -n "$REF" ] && python && python  "$PYTHON" "$(dirname "$0")/shot_diff.py" "$OUT_ROOT" "$REF" "$LABEL" > /dev/null 2>&1  # diff.png

if [ ${#reasons[@]} -eq 0 ]; then
  echo "PASS" >> "$OUT/summary.txt"
else
  echo "FAIL: $(IFS=';'; echo "${reasons[*]}")" >> "$OUT/summary.txt"
fi
cat "$OUT/summary.txt"
