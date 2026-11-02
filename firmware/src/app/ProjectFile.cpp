#include "app/ProjectFile.h"

#include <stddef.h>
#include <string.h>

#include "app/CostBridge.h"

namespace app {

// The layout the file format is written in. If one of these fails, the struct changed: bump kProjectVersion (and accept that older
// files no longer load, or add a converter).
static_assert(sizeof(Lock) == 4, "Lock layout");
static_assert(sizeof(Step) == 42, "Step layout");
static_assert(sizeof(Arp) == 4, "Arp layout");
static_assert(offsetof(Lane, smp) == 14, "Lane layout");
static_assert(sizeof(Lane) == 14 + kLanePages * kParamsPerPage * 2 + kMaxSteps * 42, "Lane layout");
static_assert(sizeof(Part) == 8 + kSynthPages * kParamsPerPage * 2, "Part layout");
static_assert(sizeof(Song) == 2 + kSongRows * 2, "Song layout");
static_assert(offsetof(Step, locks) == 10, "Step layout");
static_assert(offsetof(Part, pages) == 8, "Part layout");
static_assert(offsetof(Project, parts) == 24, "Project layout");
static_assert(offsetof(Project, patterns) == 24 + kParts * sizeof(Part), "Project layout");
static_assert(offsetof(Project, midi) == 24 + kParts * sizeof(Part) + kPatterns * sizeof(Pattern) + kSongs * sizeof(Song), "Project layout");
static_assert(offsetof(Project, fx) == offsetof(Project, midi) + kLanes * sizeof(LaneMidi), "Project layout");
static_assert(offsetof(Project, slices) == offsetof(Project, fx) + sizeof(FxSettings), "Project layout");
static_assert(offsetof(Project, loops) == offsetof(Project, slices) + kSliceMaps * sizeof(SliceMap), "Project layout");
static_assert(sizeof(Project) == offsetof(Project, loops) + kLoopFits * sizeof(LoopFit), "Project layout");
static_assert(sizeof(LoopFit) == 24, "LoopFit layout");
static_assert(sizeof(Slice) == 8 && sizeof(SliceMap) == 8 + kSlices * 8, "SliceMap layout");
static_assert(sizeof(FxSettings) == (4 * kFxParams + kParts * kFxParams + kParts * kSends) * 2, "FxSettings layout");
// What older files looked like. Before version 8 a lane had no LOOP page and a project no loop points; before version 7 no SLICE page and no slices; before
// version 6 a project had three parts: three Parts ahead of the patterns, and the effects' amps and sends for three.
static constexpr int kOldParts = 3;
static constexpr uint32_t laneBytes(int pages) { return sizeof(Lane) - (uint32_t)(kLanePages - pages) * kParamsPerPage * 2; }
static constexpr uint32_t patternBytes(int pages) { return kPatterns * kLanes * laneBytes(pages); }
static constexpr uint32_t kSongBytes = kSongs * sizeof(Song);
static constexpr uint32_t kOldFxShared = 4 * kFxParams * 2, kOldFx = kOldFxShared + kOldParts * (kFxParams + kSends) * 2;
static constexpr uint32_t kPartsAt = offsetof(Project, parts);
// versions 1 and 2 stored the project without its MIDI table and effects, version 3 without the effects
static constexpr uint32_t kPayloadV2 = kPartsAt + kOldParts * sizeof(Part) + patternBytes(5) + kSongBytes;
static constexpr uint32_t kPayloadV3 = kPayloadV2 + kLanes * sizeof(LaneMidi);
static constexpr uint32_t kPayloadV4 = kPayloadV3 + kOldFx;
static constexpr uint32_t kPayloadV6 = kPartsAt + kParts * sizeof(Part) + patternBytes(5) + kSongBytes + kLanes * sizeof(LaneMidi) + sizeof(FxSettings);
static constexpr uint32_t kPayloadV7 = kPartsAt + kParts * sizeof(Part) + patternBytes(6) + kSongBytes + kLanes * sizeof(LaneMidi) + sizeof(FxSettings) + kSliceMaps * sizeof(SliceMap);
static_assert(sizeof(ProjectHeader) == 36, "header layout");
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the project file is little-endian (host and Cortex-M7 both are)"
#endif

uint32_t crc32(const void* data, size_t len, uint32_t crc) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void makeHeader(const Project& pj, ProjectHeader& h) {
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "DBPJ", 4);
    h.version = kProjectVersion;
    h.headerSize = sizeof(ProjectHeader);
    h.payloadSize = sizeof(Project);
    h.crc = crc32(&pj, sizeof(Project));
    memcpy(h.name, pj.name, sizeof(h.name));
    h.name[sizeof(h.name) - 1] = 0;
    h.bpm = pj.bpm;
    h.usedPatterns = (uint8_t)usedPatterns(pj);
    int songs = 0;
    for (const Song& s : pj.songs) songs += s.nRows > 0;
    h.songs = (uint8_t)songs;
}

