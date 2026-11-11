#!/usr/bin/env bash
# Everything that is checked before a change goes in, on this computer (there is no CI): the host tests, the preset files, the DSP fork, the ARM build.
#   tools/check_all.sh           all of it
#   tools/check_all.sh --quick   the host tests and the preset files only (about a minute)
set -u
cd "$(dirname "$0")/.."
quick=0; [ "${1:-}" = "--quick" ] && quick=1
fail=0
# like runf, but shows the last $1 lines of the output
runt() { local n=$1; shift; local out; out=$("$@" 2>&1); local rc=$?; printf '%s\n' "$out" | tail -n "$n"; [ $rc = 0 ] || { echo "FAILED ($rc): $*"; fail=1; }; }
step() { printf '\n== %s\n' "$1"; }
run() { "$@" || { echo "FAILED: $*"; fail=1; return 1; }; }
# runs a command, shows only the lines of its output that match $1, and fails if the command did (a pipe into grep would hide that)
runf() { local pat=$1; shift; local out; out=$("$@" 2>&1); local rc=$?; printf '%s\n' "$out" | grep -E "$pat"; [ $rc = 0 ] || { echo "FAILED ($rc): $*"; fail=1; }; }

step "host tests (tests/build)"
run cmake -S tests -B tests/build >/dev/null
runf "error|\*\*\*" cmake --build tests/build -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
mkdir -p content/data/wtp
if [ ! -f content/data/wtp/wt004.wtp ]; then echo "making the wavetable pyramids (the audio test reads six of them)"; runf "converted" tests/build/wt_pyramid content/data/wavetables content/data/wtp; fi
runf "tests passed|tests failed|Failed|FAIL" ctest --test-dir tests/build --output-on-failure

step "the preset editor's data (web/www/preset-schema.js is what the firmware's tables make)"
tmp=$(mktemp -d)
if tests/build/preset_schema content/data/wavetables > "$tmp/schema.js"; then
    if cmp -s "$tmp/schema.js" web/www/preset-schema.js; then echo "web/www/preset-schema.js matches the firmware"; else echo "FAILED: web/www/preset-schema.js differs from the firmware's tables: tests/build/preset_schema content/data/wavetables > web/www/preset-schema.js"; fail=1; fi
else echo "FAILED: tests/build/preset_schema"; fail=1; fi
rm -rf "$tmp"

step "preset files"
runf "." python3 tools/prs.py check content/presets
tmp=$(mktemp -d); cp -R content/presets "$tmp/before"
python3 tools/gen_presets.py >/dev/null
if diff -rq "$tmp/before" content/presets >/dev/null; then echo "content/presets is what gen_presets.py makes"; else echo "FAILED: content/presets differs from what tools/gen_presets.py makes (it has been rewritten: look at git diff)"; fail=1; fi
rm -rf "$tmp"

step "second round (content/candidates, content/presets2) and the genre sets (content/synthwave, techno, synthpop, trance, ebm, royksopp, delerium, voice)"
tmp=$(mktemp -d); mkdir "$tmp/all" "$tmp/cut"
cp content/presets/*.PRS "$tmp/all/"
for d in content/presets2 content/synthwave content/techno content/synthpop content/trance content/ebm content/royksopp content/delerium content/voice; do ls "$d"/*.PRS >/dev/null 2>&1 && cp "$d"/*.PRS "$tmp/all/"; done
runf "." python3 tools/prs.py check "$tmp/all" --renamed   # everything the card is given together: no id twice (genre_presets.py and cut_more.py also keep the names apart)
runf "." python3 tools/prs.py check content/candidates
before=$fail
run python3 tools/cut_more.py --out "$tmp/cut" >/dev/null
if diff -rq "$tmp/cut" content/presets2 >/dev/null; then echo "content/presets2 is what cut_more.py makes"; else echo "FAILED: content/presets2 differs from what tools/cut_more.py makes from the ratings"; fail=1; fi
for set in synthwave techno synthpop trance ebm royksopp delerium voice; do
    if ls content/$set/batch*.py >/dev/null 2>&1; then
        mkdir "$tmp/b_$set"
        run python3 tools/genre_presets.py check --set $set
        run python3 tools/genre_presets.py build --set $set -d "$tmp/b_$set" >/dev/null
        for f in "$tmp/b_$set"/*.PRS; do [ -e "$f" ] && { cmp -s "$f" "content/$set/$(basename "$f")" || { echo "FAILED: $(basename "$f") differs from what tools/genre_presets.py makes for $set"; fail=1; }; }; done
        [ "$(ls "$tmp/b_$set" | wc -l)" = "$(ls content/$set/*.PRS 2>/dev/null | wc -l)" ] || { echo "FAILED: content/$set has presets that tools/genre_presets.py does not make"; fail=1; }
    fi
done
[ $fail = $before ] && echo "the genre sets are what genre_presets.py makes"
rm -rf "$tmp"

if [ $quick = 0 ]; then
    step "DSP fork against upstream"
    if [ -d ref/upstream ]; then runt 3 tools/check_fork.sh --no-sync; else echo "skipped: ref/upstream is not here (the private upstream sources)"; fi
    step "the website (web/test: the librarian's protocol, the preset editor's bytes against the firmware and prs.py, the pages with real pointer events, then every lesson on the real firmware, all in headless Chrome)"
    runt 2 node web/test/sysex.mjs
    runt 2 node web/test/link.mjs
    runt 3 node web/test/preset-edit.mjs
    [ -d build-web ] && run cmake --build build-web -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"   # (else these test the last engine built, not this code)
    if [ -f web/www/drumboy.wasm ]; then
        [ -f web/www/data/prs.bin ] || runf "." python3 web/tools/make_data.py   # (a fresh clone has no data/: the simulated device starts from it)
        python3 web/serve.py 8799 >/dev/null 2>&1 &
        srv=$!
        sleep 1
        if kill -0 $srv 2>/dev/null; then
            runt 6 node web/test/site.mjs http://localhost:8799/
            runt 4 node web/test/learn.mjs http://localhost:8799/
            kill $srv
        else echo "FAILED: web/serve.py did not start: is port 8799 taken?"; fail=1; fi
    else echo "skipped: no web/www/drumboy.wasm (the web build: see web/CMakeLists.txt)"; fi
    step "firmware (ARM)"
    if [ -d firmware/build ]; then runt 8 cmake --build firmware/build; else echo "skipped: no firmware/build (see docs/BRINGUP.md for the toolchain)"; fi
fi
echo
[ $fail = 0 ] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $fail
