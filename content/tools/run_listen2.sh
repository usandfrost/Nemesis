#!/bin/sh
# The second listening page, start to finish (about 2 minutes): render_listen (firmware code) -> upstream originals -> mp3 -> content/out/listen2.html.
# Everything is built in the scratch directory $S, the repository gets content/out/listen2.html and listen2_checks.json only.
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
S="${DBOY_LISTEN2_SCRATCH:-/tmp/dboy-content}"
UPSTREAM="${DBOY_UPSTREAM_BUILD:-$S/../content/upstream_build}"   # built by build_content.py stage upstream (content/tools/upstream_ref)
mkdir -p "$S/wav" "$S/hy" "$S/src"
cp "$REPO/content/tools/listen.cmake" "$S/src/CMakeLists.txt"
sed -i.bak "s#get_filename_component(REPO .*#set(REPO $REPO)#" "$S/src/CMakeLists.txt"
cmake -S "$S/src" -B "$S/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$S/build" -j8
export DBOY_WTP_DIR="$REPO/content/data/wtp"
for s in 0 1 2 3 4 5 6 7; do "$S/build/render_listen" "$S/wav" $s 8 > "$S/stats.$s.jsonl" & done
wait
cat "$S"/stats.?.jsonl > "$S/stats.jsonl"; rm -f "$S"/stats.?.jsonl
python3 - "$REPO" "$S" <<'PY'
import json, os, sys
R, S = sys.argv[1] + "/", sys.argv[2]
cut = json.load(open(R + "content/data/cut.json"))["presets"]
sel = {c["name"]: c for c in json.load(open(R + "content/out/selection.json"))["chosen"]}
st = {json.loads(l)["name"]: json.loads(l)["i"] for l in open(S + "/stats.jsonl")}
st["Iron Squelch"] = st["Iron Squelch 2"]   # renamed in the firmware list
arch = R + "ref/upstream/docs/dev/sound-design/archive/presets/"
open(S + "/hy_lines.txt", "w").write("".join("%s\t%s\t%s\t%d\t%s/hy/%03d.wav\n" % (arch + sel[p["name"]]["file"], p["name"], p["category"], st[p["name"]], S, st[p["name"]]) for p in cut))
PY
"$UPSTREAM/upstream_render" < "$S/hy_lines.txt" > "$S/up_stats.jsonl"
python3 "$REPO/content/tools/build_listen2.py"
