// The Drumboy's instrument for the browser (web/README.md): the firmware's app core, instrument and screen, both CATHODE engines (the full one and the lean
// voice), in one WebAssembly module that runs in a Web Worker. JS feeds it the preset files and the wavetable banks, sends keys, knobs and MIDI notes, and
// pulls audio (128 frames at a time) and the 960x540 screen.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

#include <emscripten.h>

#include <nemesis/dsp/CathodeOscCore.h>
#include <nemesis/dsp/ForkConfig.h>

#include "HostServices.h"
#include "app/Model.h"
#include "app/Params.h"
#include "app/SampleLib.h"
#include "app/MidiExport.h"
#include "audio/Recorder.h"
#include "audio/SongExport.h"
#include "app/Ui.h"
#include "app/WaveLib.h"
#include "audio/Instrument.h"
#include "audio/WaveStore.h"
#include "gfx/BootImage.h"
#include "gfx/Canvas.h"
#include "gfx/Screen.h"
#include "midi/MidiTypes.h"

using namespace app;

namespace {

constexpr double kSr = 44100.0;
constexpr int kBlock = 32;

// The library in memory: the files JS gives, behind the same storage the card implements.
struct MemPresets : PresetStorage {
    std::map<std::string, std::vector<uint8_t>> files;
    std::vector<uint8_t> index;
    bool list(void (*fn)(void*, const char*, uint32_t, uint32_t), void* ctx) override {
        for (auto& kv : files) fn(ctx, kv.first.c_str(), (uint32_t)kv.second.size(), 1u);
        return true;
    }
    bool readFile(const char* name, uint8_t* buf, size_t cap, size_t& n) override {
        n = 0;
        auto it = files.find(name);
        if (it == files.end() || it->second.size() > cap) return false;
        memcpy(buf, it->second.data(), it->second.size());
        n = it->second.size();
        return true;
    }
    bool indexSize(size_t& n) override { if (index.empty()) return false; n = index.size(); return true; }
    bool readIndex(size_t off, uint8_t* buf, size_t len) override {
        if (off + len > index.size()) return false;
        memcpy(buf, index.data() + off, len);
        return true;
    }
    bool writeIndex(size_t off, const uint8_t* buf, size_t len) override {
        if (off == 0) index.clear();
        if (index.size() < off + len) index.resize(off + len);
        memcpy(index.data() + off, buf, len);
        return true;
    }
    bool writeFile(const char* name, const uint8_t* buf, size_t n) override { files[name].assign(buf, buf + n); return true; }
    bool fileExists(const char* name) override { return files.count(name) != 0; }
    bool removeFile(const char* name) override { return files.erase(name) != 0; }
};

// The wavetable banks in memory (wtNNN.wtp), as the card's files are read by the WaveStore.
struct MemWaves : audio::WaveFile {
    std::map<int, std::vector<uint8_t>> banks;
    const std::vector<uint8_t>* cur = nullptr;
    size_t pos = 0;
    bool open(int bank) override {
        auto it = banks.find(bank);
        cur = it == banks.end() ? nullptr : &it->second;
        pos = 0;
        return cur != nullptr;
    }
    bool read(void* dst, size_t bytes) override {
        if (!cur || pos + bytes > cur->size()) return false;
        memcpy(dst, cur->data() + pos, bytes);
        pos += bytes;
        return true;
    }
    void close() override { cur = nullptr; }
};

struct Web;
Web* W = nullptr;

// An export's files: memory here, and the page hands them to the browser as a download
struct WebFiles : audio::ExportFiles {
    struct File { std::string name; std::vector<uint8_t> bytes; };
    static constexpr size_t kMostBytes = 400u * 1024u * 1024u;
    std::vector<File> done;
    File now;
    size_t total = 0;
    bool open(const char*, const char* name) override { now = File{name, {}}; return true; }
    bool write(const void* d, size_t n) override {
        if (total + n > kMostBytes) return false;   // (a page's heap is not a card: this ends as a failed export, not a dead page)
        total += n;
        now.bytes.insert(now.bytes.end(), (const uint8_t*)d, (const uint8_t*)d + n);
        return true;
    }
    bool rewrite(uint32_t at, const void* d, size_t n) override { memcpy(now.bytes.data() + at, d, n); return true; }
    bool close(bool keep) override { if (keep) done.push_back(std::move(now)); return true; }
};

struct WebServices : HostServices {
    void previewPreset(int part, int preset) override;
    void auditionSample(int sample) override;
    void auditionSlice(int sample, int slice) override;
    void wavesChanged() override { wavesDirty = true; }
    bool wavesDirty = false;

