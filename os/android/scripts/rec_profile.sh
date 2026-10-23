#!/bin/bash
# CPU profile of one thread (default GtaRecorder) of the running game,
# aggregated by source line and by function (unstripped renderer library).
#   rec_profile.sh [thread] [seconds] [dso-substring]
set -uo pipefail
. "$(dirname "$0")/env.sh"
THREAD="${1:-GtaRecorder}"; DURATION="${2:-8}"; DSO="${3:-librexgpu-gta4-native}"
OUT="$(mktemp -d)"
adbs shell "rm -f /data/local/tmp/perf.data"
adbs shell "simpleperf record --app $PKG -e cpu-clock -f 4000 --duration $DURATION -o /data/local/tmp/perf.data" >/dev/null 2>&1
adbs shell "simpleperf report -i /data/local/tmp/perf.data --comms $THREAD --sort dso,vaddr_in_file -n" 2>/dev/null > "$OUT/full.txt"
grep "$DSO" "$OUT/full.txt" | awk '{print $1, $2, $4}' > "$OUT/vaddr.txt"
LIB="$APP_DIR/app/src/main/jniLibs/arm64-v8a/$DSO.so"
awk '{print $3}' "$OUT/vaddr.txt" > "$OUT/addrs.txt"
"$NDK_BIN/llvm-addr2line$_exe" -f -e "$(winpath "$LIB")" < "$OUT/addrs.txt" | paste - - > "$OUT/fl.txt"
"$PYTHON" - "$OUT" <<'PY'
import collections, re, sys
d = sys.argv[1] + "/"
total = int(next(l for l in open(d + "full.txt") if l.startswith("Samples")).split()[1])
rows = [l.split() for l in open(d + "vaddr.txt")]
fl = [l.rstrip("\n").split("\t") for l in open(d + "fl.txt")]
lines, funcs = collections.Counter(), collections.Counter()
for r, f in zip(rows, fl):
    n = int(r[1]); line = re.sub(r".*/(gta4_native|include|v1)/", "", f[1]); line = re.sub(r" \(discriminator.*", "", line)
    lines[line] += n; funcs[f[0][:110]] += n
print(f"thread samples {total}; in library {sum(int(r[1]) for r in rows)}")
print("-- lines"); [print(f"{100*n/total:5.2f}% {l}") for l, n in lines.most_common(25)]
print("-- functions"); [print(f"{100*n/total:5.2f}% {l}") for l, n in funcs.most_common(15)]
PY