// Version 2: after the project, the samples it uses by their lasting name (SampleLib::keyOf), so a card whose sample folders changed
// still finds them: uint16 count, then per sample: int16 number in the saving card's list, uint8 length, the key's bytes. The CRC
// covers the project and this table.
static constexpr size_t kTableMax = 48u * 1024u;
static uint8_t* gTable = nullptr;   // made when first saved (the SDRAM heap on the board: a save is no reason to hold 48 KB of internal RAM)
static bool gUsed[kMaxSamples];
static int16_t gMap[kMaxSamples];

static size_t buildTable(const Project& pj, const SampleLib* lib) {
    if (!gTable) gTable = new uint8_t[kTableMax];
    size_t n = 2;
    uint16_t count = 0;
    if (lib) {
        samplesUsed(pj, gUsed, lib->count);
        char key[256];
        for (int i = 0; i < lib->count; ++i) {
            if (!gUsed[i]) continue;
            lib->keyOf(i, key, sizeof(key));
            const size_t len = strlen(key);
            if (!len || n + 3 + len > kTableMax) continue;
            gTable[n++] = (uint8_t)(i & 0xFF);
            gTable[n++] = (uint8_t)(i >> 8);
            gTable[n++] = (uint8_t)len;
            memcpy(gTable + n, key, len);
            n += len;
            ++count;
        }
    }
    gTable[0] = (uint8_t)(count & 0xFF);
    gTable[1] = (uint8_t)(count >> 8);
    return n;
}

bool writeProject(const Project& pj, ByteSink& out, const SampleLib* lib) {
    ProjectHeader h;
    makeHeader(pj, h);
    const size_t tableLen = buildTable(pj, lib);
    h.crc = crc32(gTable, tableLen, h.crc);
    return out.write(&h, sizeof(h)) && out.write(&pj, sizeof(pj)) && out.write(gTable, tableLen);
}

static bool headerOk(const ProjectHeader& h) {
    if (memcmp(h.magic, "DBPJ", 4) != 0 || h.headerSize != sizeof(ProjectHeader)) return false;
    if (h.version == 1 || h.version == 2) return h.payloadSize == kPayloadV2;
    if (h.version == 3) return h.payloadSize == kPayloadV3;
    if (h.version == 4 || h.version == 5) return h.payloadSize == kPayloadV4;
    if (h.version == 6) return h.payloadSize == kPayloadV6;
    if (h.version == 7) return h.payloadSize == kPayloadV7;
    return (h.version == 8 || h.version == kProjectVersion) && h.payloadSize == sizeof(Project);
}

