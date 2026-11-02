#!/usr/bin/env bash
# The website as a zip for a static host: web/www with the wavetables copied in, without the bench (a local page, it needs web/serve.py's /api).
#   tools/make_site_zip.sh [--php] [out.zip]     default: ./nemesis-website.zip, or ./nemesis-website-php.zip
# --php: for a host that runs PHP and cannot be told which headers to send (tools/make_site_php.py)
set -eu
cd "$(dirname "$0")/.."
php=0; name=nemesis-website.zip
if [ "${1:-}" = "--php" ]; then php=1; name=nemesis-website-php.zip; shift; fi
out=$(cd "$(dirname "${1:-$name}")" && pwd)/$(basename "${1:-$name}")
for f in drumboy.wasm drumboy_hr.wasm data/prs.bin wtp/wt004.wtp downloads/nemesis-beta-2-install.zip downloads/nemesis-manual.pdf; do
    [ -e "web/www/$f" ] || { echo "no web/www/$f: build the web version, tools/make_install_zip.sh and tools/make_manual.sh first"; exit 1; }
done
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp -RL web/www/. "$tmp/"
cp LICENSE "$tmp/COPYING.txt"
rm -f "$tmp/bench.html"
[ $php = 0 ] || python3 tools/make_site_php.py "$tmp"
rm -f "$out"
(cd "$tmp" && zip -qr9 "$out" .)
ls -l "$out"