    // REC: the firmware's own recorder. A browser has no line input, so a short drum loop made of the built-in samples comes in where the cable would, with a gap after it
    // for AUTO to start on and AUTO STOP to end on. Takes stay in memory until the page goes.
    bool recAvailable() override { return true; }
    void recListen(const RecSettings& s, bool on) override {
        recorder.setSource(s.source == 0 ? audio::Recorder::Line : audio::Recorder::Output);
        recorder.setMonitor(on ? s.monitor / 127.0f : 0.0f);
        recorder.setListening(on);
        lineGain = powf(10.0f, s.gain * 1.5f / 20.0f);
    }
    bool recStart(const RecSettings& s) override {
        if (kept.size() >= 64) return false;   // (a minute each at most: the page has a heap to live in)
        audio::Recorder::Config c;
        c.source = s.source == 0 ? audio::Recorder::Line : audio::Recorder::Output;
        c.channel = s.channel;
        c.autoCapture = s.autoCapture != 0;
        c.threshold = powf(10.0f, s.thresholdDb / 20.0f);
        c.stopSeconds = kRecStops[s.stop < kRecStopChoices ? s.stop : 0];
        c.rate = (uint32_t)kSr;
        take.resize(60 * (size_t)kSr);
        recorder.start(take.data(), (uint32_t)take.size(), c);
        if (c.autoCapture) linePos = (int)line.size() + kLineGap - (int)(kSr * 0.8);   // armed: the loop comes round in a moment and is taken whole
        return true;
    }
    const char* recNoRoom() override { return "THIS PAGE IS FULL OF TAKES: RELOAD IT"; }
    void recStop() override { recorder.stop(); }
    void recStatus(RecStatus& o) override {
        o.state = (uint8_t)recorder.state();
        o.clipped = recorder.clipped();
        o.peak = recorder.peak();
        o.frames = recorder.frames();
        o.capacity = recorder.capacity();
        o.rate = (uint32_t)kSr;
        o.data = recorder.data();
    }
    int recKeep() override;
    int recSaveError() override { return 0; }
    void recDiscard() override { recorder.stop(); recorder.clear(); }

    bool exportStart(int kind, int song) override;
    bool exportStep(ExportStatus& out) override;
    void exportCancel() override { if (exporter) exporter->cancel(); }
    const char* exportWhy() override { return exporter && exporter->why() ? exporter->why() : "TOO LONG FOR THIS PAGE"; }
    const char* exportWhere() override { return "YOUR DOWNLOADS"; }
    const char* exportPlace() override { return "It comes to your browser as a download, several files as a zip."; }
    audio::SongExport* exporter = nullptr;
    int exportsDone = 0;
    WebFiles exported;
    Project* exportScratch = nullptr;

