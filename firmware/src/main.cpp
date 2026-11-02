#include "main.h"

#include <string.h>

#include "Global.h"
#include <nemesis/dsp/CathodeOscCore.h>
#include <nemesis/dsp/ForkConfig.h>
#include "app/Model.h"
#include "app/SampleLib.h"
#include "app/Services.h"
#include "app/Ui.h"
#include "audio/AudioEngine.h"
#include "app/MidiExport.h"
#include "audio/SongExport.h"
#include "audio/Instrument.h"
#include "audio/WaveStore.h"
#include "bdma.h"
#include "board/Card.h"
#include "board/CardSoak.h"
#include "board/DebugHook.h"
#include "board/Input.h"
#include "board/Lcd.h"
#include "board/Leds.h"
#include "codec/Dac.h"
#include "dma.h"
#include "fmc.h"
#include "gfx/BootImage.h"
#include "gfx/Canvas.h"
#include "gfx/Screen.h"
#include "gpio.h"
#include "i2c.h"
#include "i2s.h"
#include "midi/MidiIn.h"
#include "midi/MidiOut.h"
#include "midi/PresetLink.h"
#include "midi/MidiOut.h"
#include "memory/Mpu.h"
#include "app/PresetFile.h"
#include "memory/SdramHeap.h"
#include "perf/CycleCounter.h"
#include "sdmmc.h"
#include "tim.h"
#include "usart.h"

extern "C" void SystemClock_Config();
extern "C" void PeriphCommonClock_Config();
extern "C" void Sdram_Initialize();
extern "C" void System_Initialize();

// I2S DMA buffers: AXI SRAM, reachable by DMA1/2. One 1 KB block so the MPU can keep it uncached (DmaBuffers.h).
DmaBuffers dmaBuffers = {};

// I2C transfer scratch in D3 RAM, as in the stock firmware (BDMA can only reach D3).
__attribute__((section(".RAM_D3"))) uint8_t i2c3_rxData[2] = {0x00};
__attribute__((section(".RAM_D3"))) uint8_t i2c3_txData[2] = {0x00};
__attribute__((section(".RAM_D3"))) uint8_t i2c4_rxData[2] = {0x00};
__attribute__((section(".RAM_D3"))) uint8_t i2c4_txData[2] = {0x00};

static Dac dac;
static audio::Instrument instrument;
static app::Project* project = nullptr;
static app::Project* scratchProject = nullptr;
static int firstCardSample = 0;

// An export's files, on the card
class CardExport : public audio::ExportFiles {
   public:
    bool open(const char* folder, const char* name) override { return board::card.exportOpen(folder, name); }
    bool write(const void* data, size_t n) override { return board::card.exportWrite(data, n); }
    bool rewrite(uint32_t at, const void* data, size_t n) override { return board::card.exportRewrite(at, data, n); }
    bool close(bool keep) override;
};
static CardExport exportFiles;
static audio::SongExport* exporter = nullptr;
static bool rendering = false;
static app::Transport transport;
static app::SampleLib* samples = nullptr;
static app::Ui* ui = nullptr;
alignas(8) static uint8_t frameArena[64 * 1024];   // one frame's display list (the busiest screen needs ~9 KB)
static board::Lcd lcd;
static gfx::Canvas* canvas = nullptr;
static constexpr uint32_t kBootImageMs = 2000;   // the least the picture stays up. Nothing waits for it: the rest of the start-up and the audio run on behind it

// Before a long card operation (a project load, a save): the screen says so, since nothing is drawn while it runs.
static void showBusy(const char* msg) {
    if (!ui || !canvas) return;
    ui->say(msg);
    gfx::drawScreen(*canvas, *ui);
    lcd.present(*canvas);
}

static void showBusy(const char* msg);
// The librarian asks for a song as a MIDI file. Not during an export: that has the scratch project, and the instrument
struct SongMidiBuffer : app::ByteSink {
    uint8_t* at = nullptr;
    uint32_t used = 0;
    static constexpr uint32_t kBytes = 256u * 1024u;
    bool write(const void* d, size_t n) override {
        if (used + n > kBytes) return false;
        memcpy(at + used, d, n);
        used += (uint32_t)n;
        return true;
    }
};
static SongMidiBuffer songMidiOut;
static const uint8_t* songMidi(int song, uint32_t& size);
static midi::PresetLink presetLink(midi::sendLong, showBusy);   // the website's librarian, over the MIDI sockets
static int notFitting = -1;   // a sample that did not fit even after eviction: not tried again until the next project load