// A file from before this version, as it was read into the start of `pj`, moved to where a project keeps it now: everything moves up, so it goes from the end.
// The parts it did not have are new parts (no lane plays them), so it sounds as it did.
static void widenOldProject(Project& pj, uint32_t payload, int version) {
    uint8_t* b = reinterpret_cast<uint8_t*>(&pj);
    const int oldParts = version < 6 ? kOldParts : kParts, oldPages = version < 7 ? 5 : 6;
    const uint32_t lane = laneBytes(oldPages), head = offsetof(Lane, st) - (uint32_t)(kLanePages - oldPages) * kParamsPerPage * 2;
    const uint32_t patterns = kPartsAt + oldParts * sizeof(Part), songs = patterns + kPatterns * kLanes * lane, midi = songs + kSongBytes, fx = midi + kLanes * sizeof(LaneMidi);
    const uint32_t slices = fx + sizeof(FxSettings);
    const bool hasMidi = payload > midi, hasFx = payload > fx, hasSlices = version == 7;
    static uint8_t oldFx[sizeof(FxSettings)];
    LaneMidi oldMidi[kLanes];
    if (hasSlices) memmove(pj.slices, b + slices, sizeof(pj.slices));
    if (hasMidi) memcpy(oldMidi, b + midi, sizeof(oldMidi));
    if (hasFx) memcpy(oldFx, b + fx, payload - fx < sizeof(oldFx) ? payload - fx : sizeof(oldFx));
    memmove(pj.songs, b + songs, kSongBytes);
    for (int i = kPatterns * kLanes - 1; i >= 0; --i) {
        Lane& l = pj.patterns[i / kLanes].lanes[i % kLanes];
        const uint8_t* from = b + patterns + (uint32_t)i * lane;
        memmove(l.st, from + head, kMaxSteps * sizeof(Step));
        memmove(&l, from, head);
    }
    int16_t def[kLanePages][kParamsPerPage];
    defaultLanePages(def);
    const int oldPageCount = PG_COUNT - (kLanePages - oldPages);   // the effects' lock keys come after the pages, and there are more pages now
    for (Pattern& p : pj.patterns)
        for (Lane& l : p.lanes) {
            for (int g = oldPages; g < kLanePages; ++g) memcpy(l.smp[g], def[g], sizeof(l.smp[0]));
            if (version < 6 && l.part >= kOldParts) l.part = kOldParts - 1;   // (damaged: such a file never had a part past C)
            for (Step& s : l.st)
                for (int k = 0; k < s.nLocks && k < kLocksPerStep; ++k)
                    if (s.locks[k].key >= oldPageCount * kParamsPerPage) s.locks[k].key = (uint8_t)(s.locks[k].key + (PG_COUNT - oldPageCount) * kParamsPerPage);
        }
    for (int i = oldParts; i < kParts; ++i) makeNewPart(pj.parts[i], i);
    if (hasMidi) memcpy(pj.midi, oldMidi, sizeof(oldMidi));
    else defaultMidi(pj.midi);
    defaultFx(pj.fx);
    if (version >= 6) memcpy(&pj.fx, oldFx, sizeof(FxSettings));
    else if (hasFx) {
        const int16_t* v = reinterpret_cast<const int16_t*>(oldFx);
        memcpy(pj.fx.delay, v, kOldFxShared);
        v += kOldFxShared / 2;
        memcpy(pj.fx.amp, v, kOldParts * kFxParams * 2);
        v += kOldParts * kFxParams;
        memcpy(pj.fx.send, v, kOldParts * kSends * 2);
    }
    if (!hasSlices) {
        memset(pj.slices, 0, sizeof(pj.slices));
        for (SliceMap& m : pj.slices) m.sample = -1;
    }
    memset(pj.loops, 0, sizeof(pj.loops));
    for (LoopFit& f : pj.loops) f.sample = -1;
}

bool readHeader(ByteSource& in, ProjectHeader& h) { return in.read(&h, sizeof(h)) && headerOk(h); }