    void makeLine(const SampleLib& samples);
    void listen(float* left, float* right, int frames) {
        for (int i = 0; i < frames; ++i) {
            const float x = linePos < (int)line.size() ? line[linePos] * lineGain : 0.0f;
            const int32_t v = (int32_t)((x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x) * 2147483392.0f);
            rx[2 * i] = rx[2 * i + 1] = (uint32_t)v;
            linePos = (linePos + 1) % ((int)line.size() + kLineGap);
        }
        recorder.block(rx, left, right, frames);
        for (int i = 0; i < frames; ++i) {   // (the level bars: the monitor joins after the instrument's own peak)
            monitorPeak[0] = fabsf(left[i]) > monitorPeak[0] ? fabsf(left[i]) : monitorPeak[0];
            monitorPeak[1] = fabsf(right[i]) > monitorPeak[1] ? fabsf(right[i]) : monitorPeak[1];
        }
    }
    float monitorPeak[2] = {};
    audio::Recorder recorder;
    static constexpr int kLineGap = (int)(2 * kSr);
    std::vector<float> line;
    std::vector<int16_t> take;
    std::deque<std::vector<int16_t>> kept;
    uint32_t rx[2 * kBlock] = {};
    int linePos = 0, nextTake = 1;
    float lineGain = 1.0f;
};

// One bar at 120 BPM: kick, snare and hat from the built-in samples, a quarter of full scale at its loudest, so that GAIN has work to do
void WebServices::makeLine(const SampleLib& samples) {
    static const struct { int sample; float gain; const char* hits; } kBar[] = {
        {1, 1.0f, "x.......x..x...."}, {2, 0.7f, "....x.......x..."}, {5, 0.4f, "..x...x...x...x."}};
    const int sixteenth = (int)(kSr / 8);
    line.assign((size_t)(sixteenth * 16), 0.0f);
    for (const auto& b : kBar) {
        const SampleItem& it = samples.items[b.sample];
        const int frames = (int)((double)it.length * kSr / it.rate);
        for (int h = 0; h < 16; ++h) {
            if (b.hits[h] == '.') continue;
            for (int i = 0; i < frames && h * sixteenth + i < (int)line.size(); ++i)
                line[(size_t)(h * sixteenth + i)] += b.gain * it.data[(size_t)((double)i * it.rate / kSr)] / 32768.0f;
        }
    }
    float top = 0.001f;
    for (const float x : line) top = fabsf(x) > top ? fabsf(x) : top;
    for (float& x : line) x *= 0.25f / top;
    linePos = (int)line.size();
}

struct Web {
    Project* pj = new Project;
    Transport tp;
    SampleLib* lib = new SampleLib;
    std::vector<int16_t> factory = std::vector<int16_t>(kFactoryFrames);
    WebServices sv;
    audio::Instrument* inst = new audio::Instrument;
    Ui* ui = nullptr;
    MemPresets presets;
    MemWaves waves;
    audio::WaveStore store;
    std::vector<nemesis::dsp::Real> arena;
    std::vector<int16_t> staging;
    midi::SpscQueue<64> midiQ;
    uint8_t* frameArena = nullptr;
    gfx::Canvas* canvas = nullptr;
    uint64_t samples = 0;
    uint32_t lastWaves = 0;
    int wantLean = 0;       // the engine asked for; switched in when the stop it needs has been taken
    bool leanPending = false;
    uint8_t previewBanks[(kWaveBanks + 7) / 8] = {};
    uint32_t previewBanksUntil = 0;
    int libraryCount = 0;

    uint32_t nowMs() const { return (uint32_t)(samples * 1000ull / (uint64_t)kSr) + 1000u; }

    void loadNeededWaves() {
        uint8_t bits[(kWaveBanks + 7) / 8];
        audio::neededBanks(*pj, bits);
        if ((int32_t)(previewBanksUntil - nowMs()) > 0)
            for (size_t i = 0; i < sizeof(bits); ++i) bits[i] |= previewBanks[i];
        store.want(bits, nowMs());
    }
};

void WebServices::previewPreset(int part, int preset) {
    if (!presetLoad(preset)) return;
    int b[2];
    presetBanks(preset, b);
    memset(W->previewBanks, 0, sizeof(W->previewBanks));
    for (int s = 0; s < 2; ++s)
        if (b[s] >= 0) W->previewBanks[b[s] >> 3] = (uint8_t)(W->previewBanks[b[s] >> 3] | (1u << (b[s] & 7)));
    W->previewBanksUntil = W->nowMs() + 4000;
    W->loadNeededWaves();
    W->inst->previewPreset(part, preset);
}

bool WebServices::exportStart(int kind, int song) {
    exported.done.clear();
    exported.total = 0;
    if (!exportScratch) exportScratch = new Project;
    if (!exporter) exporter = new audio::SongExport(*W->pj, W->tp, *W->inst, *W->lib, *exportScratch, exported);
    return exporter->start(kind, song);
}

bool WebServices::exportStep(ExportStatus& out) {
    for (int i = 0; i < 3; ++i) {
        if (exporter->step(out)) continue;
        if (out.result == EXPORT_DONE) ++exportsDone;
        return false;
    }
    return true;
}

int WebServices::recKeep() {
    SampleLib& lib = *W->lib;
    if (recorder.state() != audio::Recorder::Done || lib.count >= kMaxSamples || !recorder.frames()) return -1;
    kept.emplace_back(recorder.data(), recorder.data() + recorder.frames());
    SampleItem& it = lib.items[lib.count];
    memset(&it, 0, sizeof(it));
    snprintf(it.name, sizeof(it.name), "REC%04d", nextTake++);
    it.cat = (uint8_t)lib.addCategory("REC");
    it.length = (uint32_t)kept.back().size();
    it.rate = (uint32_t)kSr;
    it.data = kept.back().data();
    recorder.clear();
    return lib.count++;
}

void WebServices::auditionSample(int sample) { W->inst->auditionSample(sample); }   // (as firmware/src/main.cpp does; the built-in samples are always in memory)
void WebServices::auditionSlice(int sample, int slice) { W->inst->auditionSlice(sample, slice); }

void pushMidi(midi::Type t, int note, int vel) {
    midi::Event e;
    e.type = t;
    e.channel = 15;
    e.data1 = (uint8_t)note;
    e.data2 = (uint8_t)vel;
    W->midiQ.push(e);
}

}  // namespace