// What the UI needs from the board: time, sound, the card.
// the wavetable banks (the OSC pages' WAVE 4 and up; see loadNeededWaves below)
static audio::WaveStore waveStore;
static int waveRefused = -1;
static uint32_t wavesDirtyAt = 0;                            // a preset or the WAVE knob changed: look once the knob has settled (0: nothing pending)
static uint8_t previewBanks[(app::kWaveBanks + 7) / 8];      // the banks of the preset being browsed: kept in memory while it plays
static uint32_t previewBanksUntil = 0;
static void loadNeededWaves();
static audio::Recorder recorder;
static bool lineInOn = false;
static bool recording() { return recorder.state() != audio::Recorder::Idle; }

static void pickCardDrums(app::Project& pj, const app::SampleLib& lib, int firstCard) {
    static const char* const kCats[3] = {"KICK", "SNARE", "HHCLOSED"};
    for (int j = 0; j < 3; ++j) {
        int found = -1;
        for (int i = firstCard; i < lib.count && found < 0; ++i)
            if (strcmp(lib.cats[lib.items[i].cat], kCats[j]) == 0) found = i;
        if (found < 0) continue;
        for (app::Pattern& p : pj.patterns) p.lanes[7 + j].smp[0][0] = (int16_t)found;
    }
}

bool CardExport::close(bool keep) {
    if (keep) showBusy("CHECKING\xE2\x80\xA6");   // (all of it is read back)
    return board::card.exportClose(keep);
}

