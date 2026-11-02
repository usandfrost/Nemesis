#pragma once
// app::Services for the host: a settable clock, a deterministic random source, an in-memory SD card, and a log of the sound previews.
#include <stdlib.h>

#include <string>
#include <vector>

#include "app/ProjectFile.h"
#include "app/Services.h"

struct MemSink : app::ByteSink {
    std::vector<uint8_t> bytes;
    bool write(const void* d, size_t n) override {
        bytes.insert(bytes.end(), (const uint8_t*)d, (const uint8_t*)d + n);
        return true;
    }
};
struct MemSource : app::ByteSource {
    const std::vector<uint8_t>& bytes;
    size_t pos = 0;
    explicit MemSource(const std::vector<uint8_t>& b) : bytes(b) {}
    bool read(void* d, size_t n) override {
        if (pos + n > bytes.size()) return false;
        memcpy(d, bytes.data() + pos, n);
        pos += n;
        return true;
    }
};

class HostServices : public app::Services {
   public:
    uint32_t now = 1000;
    uint32_t seed = 12345;
    std::vector<uint8_t> card[app::kCardSlots], backup[app::kCardSlots];
    bool hadBackup[app::kCardSlots] = {};
    bool deleteFails = false, saveFails = false, fellBack = false, pulled = false, readFails = false;
    int peekMissing = 0, slotReads = 0;
    bool exportRefuses = false;
    int exportCalls = 0, exportCancels = 0, exportKind = -1, exportSong = -1, exportPercent = 0, exportResult = app::EXPORT_RUNNING, exportMissing = 0;
    std::vector<int> auditions;
    std::vector<int> sliceAuditions;
    struct Preview { int part, preset; };
    std::vector<Preview> previews;
    float load = 0.0f;
    int level = 0;
    int active[app::kParts] = {};
    bool autoDeg = true;
    int stops = 0;
    app::Project scratch;
    const app::SampleLib* lib = nullptr;   // the samples projects are written and read against (their paths go into the file)

    uint32_t nowMs() override { return now; }
    uint32_t random() override {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return seed;
    }
    void auditionSample(int s) override { auditions.push_back(s); }
    const char* auditionWhy = nullptr;   // what the card would say about the sample auditioned
    const char* auditionProblem() override { return auditionWhy; }
    void auditionSlice(int s, int k) override { sliceAuditions.push_back(k); }
    // a recorder that does what the test says
    bool recOn = false, recListening = false, recRoom = true;
    app::RecSettings recLast;
    app::RecStatus recNow;
    int recNext = 3, recDrops = 0, recKeeps = 0;
    bool recAvailable() override { return recOn; }
    void recListen(const app::RecSettings& s, bool on) override { recLast = s; recListening = on; }
    bool recStart(const app::RecSettings& s) override {
        if (!recRoom) return false;
        recLast = s;
        recNow.state = s.autoCapture ? app::REC_ARMED : app::REC_TAKING;
        recNow.frames = 0;
        recNow.capacity = 441000;
        return true;
    }
    void recStop() override { recNow.state = recNow.state == app::REC_ARMED ? app::REC_IDLE : app::REC_DONE; }
    void recStatus(app::RecStatus& o) override { o = recNow; }
    int recKeep() override { if (recNow.state != app::REC_DONE) return -1; recNow.state = app::REC_IDLE; ++recKeeps; return recNext; }
    void recDiscard() override { recNow.state = app::REC_IDLE; ++recDrops; }
    void previewPreset(int part, int preset) override { previews.push_back({part, preset}); }
    bool slotInfo(int slot, app::SlotInfo& out) override {
        ++slotReads;
        if (pulled || card[slot].empty()) return false;
        MemSource src(card[slot]);
        app::ProjectHeader h;
        if (!app::readHeader(src, h)) return false;
        app::headerInfo(h, out);
        return true;
    }
    bool saveSlot(int slot, const app::Project& pj) override {
        if (saveFails || pulled) return false;
        backup[slot] = card[slot];
        hadBackup[slot] = true;
        MemSink sink;
        if (!app::writeProject(pj, sink, lib)) return false;
        card[slot] = sink.bytes;
        return true;
    }
    bool loadSlot(int slot, app::Project& pj, int& missing) {   // (what a test reads a slot back with)
        MemSource src(card[slot]);
        return app::readProject(src, scratch, pj, lib, &missing);
    }
    bool undoSave(int slot) override {
        if (!hadBackup[slot]) return false;
        card[slot] = backup[slot];
        hadBackup[slot] = false;
        return true;
    }
    const app::Project* peekSlot(int slot, int& missing) override {
        if (pulled || readFails) return nullptr;
        const bool ok = loadSlot(slot, scratch, missing);
        missing += peekMissing;
        return ok ? &scratch : nullptr;
    }
    void takePeek(app::Project& pj) override { pj = scratch; }
    bool loadFellBack() override { return fellBack; }
    bool exportStart(int kind, int song) override {
        ++exportCalls;
        if (exportRefuses) return false;
        exportKind = kind;
        exportSong = song;
        return true;
    }
    bool exportStep(app::ExportStatus& s) override {
        s.percent = (uint8_t)exportPercent;
        s.result = (uint8_t)exportResult;
        s.missing = (uint16_t)exportMissing;
        return exportResult == app::EXPORT_RUNNING;
    }
    void exportCancel() override { ++exportCancels; }
    bool deleteSlot(int slot) override {
        if (deleteFails || pulled) return false;
        card[slot].clear();
        backup[slot].clear();
        hadBackup[slot] = false;
        return true;
    }
    int activeVoices(int part) override { return active[part]; }
    float cpuLoad() override { return load; }
    int degradeLevel() override { return level; }
    void setAutoDegrade(bool on) override { autoDeg = on; }
    void stopAllSound() override { ++stops; }
    int held = 0, holds = 0;   // holdAudio: open now, and how many times
    void holdAudio(bool h) override { held += h ? 1 : -1; holds += h; }
};
