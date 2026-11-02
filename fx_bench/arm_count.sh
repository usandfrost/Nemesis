#!/bin/zsh
# fx_bench/arm_count.sh SCRATCH_DIR  (after flash_size.sh has built SCRATCH_DIR/arm/libfork.a / libfx.a)
S=${1:?scratch dir}
T=${ARM_GNU_ROOT:-$HOME/toolchains/arm-gnu-pkg/Payload}/bin/arm-none-eabi-
R=$(cd "$(dirname "$0")/.." && pwd)
FL=(-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -O2 -ffunction-sections -fno-exceptions -fno-rtti -fno-math-errno -std=gnu++17
    -fsingle-precision-constant -w -Dthread_local= -DNEMESIS_DSP_FLOAT=1 -DNEMESIS_DSP_FAST_MATH=1 -DNEMESIS_FORK_EXACT=1 -DNEMESIS_FORK_APPROX=1
    -DNEMESIS_FORK_MAX_VOICES=8 -DDSP_IS_FORK=1 -I$S/tree/inc -I$R/firmware/dsp/include -I$R/fx_bench)
cd $S/arm
${T}g++ $FL -c $R/fx_bench/arm_kernels.cpp -o arm_kernels.o
${T}g++ -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections arm_kernels.o libfx.a libfork.a -o arm_kernels.elf 2>/dev/null
${T}objdump -d --no-show-raw-insn -C arm_kernels.elf > arm_kernels.dis
python3 -I - arm_kernels.dis <<'PY'
import re, sys
fn, cur, calls, cnt = {}, None, {}, {}
for ln in open(sys.argv[1]):
    m = re.match(r'^[0-9a-f]+ <(.*)>:$', ln.rstrip())
    if m:
        cur = m.group(1); cnt[cur] = 0; calls[cur] = []
        continue
    if cur and re.match(r'^\s+[0-9a-f]+:', ln):
        cnt[cur] += 1
        m2 = re.search(r'\tbl?\t[0-9a-f]+ <(.*)>', ln)
        if m2 and not m2.group(1).startswith('__aeabi'):
            calls[cur].append(m2.group(1).split('+')[0])
def total(f, seen=()):
    if f in seen or f not in cnt: return 0, []
    t, ext = cnt[f], []
    for c in set(calls[f]):
        if c == f: continue
        if c in cnt and c not in seen + (f,):
            sub, e = total(c, seen + (f,)); t += sub; ext += e
        elif c not in cnt:
            ext.append(c)
    return t, ext
for f in sorted(cnt):
    if f.startswith('k_'):
        t, ext = total(f)
        print("%-26s %4d instructions (incl. callees)   libm/external: %s" % (f, t, " ".join(sorted(set(ext))) or "-"))
PY