class BoardServices : public app::Services {
   public:
    void holdAudio(bool hold) override {
        static uint32_t primask = 0;
        if (hold) { primask = __get_PRIMASK(); __disable_irq(); }   // the audio DMA interrupt is priority 0: only PRIMASK holds it
        else __set_PRIMASK(primask);
    }
    uint32_t nowMs() override { return HAL_GetTick(); }
    uint32_t random() override {
        seed_ ^= seed_ << 13;
        seed_ ^= seed_ >> 17;
        seed_ ^= seed_ << 5;
        return seed_;
    }
    void auditionSample(int s) override {
        // into the audition buffer (the cache is for what the project plays)
        auditionLoad_ = board::card.loadForAudition(*samples, s);
        if (auditionLoad_ == board::Card::Load::Ok) instrument.auditionSample(s);
    }
    const char* auditionProblem() override {
        switch (auditionLoad_) {
            case board::Card::Load::Ok: return nullptr;
            case board::Card::Load::Full: return "TOO LONG TO PREVIEW \xC2\xB7 IT CAN STILL PLAY ON A LANE";   // more than an audition buffer holds, about 12 s
            case board::Card::Load::NoCard: return "NO CARD";
            default: return "THIS FILE CANNOT BE READ";
        }
    }
    bool recAvailable() override { return true; }
    void recListen(const app::RecSettings& s, bool on) override {
        const bool line = on && s.source == 0;
        if (line && !lineInOn) { dac.enableLineIn(); lineInOn = true; }   // (a moment: the input settles. Off when not in use: the ADC adds a little hiss)
        if (!line && lineInOn) { dac.disableLineIn(); lineInOn = false; }
        if (line) dac.setLineInGain(s.gain);
        recorder.setSource(s.source == 0 ? audio::Recorder::Line : audio::Recorder::Output);
        recorder.setMonitor(on ? s.monitor / 127.0f : 0.0f);
        recorder.setListening(on);
    }
    bool recStart(const app::RecSettings& s) override {
        size_t frames = 0;
        int16_t* at = board::card.lendPool(frames);
        if (frames < AUDIO_SAMPLE_RATE / 2) { board::card.endLend(); return false; }
        audio::Recorder::Config c;
        c.source = s.source == 0 ? audio::Recorder::Line : audio::Recorder::Output;
        c.channel = s.channel;
        c.autoCapture = s.autoCapture != 0;
        c.threshold = powf(10.0f, s.thresholdDb / 20.0f);
        c.stopSeconds = app::kRecStops[s.stop < app::kRecStopChoices ? s.stop : 0];
        c.rate = (uint32_t)AUDIO_SAMPLE_RATE;
        holdAudio(true);
        recorder.start(at, (uint32_t)frames, c);
        holdAudio(false);
        return true;
    }
    void recStop() override {
        recorder.stop();
        if (recorder.state() == audio::Recorder::Idle) board::card.endLend();   // it was only waiting (read after: the sound may have come just now)
    }
    void recStatus(app::RecStatus& o) override {
        o.state = (uint8_t)recorder.state();   // (the same order)
        o.clipped = recorder.clipped();
        o.peak = recorder.peak();
        o.frames = recorder.frames();
        o.capacity = recorder.capacity();
        o.rate = (uint32_t)AUDIO_SAMPLE_RATE;
        o.data = recorder.data();
    }
    int recKeep() override {
        if (recorder.state() != audio::Recorder::Done) return -1;
        showBusy("SAVING\xE2\x80\xA6");
        const int n = board::card.keepRecording(*samples, recorder.frames(), (uint32_t)AUDIO_SAMPLE_RATE);
        recorder.clear();
        board::card.endLend();
        return n;
    }
    int recSaveError() override { return board::card.lastSaveError(); }
    void recDiscard() override {
        recorder.stop();
        recorder.clear();
        board::card.endLend();
    }
    void auditionSlice(int s, int slice) override { instrument.auditionSlice(s, slice); }   // (cut from what is in memory: nothing to load)
    void previewPreset(int part, int preset) override {
        // the preset's wavetable banks must be in memory before its notes start: load them now (tens of ms) and keep them while it plays
        if (!app::presetLoad(preset)) return;   // its file (the sound) is read now: a few ms
        int b[2];
        app::presetBanks(preset, b);
        memset(previewBanks, 0, sizeof(previewBanks));
        for (int s = 0; s < 2; ++s)
            if (b[s] >= 0) previewBanks[b[s] >> 3] = (uint8_t)(previewBanks[b[s] >> 3] | (1u << (b[s] & 7)));
        previewBanksUntil = HAL_GetTick() + 4000;
        loadNeededWaves();
        instrument.previewPreset(part, preset);
    }
    void busy(const char* msg) override { showBusy(msg); }
    void wavesChanged() override { wavesDirtyAt = HAL_GetTick() + 120; if (wavesDirtyAt == 0) wavesDirtyAt = 1; }
    bool slotInfo(int slot, app::SlotInfo& out) override { return board::card.slotInfo(slot, out); }
    bool saveSlot(int slot, const app::Project& pj) override {
        showBusy("SAVING\xE2\x80\xA6");
        const bool ok = board::card.saveSlot(slot, pj, *samples);
        if (ok) app::presetsWriteIndex();
        return ok;
    }
    void takePeek(app::Project& pj) override {
        beforeLoad();
        pj = *scratchProject;
        afterLoad(pj);
    }
    void loadFactory(int i, app::Project& pj) override {
        showBusy("LOADING\xE2\x80\xA6");
        beforeLoad();
        app::makeFactoryProject(i, pj);
        if (i == 0 && samples->count > firstCardSample) pickCardDrums(pj, *samples, firstCardSample);
        afterLoad(pj);
    }
    bool undoSave(int slot) override { return board::card.undoSave(slot); }
    const app::Project* peekSlot(int slot, int& missing) override {
        return board::card.loadSlot(slot, *scratchProject, *scratchProject, *samples, missing) ? scratchProject : nullptr;   // (the scratch is where a read ends up anyway)
    }
    bool deleteSlot(int slot) override { return board::card.deleteSlot(slot); }
    bool exportStart(int kind, int song) override {
        if (!board::card.beginWrite()) return false;
        if (kind == app::EXPORT_MIDI) return exporter->start(kind, song);   // (it does not need the instrument, and what plays plays on)
        // the instrument leaves the audio interrupt, which plays silence meanwhile, and the main loop drives it as fast as it goes
        audioEngine.setRenderer(nullptr);
        HAL_Delay(2);
        showBusy("LOADING\xE2\x80\xA6");
        exporter->setMissing(board::card.ensureProjectSamples(*samples, *project));
        if (exporter->start(kind, song)) { rendering = true; return true; }
        audioEngine.setRenderer(&instrument);
        return false;
    }
    bool exportStep(app::ExportStatus& out) override {
        if (exporter->step(out)) return true;
        if (rendering) audioEngine.setRenderer(&instrument);
        rendering = false;
        return false;
    }
    void exportCancel() override { exporter->cancel(); }
    const char* exportWhy() override {
        if (exporter && exporter->why()) return exporter->why();
        if (const char* w = board::card.exportProblem()) return w;
        return board::card.writeProblem();
    }
    const char* cardProblem() override { return board::card.writeProblem(); }
    bool loadFellBack() override { return board::card.loadFellBack(); }
    int activeVoices(int part) override { return instrument.activeVoices(part); }
    float cpuLoad() override { return instrument.cpuLoad(); }
    int degradeLevel() override { return instrument.degradeLevel(); }
    void setAutoDegrade(bool on) override { instrument.setAutoDegrade(on); }
    void stopAllSound() override { instrument.stopAllSound(); }

