// render_listen: the 131 library presets through the firmware's REAL code (no shadow mapper): applyPresetIndex / applyPresetFx / fitVoicesToBudget as BROWSE
// loads a preset, wavetable banks through audio::WaveStore from content/data/wtp, the Instrument with its effects section and master.
//   render_listen <outdir> [shard n]   for every library preset (list index >= kBasePresets): fx_NNN.wav (stereo, WITH EFFECTS) and dry_NNN.wav (mono, sends 0
//                                      and amp off), level-matched with ONE gain per preset (the dry twin's), and a JSON line per preset on stdout.
// DBOY_PRESET_SRC=<folder of .PRS files>: render that folder's presets (plus the factory presets, which are skipped unless DBOY_RENDER_FACTORY=1) instead of the library.
// NNN is the preset's number in the firmware's list. Wavetables: DBOY_WTP_DIR (default content/data/wtp next to this file's repo).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include <nemesis/dsp/CathodeOscCore.h>
#include <nemesis/dsp/ForkConfig.h>

#include "PresetHost.h"
#include "app/CostBridge.h"
#include "app/FxParams.h"
#include "app/Model.h"
#include "app/Params.h"
#include "app/SampleLib.h"
#include "app/WaveLib.h"
#include "audio/Instrument.h"
#include "audio/WaveStore.h"

#include "phrases.h"

using namespace app;
using phr::kSr;

