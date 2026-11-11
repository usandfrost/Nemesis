#!/usr/bin/env bash
# The package a user puts on their own card: a release build (no debug mailbox), every preset and wavetable, the instructions and the licences. Not the samples:
# every Drumboy's card has them, and the install leaves them where they are.
#   tools/make_install_zip.sh VERSION [out.zip]     VERSION like Beta2. Needs ARM_GNU_ROOT, as the firmware build does
set -eu
cd "$(dirname "$0")/.."
version=${1:?a version, like Beta2}
name=nemesis-${version}-install.zip
out=$(cd "$(dirname "${2:-$name}")" && pwd)/$(basename "${2:-$name}")
build=firmware/build-release
cmake -S firmware -B "$build" -DDBOY_DEBUG_HOOK=OFF >/dev/null
cmake --build "$build" -j8 >/dev/null
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
python3 tools/make_sd_content.py "$tmp" --all >/dev/null
short=${version/Beta/B}   # (the file name has a limit: see make_sd_firmware.py)
python3 tools/make_sd_firmware.py "$build/dboy_cathode.bin" "$tmp/Drumboy_Pro_Fw_Nemesis_$short.bin"
# Notepad and every other text reader take CRLF
sed "s/@VERSION@/$version/g; s/@SHORT@/$short/g" docs/INSTALL.txt | sed 's/$/\r/' > "$tmp/INSTALL.txt"
# the website's LICENSES.txt, then the long texts that only the firmware needs: Apache 2.0 (CMSIS, the device headers) and the screen fonts' OFL
{ cat web/www/LICENSES.txt
  printf '\n\nThe Apache License 2.0 (CMSIS, by Arm Limited; the STM32H7 device headers, by STMicroelectronics)\n\n'; cat firmware/platform/Drivers/CMSIS/LICENSE.txt
  printf '\n\nThe screen font Chakra Petch\n\n'; cat tools/fonts/OFL-ChakraPetch.txt
  printf '\n\nThe screen font IBM Plex Mono\n\n'; cat tools/fonts/OFL-IBMPlexMono.txt
} | sed 's/$/\r/' > "$tmp/LICENSES.txt"
sed 's/$/\r/' LICENSE > "$tmp/COPYING.txt"
rm -f "$out"
(cd "$tmp" && zip -qr9 "$out" .)
ls -l "$out"