   private:
    // the sample cache is refilled for the new project: nothing may be playing from it. The audio interrupt takes the stop
    // (every voice fades out in 6 ms), then the cache is free.
    void beforeLoad() {
        instrument.stopAllSound();
        const uint32_t t0 = HAL_GetTick();
        while (!instrument.stopsTaken() && HAL_GetTick() - t0 < 100) {}
        HAL_Delay(10);
    }
    void afterLoad(app::Project& pj) {
        instrument.clearFx();   // the old project's echoes and reverb tail must not ring into the new one
        board::card.loadProjectSamples(*samples, pj);
        notFitting = -1;
    }
    uint32_t seed_ = 0x2545F491u;
    board::Card::Load auditionLoad_ = board::Card::Load::Ok;
};
static BoardServices services;

static const uint8_t* songMidi(int song, uint32_t& size) {
    if (ui->exporting() || song < 0 || song >= app::kSongs) return nullptr;
    songMidiOut.used = 0;
    *scratchProject = *project;
    showBusy("MIDI\xE2\x80\xA6");
    if (!app::writeSongMidi(*scratchProject, song, samples, songMidiOut)) return nullptr;
    size = songMidiOut.used;
    return songMidiOut.at;
}

static float blockLoad() { return (float)audioEngine.stats.last / (float)audioEngine.budgetCycles(); }
static uint32_t tickMs() { return HAL_GetTick(); }

// With the card's samples, the boot project's drum lanes (8-10) play the card's first kick, snare and closed hat.
// The samples the pattern on screen (and a queued one) needs, loaded before they are played: a lane moved to the sampler part, a
// sample picked with a knob, a sample lock.
// One sample into the cache. If the cache is full, the samples the project no longer uses are dropped first; the samplers fade out for
// that (their voices read from memory that moves), the synth parts play on. False: stop trying for now (no card, or no room at all).
static bool need(int i) {
    // (memory comes back without anyone saying so: a wavetable's slot retires a while after its last note. So it is tried again, and only the first time says so)
    static uint32_t triedAt = 0;
    const bool again = i == notFitting;
    if (again && HAL_GetTick() - triedAt < 2000) return true;
    triedAt = HAL_GetTick();
    const auto noRoom = [&] { notFitting = i; if (!again) ui->say("SAMPLE MEMORY FULL"); return false; };
    board::Card::Load r = board::card.ensureLoaded(*samples, i);
    if (r == board::Card::Load::Full) {
        if (!board::card.canEvict(*samples, *project)) return noRoom();
        instrument.stopSamplers();
        const uint32_t t0 = HAL_GetTick();
        while (!instrument.stopsTaken() && HAL_GetTick() - t0 < 100) {}
        HAL_Delay(8);
        if (board::card.evictUnused(*samples, *project)) r = board::card.ensureLoaded(*samples, i);
        if (r == board::Card::Load::Full) return noRoom();
    }
    if (again) notFitting = -1;
    return r != board::Card::Load::NoCard;   // a bad file is marked and skipped, the rest still load
}

// ---- the wavetable banks the project plays (the OSC pages' WAVE 4 and up): into the arena, from /CATHODE/WT on the card

static void loadNeededWaves() {
    uint8_t bits[(app::kWaveBanks + 7) / 8];
    audio::neededBanks(*project, bits);
    const uint32_t now = HAL_GetTick();
    if ((int32_t)(previewBanksUntil - now) > 0)
        for (size_t i = 0; i < sizeof(bits); ++i) bits[i] |= previewBanks[i];
    if (recording()) return;   // (a recording is in the free memory: nothing moves until it ends)
    waveStore.setSlotLimit(board::card.pool().slotsThatFit());
    waveStore.want(bits, now);
    board::card.pool().useSlots(waveStore.highSlot());
    if (waveStore.refused() != waveRefused) {
        waveRefused = waveStore.refused();
        if (waveRefused >= 0) ui->say("WAVETABLE MEMORY FULL: A PART PLAYS A BASIC WAVE");
    }
}

static void loadNeededSamples() {
    if (recording()) return;   // (a recording is in the free memory: nothing moves until it ends)
    const int pats[2] = {transport.pat, transport.nextPat};
    for (int pi : pats) {
        if (pi < 0) continue;
        const app::Pattern& p = project->patterns[pi];
        for (const app::Lane& l : p.lanes) {
            if (!app::isSampler(*project, l.part)) continue;
            if (!need(l.smp[0][0])) return;
            for (const app::Step& s : l.st) {
                const int v = s.lockValue(app::lockKey(app::PG_SMP, 0), -1);
                if (v >= 0 && !need(v)) return;
            }
        }
    }
}

