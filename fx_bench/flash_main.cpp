// Flash-size probe: one tiny main per candidate, linked for the Cortex-M7 with the firmware's flags and --gc-sections, on top of the
// CATHODE engine (which is already in the firmware). The difference to the "engine only" link is what the candidate adds to flash.
// Select with -DPROBE_xxx.
#include <nemesis/dsp/CathodeVoiceEngine.h>
#if defined(PROBE_DELAY)
#include <nemesis/dsp/DelayEffect.h>
#elif defined(PROBE_REVERB) || defined(PROBE_HALL)
#include <nemesis/dsp/ReverbEffect.h>
#elif defined(PROBE_PLATE)
#include <nemesis/dsp/PlateReverbCore.h>
#elif defined(PROBE_DELAYCORE)
#include <nemesis/dsp/DelayCore.h>
#elif defined(PROBE_FDN)
#include <nemesis/dsp/ReverbCore.h>
#elif defined(PROBE_AMP)
#include <nemesis/dsp/GampEffect.h>
#elif defined(PROBE_CHORUS)
#include <nemesis/dsp/ChorusEffect.h>
#elif defined(PROBE_COMP)
#include <nemesis/dsp/CompEffect.h>
#elif defined(PROBE_DRIVE)
#include <nemesis/dsp/DriveEffect.h>
#elif defined(PROBE_PHASER)
#include <nemesis/dsp/PhaserEffect.h>
#elif defined(PROBE_PROTO)
#include "proto_fx.h"
#elif defined(PROBE_STOCK)
#include "stock_fx.h"
#endif

using namespace nemesis::dsp;
float gL[32], gR[32];
volatile int gSel = 0;

int main() {
    auto* eng = new CathodeVoiceEngine;
    eng->prepare(44100.0);
    eng->noteOn(60, 0.8f);
    float s = 0;
    for (int i = 0; i < 32; ++i) s += eng->processSample().left;
    gL[0] = s;
#if defined(PROBE_DELAY)
    auto* fx = new DelayEffect; fx->prepare(44100.0, 32); fx->setType(gSel); fx->setTimeMs(300); fx->setFeedbackPercent(30); fx->setTonePercent(50);
    fx->setWidthPercent(0); fx->setModPercent(0); fx->setMixPercent(100); fx->process(gL, gR, 32);
#elif defined(PROBE_REVERB) || defined(PROBE_HALL)
    auto* fx = new ReverbEffect; fx->prepare(44100.0, 32); fx->setType(gSel); fx->setDecayMs(2000); fx->setPreDelayMs(10); fx->setDampPercent(50); fx->setSizePercent(50);
    fx->setDiffusePercent(50); fx->setModPercent(20); fx->setMixPercent(100); fx->process(gL, gR, 32);
#elif defined(PROBE_PLATE)
    auto* fx = new PlateReverbCore; fx->prepare(44100.0); fx->setRt60Seconds(2.0); fx->setPreDelayMs(10); fx->setDamping(0.3); fx->setModDepthSamples(8);
    for (int i = 0; i < 32; ++i) { auto o = fx->processSample(gL[i]); gL[i] = o.l; gR[i] = o.r; }
#elif defined(PROBE_DELAYCORE)
    auto* c = new DelayCore[2]; for (int k = 0; k < 2; ++k) { c[k].prepare(44100.0, 1500.0); c[k].setDelayMs(300); c[k].setFeedback(0.3); c[k].setToneLpHz(5000); c[k].setToneHpHz(40); }
    for (int i = 0; i < 32; ++i) { gL[i] = c[0].processSample(gL[i]); gR[i] = c[1].processSample(gR[i]); }
#elif defined(PROBE_FDN)
    auto* c = new ReverbCore; c->prepare(44100.0, 8, ReverbCore::Mixing::Hadamard, 140.0, 200.0); c->setRt60Seconds(2); c->setDampingHz(6000); c->setDiffuserCount(4);
    c->setDiffuserDelayMs(0, 5); c->setDiffusion(0.7); c->setPreDelayMs(20); c->setModRateHz(0.6); c->setModDepthSamples(4); c->setInputHpHz(100);
    for (int i = 0; i < 32; ++i) { auto o = c->processSample(gL[i]); gL[i] = o.l; gR[i] = o.r; }
#elif defined(PROBE_AMP)
    auto* fx = new GampEffect; fx->prepare(44100.0, 32); fx->setModel(gSel); fx->setGainDb(12); fx->setBassPercent(50); fx->setMidPercent(50); fx->setTreblePercent(50);
    fx->setPresencePercent(50); fx->setCab(2); fx->setMic(0); fx->setLevelDb(0); fx->setMixPercent(100); fx->process(gL, gR, 32);
#elif defined(PROBE_CHORUS)
    auto* fx = new ChorusEffect; fx->prepare(44100.0, 32); fx->setType(gSel); fx->setVoices(3); fx->setRateHz(1); fx->setDepthPercent(50); fx->setMixPercent(50);
    fx->process(gL, gR, 32);
#elif defined(PROBE_COMP)
    auto* fx = new CompEffect; fx->prepare(44100.0, 32); fx->setType(gSel); fx->setThresholdDb(-20); fx->setRatio(4); fx->setAttackMs(5); fx->setReleaseMs(100);
    fx->setMakeupDb(3); fx->setMixPercent(100); fx->process(gL, gR, 32);
#elif defined(PROBE_DRIVE)
    auto* fx = new DriveEffect; fx->prepare(44100.0, 32); fx->setType(gSel); fx->setDriveDb(12); fx->setTonePercent(50); fx->setMixPercent(100); fx->process(gL, gR, 32);
#elif defined(PROBE_PHASER)
    auto* fx = new PhaserEffect; fx->prepare(44100.0, 32); fx->setRateHz(1); fx->setMixPercent(50); fx->process(gL, gR, 32);
#elif defined(PROBE_PROTO)
    static proto::Delay d; static proto::Chorus c; static proto::Comp k; static proto::Amp a;
    d.init(44100, 2); d.set(300, 0.3f, 5000, gSel); c.init(44100); c.set(1, 0.5f, 0.5f); k.init(44100); k.set(-20, 4, 5, 100, 3); a.init(44100);
    for (int i = 0; i < 32; ++i) { d.process(gL[i], gR[i], gL[i], gR[i]); c.process(gL[i], gR[i], gL[i], gR[i]); k.process(gL[i], gR[i], gL[i], gR[i]); a.process(gL[i], gR[i], gL[i], gR[i]); }
#elif defined(PROBE_STOCK)
    static stock::Delay d; static stock::Chorus c; static stock::Compressor k; static stock::Reverb r; static stock::Flanger f; static stock::Overdrive o; static stock::Distortion di;
    d.init(5000); c.init(); k.init(); r.init(); f.init(); o = {}; di = {};
    for (int i = 0; i < 32; ++i) { float x = gL[i], y; x = d.process(x); x = c.process(x); x = k.process(x); x = f.process(x); x = o.process(x); x = di.process(x); r.process(x, gL[i], y); gR[i] = y; }
#endif
    return (int)(gL[1] + gR[2]);
}
