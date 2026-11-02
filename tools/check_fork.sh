#!/bin/zsh
# Regenerates the DSP fork and proves the optimizations (docs/DSP_REVIEW.md):
#   1. the fork in double without switches == upstream, bit for bit (render_ref + render_cov)
#   2. NEMESIS_FORK_EXACT in double == upstream, bit for bit (render_cov)
#   3. NEMESIS_FORK_EXACT in float (GCC, the firmware's literal typing) == float without it, bit for bit
#   4. the firmware config (exact + approx) vs float: spectral report only
# Usage: tools/check_fork.sh [--no-sync] [--approx]
set -e
cd "$(dirname "$0")/.."
export SDKROOT=$(xcrun --show-sdk-path)
[[ "$1" == "--no-sync" ]] || python3 tools/sync_dsp.py ref/upstream/sdk/dsp firmware/dsp > /tmp/sync.log
cd bench
cmake -S . -B build_new > /dev/null
cmake -S . -B build_gcc > /dev/null
# a failed build must stop the check: a stale binary would "pass" with the old code
cmake --build build_new -j10 --target render_ref_upstream render_ref_upstream_cov render_ref_fork_double render_ref_fork_double_cov render_ref_fork_double_exact_cov > /tmp/build_new.log 2>&1 || { grep -E "error" /tmp/build_new.log | head -20; echo "BUILD FAILED (clang)"; exit 1; }
cmake --build build_gcc -j10 --target cov_diff render_ref_fork_float_fast_cov render_ref_fork_float_exact_cov render_ref_fork_float_fw_cov > /tmp/build_gcc.log 2>&1 || { grep -E "error" /tmp/build_gcc.log | head -20; echo "BUILD FAILED (gcc)"; exit 1; }
mkdir -p out_cov
[[ -f out_cov/up.f32 ]] || build_new/render_ref_upstream_cov out_cov/up.f32 > out_cov/names.txt
[[ -f out_cov/upref.f32 ]] || build_new/render_ref_upstream out_cov/upref.f32 > /dev/null
build_new/render_ref_fork_double out_cov/fdref.f32 > /dev/null
cmp -s out_cov/upref.f32 out_cov/fdref.f32 && echo "1a. render_ref: fork double == upstream" || { echo "1a. render_ref: fork double DIFFERS from upstream"; exit 1; }
build_new/render_ref_fork_double_cov out_cov/fd.f32 > /dev/null
cmp -s out_cov/up.f32 out_cov/fd.f32 && echo "1b. render_cov: fork double == upstream" || { echo "1b. render_cov: fork double DIFFERS"; exit 1; }
build_new/render_ref_fork_double_exact_cov out_cov/fdx.f32 > /dev/null
build_gcc/cov_diff out_cov/up.f32 out_cov/fdx.f32 out_cov/names.txt --exact > out_cov/exact_double.txt && echo "2.  EXACT in double == upstream" || { echo "2.  EXACT in double DIFFERS:"; grep -v " yes " out_cov/exact_double.txt; exit 1; }
build_gcc/render_ref_fork_float_fast_cov out_cov/ff.f32 > /dev/null
build_gcc/render_ref_fork_float_exact_cov out_cov/ffx.f32 > /dev/null
build_gcc/cov_diff out_cov/ff.f32 out_cov/ffx.f32 out_cov/names.txt --exact > out_cov/exact_float.txt && echo "3.  EXACT in float == float" || { echo "3.  EXACT in float DIFFERS:"; grep -v " yes " out_cov/exact_float.txt; exit 1; }
if [[ "$1" == "--approx" || "$2" == "--approx" ]]; then
  build_gcc/render_ref_fork_float_fw_cov out_cov/fw.f32 > /dev/null
  echo "4.  firmware config (exact + approx) vs float without them:"
  build_gcc/cov_diff out_cov/ff.f32 out_cov/fw.f32 out_cov/names.txt | tail -n +1
  echo "    ... and vs the double reference (upstream):"
  build_gcc/cov_diff out_cov/up.f32 out_cov/fw.f32 out_cov/names.txt | tail -1
fi