#if DBOY_DEBUG_HOOK
// What the debug mailbox shows the script (board/DebugHook.h): once a main-loop pass, from the values the loop already has.
static void publishDebugStatus(uint32_t now) {
    static uint32_t winStart = 0, winCount = 0;
    static uint64_t winSum = 0;
    board::DebugMailbox& m = board::debugMail;
    ++m.loops;
    static uint32_t lastPublish = 0;
    if (now - lastPublish < 20) return;   // 50 Hz is plenty for a script, and the 64-bit divides and the string copy are not free
    lastPublish = now;
    const uint32_t budget = audioEngine.budgetCycles();
    m.revision = ui->revision();
    m.nowMs = now;
    m.mode = (uint32_t)ui->mode();
    m.part = (uint32_t)ui->part();
    m.playing = transport.playing;
    m.songPlaying = transport.songPlaying;
    m.pattern = transport.pat;
    m.step = (uint32_t)(int32_t)transport.shownStep;
    m.degrade = (uint32_t)instrument.degradeLevel();
    m.loadLastPct10 = (uint32_t)((uint64_t)audioEngine.stats.last * 1000u / budget);
    m.loadMaxPct10 = (uint32_t)((uint64_t)audioEngine.stats.max * 1000u / budget);
    m.overruns = audioEngine.stats.overruns;
    if (now - winStart >= 1000) {   // the average over the last second, from the running sum
        const uint64_t sum = audioEngine.stats.sum;
        const uint32_t count = audioEngine.stats.count;
        if (winStart && count != winCount) m.loadAvgPct10 = (uint32_t)((sum - winSum) * 1000u / (uint64_t)(count - winCount) / budget);
        winStart = now; winSum = sum; winCount = count;
    }
    {   // where the block's cycles go, averaged over the last second (the sums run on; only differences are used)
        static uint32_t secStart = 0, secBlocks = 0;
        static uint64_t secPart[app::kParts] = {}, secSampler = 0, secFx = 0, secSeq = 0;
        const audio::Instrument::SectionCycles& c = instrument.sectionCycles();
        for (int p = 0; p < app::kParts; ++p) { m.voices[p] = (uint32_t)instrument.activeVoices(p); m.cfgVoices[p] = project->parts[p].voices; m.partPreset[p] = project->parts[p].preset; }
        if (now - secStart >= 1000) {
            const uint32_t blocks = c.blocks - secBlocks;
            if (secStart && blocks) {
                const uint64_t denom = (uint64_t)blocks * budget;   // cycles the blocks had
                for (int p = 0; p < app::kParts; ++p) m.partPct10[p] = (uint32_t)((c.part[p] - secPart[p]) * 1000u / denom);
                m.samplerPct10 = (uint32_t)((c.sampler - secSampler) * 1000u / denom);
                m.fxPct10 = (uint32_t)((c.fx - secFx) * 1000u / denom);
                m.seqPct10 = (uint32_t)((c.seq - secSeq) * 1000u / denom);
            }
            secStart = now; secBlocks = c.blocks;
            for (int p = 0; p < app::kParts; ++p) secPart[p] = c.part[p];
            secSampler = c.sampler; secFx = c.fx; secSeq = c.seq;
        }
    }
    {
        const audio::Instrument::SectionCycles& c = instrument.sectionCycles();
        m.startOnMaxKc = c.noteOnMax / 1000u;
        m.startPatchMaxKc = c.patchMax / 1000u;
        m.starts = c.starts;
        m.startOnSumKc = (uint32_t)(c.noteOnSum / 1000u);
    }
    m.waveSlots = (uint32_t)waveStore.slots();
    m.waveResident = (uint32_t)waveStore.residentCount();
    m.waveRefused = waveStore.refused();
    m.sdramFreeKb = (uint32_t)(memory::sdramHeapBytesFree() / 1024u);
    m.tilesSent = lcd.tilesSent;
    m.presentUs = lcd.lastPresentUs;
    const char* t = ui->toast();
    m.toastOn = t != nullptr;
    strncpy(m.toast, t ? t : "", sizeof(m.toast) - 1);
    m.toast[sizeof(m.toast) - 1] = 0;
}
#endif

