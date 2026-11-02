#!/bin/sh
# A listening page for a batch of new presets: run_batch.sh [folder of .PRS files (default content/synthwave)] [out.html (default content/out/synthwave.html)] [title]
# builds render_listen (firmware code) in a scratch folder, renders every preset of the folder (with effects and dry), then build_listen_batch.py writes the page.
# Fails if any preset of the folder could not be rendered (a damaged file, a crashed render): the page always holds every file. Wavetable banks come from
# content/data/wtp (a bank without a file there plays a basic wave: tools/genre_presets.py banks lists the ones that exist).
# Scratch: $DBOY_BATCH_SCRATCH (default build/listen_batch in the repo, which git ignores). Needs ffmpeg, cmake and ninja.
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DIR="${1:-$REPO/content/synthwave}"
OUT="${2:-$REPO/content/out/synthwave.html}"
TITLE="${3:-Drumboy Synthwave Listening}"
S="${DBOY_BATCH_SCRATCH:-$REPO/build/listen_batch}"
ls "$DIR"/*.PRS >/dev/null 2>&1 || { echo "run_batch.sh: no .PRS files in $DIR" >&2; exit 1; }
python3 "$REPO/tools/prs.py" check "$DIR" >/dev/null || { echo "run_batch.sh: $DIR has damaged presets (tools/prs.py check shows which)" >&2; exit 1; }
mkdir -p "$S/src" "$S/prs" "$S/wav" "$REPO/content/out"
rm -f "$S"/prs/* "$S"/wav/* "$S"/stats.*
cp "$DIR"/*.PRS "$S/prs/"
cp "$REPO/content/tools/listen.cmake" "$S/src/CMakeLists.txt"
sed -i.bak "s#get_filename_component(REPO .*#set(REPO \"$REPO\")#" "$S/src/CMakeLists.txt"
cmake -S "$S/src" -B "$S/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$S/build" -j8 >"$S/build.log" 2>&1 || { tail -30 "$S/build.log" >&2; echo "run_batch.sh: the renderer did not build" >&2; exit 1; }
export DBOY_WTP_DIR="$REPO/content/data/wtp" DBOY_PRESET_SRC="$S/prs"
pids=""
for s in 0 1 2 3 4 5 6 7; do "$S/build/render_listen" "$S/wav" $s 8 > "$S/stats.$s.jsonl" 2>"$S/render.$s.log" & pids="$pids $!"; done
fail=0
for p in $pids; do wait "$p" || fail=1; done
[ $fail = 0 ] || { cat "$S"/render.?.log >&2; echo "run_batch.sh: a render crashed" >&2; exit 1; }
cat "$S"/stats.?.jsonl > "$S/stats.jsonl"
want=$(ls "$S"/prs/*.PRS | wc -l | tr -d ' '); got=$(wc -l < "$S/stats.jsonl" | tr -d ' ')
[ "$want" = "$got" ] || { echo "run_batch.sh: $got of $want presets were rendered (a file the firmware would leave out?)" >&2; exit 1; }
if [ -n "${DBOY_IDS_JSON:-}" ]; then   # the originals: upstream_render plays the upstream preset each id was converted from (ids.json from gen_presets.py --more)
    HB="${DBOY_UPSTREAM_BUILD:-$REPO/build/upstream_ref}"
    { cmake -S "$REPO/content/tools/upstream_ref" -B "$HB" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$S/upstream_build.log" 2>&1 && cmake --build "$HB" -j8 >>"$S/upstream_build.log" 2>&1; } || { tail -20 "$S/upstream_build.log" >&2; echo "run_batch.sh: upstream_render did not build" >&2; exit 1; }
    ARCH="$REPO/ref/upstream/docs/dev/sound-design/archive/presets"
    mkdir -p "$S/hy"; rm -f "$S"/hy/*
    python3 - "$DBOY_IDS_JSON" "$ARCH" "$S" > "$S/hy_lines.txt" <<'PY'
import json, sys
ids, arch, s = json.load(open(sys.argv[1])), sys.argv[2], sys.argv[3]
for i, p in sorted(ids.items(), key=lambda kv: int(kv[0])):
    print("%s/%s\t%s\t%s\t%d\t%s/hy/%03d.wav" % (arch, p["file"], p["name"], p["category"], int(i), s, int(i)))
PY
    "$HB/upstream_render" < "$S/hy_lines.txt" > "$S/up_stats.jsonl" || { echo "run_batch.sh: upstream_render failed" >&2; exit 1; }
    wantf=$(wc -l < "$S/hy_lines.txt" | tr -d ' '); gotf=$(wc -l < "$S/up_stats.jsonl" | tr -d ' ')
    if grep -q '"error"' "$S/up_stats.jsonl" || [ "$wantf" != "$gotf" ]; then grep '"error"' "$S/up_stats.jsonl" | head -5 >&2; echo "run_batch.sh: upstream_render rendered $gotf of $wantf originals" >&2; exit 1; fi
    python3 "$REPO/content/tools/build_listen_batch.py" "$S/wav" "$S/stats.jsonl" "$OUT" "$TITLE" --hy "$S/hy" "$S/up_stats.jsonl"
else
    python3 "$REPO/content/tools/build_listen_batch.py" "$S/wav" "$S/stats.jsonl" "$OUT" "$TITLE"
fi