bool readProject(ByteSource& in, Project& scratch, Project& out, const SampleLib* lib, int* missing) {
    if (missing) *missing = 0;
    ProjectHeader h;
    if (!readHeader(in, h)) return false;
    if (!in.read(&scratch, h.payloadSize)) return false;
    uint32_t crc = crc32(&scratch, h.payloadSize);
    if (h.version < 5)   // before version 5 a part's preset was one signed byte (-1 = none) with padding after it
        for (int i = 0; i < kOldParts; ++i) scratch.parts[i].preset = (int16_t)(int8_t)(scratch.parts[i].preset & 0xFF);
    if (h.version < kProjectVersion) widenOldProject(scratch, h.payloadSize, h.version);   // (an older file has the default lane channels and no effects: it sounds as it did)
    if (h.version >= 2) {
        // the sample table: read, checked with the project, then every reference renumbered for this card
        uint8_t c[2];
        if (!in.read(c, 2)) return false;
        crc = crc32(c, 2, crc);
        const int count = c[0] | (c[1] << 8);
        for (int i = 0; i < kMaxSamples; ++i) gMap[i] = -1;
        char key[256];
        for (int e = 0; e < count; ++e) {
            uint8_t hd[3];
            if (!in.read(hd, 3)) return false;
            crc = crc32(hd, 3, crc);
            const int old = hd[0] | (hd[1] << 8), len = hd[2];
            if (!in.read(key, (size_t)len)) return false;
            crc = crc32(key, (size_t)len, crc);
            key[len] = 0;
            if (!lib || old < 0 || old >= kMaxSamples) continue;
            const int now = lib->find(key);
            if (now >= 0) gMap[old] = (int16_t)now;
            else {
                if (missing) ++*missing;
                gMap[old] = 0;   // a sample that is not on this card: the lane plays the first sample instead of a wrong one
                for (SliceMap& m : scratch.slices) if (m.sample == old) { m.sample = -1; m.count = 0; }   // (but not with its slices)
                for (LoopFit& f : scratch.loops) if (f.sample == old) f.sample = -1;
            }
        }
        if (crc != h.crc) return false;
        if (lib) remapSamples(scratch, gMap, kMaxSamples);
    } else if (crc != h.crc) {
        return false;
    }
    sanitize(scratch);
    // CLOCK is the project's. Before version 9 it did nothing, so what a file holds there means nothing
    const int16_t clock = h.version < 9 ? (int16_t)0 : scratch.parts[0].pages[PG_GLOB][4];
    for (Part& p : scratch.parts) p.pages[PG_GLOB][4] = clock;
    out = scratch;
    return true;
}

void headerInfo(const ProjectHeader& h, SlotInfo& out) {
    memcpy(out.name, h.name, sizeof(out.name));
    out.name[sizeof(out.name) - 1] = 0;
    out.usedPatterns = h.usedPatterns;
    out.songs = h.songs;
    out.bpm = h.bpm;
}

template <class T>
static void clampField(T& v, int lo, int hi) {
    if ((int)v < lo) v = (T)lo;
    if ((int)v > hi) v = (T)hi;
}

static void sanitizePages(int16_t* v, const uint8_t* pageIds, int pages, int sampleMax) {
    for (int g = 0; g < pages; ++g)
        for (int i = 0; i < kParamsPerPage; ++i) {
            const ParamDef& p = pageDef(pageIds[g]).p[i];
            clampField(v[g * kParamsPerPage + i], p.min, p.unit == Unit::Sample ? sampleMax : p.max);
        }
}