namespace {
constexpr double kTail = 2.5;   // seconds added after the phrase so the delay and reverb decay is heard

struct StdioWaves : audio::WaveFile {
    std::string dir;
    FILE* f = nullptr;
    bool open(int bank) override {
        char n[64];
        snprintf(n, sizeof n, "/wt%03d.wtp", bank);
        f = fopen((dir + n).c_str(), "rb");
        return f != nullptr;
    }
    bool read(void* dst, size_t bytes) override { return f && fread(dst, 1, bytes, f) == bytes; }
    void close() override { if (f) fclose(f); f = nullptr; }
};

void setupDsp() {   // the firmware's DSP settings (src/main.cpp, the CMake defaults)
    nemesis::dsp::CathodeOscCore::setLazyMacro(true);
    nemesis::dsp::fork::Config::idleMatrixSkip = true;
    nemesis::dsp::fork::Config::skipSilentSlots = true;
    nemesis::dsp::fork::Config::filterOversample = 2;
    nemesis::dsp::fork::Config::pdOversample = 4;
    nemesis::dsp::fork::Config::driveOversample = 2;
    nemesis::dsp::fork::Config::classicOversample = 0;
    nemesis::dsp::fork::Config::stereoTwin = true;
    nemesis::dsp::fork::Config::lanes = false;
}

struct Stereo { std::vector<float> l, r; bool finite = true; };

// Plays the phrase on part A of `pj` (already holding the preset, its voices and its effects) and returns the master output.
Stereo play(Project& pj, const SampleLib& lib, const phr::Phrase& ph, const std::string& wtDir, std::vector<nemesis::dsp::Real>& arena, std::vector<int16_t>& staging) {
    static Transport tp;
    tp = Transport();
    audio::Instrument* inst = new audio::Instrument;
    inst->init(kSr, pj, tp, lib);
    inst->setAutoDegrade(false);   // the governor off: nothing is degraded here
    StdioWaves files;
    files.dir = wtDir;
    audio::WaveStore st;
    st.init(arena.data(), 3, staging.data(), &inst->sharedTables(), &files);
    uint8_t bits[(kWaveBanks + 7) / 8] = {};
    audio::neededBanks(pj, bits);
    st.want(bits, 1000);
    struct Ev { int at; bool on; size_t n; };
    std::vector<Ev> evs;
    for (size_t k = 0; k < ph.notes.size(); ++k) {
        evs.push_back({(int)(ph.notes[k].t * kSr), true, k});
        evs.push_back({(int)((ph.notes[k].t + ph.notes[k].d) * kSr), false, k});
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
    std::vector<int32_t> handle(ph.notes.size(), -1);
    const int block = 32, total = (int)((ph.len + kTail) * kSr) / block * block;
    Stereo o;
    o.l.resize(total);
    o.r.resize(total);
    size_t e = 0;
    float l[block], r[block];
    for (int b0 = 0; b0 < total; b0 += block) {
        inst->beginBlock();
        while (e < evs.size() && evs[e].at < b0 + block) {
            const Ev& ev = evs[e++];
            const int off = ev.at > b0 ? ev.at - b0 : 0;
            if (ev.on) {
                NoteStart ns{};
                ns.part = 0; ns.pitch = (int8_t)ph.notes[ev.n].n; ns.vel01 = ph.notes[ev.n].v;
                handle[ev.n] = inst->noteOn(ns, off);
            } else {
                inst->noteOff(0, handle[ev.n], off);
            }
        }
        inst->render(l, r, block);
        for (int k = 0; k < block; ++k) {
            if (!std::isfinite(l[k]) || !std::isfinite(r[k])) { o.finite = false; l[k] = r[k] = 0; }
            o.l[b0 + k] = l[k];
            o.r[b0 + k] = r[k];
        }
    }
    delete inst;
    return o;
}

// ---- figures
struct Fig {
    double peak = 0, rmsAll = 0, rmsActive = 0, dc = 0, tailSec = 0, tailDb = -200;
    int clipped = 0;
    bool tailOpen = false, silent = false;
};
// 50 ms RMS envelope of (l+r)/2 energy-wise: sqrt(mean(l^2+r^2)/2), so a mono render and a stereo one are compared on the same footing
std::vector<double> envelope(const std::vector<float>& l, const std::vector<float>* r) {
    const int fl = (int)(0.05 * kSr);
    std::vector<double> env;
    for (size_t s = 0; s + fl <= l.size(); s += fl) {
        double q = 0;
        for (int k = 0; k < fl; ++k) q += (double)l[s + k] * l[s + k] + (r ? (double)(*r)[s + k] * (*r)[s + k] : (double)l[s + k] * l[s + k]);
        env.push_back(sqrt(q / (2.0 * fl)));
    }
    return env;
}
double activeRms(const std::vector<double>& env) {
    double emax = 1e-12, s = 0;
    int n = 0;
    for (double v : env) emax = fmax(emax, v);
    for (double v : env) if (v > emax * 0.0316) { s += v * v; ++n; }
    return n ? sqrt(s / n) : 0.0;
}
double db(double x) { return 20 * log10(x + 1e-12); }

// figures of a (scaled) signal; tail: time after the last note-off until the 50 ms envelope is below -60 dBFS for good
Fig figures(const std::vector<float>& l, const std::vector<float>* r, double gain, double lastOff) {
    Fig f;
    const size_t n = l.size();
    double sum = 0, dcL = 0, dcR = 0;
    for (size_t i = 0; i < n; ++i) {
        const double a = l[i] * gain, b = r ? (*r)[i] * gain : a;
        f.peak = fmax(f.peak, fmax(fabs(a), fabs(b)));
        sum += a * a + b * b;
        dcL += a; dcR += b;
        if (fabs(a) >= 0.999 || fabs(b) >= 0.999) ++f.clipped;
    }
    f.rmsAll = sqrt(sum / (2.0 * n));
    f.dc = fmax(fabs(dcL), fabs(dcR)) / n;
    std::vector<float> sl(n), sr(r ? n : 0);
    for (size_t i = 0; i < n; ++i) sl[i] = (float)(l[i] * gain);
    if (r) for (size_t i = 0; i < n; ++i) sr[i] = (float)((*r)[i] * gain);
    const std::vector<double> env = envelope(sl, r ? &sr : nullptr);
    f.rmsActive = activeRms(env);
    f.silent = f.rmsActive < 3e-4 * 0.1;
    const double thr = pow(10.0, -60.0 / 20.0);
    int last = -1;
    for (size_t k = 0; k < env.size(); ++k) if (env[k] > thr) last = (int)k;
    const double endAbove = (last + 1) * 0.05;   // time the envelope last was above -60 dB
    f.tailOpen = last >= (int)env.size() - 1;
    f.tailSec = fmax(0.0, endAbove - lastOff);
    // the level of the last second of the render (what is left of the tail)
    const int k0 = (int)env.size() - 20;
    double q = 0;
    int c = 0;
    for (int k = fmax(0, k0); k < (int)env.size(); ++k) { q += env[k] * env[k]; ++c; }
    f.tailDb = c ? db(sqrt(q / c)) : -200;
    return f;
}

void writeWav(const char* path, const std::vector<float>& l, const std::vector<float>* r, double gain) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    const int ch = r ? 2 : 1;
    const uint32_t n = (uint32_t)l.size(), bytes = n * 2 * ch, rate = (uint32_t)kSr;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16((uint16_t)ch); u32(rate); u32(rate * 2 * ch); u16((uint16_t)(2 * ch)); u16(16);
    fwrite("data", 1, 4, f); u32(bytes);
    auto s16 = [&](double v) { return (int16_t)lrint(fmax(-1.0, fmin(1.0, v)) * 32767.0); };
    for (uint32_t i = 0; i < n; ++i) {
        const int16_t a = s16(l[i] * gain);
        fwrite(&a, 2, 1, f);
        if (r) { const int16_t b = s16((*r)[i] * gain); fwrite(&b, 2, 1, f); }
    }
    fclose(f);
}

std::string esc(const char* s) {
    std::string o;
    for (; *s; ++s) { if (*s == '"' || *s == '\\') o += '\\'; o += *s; }
    return o;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: render_listen <outdir> [shard n]\n"); return 2; }
    const char* outdir = argv[1];
    const int shard = argc > 2 ? atoi(argv[2]) : 0, nsh = argc > 3 ? atoi(argv[3]) : 1;
    std::string wt = getenv("DBOY_WTP_DIR") ? getenv("DBOY_WTP_DIR") : "";
    if (wt.empty()) { fprintf(stderr, "set DBOY_WTP_DIR=content/data/wtp\n"); return 2; }
    shippedPresets();   // the library from content/presets, as the card gives it
    setupDsp();
    static SampleLib lib;
    static std::vector<int16_t> store(kFactoryFrames);
    buildFactorySamples(lib, store.data(), store.size());
    WaveSet& ws = waveSet();
    for (int b = 0; b < kWaveBanks; ++b) {
        char n[64];
        snprintf(n, sizeof n, "/wt%03d.wtp", b);
        FILE* t = fopen((wt + n).c_str(), "rb");
        ws.present[b] = t != nullptr;
        if (t) fclose(t);
    }
    std::vector<nemesis::dsp::Real> arena(audio::WaveStore::slotSamples() * 3);
    std::vector<int16_t> staging(audio::WaveStore::slotSamples());
    static Project pj;
    const int first = getenv("DBOY_RENDER_FACTORY") ? 0 : kBasePresets;
    for (int k = first + shard; k < presetCount(); k += nsh) {
        const int i = presetIdAt(k);
        presetLoad(i);
        const char* cat = kPresetCats[presetCat(i)];
        const phr::Phrase ph = phr::phraseFor(cat);
        double lastOff = 0;
        for (const auto& n : ph.notes) lastOff = fmax(lastOff, n.t + n.d);
        // load it as BROWSE does (Ui.cpp): the effects, the pages, the voices it was made for as many as it and its effects fit
        makeDefaultProject(pj);
        applyPresetFx(pj.fx, 0, i);
        applyPresetIndex(pj.parts[0].pages, i);
        const int asked = presetVoices(i);
        if (asked > 0) pj.parts[0].voices = (uint8_t)asked;   // the factory presets ask for none: BROWSE leaves the part's own voices
        const int fit = fitVoicesToBudget(pj, 0);
        if (getenv("DBOY_DEBUG_VOICES")) pj.parts[0].voices = (uint8_t)atoi(getenv("DBOY_DEBUG_VOICES"));
        pj.parts[0].preset = (int16_t)i;
        const FxSettings fxOn = pj.fx;
        const Stereo wet = play(pj, lib, ph, wt, arena, staging);
        // the dry twin: the same part and voices, every send 0 and the amp off
        FxSettings dryFx = fxOn;
        for (int k = 0; k < kSends; ++k) dryFx.send[0][k] = 0;
        dryFx.amp[0][0] = 0;
        pj.fx = dryFx;
        const Stereo dry = play(pj, lib, ph, wt, arena, staging);
        // loudness: the dry twin's active RMS (mono sum, as the first page) to -20 dBFS, peak <= 0.89 over BOTH renders; the SAME gain for both
        std::vector<float> dm(dry.l.size());
        for (size_t k = 0; k < dm.size(); ++k) dm[k] = 0.5f * (dry.l[k] + dry.r[k]);
        const double rawActive = activeRms(envelope(dm, nullptr));
        double gain = pow(10.0, (-20.0 - db(rawActive)) / 20.0), pkDry = 0, pkWet = 0, pkDm = 0;
        for (size_t k = 0; k < dm.size(); ++k) { pkDm = fmax(pkDm, fabs(dm[k])); pkDry = fmax(pkDry, fmax(fabs(dry.l[k]), fabs(dry.r[k]))); }
        for (size_t k = 0; k < wet.l.size(); ++k) pkWet = fmax(pkWet, fmax(fabs(wet.l[k]), fabs(wet.r[k])));
        const double gainWanted = gain;
        gain = fmin(gain, 0.89 / fmax(fmax(pkDm, pkWet), 1e-9));
        gain = fmin(gain, 31.6);
        const bool limitedByWet = gain < gainWanted - 1e-9 && 0.89 / fmax(pkWet, 1e-9) < 0.89 / fmax(pkDm, 1e-9);
        // figures: raw (gain 1) and as written
        const Fig rawDry = figures(dry.l, &dry.r, 1.0, lastOff), rawWet = figures(wet.l, &wet.r, 1.0, lastOff);
        const Fig outDry = figures(dm, nullptr, gain, lastOff), outWet = figures(wet.l, &wet.r, gain, lastOff);
        // added energy: whole render, stereo, raw (the same signal path, only the effects differ)
        double eDry = 1e-30, eWet = 0, eDiff = 0, eDryT = 0, eWetT = 0;
        const size_t t0 = (size_t)((lastOff + 0.5) * kSr);
        const double tailN = 2.0 * (double)(wet.l.size() - t0);   // 2 channels
        for (size_t k = 0; k < wet.l.size(); ++k) {
            const double a = dry.l[k], b = dry.r[k], c = wet.l[k], d = wet.r[k];
            eDry += a * a + b * b; eWet += c * c + d * d;
            eDiff += (c - a) * (c - a) + (d - b) * (d - b);
            if (k >= t0) { eDryT += a * a + b * b; eWetT += c * c + d * d; }
        }
        // stereo width of the with-effects render: side/mid energy ratio in dB (-inf: mono)
        double eM = 1e-30, eS = 1e-30;
        for (size_t k = 0; k < wet.l.size(); ++k) { const double m = 0.5 * (wet.l[k] + wet.r[k]), s = 0.5 * (wet.l[k] - wet.r[k]); eM += m * m; eS += s * s; }
        // every note onset must make sound: the dry mono render's RMS in the 60 ms after each note start (a note the instrument dropped leaves it silent)
        int missed = 0;
        for (const auto& nt : ph.notes) {
            const size_t a = (size_t)((nt.t + 0.01) * kSr), z = std::min(dm.size(), a + (size_t)(fmin(nt.d, 0.07) * kSr));
            double q = 0;
            for (size_t k = a; k < z; ++k) q += (double)dm[k] * dm[k];
            if (z <= a || sqrt(q / (z - a)) < 1e-3) ++missed;
        }
        const int silentDry = rawDry.rmsActive < 1e-4, silentWet = rawWet.rmsActive < 1e-4;
        char path[512];
        snprintf(path, sizeof path, "%s/fx_%03d.wav", outdir, i);
        writeWav(path, wet.l, &wet.r, gain);
        snprintf(path, sizeof path, "%s/dry_%03d.wav", outdir, i);
        writeWav(path, dm, nullptr, gain);
        int banks[2];
        presetBanks(i, banks);
        const FxSettings& f = fxOn;
        char b[4096];
        snprintf(b, sizeof b,
                 "{\"i\":%d,\"name\":\"%s\",\"cat\":\"%s\",\"phrase\":\"%s\",\"banks\":[%d,%d],\"bankNames\":[\"%s\",\"%s\"],\"voicesAsked\":%d,\"voices\":%d,"
                 "\"sends\":[%d,%d,%d],\"ampModel\":%d,\"ampName\":\"%s\",\"ampDrive\":%d,\"ampCab\":%d,\"ampLevel\":%d,"
                 "\"delayMs\":%.0f,\"reverbDecayS\":%.2f,\"chorusDepth\":%d,\"gainDb\":%.1f,\"gainLimitedByFx\":%d,\"finiteDry\":%d,\"finiteFx\":%d,"
                 "\"dry\":{\"peakDb\":%.1f,\"rmsDb\":%.1f,\"rmsActiveDb\":%.1f,\"clipped\":%d,\"dc\":%.5f,\"tail60S\":%.2f,\"tailOpen\":%d,\"silent\":%d,\"rawPeakDb\":%.1f,\"rawRmsActiveDb\":%.1f},"
                 "\"fx\":{\"peakDb\":%.1f,\"rmsDb\":%.1f,\"rmsActiveDb\":%.1f,\"clipped\":%d,\"dc\":%.5f,\"tail60S\":%.2f,\"tailOpen\":%d,\"silent\":%d,\"rawPeakDb\":%.1f,\"rawRmsActiveDb\":%.1f,\"widthDb\":%.1f},"
                 "\"notes\":%d,\"notesSilent\":%d,\"addedDb\":%.2f,\"diffDb\":%.1f,\"tailFxDb\":%.1f,\"tailDryDb\":%.1f}",
                 i, esc(presetName(i)).c_str(), cat, ph.name, banks[0], banks[1], banks[0] >= 0 ? esc(waveBankName(banks[0])).c_str() : "", banks[1] >= 0 ? esc(waveBankName(banks[1])).c_str() : "",
                 asked, fit, f.send[0][0], f.send[0][1], f.send[0][2], f.amp[0][0], fxDef(FX_AMP, 0).list[f.amp[0][0]], f.amp[0][1], f.amp[0][2], f.amp[0][7],
                 delayMs(f.delay[0], f.delay[1], pj.bpm), fxReal(fxDef(FX_REVERB, 1), f.reverb[1]), f.chorus[1], db(gain), limitedByWet, dry.finite, wet.finite,
                 db(outDry.peak), db(outDry.rmsAll), db(outDry.rmsActive), rawDry.clipped, outDry.dc, outDry.tailSec, outDry.tailOpen, silentDry, db(rawDry.peak), db(rawDry.rmsActive),
                 db(outWet.peak), db(outWet.rmsAll), db(outWet.rmsActive), rawWet.clipped, outWet.dc, outWet.tailSec, outWet.tailOpen, silentWet, db(rawWet.peak), db(rawWet.rmsActive),
                 10 * log10(eS / eM), (int)ph.notes.size(), missed, 10 * log10((eWet + 1e-30) / eDry), 10 * log10((eDiff + 1e-30) / eDry), 10 * log10(eWetT / tailN + 1e-12) + db(gain), 10 * log10(eDryT / tailN + 1e-12) + db(gain));
        printf("%s\n", b);
        fflush(stdout);
    }
    return 0;
}
