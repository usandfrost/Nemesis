// Static instruction counts of the per-sample kernels on the Cortex-M7 (arm-none-eabi-g++ -O2, firmware flags): one noinline function per
// kernel, so the disassembly of that function is the work of ONE sample (straight-line count; untaken branches are included, so it is an
// upper bound for a path, a fair size for a cost). Run fx_bench/arm_count.sh.
#include <nemesis/dsp/DelayCore.h>
#include <nemesis/dsp/PlateReverbCore.h>
#include <nemesis/dsp/ReverbCore.h>
#include <nemesis/dsp/CompEffect.h>

#include "proto_fx.h"
#include "stock_fx.h"

using namespace nemesis::dsp;
static float gIn[2] = {0.1f, 0.2f};
static float gOut[2];

static DelayCore gDc;
static PlateReverbCore gPlate;
static ReverbCore gHall;
static proto::Delay gPd;
static proto::Chorus gPc;
static proto::Comp gPk;
static proto::Amp gPa;
static stock::Reverb gSr;
static stock::Delay gSd;
static stock::Chorus gSc;
static stock::Compressor gSk;

#define KERNEL(name, body) __attribute__((noinline, used)) void name() { body; }
KERNEL(k_nemesis_delaycore, gOut[0] = gDc.processSample(gIn[0]))
KERNEL(k_nemesis_plate, auto s = gPlate.processSample(gIn[0]); gOut[0] = s.l; gOut[1] = s.r)
KERNEL(k_nemesis_fdn8, auto s = gHall.processSample(gIn[0]); gOut[0] = s.l; gOut[1] = s.r)
KERNEL(k_proto_delay_stereo, gPd.process(gIn[0], gIn[1], gOut[0], gOut[1]))
KERNEL(k_proto_chorus_stereo, gPc.process(gIn[0], gIn[1], gOut[0], gOut[1]))
KERNEL(k_proto_comp_stereo, gPk.process(gIn[0], gIn[1], gOut[0], gOut[1]))
KERNEL(k_proto_amp_mono, gPa.process(gIn[0], gIn[1], gOut[0], gOut[1]))
KERNEL(k_stock_freeverb, gSr.process(gIn[0], gOut[0], gOut[1]))
KERNEL(k_stock_delay_mono, gOut[0] = gSd.process(gIn[0]))
KERNEL(k_stock_chorus_mono, gOut[0] = gSc.process(gIn[0]))
KERNEL(k_stock_comp_mono, gOut[0] = gSk.process(gIn[0]))

int main() {
    k_nemesis_delaycore(); k_nemesis_plate(); k_nemesis_fdn8(); k_proto_delay_stereo(); k_proto_chorus_stereo(); k_proto_comp_stereo(); k_proto_amp_mono();
    k_stock_freeverb(); k_stock_delay_mono(); k_stock_chorus_mono(); k_stock_comp_mono();
    return (int)gOut[0];
}