void sanitize(Project& pj) {
    pj.name[sizeof(pj.name) - 1] = 0;
    clampField(pj.bpm, 40, 240);
    clampField(pj.swing, 0, 50);
    clampField(pj.key, 0, 11);
    clampField(pj.scale, 0, kScaleCount - 1);
    clampField(pj.master, 0, 127);
    static const uint8_t kSynthIds[kSynthPages] = {PG_OSC1, PG_OSC2, PG_SUB, PG_FLT1, PG_FLT2, PG_GRIT, PG_AMP, PG_MOD, PG_GLOB};
    for (Part& p : pj.parts) {
        clampField(p.type, 0, 1);
        clampField(p.voices, 1, p.type == PT_SAMPLER ? 8 : 5);
        clampField(p.level, 0, 127);
        clampField(p.priority, 0, 3);
        clampField(p.preset, -1, 32000);   // an id; one the card no longer has shows no name
        migratePages(p.pages);
        sanitizePages(&p.pages[0][0], kSynthIds, kSynthPages, 0);
    }
    // the one-heavy-filter rule, as the knobs keep it (a file from elsewhere may break it): FILTER 2 falls back
    for (int i = 0; i < kParts; ++i) {
        const cost::VoiceDesc d = descOf(pj, i);
        if (!cost::heavyRuleOk(d)) pj.parts[i].pages[PG_FLT2][0] = (int16_t)menuOf(cost::lightFallback(d.f2));
    }
    for (Pattern& pat : pj.patterns)
        for (Lane& l : pat.lanes) {
            clampField(l.part, 0, kParts - 1);
            clampField(l.root, 12, 108);
            clampField(l.n, 1, kMaxSteps);
            clampField(l.k, 0, l.n);
            l.r = (uint8_t)(l.r % l.n);
            l.eu = l.eu ? 1 : 0;
            clampField(l.level, 0, 127);
            l.mute = l.mute ? 1 : 0;
            clampField(l.gate, 1, 127);
            clampField(l.arp.mode, 0, kArpModeCount - 1);
            clampField(l.arp.rate, 0, kArpRateCount - 1);
            clampField(l.arp.oct, 1, 4);
            clampField(l.arp.gate, 5, 100);
            // the sample number is checked against the library at play time (the card's samples may differ from the saving one)
            sanitizePages(&l.smp[0][0], kLanePageIds, kLanePages, 32767);
            for (Step& s : l.st) {
                s.ov = s.ov ? 1 : 0;
                clampField(s.note, -24, 24);
                clampField(s.vel, 1, 127);
                clampField(s.gate, 1, 127);
                clampField(s.prob, 0, 100);
                clampField(s.chord, 0, kChordCount - 1);
                clampField(s.inv, 0, 3);
                s.spread = s.spread ? 1 : 0;
                clampField(s.strum, 0, 120);
                clampField(s.nLocks, 0, kLocksPerStep);
                int n = 0;
                for (int i = 0; i < s.nLocks; ++i) {
                    Lock lk = s.locks[i];
                    const int lpg = lockPage(lk.key), lpr = lockParam(lk.key);
                    if (lpg >= PG_COUNT && !fxLockableParam(lpg, lpr)) continue;   // (an effects lock: a send, or one of the amp's knobs)
                    bool dup = false;
                    for (int j = 0; j < n; ++j) dup = dup || s.locks[j].key == lk.key;
                    if (dup) continue;
                    if (lpg >= PG_COUNT) clampField(lk.value, 0, fxMax(fxDef(lpg == PG_FXSEND ? FX_SENDS : FX_AMP, lpr)));
                    else {
                        const ParamDef& p = pageDef(lpg).p[lpr];
                        clampField(lk.value, p.min, p.unit == Unit::Sample ? 32767 : p.max);
                    }
                    s.locks[n++] = lk;
                }
                s.nLocks = (uint8_t)n;
            }
        }
    refreshIdleParts(pj);
    sanitizeFx(pj.fx);
    for (LoopFit& f : pj.loops)
        if (f.sample < 0 || f.start >= f.end || f.end >= f.frames) f.sample = -1;
    for (SliceMap& m : pj.slices) {
        if (m.sample < 0) { m.sample = -1; m.count = 0; continue; }
        clampField(m.count, 0, kSlices);
        uint32_t last = 0;
        int n = 0;
        for (int i = 0; i < m.count; ++i) {   // in order and inside the sample, or not at all
            Slice s = m.s[i];
            if (s.start >= m.frames || (n > 0 && s.start <= last)) continue;
            clampField(s.pitch, -24, 24);
            clampField(s.level, 0, 127);
            s.flags &= SLICE_REVERSE | SLICE_MUTED;
            s.pad = 0;
            last = s.start;
            m.s[n++] = s;
        }
        m.count = (uint8_t)n;
    }
    for (LaneMidi& m : pj.midi) {
        if (m.ch > 16) m.ch = 0;   // a damaged channel: the lane's MIDI off, not some other channel
        clampField(m.note, 0, 127);
    }
    for (Song& s : pj.songs) {
        clampField(s.nRows, 0, kSongRows);
        s.loop = s.loop ? 1 : 0;
        for (SongRow& r : s.rows) {
            clampField(r.pat, 0, kPatterns - 1);
            clampField(r.rep, 1, 16);
        }
    }
}

}  // namespace app