extern "C" {

// ---- start-up: the module, then the library (wb_preset_add per file, then wb_presets_ready), then banks as they arrive (wb_bank_add)
EMSCRIPTEN_KEEPALIVE void wb_init() {
    if (W) return;
    W = new Web;
    nemesis::dsp::CathodeOscCore::setLazyMacro(true);
    nemesis::dsp::fork::Config::idleMatrixSkip = true;
    nemesis::dsp::fork::Config::skipSilentSlots = true;
    nemesis::dsp::fork::Config::filterOversample = 2;
    nemesis::dsp::fork::Config::pdOversample = 4;
    nemesis::dsp::fork::Config::driveOversample = 2;
    nemesis::dsp::fork::Config::classicOversample = 0;
    nemesis::dsp::fork::Config::stereoTwin = true;
    nemesis::dsp::fork::Config::lanes = false;
    makeDefaultProject(*W->pj);
    buildFactorySamples(*W->lib, W->factory.data(), W->factory.size());
    W->sv.makeLine(*W->lib);
    presetsInit(new PresetInfo[kPresetCap], kPresetCap);
    W->ui = new Ui(*W->pj, W->tp, *W->lib, W->sv);
    W->inst->init(kSr, *W->pj, W->tp, *W->lib);
    W->inst->setClockMs([]() -> uint32_t { return W->nowMs(); });   // the sequencer stamps when a lane played (the layer LED flash)
    W->inst->setMidi(&W->midiQ, nullptr);
    W->inst->setLivePart(0);
    const size_t slotN = audio::WaveStore::slotSamples();
    const int slots = audio::WaveStore::kMaxSlots;
    W->arena.assign(slotN * slots, 0);
    W->staging.assign(slotN, 0);
    W->store.init(W->arena.data(), slots, W->staging.data(), &W->inst->sharedTables(), &W->waves);
    W->frameArena = (uint8_t*)aligned_alloc(8, 1 << 20);
    W->canvas = new gfx::Canvas(W->frameArena, 1 << 20);
    for (int b = 0; b < kWaveBanks; ++b) waveSet().present[b] = false;
}

EMSCRIPTEN_KEEPALIVE void wb_preset_add(const char* name, const uint8_t* data, int n) { W->presets.files[name].assign(data, data + n); }
EMSCRIPTEN_KEEPALIVE int wb_presets_ready() {
    const PresetOpen r = presetsOpen(W->presets);
    W->libraryCount = r.count;
    return r.count;
}
EMSCRIPTEN_KEEPALIVE void wb_bank_add(int bank, const uint8_t* data, int n) {
    W->waves.banks[bank].assign(data, data + n);
    if (bank >= 0 && bank < kWaveBanks) waveSet().present[bank] = true;
}
EMSCRIPTEN_KEEPALIVE int wb_bank_have(int bank) { return W->waves.banks.count(bank) != 0; }

// ---- the library
EMSCRIPTEN_KEEPALIVE int wb_preset_count() { return presetCount(); }
EMSCRIPTEN_KEEPALIVE int wb_preset_id(int k) { return presetIdAt(k); }
EMSCRIPTEN_KEEPALIVE const char* wb_preset_name(int id) { return presetName(id); }
EMSCRIPTEN_KEEPALIVE int wb_preset_cat(int id) { return presetCat(id); }
EMSCRIPTEN_KEEPALIVE int wb_preset_voices(int id) { return presetVoices(id); }
EMSCRIPTEN_KEEPALIVE const char* wb_cat_name(int c) { return c >= 0 && c < kPresetCatCount ? kPresetCats[c] : "?"; }
EMSCRIPTEN_KEEPALIVE void wb_preset_banks(int id, int* out) { presetBanks(id, out); }

// Puts the preset on part A as the panel's BROWSE does: its sound, voices and effects (which also asks for its banks).
EMSCRIPTEN_KEEPALIVE int wb_load_preset(int id) {
    if (W->ui->exporting()) return 0;
    if (!presetLoad(id)) return 0;
    if (!W->ui->debugLoadPreset(0, id)) return 0;
    W->loadNeededWaves();
    return 1;
}
// 1 when every bank the project (and a preview) plays is in memory
EMSCRIPTEN_KEEPALIVE int wb_ready() {
    uint8_t bits[(kWaveBanks + 7) / 8];
    audio::neededBanks(*W->pj, bits);
    if ((int32_t)(W->previewBanksUntil - W->nowMs()) > 0)
        for (size_t i = 0; i < sizeof(bits); ++i) bits[i] |= W->previewBanks[i];
    for (int b = 0; b < kWaveBanks; ++b)
        if ((bits[b >> 3] & (1u << (b & 7))) && W->waves.banks.count(b) && !W->store.resident(b)) return 0;
    return 1;
}
// The three notes the panel plays when a row is picked in BROWSE
EMSCRIPTEN_KEEPALIVE void wb_device_preview(int id) { if (!W->ui->exporting()) W->sv.previewPreset(0, id); }

// ---- engine and sound
EMSCRIPTEN_KEEPALIVE void wb_set_engine(int lean) {
#if DBOY_LEAN
    if (W->inst->lean() == (lean != 0) && !W->leanPending) return;
    W->wantLean = lean;
    W->leanPending = true;
    W->inst->stopAllSound();   // taken in the next block; the engine is switched after that
#endif
}
EMSCRIPTEN_KEEPALIVE int wb_engine() {
#if DBOY_LEAN
    return W->inst->lean() ? 1 : 0;
#else
    return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE int wb_exporting() { return W->ui->exporting() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE void wb_note_on(int note, int vel) { if (W->ui->exporting()) return; pushMidi(midi::Type::NoteOn, note, vel); }
EMSCRIPTEN_KEEPALIVE void wb_note_off(int note) { if (W->ui->exporting()) return; pushMidi(midi::Type::NoteOff, note, 0); }
EMSCRIPTEN_KEEPALIVE void wb_all_off() { if (!W->ui->exporting()) W->inst->stopAllSound(); }
EMSCRIPTEN_KEEPALIVE void wb_master(int v) { if (W->ui->exporting()) return; W->pj->master = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v); }
EMSCRIPTEN_KEEPALIVE float wb_peak(int ch) { const float a = W->inst->peak(ch), b = W->sv.monitorPeak[ch & 1]; return a > b ? a : b; }
EMSCRIPTEN_KEEPALIVE void wb_clear_peaks() { W->inst->clearPeaks(); W->sv.monitorPeak[0] = W->sv.monitorPeak[1] = 0.0f; }
EMSCRIPTEN_KEEPALIVE int wb_voices() { int n = 0; for (int p = 0; p < kParts; ++p) n += W->inst->activeVoices(p); return n; }

// ---- the panel
EMSCRIPTEN_KEEPALIVE void wb_key(int k, int down) { if (k < 0 || k >= (int)Key::Count) return; down ? W->ui->keyDown((Key)k) : W->ui->keyUp((Key)k); }
EMSCRIPTEN_KEEPALIVE void wb_layer(int i, int down) { if (i < 0 || i >= kLanes) return; down ? W->ui->layerDown(i) : W->ui->layerUp(i); }
// What the panel's knobs show: where each value sits between its limits, 0..1, or below 0 when it has none (an action, say)
EMSCRIPTEN_KEEPALIVE void wb_knob_places(float* out) {
    for (int i = 0; i < 8; ++i) out[i] = W->ui->knobPlace(i);
}
// A knob dragged to a place on its travel. The firmware only knows detents, so it turns one at a time until the value gets there or stops moving: that keeps
// everything a turn does (locks, undo, the cost ladder), and the firmware's own limits are the knob's stops. A value stays until the pointer is nearer the next one: going
// on to the first value past the pointer would flip a short list between two values at every pixel of a drag, and each flip does what a turn does.
EMSCRIPTEN_KEEPALIVE void wb_knob_to(int enc, float target) {
    if (W->ui->exporting()) return;
    constexpr int kMostDetents = 1024;
    if (enc < 0 || enc >= 8) return;
    float stop = 0.0f;
    float was = W->ui->knobPlace(enc, &stop);
    const float near = stop * 0.5f;
    if (was < 0.0f || fabsf(target - was) <= near) return;
    const int dir = target > was ? 1 : -1;
    for (int n = 0; n < kMostDetents; ++n) {
        W->ui->turn(enc, dir);
        const float now = W->ui->knobPlace(enc);
        if (now < 0.0f || now == was || (dir > 0 ? now >= target - near : now <= target + near)) return;
        was = now;
    }
}
// The computer keyboard's piano in the tracker (panel.js): the note goes on the cursor's step by the firmware's own road, OK on an empty step and then the NOTE knob, a detent at a
// time, so that IN SCALE, the limit and undo are what they are on the panel. `offset`: from the lane's root.
EMSCRIPTEN_KEEPALIVE void wb_tracker_note(int offset) {
    if (W->ui->exporting()) return;
    Ui& ui = *W->ui;
    if (!ui.tracker()) return;
    if (!trig(ui.lane(), ui.cursor(), false)) { ui.keyDown(Key::Ok); ui.keyUp(Key::Ok); }
    for (int n = 0, was = ui.trackerNote(); n < 48 && was != offset; ++n) {
        const int dir = offset > was ? 1 : -1;
        ui.turn(4, dir);
        const int now = ui.trackerNote();
        if (now == was || (now - offset) * dir > 0) break;   // the limit, or IN SCALE has gone by a note outside the scale
        was = now;
    }
}
EMSCRIPTEN_KEEPALIVE void wb_turn(int enc, int detents) { W->ui->turn(enc, detents < -64 ? -64 : detents > 64 ? 64 : detents); }
EMSCRIPTEN_KEEPALIVE uint32_t wb_revision() { return W->ui->revision(); }
static uint16_t stripBuf[gfx::kWidth * 24];
static void stripToRgba(uint8_t* rgba, int y0, int rows) {
    for (int i = 0; i < rows * gfx::kWidth; ++i) {
        const uint16_t p = stripBuf[i];
        uint8_t* d = rgba + ((size_t)y0 * gfx::kWidth + (size_t)i) * 4;
        d[0] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
        d[1] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
        d[2] = (uint8_t)((p & 31) * 255 / 31);
        d[3] = 255;
    }
}
// The screen as RGBA (960 x 540 x 4 bytes)
EMSCRIPTEN_KEEPALIVE void wb_frame(uint8_t* rgba) {
    gfx::drawScreen(*W->canvas, *W->ui);
    for (int y0 = 0; y0 < gfx::kHeight; y0 += 24) {
        const int rows = gfx::kHeight - y0 < 24 ? gfx::kHeight - y0 : 24;
        W->canvas->raster(stripBuf, y0, rows);
        stripToRgba(rgba, y0, rows);
    }
}
// The boot image, through the firmware's decoder
EMSCRIPTEN_KEEPALIVE void wb_boot_image(uint8_t* rgba) {
    gfx::BootImage img;
    for (int y0 = 0; y0 < gfx::kHeight; y0 += 24) {
        const int rows = gfx::kHeight - y0 < 24 ? gfx::kHeight - y0 : 24;
        img.rows(stripBuf, rows);
        stripToRgba(rgba, y0, rows);
    }
}

// The ten layer-key LEDs, as board/Leds.cpp lights them (same rules; `ms` is the page's clock for the blinks): in STEP the lit steps of the page
// (the cursor blinks, an open lock blinks fast, bits 8 and 9: a page before / after), elsewhere the selected lane, flipped for a moment when a lane plays.
EMSCRIPTEN_KEEPALIVE uint32_t wb_layer_leds(uint32_t ms) {
    Ui& ui = *W->ui;
    const Transport& tp = ui.transport();
    const Lane& ln = ui.lane();
    const bool blink = (ms / 250) & 1, fast = (ms / 90) & 1;
    uint32_t bits = 0;
    if (ui.mode() == Mode::Step && !ui.tracker()) {
        const int s0 = ui.stepPage() * 8;
        for (int i = 0; i < 8; ++i) {
            const int idx = s0 + i;
            if (idx >= ln.n) continue;
            bool on = trig(ln, idx, tp.fill);
            if (idx == ui.cursor()) on = on != blink;
            if (ui.lockOpen() && ui.lockLane() == ui.sel() && ui.lockIdx() == idx) on = fast;
            if (on) bits |= 1u << i;
        }
        if (ui.stepPage() > 0) bits |= 1u << 8;
        if (s0 + 8 < ln.n) bits |= 1u << 9;
    } else {
        for (int i = 0; i < kLanes; ++i) {
            bool on = i == ui.sel();
            if (tp.playing && (uint32_t)(W->nowMs() - tp.hitAtMs[i]) < 80) on = !on;   // (the flash is on the firmware's clock; `ms` is only for the blinks)
            if (on) bits |= 1u << i;
        }
    }
    return bits;
}

constexpr int kStateValues = 83;   // (engine-worker.js reads this many)
// What the Learn tab reads (web/www/learn.js): the firmware's own state, so a lesson moves on only when the panel really got there. kStateValues values, in the order readState expects.
EMSCRIPTEN_KEEPALIVE void wb_state(int32_t* out) {
    Ui& ui = *W->ui;
    const Project& pj = *W->pj;
    const Transport& tp = ui.transport();
    const Lane& ln = ui.lane();
    const int pi = ui.part();
    const Step& st = ln.st[ui.cursor() < kMaxSteps ? ui.cursor() : 0];
    uint32_t steps = 0, muted = 0;
    for (int i = 0; i < kMaxSteps; ++i) if (i < ln.n && trig(ln, i, false)) steps |= 1u << i;
    for (int i = 0; i < kLanes; ++i) if (ui.pattern().lanes[i].mute) muted |= 1u << i;
    const Song& so = pj.songs[tp.songIdx];
    const int row = ui.songRow() < so.nRows ? ui.songRow() : 0;
    int32_t v[kStateValues] = {(int32_t)ui.mode(), pi, ui.sel(), ui.page(), tp.playing ? 1 : 0, tp.shownStep, tp.pat, ui.shift() ? 1 : 0, ui.fxView(), ui.stepPage(),
                     ui.cursor(), ui.browseCat(), ui.browseIdx(), (int32_t)steps, (int32_t)muted, pj.parts[pi].pages[PG_FLT1][1],
                     ln.level, ln.n, ln.eu, ln.k, pj.parts[0].preset, pj.parts[1].preset, pj.parts[2].preset, pj.parts[0].level, pj.parts[1].level, pj.parts[2].level,
                     pj.parts[0].voices, pj.parts[1].voices, pj.parts[2].voices, pj.master, pj.bpm, so.nRows, tp.songPlaying, row, so.rows[row].pat, so.rows[row].rep,
                     ui.lockOpen() ? 1 : 0, ui.lockIdx(), st.nLocks, st.note, st.vel, st.gate, ui.mixSub(), ui.stepSub(),
                     pj.fx.send[pi][0], pj.fx.send[pi][1], pj.fx.send[pi][2], pj.fx.amp[pi][0]};
    if (ui.mode() == Mode::Sound) for (int i = 0; i < 8; ++i) v[48 + i] = ui.shownValue(ui.page(), i);
    v[56] = pj.parts[pi].type;
    v[57] = ln.part;
    v[58] = ln.sample();
    // the lane's sample, cut and looped: what the SLICE and LOOP pages have really done
    const SliceMap* cut = findSlices(pj, ln.sample());
    const int16_t* sm = ln.smp[lanePageIndex(PG_SMP)];
    const int16_t* lp = ln.smp[lanePageIndex(PG_LOOP)];
    const LoopFit* fit = findLoopFit(pj, ln.sample(), sm[1], sm[2], lp[LP_AUTO], lp[LP_SEARCH], loopFadeKey(lp));
    v[59] = cut ? cut->count : 0;
    v[60] = ui.sliceEdit() ? 1 : 0;
    v[61] = ui.sliceSel();
    v[62] = st.prob;
    v[63] = st.lockValue(lockKey(PG_SLC, SLC_SLICE), 0);
    v[64] = fit && fit->frames == W->lib->items[ln.sample()].length ? 1 : 0;   // the search has landed (with no better points, too: the screen says which)
    v[65] = ui.trackerRow();
    v[66] = ui.popover() ? 1 : 0;
    v[67] = st.chord;
    v[68] = ln.root;
    v[69] = isSampler(pj, ln.part) ? 1 : 0;   // (v[56] is the part on screen, and PART can leave that on another)
    const RecSettings& rs = ui.recSettings();
    v[70] = (int32_t)W->sv.recorder.state();
    v[71] = rs.source;
    v[72] = rs.gain;
    v[73] = rs.autoCapture;
    v[74] = rs.monitor;
    v[75] = rs.stop;
    SlotInfo si;
    v[76] = ui.slot();
    v[77] = ui.loadedSlot();
    v[78] = ui.slot() >= 0 && ui.slotInfo(ui.slot(), si) ? 1 : 0;
    v[79] = ui.deleteAsked();
    v[80] = ui.projSub();
    v[81] = W->sv.exportsDone;
    v[82] = ui.exportKind();
    memcpy(out, v, sizeof(v));
}

// A wavetable bank the project plays and the page has not fetched yet, or -1. Loading a preset from the panel only names its banks; the worker fetches them from here.
EMSCRIPTEN_KEEPALIVE int wb_missing_bank() {
    uint8_t bits[(kWaveBanks + 7) / 8];
    audio::neededBanks(*W->pj, bits);
    if ((int32_t)(W->previewBanksUntil - W->nowMs()) > 0)
        for (size_t i = 0; i < sizeof(bits); ++i) bits[i] |= W->previewBanks[i];
    for (int b = 0; b < kWaveBanks; ++b)
        if ((bits[b >> 3] >> (b & 7)) & 1 && !W->waves.banks.count(b)) return b;
    return -1;
}

// ---- what EXPORT made, for the page to hand to the browser; wb_export_clear when it has them
EMSCRIPTEN_KEEPALIVE int wb_export_count() { return W->ui->exporting() ? 0 : (int)W->sv.exported.done.size(); }
EMSCRIPTEN_KEEPALIVE const char* wb_export_name(int i) { return W->sv.exported.done[(size_t)i].name.c_str(); }
EMSCRIPTEN_KEEPALIVE int wb_export_size(int i) { return (int)W->sv.exported.done[(size_t)i].bytes.size(); }
EMSCRIPTEN_KEEPALIVE const uint8_t* wb_export_data(int i) { return W->sv.exported.done[(size_t)i].bytes.data(); }
EMSCRIPTEN_KEEPALIVE void wb_export_clear() { W->sv.exported.done.clear(); }

// ---- audio: `frames` (a multiple of 32) into left/right
EMSCRIPTEN_KEEPALIVE void wb_render(float* left, float* right, int frames) {
    Web& w = *W;
    w.sv.now = w.nowMs();
    w.ui->tick();
    if (w.ui->exporting()) {   // the instrument is taken: silence, and the clock goes on
        memset(left, 0, sizeof(float) * (size_t)frames);
        memset(right, 0, sizeof(float) * (size_t)frames);
        w.samples += (uint64_t)frames;
        return;
    }
    if (w.sv.wavesDirty || w.nowMs() - w.lastWaves >= 150) { w.sv.wavesDirty = false; w.lastWaves = w.nowMs(); w.loadNeededWaves(); }
#if DBOY_LEAN
    if (w.leanPending && w.inst->stopsTaken()) { w.inst->setLean(w.wantLean != 0); w.leanPending = false; }
#endif
    for (int off = 0; off < frames; off += kBlock) {
        w.inst->beginBlock();
        w.inst->render(left + off, right + off, kBlock);
        w.sv.listen(left + off, right + off, kBlock);
        w.samples += kBlock;
    }
}

}  // extern "C"