int main() {
    __disable_irq();
    SCB->VTOR = APPLICATION_ADDRESS;   // the bootloader owns sector 0; we live at 0x08020000
    SCB_InvalidateICache();
    SCB_InvalidateDCache();
    SCB_EnableICache();
    HAL_Init();

    SCB->CPACR |= ((3UL << 10 * 2) | (3UL << 11 * 2));          // FPU on
    __set_FPSCR(__get_FPSCR() | (1 << 24) | (1 << 25));         // flush-to-zero + default NaN
    __enable_irq();

    SystemClock_Config();
    PeriphCommonClock_Config();

    System_Initialize();
    Sdram_Initialize();
    memory::mpuConfigure(ENABLE_DCACHE);   // SDRAM as Normal memory (and cacheable + D-cache on if ENABLE_DCACHE), see Mpu.h. Needs SDRAM initialised.
    memory::sdramHeapEnable();   // from here on `new` comes from SDRAM (see SdramHeap.h)
    perf::cycleCounterEnable();

    lcd.init();
    lcd.showBootImage();
    gfx::BootHold bootImage(HAL_GetTick() + kBootImageMs);
    board::input.init();
    board::leds.init();

    // the samples: the twelve built-in drums, then the card's (indexed now, loaded when needed)
    samples = new app::SampleLib;
    int16_t* factory = new int16_t[app::kFactoryFrames];
    app::buildFactorySamples(*samples, factory, app::kFactoryFrames);
    const int firstCard = samples->count;
    firstCardSample = firstCard;
    // what a load, a render or the librarian will want later comes now: after the card has taken its cache, SDRAM is the engines' and what is asked for too late never comes
    project = new app::Project;
    scratchProject = new app::Project;
    exporter = new audio::SongExport(*project, transport, instrument, *samples, *scratchProject, exportFiles);
    songMidiOut.at = new uint8_t[SongMidiBuffer::kBytes];
    app::midiExportInit();
    const bool cardOk = board::card.mount();
    if (cardOk) board::card.indexSamples(*samples);

    // the preset library: the index (64 bytes a preset, in SDRAM); a preset's file is read when it is previewed or loaded. A changed folder is read again.
    app::presetsInit(new app::PresetInfo[app::kPresetCap], app::kPresetCap);
    app::PresetOpen presets;
    if (cardOk) {
        presets = app::presetsOpen(board::card.presets());
    }

    app::makeNewProject(*project);
    if (samples->count > firstCard) pickCardDrums(*project, *samples, firstCard);
    ui = new app::Ui(*project, transport, *samples, services);
    if (presets.ignored > 0) {
        app::Str<48> t;
        t.num(presets.ignored).add(presets.ignored == 1 ? " PRESET FILE IGNORED" : " PRESET FILES IGNORED");
        ui->say(t.c_str());
    }
    static gfx::Canvas frame(frameArena, sizeof(frameArena));
    canvas = &frame;

    // the CATHODE DSP's settings (CMake options), before any engine is prepared
    nemesis::dsp::CathodeOscCore::setLazyMacro(true);   // the 27 KB-per-slot Plaits blocks only when a slot uses Macro
    nemesis::dsp::fork::Config::idleMatrixSkip = DBOY_IDLE_MATRIX_VALUE != 0;
    nemesis::dsp::fork::Config::skipSilentSlots = DBOY_SKIP_SILENT_SLOTS_VALUE != 0;
    nemesis::dsp::fork::Config::filterOversample = DBOY_FILTER_OVERSAMPLE_VALUE;
    nemesis::dsp::fork::Config::pdOversample = DBOY_PD_OVERSAMPLE_VALUE;
    nemesis::dsp::fork::Config::driveOversample = DBOY_DRIVE_OVERSAMPLE_VALUE;
    nemesis::dsp::fork::Config::classicOversample = DBOY_CLASSIC_OVERSAMPLE_VALUE;
    nemesis::dsp::fork::Config::stereoTwin = DBOY_STEREO_TWIN_VALUE != 0;
    nemesis::dsp::fork::Config::lanes = false;   // no NEON on the M7: never prepare the lane groups

    instrument.init(AUDIO_SAMPLE_RATE, *project, transport, *samples);
    instrument.setCycleCounter(perf::cycles);   // the per-section CPU numbers (the debug mailbox, the CPU meter)
    instrument.setLoadSource(blockLoad);
    instrument.setClockMs(tickMs);
    // the memory the samples and up to six wavetable banks (about 1 MB each) share: a bank only takes its share while a part plays it
    {
        const int onCard = board::card.scanWaves();
        const size_t slotBytes = audio::WaveStore::slotSamples() * sizeof(nemesis::dsp::Real);
        int slots = onCard < audio::WaveStore::kMaxSlots ? onCard : audio::WaveStore::kMaxSlots;
        void* arena = nullptr;
        board::card.openPool(slotBytes, slots, arena);
        if (slots > 0) waveStore.init(reinterpret_cast<nemesis::dsp::Real*>(arena), slots, nullptr, &instrument.sharedTables(), &board::card.waves());
    }
    instrument.setMidi(&midi::rxQueue, midi::send);   // the lanes play and send on their MIDI channels (PROJECT, view 2)
    presetLink.setSongMidi(songMidi);
    instrument.setClockInput(true);
    instrument.setSyncPins([] { return (SYNC_IN_PULSE_GPIO_Port->IDR & SYNC_IN_PULSE_Pin) != 0; }, [](bool pulse, bool gate) {
        SYNC_OUT_PULSE_GPIO_Port->BSRR = pulse ? SYNC_OUT_PULSE_Pin : (uint32_t)SYNC_OUT_PULSE_Pin << 16;
        SYNC_OUT_GATE_GPIO_Port->BSRR = gate ? SYNC_OUT_GATE_Pin : (uint32_t)SYNC_OUT_GATE_Pin << 16;
    });
    audioEngine.setRenderer(&instrument);
    audioEngine.setRecorder(&recorder);
    // the card's samples for the boot project: after the instrument, so the cache is sized from what SDRAM has left
    if (samples->count > firstCard) {
        if (!bootImage.up(HAL_GetTick(), false)) showBusy("LOADING SAMPLES\xE2\x80\xA6");   // (a slow card: the picture has had its time)
        board::card.loadProjectSamples(*samples, *project);
    }

    midi::setChannel(0);   // every channel: the lanes' table picks
    midi::start();         // DIN MIDI in on USART1
    midi::startOut();      // DIN MIDI out on USART6: before the audio, which sends on it
    dac.initialize();      // also starts the I2S DMA

    uint32_t drawnRev = 0, lastFrame = 0, lastBlink = 0, lastSamples = 0;
    bool screenDark = false;   // the screen saver has the display off
    board::debugHookInit();
    while (true) {
        const uint32_t now = HAL_GetTick();
        board::input.poll(*ui);
#if DBOY_DEBUG_HOOK
#if DBOY_LEAN
        board::debugHookService(*ui, [](bool on) { instrument.setAutoDegrade(on); }, [](bool on) { instrument.requestLean(on); }, *canvas);   // events from the script on the computer (tools/board_script.py)
#else
        board::debugHookService(*ui, [](bool on) { instrument.setAutoDegrade(on); }, nullptr, *canvas);
#endif
#endif
#if DBOY_DEBUG_HOOK
        board::soakStep(*project, *scratchProject, *samples);
        memcpy(board::debugMail.soak, &board::soakStatus, sizeof(board::soakStatus));
#endif
        {   // (the card it may touch is the main loop's, and the audio plays on through it)
            static uint8_t message[midi::kSysexMax];
            int bytes = 0;
            if (app::presetLibraryOpen() && !board::card.mounted()) app::presetsForget();   // pulled: nothing to offer any more
            if (midi::sysexIn.take(message, bytes)) presetLink.receive(message, bytes, now);
            presetLink.step(now);
            if (presetLink.changed()) ui->dirty();
        }
        ui->tick();
        instrument.setLivePart(ui->part());   // a MIDI channel no lane uses plays the part on screen
        if (instrument.fxFault()) instrument.clearFx();   // an effect went non-finite: empty its lines
        if (wavesDirtyAt && (int32_t)(now - wavesDirtyAt) >= 0) { wavesDirtyAt = 0; loadNeededWaves(); }
        if (now - lastSamples >= 200 && !rendering) { lastSamples = now; loadNeededWaves(); loadNeededSamples(); }   // (a render has what it can have, and must not lose a sample midway)
#if SCREENSAVER_MS > 0
        // screen saver: dark after SCREENSAVER_MS without a key or knob action (MIDI in counts), woken by the next action, which is not delivered to the UI
        {
            static bool dark = false;
            static uint32_t lastMidi = 0, activity = 0;
            if (midi::stats.messages != lastMidi) { lastMidi = midi::stats.messages; activity = now; }
            if (ui->exporting()) activity = now;   // (a long render is not an idle Drumboy, and it ends with a message)
            const uint32_t last = board::input.activityMs() > activity ? board::input.activityMs() : activity;
            if (!dark && now - last >= (uint32_t)SCREENSAVER_MS) { dark = true; lcd.sleep(true); board::input.setScreenDark(true); }
            else if (dark && (board::input.wakeAsked() || now - last < (uint32_t)SCREENSAVER_MS)) {
                dark = false;
                lcd.sleep(false);
                board::input.setScreenDark(false);
                activity = now;
                drawnRev = ui->revision() - 1;   // draw at once
            }
            if (dark) { lastFrame = now; }       // nothing is drawn while it is dark (saves the CPU too)
            screenDark = dark;
        }
#endif
        // the screen: redrawn when something changed, at most ~30 times a second. The boot picture goes when its time is up, at the first key or knob, or for a toast (a warning lasts a second)
        const bool bootImageUp = bootImage.up(now, board::input.activityMs() != 0 || ui->toast() != nullptr);
        if (!screenDark && !bootImageUp && ui->revision() != drawnRev && now - lastFrame >= 33) {
            drawnRev = ui->revision();
            lastFrame = now;
            gfx::drawScreen(*canvas, *ui);
            lcd.present(*canvas);
        }
        if (board::leds.update(*ui, instrument.peak(0), instrument.peak(1))) instrument.clearPeaks();   // the peaks since the last update
        if (now - lastBlink >= 500) { lastBlink = now; LED2_TOGGLE; }   // heartbeat: the main loop is alive
        if (audioEngine.stats.overruns) LED0_ON;                          // a block went over its budget
        if (audioEngine.i2sOverruns || audioEngine.i2sUnderruns) LED1_ON;
#if DBOY_DEBUG_HOOK
        publishDebugStatus(now);
#endif
    }
}

