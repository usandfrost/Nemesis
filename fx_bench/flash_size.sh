#!/bin/zsh
# Flash cost of each FX candidate on the Cortex-M7: compile the fork + the converted FX tree with the firmware's flags (${T}g++ -O2,
# float, fast math, -fsingle-precision-constant), link a tiny main per candidate with --gc-sections on top of the engine-only baseline.
#   fx_bench/flash_size.sh SCRATCH_DIR     (needs SCRATCH_DIR/tree from make_tree.py)
set -e
S=${1:?scratch dir}
T=${ARM_GNU_ROOT:-$HOME/toolchains/arm-gnu-pkg/Payload}/bin/arm-none-eabi-   # the toolchain the firmware builds with (cmake_arm.cmake)
R=$(cd "$(dirname "$0")/.." && pwd)
FL=(-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -O2 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -fno-math-errno -std=gnu++17 -Dthread_local= -DNEMESIS_DSP_FLOAT=1 -DNEMESIS_DSP_FAST_MATH=1 -DNEMESIS_FORK_EXACT=1 -DNEMESIS_FORK_APPROX=1
    -DNEMESIS_FORK_MAX_VOICES=8 -DDSP_IS_FORK=1 -I$S/tree/inc -I$R/firmware/dsp/include -I$R/firmware/dsp/thirdparty/plaits -I$R/fx_bench)
mkdir -p $S/arm
cd $S/arm
compile() {   # src obj
  local extra=()
  if grep -q "nemesis/dsp/Real.h" $1 2>/dev/null; then extra=(-fsingle-precision-constant); fi
  ${T}g++ $FL $extra -w -c $1 -o $2
}
typeset -a objs
for f in $R/firmware/dsp/src/*.cpp $S/tree/src/*.cpp; do
  o=$S/arm/$(basename $f .cpp).o; objs+=$o
  compile $f $o &
  while (( $(jobs -r | wc -l) >= 8 )); do sleep 0.2; done
done
for f in $(find $R/firmware/dsp/thirdparty/plaits -name '*.cc'); do
  o=$S/arm/p_$(basename $f .cc).o
  ${T}g++ $FL -w -c $f -o $o &
  objs+=$o
  while (( $(jobs -r | wc -l) >= 8 )); do sleep 0.2; done
done
wait
rm -f libfork.a libfx.a
fork_objs=(); fx_objs=()
for o in $objs; do
  b=$(basename $o .o)
  if [[ -f $S/tree/src/$b.cpp ]]; then fx_objs+=$o; else fork_objs+=$o; fi
done
${T}ar rcs libfork.a $fork_objs
${T}ar rcs libfx.a $fx_objs
probe() {   # name define
  ${T}g++ $FL -fsingle-precision-constant -w $2 -c $R/fx_bench/flash_main.cpp -o probe_$1.o
  ${T}g++ -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections \
      probe_$1.o libfx.a libfork.a -o probe_$1.elf 2>link_$1.log || { echo "link failed: $1"; tail -3 link_$1.log; return; }
  ${T}size probe_$1.elf | tail -1 | awk -v n=$1 '{printf "%-10s text+data %d  bss %d\n", n, $1, $3}'
}
probe base ""
probe delay -DPROBE_DELAY
probe reverb -DPROBE_REVERB
probe plate -DPROBE_PLATE
probe delaycore -DPROBE_DELAYCORE
probe fdn -DPROBE_FDN
probe amp -DPROBE_AMP
probe chorus -DPROBE_CHORUS
probe comp -DPROBE_COMP
probe drive -DPROBE_DRIVE
probe phaser -DPROBE_PHASER
probe proto -DPROBE_PROTO
probe stock -DPROBE_STOCK