/* I2S DMA interrupts: half transfer renders the first block, complete transfer the second. */
#if DBOY_DEBUG_HOOK
// After every block, in the audio interrupt: the running cycle totals, so a script can read the true load even while an overloaded audio leaves the
// main loop no time to publish anything (see board/DebugHook.h)
static inline void publishIsrCounters() {
    board::DebugMailbox& m = board::debugMail;
    const audio::Instrument::SectionCycles& c = instrument.sectionCycles();
    m.isrBlocks = audioEngine.stats.count;
    m.isrTotalCyc = (uint32_t)audioEngine.stats.sum;
    for (int p = 0; p < app::kParts; ++p) m.isrPartCyc[p] = (uint32_t)c.part[p];
    m.isrSamplerCyc = (uint32_t)c.sampler;
    m.isrFxCyc = (uint32_t)c.fx;
    for (int p = 0; p < app::kParts; ++p) m.isrVoices[p] = (uint32_t)instrument.activeVoices(p);
}
#define PUBLISH_ISR_COUNTERS() publishIsrCounters()
#else
#define PUBLISH_ISR_COUNTERS() ((void)0)
#endif

void HAL_I2SEx_TxRxHalfCpltCallback(I2S_HandleTypeDef* hi2s) {
    if (hi2s->Instance == SPI3) { audioEngine.onBlock(0); PUBLISH_ISR_COUNTERS(); }
}

void HAL_I2SEx_TxRxCpltCallback(I2S_HandleTypeDef* hi2s) {
    if (hi2s->Instance == SPI3) { audioEngine.onBlock(1); PUBLISH_ISR_COUNTERS(); }
}

void HAL_I2SEx_TxRxErrorCallback(I2S_HandleTypeDef* hi2s) {}

/* MIDI in: one byte per interrupt, parsed in MidiIn.cpp. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef* huart) {
    if (huart == &huart1) midi::onByteReceived();
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart) {
    if (huart == &huart1) midi::onReceiveError();
}

/* Keys and encoders: the keypads' interrupt lines and the encoders' A pins (gpio.c enables the EXTI lines). */
void HAL_GPIO_EXTI_Callback(uint16_t pin) {
    switch (pin) {
        case MX_X_INT_Pin: board::input.onKeypadInterrupt(0); break;   // layer keys
        case MX_A_INT_Pin: board::input.onKeypadInterrupt(1); break;   // left keypad
        case MX_B_INT_Pin: board::input.onKeypadInterrupt(2); break;   // right keypad
        case ENCO_0A_Pin: board::input.onEncoderInterrupt(0); break;
        case ENCO_1A_Pin: board::input.onEncoderInterrupt(1); break;
        case ENCO_2A_Pin: board::input.onEncoderInterrupt(2); break;
        case ENCO_3A_Pin: board::input.onEncoderInterrupt(3); break;
        case ENCO_4A_Pin: board::input.onEncoderInterrupt(4); break;
        case ENCO_5A_Pin: board::input.onEncoderInterrupt(5); break;
        case ENCO_6A_Pin: board::input.onEncoderInterrupt(6); break;
        case ENCO_7A_Pin: board::input.onEncoderInterrupt(7); break;
        default: break;
    }
}
