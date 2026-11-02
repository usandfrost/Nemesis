#include "app/Params.h"

#include <math.h>

#include "app/PresetFile.h"
#include "app/SampleLib.h"
#include "app/WaveLib.h"
#include "app/Theory.h"

namespace app {

const char* const kWaves[] = {"SIN", "TRI", "SAW", "SQR"};
const char* const kModels[] = {"LP", "BP", "HP", "NOTCH", "LADDER", "ACID", "SOVIET", "CLASSIC"};
static const char* const kRoute[] = {"SERIES", "PARALLEL"};
static const char* const kOffOn[] = {"OFF", "ON"};
static const char* const kLfoDest[] = {"CUTOFF", "PITCH", "AMP", "PD AMT"};
static const char* const kClock[] = {"INT", "MIDI", "SYNC"};
static const char* const kSmpModels[] = {"LP", "BP", "HP"};
static const char* const kPlayModes[] = {"ONESHOT", "GATED", "LOOP"};
static const char* const kChoke[] = {"OFF", "G1", "G2", "G3", "G4"};
static const char* const kSliceModes[] = {"OFF", "STEP", "SYNC"};
static const char* const kSliceBeatNames[] = {"1", "2", "4", "8", "16", "32"};
static const char* const kSliceFits[] = {"SLICES", "PITCH"};
static const char* const kSliceGrids[] = {"AUTO", "4", "8", "16", "32"};
static const char* const kLoopAutos[] = {"OFF", "EQ AMP", "EQ POWER"};
static const char* const kLoopUnits[] = {"MS", "%"};
static const char* const kLoopCurves[] = {"LINEAR", "EQ POWER", "LOG", "MATCH"};

#define P(n, v, lo, hi, u) {n, lo, hi, v, nullptr, Unit::u}
#define L(n, v, lo, hi, l) {n, lo, hi, v, l, Unit::Plain}
#define NONE {"-", 0, 0, 0, nullptr, Unit::Plain}
// a knob that is gone from the panel but whose slot is in the files: it keeps the range (older files are valid), cannot be turned, and migratePages() empties it on load
#define RETIRED {"-", 0, 127, 0, nullptr, Unit::Plain}

static const PageDef kPages[PG_COUNT] = {
    {"OSC 1", 0xff3da6, {P("WAVE", 0, 0, kWaveMax, Wave), P("PD SHAPE", 40, 0, 127, Pct), P("PD AMT", 0, 0, 127, Pct), P("OCT", 0, -2, 2, Oct),
                         P("SEMI", 0, -12, 12, Semi), P("FINE", 0, -50, 50, Cents), P("LEVEL", 100, 0, 127, Pct), RETIRED}},
    {"OSC 2", 0xff3da6, {P("WAVE", 2, 0, kWaveMax, Wave), P("PD SHAPE", 40, 0, 127, Pct), P("PD AMT", 0, 0, 127, Pct), P("OCT", 0, -2, 2, Oct),
                         P("SEMI", 7, -12, 12, Semi), P("FINE", 6, -50, 50, Cents), P("LEVEL", 0, 0, 127, Pct), P("SYNC", 0, 0, 127, Pct)}},
    {"SUB / UNISON", 0xc96bff, {P("SUB LVL", 0, 0, 127, Pct), P("SUB OCT", -1, -2, -1, Oct), P("NOISE", 0, 0, 127, Pct), P("RING", 0, 0, 127, Pct),
                                P("UNISON", 1, 1, 4, Unison), P("UNI DET", 20, 0, 127, Pct), P("UNI BLND", 64, 0, 127, Pct), P("WIDTH", 64, 0, 127, Pct)}},
    {"FILTER 1", 0x5ff08a, {L("MODEL", 0, 0, 7, kModels), P("CUTOFF", 80, 0, 127, Cutoff), P("RESO", 30, 0, 127, Pct), P("DRIVE", 0, 0, 127, Pct),
                            P("KEYTRK", 40, 0, 127, Pct), P("ENV AMT", 40, 0, 127, Pct), P("HPF", 0, 0, 127, Hpf), P("MIX F1/F2", 0, 0, 127, Pct)}},
    {"FILTER 2", 0x5ff08a, {L("MODEL", 1, 0, 7, kModels), P("CUTOFF", 100, 0, 127, Cutoff), P("RESO", 10, 0, 127, Pct), P("DRIVE", 0, 0, 127, Pct),
                            L("ROUTE", 0, 0, 1, kRoute), L("ACTIVE", 0, 0, 1, kOffOn), P("KEYTRK", 0, 0, 127, Pct), P("ENV AMT", 0, 0, 127, Pct)}},
    {"GRIT", 0xff5d5d, {P("BITS", 16, 1, 16, Bits), P("COMPAND", 0, 0, 127, Pct), P("RATE", 127, 0, 127, Rate), P("DRIFT", 0, 0, 127, Pct),
                        P("LOOP ERR", 0, 0, 127, Pct), P("WOW", 0, 0, 127, Pct), P("CLIP", 0, 0, 127, Pct), P("MIX", 127, 0, 127, Pct)}},
    {"AMP ENV", 0xffd23d, {P("ATTACK", 4, 0, 127, Ms), P("DECAY", 50, 0, 127, Ms), P("SUSTAIN", 70, 0, 127, Pct), P("RELEASE", 40, 0, 127, Ms),
                           P("VEL SENS", 80, 0, 127, Pct), P("LEVEL", 100, 0, 127, Pct), P("PAN", 64, 0, 127, Pan), P("VEL>ATK", 0, 0, 127, Pct)}},
    {"MOD", 0x9d7bff, {P("LFO RATE", 40, 0, 127, LfoRate), P("LFO DEPTH", 0, 0, 127, Pct), L("LFO DEST", 0, 0, 3, kLfoDest), L("LFO SHAPE", 0, 0, 3, kWaves),
                       P("ENV>PD", 0, 0, 127, Pct), P("ENV AMT+", 0, 0, 127, Pct), P("MOD WHL", 64, 0, 127, Pct), P("AFTERT", 0, 0, 127, Pct)}},
    {"GLOBAL", 0x19d3e6, {P("MASTER", 100, 0, 127, Pct), P("BEND RNG", 2, 0, 24, Bend), P("MIDI CH", 0, 0, 16, MidiCh), P("TUNE", 0, -50, 50, Cents),
                          L("CLOCK", 0, 0, 2, kClock), P("PREVIEW", 80, 0, 127, Pct), NONE, NONE}},
    {"SAMPLE", 0xff7e5f, {P("SAMPLE", 0, 0, 0, Sample), P("START", 0, 0, 127, Pct), P("END", 127, 0, 127, Pct), L("PLAY", 0, 0, 2, kPlayModes),
                          P("PITCH", 0, -24, 24, Semi), P("FINE", 0, -50, 50, Cents), L("REVERSE", 0, 0, 1, kOffOn), P("LEVEL", 110, 0, 127, Pct)}},
    {"FILTER", 0x5ff08a, {L("MODEL", 0, 0, 2, kSmpModels), P("CUTOFF", 127, 0, 127, Cutoff), P("RESO", 10, 0, 127, Pct), P("DRIVE", 0, 0, 127, Pct),
                          P("ENV AMT", 0, 0, 127, Pct), P("HPF", 0, 0, 127, Hpf), NONE, NONE}},
    {"AMP ENV", 0xffd23d, {P("ATTACK", 0, 0, 127, Ms), P("DECAY", 60, 0, 127, Ms), P("SUSTAIN", 127, 0, 127, Pct), P("RELEASE", 30, 0, 127, Ms),
                           P("VEL SENS", 80, 0, 127, Pct), P("LEVEL", 110, 0, 127, Pct), P("PAN", 64, 0, 127, Pan), L("CHOKE", 0, 0, 4, kChoke)}},
    {"SLICE", 0xffb13d, {L("MODE", 0, 0, 2, kSliceModes), P("SLICE", 0, 0, kSlices, Slice), P("RANDOM", 0, 0, 127, Pct), L("BEATS", 3, 0, 5, kSliceBeatNames),
                         L("FIT", 0, 0, 1, kSliceFits), P("RND REV", 0, 0, 127, Pct), P("SENS", 64, 0, 127, Pct), L("GRID", 0, 0, 4, kSliceGrids)}},
    {"LOOP", 0x6fd3ff, {L("AUTO", 0, 0, 2, kLoopAutos), P("XFADE", 0, 0, 127, Plain), L("UNIT", 0, 0, 1, kLoopUnits), L("CURVE", 1, 0, 3, kLoopCurves),
                        P("S FINE", 0, -63, 63, Plain), P("E FINE", 0, -63, 63, Plain), P("SEARCH", 32, 1, 127, Pct), NONE}},
};
#undef P
#undef L
#undef NONE

static const char* const kPageKeys[PG_COUNT] = {"OSC1", "OSC2", "SUB", "FLT1", "FLT2", "GRIT", "AMP", "MOD", "GLOB", "SMP", "FLT1", "AMP", "SLC", "LOOP"};

const PageDef& pageDef(int page) { return kPages[page]; }
const char* pageKey(int page) { return kPageKeys[page]; }

double cutHz(int v) { return 30.0 * pow(16000.0 / 30.0, v / 127.0); }
double envMs(int v) { return 2.0 + pow(v / 127.0, 2.2) * 2500.0; }

void fmtHz(Text& out, double h) {
    if (h >= 1000) out.fixed(h / 1000, h >= 10000 ? 0 : 1).ch('k');
    else out.num(jsRound(h));
    out.add(" Hz");
}
void fmtMs(Text& out, double m) {
    if (m >= 1000) out.fixed(m / 1000, m >= 10000 ? 0 : 2).add(" s");
    else out.num(jsRound(m)).add(" ms");
}

void formatParam(Text& out, int page, int param, int v, const SampleLib* lib) {
    const ParamDef& p = kPages[page].p[param];
    if (p.list) { out.add(p.list[clampi(v, p.min, p.max)]); return; }
    switch (p.unit) {
        case Unit::Pct: out.num(jsRound(v / 1.27)).ch('%'); return;
        case Unit::Cutoff: fmtHz(out, cutHz(v)); return;
        case Unit::Hpf: if (v == 0) out.add("OFF"); else fmtHz(out, cutHz(v) / 8); return;
        case Unit::Ms: fmtMs(out, envMs(v)); return;
        case Unit::Pan:
            if (v == 64) out.add("C");
            else if (v < 64) out.ch('L').num(jsRound((64 - v) / 0.64));
            else out.ch('R').num(jsRound((v - 64) / 0.63));
            return;
        case Unit::Oct: out.sgn(v).add(" oct"); return;
        case Unit::Semi: out.sgn(v).add(" st"); return;
        case Unit::Cents: out.sgn(v).add(" ct"); return;
        case Unit::Bend: out.add("\xC2\xB1").num(v).add(" st"); return;   // ±
        case Unit::Bits: out.num(v).add(" bit"); return;
        case Unit::Rate: out.fixed(1 + (v / 127.0) * 43.1, 1).add(" kHz"); return;
        case Unit::LfoRate: out.fixed(0.2 + (v / 127.0) * 12, 1).add(" Hz"); return;
        case Unit::Unison: out.ch('x').num(v); return;
        case Unit::MidiCh: if (v == 0) out.add("OMNI"); else out.add("CH").num(v); return;
        case Unit::Sample: out.add(lib && v >= 0 && v < lib->count ? lib->items[v].name : "?"); return;
        case Unit::Wave: formatWave(out, v); return;
        case Unit::Slice: if (v == 0) out.add("AUTO"); else out.num(v); return;
        case Unit::Plain: break;
    }
    if (p.min < 0) out.sgn(v);
    else out.num(v);
}

const char* const kPresetCats[] = {"ALL", "BASS", "LEAD", "PAD", "PLUCK", "STAB", "POLY", "KEYS", "FX", "SEQ", "TEXTURE", "BELL"};
const Preset kPresets[kBasePresets] = {
    {"Iron Squelch", 1, {0, 2}, {1, 40, 3, 70}, {0, 3, 1, 36, 8}},
    {"Sub Pressure", 1, {0, 0}, {1, 36, 1, 0}, {0, 4, 3, 50, 10}},
    {"Dimension Bloom", 3, {0, 2}, {1, 58, 2, 8}, {60, 80, 7, 110, 80}},
};
// The sounds of a new project's parts A, B and C (Model.cpp makeDefaultProject): not on the preset list (the list's other seven factory presets were
// taken out on 2026-10-28), so the parts show no preset name until one is chosen.
const Preset kBootSounds[kBootSoundCount] = {
    {"Boot bass", 1, {0, 2}, {1, 52, 2, 20}, {0, 3, 1, 28, 6}},
    {"Boot lead", 2, {0, 3}, {1, 100, 2, 10}, {0, 5, 2, 60, 12}},
    {"Boot pad", 3, {0, 1}, {1, 64, 1, 4}, {50, 60, 6, 100, 70}},
};

// ---- the library on the card (app/PresetFile.h)
static PresetInfo* gList = nullptr;     // the index, in id order
static int gListN = 0, gListCap = 0;
static PresetStorage* gStorage = nullptr;

// The presets read from their files: a small cache. Only the main loop fills it. The audio interrupt reads it (the preview), and only a preset that
// presetLoad has just made the most recently used: replacing one takes kPresetCache other loads, none of which can happen in between.
constexpr int kPresetCache = 16;
static struct CacheSlot {
    PresetRecord rec;
    uint32_t used;
    volatile int32_t id;     // -1: empty
} gCache[kPresetCache];
static uint32_t gCacheTick = 0;

static int listFind(int id) {
    int lo = 0, hi = gListN - 1;
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (gList[m].id == id) return m;
        if (gList[m].id < id) lo = m + 1; else hi = m - 1;
    }
    return -1;
}
static CacheSlot* cacheFind(int id) {
    for (CacheSlot& s : gCache) if (s.id == id) return &s;
    return nullptr;
}
static void cacheClear() {
    for (CacheSlot& s : gCache) { s.id = -1; s.used = 0; }
    gCacheTick = 0;
}

// Puts a record in the cache (the least recently used slot, or the preset's own). The slot is emptied before it is rewritten, so the audio
// interrupt never reads one that is half written.
static void cacheStore(const PresetRecord& rec) {
    CacheSlot* victim = cacheFind(rec.id);
    if (!victim) {
        victim = &gCache[0];
        for (CacheSlot& s : gCache) {
            if (s.id < 0) { victim = &s; break; }
            if (s.used < victim->used) victim = &s;
        }
    }
    victim->id = -1;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    victim->rec = rec;
    victim->used = ++gCacheTick;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    victim->id = rec.id;
}
static void cacheDrop(int id) {
    if (CacheSlot* s = cacheFind(id)) s->id = -1;
}

void presetsInit(PresetInfo* mem, int capacity) {
    gList = mem;
    gListCap = capacity < 0 ? 0 : capacity > kPresetCap ? kPresetCap : capacity;
    gListN = 0;
    cacheClear();
}
void presetsForget() {
    gListN = 0;
    gStorage = nullptr;
    cacheClear();
}

int presetCount() { return kBasePresets + gListN; }
int presetIdAt(int k) { return k < 0 ? -1 : k < kBasePresets ? k : k - kBasePresets < gListN ? gList[k - kBasePresets].id : -1; }
bool presetExists(int id) { return id >= 0 && (id < kBasePresets || listFind(id) >= 0); }
const char* presetName(int id) {
    if (id < 0) return "";
    if (id < kBasePresets) return kPresets[id].name;
    const int k = listFind(id);
    return k < 0 ? "" : gList[k].name;
}
int presetCat(int id) {
    if (id < 0) return 0;
    if (id < kBasePresets) return kPresets[id].cat;
    const int k = listFind(id);
    return k < 0 ? 0 : gList[k].cat;
}
int presetVoices(int id) {
    if (id < kBasePresets) return 0;
    const int k = listFind(id);
    return k < 0 ? 0 : gList[k].voices;
}
void presetBanks(int id, int out[2]) {
    out[0] = out[1] = -1;
    if (id < kBasePresets) return;
    const int k = listFind(id);
    if (k < 0) return;
    for (int s = 0; s < 2; ++s) out[s] = gList[k].bank[s] == 255 ? -1 : gList[k].bank[s];
}

bool presetReady(int id) { return id >= 0 && (id < kBasePresets || cacheFind(id) != nullptr); }

bool presetLoad(int id) {
    if (id < 0) return false;
    if (id < kBasePresets) return true;
    if (CacheSlot* hit = cacheFind(id)) { hit->used = ++gCacheTick; return true; }
    const int k = listFind(id);
    if (k < 0 || !gStorage) return false;
    uint8_t buf[kPresetBodySize + 1];
    size_t n = 0;
    PresetRecord rec;
    if (!gStorage->readFile(gList[k].file, buf, sizeof(buf), n) || parsePresetFile(buf, n, rec) != PresetParse::Ok || rec.id != id) return false;
    // the file is what plays; if the index was stale (a file replaced with the same name, size and date) the list takes its description
    gList[k].cat = rec.cat;
    gList[k].voices = rec.voices;
    gList[k].bank[0] = rec.bank[0];
    gList[k].bank[1] = rec.bank[1];
    memcpy(gList[k].name, rec.name, sizeof(gList[k].name));
    cacheStore(rec);
    return true;
}

// ---- reading the folder
static bool isPrsName(const char* name) {
    const size_t n = strlen(name);
    return n >= 4 && name[0] != '.' && name[n - 4] == '.' && (name[n - 3] | 32) == 'p' && (name[n - 2] | 32) == 'r' && (name[n - 1] | 32) == 's';   // *.PRS (not ._hidden)
}
struct OpenCtx {
    PresetStorage* st;
    uint32_t fp;
    int files;      // the .PRS files the first listing saw
    int seen;       // ... and the second (reading) pass
    int ignored;    // files that are not presets (damaged, invalid, a duplicate id, no room)
    int ioErrors;   // files the card would not give: not the files' fault, so the index is not written
};
static void stampFile(void* p, const char* name, uint32_t size, uint32_t stamp) {
    OpenCtx& c = *static_cast<OpenCtx*>(p);
    if (!isPrsName(name)) return;
    ++c.files;
    c.fp += presetFileStamp(name, size, stamp);
}
// Puts one preset on the list in id order. An id that is taken: the file with the smaller name keeps it (`force`: the new one replaces it). A full list:
// the largest id goes, so the same files are kept whatever order the folder lists them in. 0 not added (left out), 1 added, 2 added and another left out.
static int listAdd(const PresetRecord& r, const char* file, bool force) {
    PresetInfo x;
    memset(&x, 0, sizeof(x));
    x.id = r.id;
    x.cat = r.cat;
    x.voices = r.voices;
    x.bank[0] = r.bank[0];
    x.bank[1] = r.bank[1];
    memcpy(x.name, r.name, sizeof(x.name));
    strncpy(x.file, file, kPresetFileMax);
    int pos = gListN;
    while (pos > 0 && gList[pos - 1].id > x.id) --pos;
    if (pos > 0 && gList[pos - 1].id == x.id) {
        if (force || strcmp(x.file, gList[pos - 1].file) < 0) gList[pos - 1] = x;
        return force ? 1 : 0;
    }
    int result = 1;
    if (gListN >= gListCap) {
        if (gListN == 0 || x.id > gList[gListN - 1].id) return 0;
        --gListN;   // the largest id makes room
        result = 2;
    }
    memmove(&gList[pos + 1], &gList[pos], (size_t)(gListN - pos) * sizeof(PresetInfo));
    gList[pos] = x;
    ++gListN;
    return result;
}
static void readOneFile(void* p, const char* name, uint32_t size, uint32_t) {
    OpenCtx& c = *static_cast<OpenCtx*>(p);
    if (!isPrsName(name)) return;
    ++c.seen;
    if (!presetFileNameOk(name) || size != (uint32_t)kPresetBodySize) { ++c.ignored; return; }
    uint8_t buf[kPresetBodySize + 1];
    size_t got = 0;
    PresetRecord rec;
    if (!c.st->readFile(name, buf, sizeof(buf), got)) { ++c.ioErrors; return; }
    if (parsePresetFile(buf, got, rec) != PresetParse::Ok) { ++c.ignored; return; }
    const int r = listAdd(rec, name, false);
    if (r == 0) ++c.ignored;
    else if (r == 2) ++c.ignored;
}

// The index file in pieces of 8 entries (one open and one transfer each: a file system open searches the folder).
constexpr size_t kIndexChunk = 8 * kPresetIndexEntry;
struct IndexReader {
    PresetStorage& st;
    size_t total, off, have, pos;
    uint8_t buf[kIndexChunk];
    uint32_t crc;
    IndexReader(PresetStorage& s, size_t n) : st(s), total(n), off(0), have(0), pos(0), crc(0xFFFFFFFFu) {}
    bool get(uint8_t* out, size_t n, bool inCrc) {
        for (size_t i = 0; i < n; ++i) {
            if (pos == have) {
                const size_t want = total - off < kIndexChunk ? total - off : kIndexChunk;
                if (want == 0 || !st.readIndex(off, buf, want)) return false;
                off += want;
                have = want;
                pos = 0;
            }
            out[i] = buf[pos++];
        }
        if (inCrc) crc = presetCrcUpdate(crc, out, n);
        return true;
    }
};
struct IndexWriter {
    PresetStorage& st;
    size_t off, fill;
    uint8_t buf[kIndexChunk];
    uint32_t crc;
    bool ok;
    explicit IndexWriter(PresetStorage& s) : st(s), off(0), fill(0), crc(0xFFFFFFFFu), ok(true) {}
    void put(const uint8_t* d, size_t n, bool inCrc) {
        if (inCrc) crc = presetCrcUpdate(crc, d, n);
        for (size_t i = 0; i < n && ok; ++i) {
            buf[fill++] = d[i];
            if (fill == kIndexChunk) flush();
        }
    }
    void flush() {
        if (fill && ok) { ok = st.writeIndex(off, buf, fill); off += fill; }
        fill = 0;
    }
};
static bool gIndexWanted = false;        // the list in memory is newer than the card's index: a start does not write, so this waits for a save
static uint32_t gIndexPrint = 0;
static bool writeIndexFile(PresetStorage& st, uint32_t fp) {
    IndexWriter w(st);
    uint8_t hdr[kPresetIndexHeader], ent[kPresetIndexEntry], tail[4];
    encodePresetIndexHeader(gListN, fp, hdr);
    w.put(hdr, sizeof(hdr), true);
    for (int i = 0; i < gListN; ++i) {
        encodePresetIndexEntry(gList[i], ent);
        w.put(ent, sizeof(ent), true);
    }
    presetWr32(tail, ~w.crc);
    w.put(tail, sizeof(tail), false);
    w.flush();
    const bool ended = st.endIndex();
    return w.ok && ended;
}
// The folder's fingerprint, with the list's size in it: an index written for a smaller list is not good for a larger one.
static uint32_t folderFingerprint(PresetStorage& st, bool& ok, int* files) {
    OpenCtx c = {&st, 0, 0, 0, 0, 0};
    ok = st.list(stampFile, &c);
    if (files) *files = c.files;
    return c.fp ^ ((uint32_t)c.files * 2654435761u) ^ ((uint32_t)gListCap * 40503u);
}

PresetOpen presetsOpen(PresetStorage& st) {
    PresetOpen res;
    gListN = 0;
    gStorage = nullptr;
    cacheClear();
    if (!gList) return res;
    bool listed = false;
    int files = 0;
    const uint32_t fp = folderFingerprint(st, listed, &files);
    if (!listed) return res;       // the card did not answer: the factory three only
    gStorage = &st;
    // the index is trusted only if all of it is good, its fingerprint matches the folder's and the CRC at the end holds
    bool good = false;
    size_t total = 0;
    int cnt = 0;
    uint32_t stored = 0;
    uint8_t hdr[kPresetIndexHeader];
    if (st.indexSize(total) && total >= kPresetIndexHeader + 4) {
        IndexReader rd(st, total);
        if (rd.get(hdr, sizeof(hdr), true) && decodePresetIndexHeader(hdr, cnt, stored) && total == presetIndexSize(cnt) && stored == fp && cnt <= gListCap) {
            good = true;
            int last = -1;
            uint8_t ent[kPresetIndexEntry];
            for (int i = 0; i < cnt && good; ++i) {
                PresetInfo x;
                good = rd.get(ent, sizeof(ent), true) && decodePresetIndexEntry(ent, x) && (int)x.id > last;
                if (!good) break;
                last = x.id;
                gList[i] = x;
            }
            uint8_t tail[4];
            const uint32_t crc = ~rd.crc;
            good = good && rd.get(tail, sizeof(tail), false) && crc == presetRd32(tail);
        }
    }
    if (good) {
        gListN = cnt;
        res.count = cnt;
        return res;
    }
    gListN = 0;
    OpenCtx c = {&st, 0, files, 0, 0, 0};
    const bool listedAgain = st.list(readOneFile, &c);
    res.rebuilt = true;
    res.ignored = c.ignored;
    res.count = gListN;
    res.readErrors = c.ioErrors + (listedAgain && c.seen == c.files ? 0 : 1);
    // the new index (best effort: a card that cannot be written is read again next time). Not when the card failed to give some files or the folder
    // changed under the pass: those presets would be missing from an index that then matches the folder.
    res.wrote = res.readErrors == 0 && writeIndexFile(st, fp);
    gIndexWanted = res.readErrors == 0 && !res.wrote;
    gIndexPrint = fp;
    return res;
}

void presetsWriteIndex() {
    if (gIndexWanted && gStorage && writeIndexFile(*gStorage, gIndexPrint)) gIndexWanted = false;
}

// ---- saving a preset from the panel
// The id after the largest (ids of presets that were taken off the card are not handed out again while there is room above); when the top is
// reached, the first free id from 1000.
int presetNextId() {
    if (!gStorage || !gList || gListN >= gListCap) return -1;
    int id = gListN && gList[gListN - 1].id >= kPresetFirstUser ? gList[gListN - 1].id + 1 : kPresetFirstUser;
    if (id <= kPresetIdMax) return id;
    id = kPresetFirstUser;
    for (int k = 0; k < gListN; ++k) {
        if (gList[k].id < id) continue;
        if (gList[k].id != id) break;
        ++id;
    }
    return id > kPresetIdMax ? -1 : id;
}
static void indexFollows() {
    bool listed = false;
    const uint32_t fp = folderFingerprint(*gStorage, listed, nullptr);
    gIndexWanted = !(listed && writeIndexFile(*gStorage, fp));   // (best effort: if it fails the next start reads the files again)
}

bool presetLibraryOpen() { return gStorage && gList; }
int presetFiles() { return gListN; }

bool presetReadFile(int id, uint8_t out[kPresetBodySize]) {
    const int k = gStorage ? listFind(id) : -1;
    size_t n = 0;
    return k >= 0 && gStorage->readFile(gList[k].file, out, kPresetBodySize, n) && n == (size_t)kPresetBodySize;
}

PresetPut presetPutFile(const uint8_t* body, size_t n, int id, bool replace) {
    if (!presetLibraryOpen()) return PresetPut::NoCard;
    if (id < kPresetIdMin) return PresetPut::BuiltIn;
    PresetRecord rec;
    if (parsePresetFile(body, n, rec) != PresetParse::Ok || rec.id != id) return PresetPut::BadPreset;
    const int k = listFind(id);
    if (k >= 0 && !replace) return PresetPut::InUse;
    if (k < 0 && gListN >= gListCap) return PresetPut::Full;
    char file[kPresetFileMax + 1];
    if (k >= 0) strcpy(file, gList[k].file);   // (a replaced file keeps its name: the id in front is what names it)
    else presetFileNameFor(id, rec.name, file);
    if (k < 0 && gStorage->fileExists(file)) return PresetPut::InUse;
    if (!gStorage->writeFile(file, body, n)) return PresetPut::WriteFailed;
    listAdd(rec, file, true);
    cacheDrop(id);
    indexFollows();
    return PresetPut::Ok;
}

PresetPut presetRemove(int id, uint32_t nameCrc) {
    if (!presetLibraryOpen()) return PresetPut::NoCard;
    if (id < kBasePresets) return PresetPut::BuiltIn;
    const int k = listFind(id);
    if (k < 0) return PresetPut::Missing;
    if (presetCrc(reinterpret_cast<const uint8_t*>(gList[k].name), kPresetNameMax + 2) != nameCrc) return PresetPut::WrongName;
    if (!gStorage->removeFile(gList[k].file)) return PresetPut::WriteFailed;
    for (int i = k; i + 1 < gListN; ++i) gList[i] = gList[i + 1];
    --gListN;
    cacheDrop(id);
    indexFollows();
    return PresetPut::Ok;
}

PresetSave presetSave(PresetRecord& rec, int overwriteId, int& idOut) {
    if (!gStorage || !gList) return PresetSave::NoCard;
    if (!presetHeadOk(kPresetIdMin, rec.cat, rec.voices, rec.bank, rec.name)) return PresetSave::BadPreset;
    int id;
    char oldFile[kPresetFileMax + 1] = {0};
    if (overwriteId >= 0) {
        const int k = overwriteId < kPresetIdMin ? -1 : listFind(overwriteId);
        if (k < 0) return PresetSave::Missing;
        id = overwriteId;
        strcpy(oldFile, gList[k].file);
    } else {
        id = presetNextId();
        if (id < 0) return PresetSave::Full;
    }
    rec.id = (uint16_t)id;
    rec.mask &= PM_ALL;
    // a new preset gets "<id> <name>.PRS"; a replaced one keeps its file's name (the id in front is what names it), so no second file is ever involved
    char file[kPresetFileMax + 1];
    if (oldFile[0]) strcpy(file, oldFile);
    else presetFileNameFor(id, rec.name, file);
    if (overwriteId < 0 && gStorage->fileExists(file)) return PresetSave::Exists;
    uint8_t body[kPresetBodySize];
    encodePresetFile(rec, body);
    if (!gStorage->writeFile(file, body, sizeof(body))) return PresetSave::WriteFailed;
    PresetInfo before;
    memset(&before, 0, sizeof(before));
    const int k0 = listFind(id);
    if (k0 >= 0) before = gList[k0];
    listAdd(rec, file, true);
    cacheDrop(id);
    // the index: not for a replace that changed nothing it lists (the folder's fingerprint is the same too: a replaced file keeps its name and size)
    if (k0 < 0 || memcmp(&before, &gList[listFind(id)], sizeof(PresetInfo)) != 0) {
        indexFollows();   // (a preset that is loaded corrects its entry)
    }
    idOut = id;
    return PresetSave::Ok;
}

bool applyPresetFx(FxSettings& fx, int part, int id) {
    if (part < 0 || part >= kFxParts || id < 0) return false;
    const CacheSlot* s = id < kBasePresets ? nullptr : cacheFind(id);
    if (id >= kBasePresets && !s) return false;   // not read yet: nothing changes
    for (int k = 0; k < kSends; ++k) fx.send[part][k] = 0;
    for (int k = 0; k < kFxParams; ++k) fx.amp[part][k] = (int16_t)fxDefault(FX_AMP, k);   // an amp OFF with its knobs at their defaults
    if (!s) return true;
    const PresetRecord& r = s->rec;
    for (int k = 0; k < kSendsUsed; ++k) fx.send[part][k] = r.send[k];
    if (r.mask & PM_AMP)
        for (int k = 0; k < kFxParams; ++k) fx.amp[part][k] = r.amp[k];
    auto otherUses = [&](int e) { for (int q = 0; q < kFxParts; ++q) if (q != part && fx.send[q][e] > 0) return true; return false; };
    if ((r.mask & PM_DELAY) && !otherUses(0)) for (int k = 0; k < kFxParams; ++k) fx.delay[k] = r.delay[k];
    if ((r.mask & PM_REVERB) && !otherUses(1)) for (int k = 0; k < kFxParams; ++k) fx.reverb[k] = r.reverb[k];
    if ((r.mask & PM_CHORUS) && !otherUses(2)) for (int k = 0; k < kFxParams; ++k) fx.chorus[k] = r.chorus[k];
    return true;
}

// Older files kept the unison detune on two pages (OSC 1 and SUB, the voice used the larger of the two): SUB's is the one left, and takes the larger value.
void migratePages(int16_t pg[kSynthPages][kParamsPerPage]) {
    if (pg[PG_OSC1][7] != 0) {
        if (pg[PG_OSC1][7] > pg[PG_SUB][5]) pg[PG_SUB][5] = pg[PG_OSC1][7];
        pg[PG_OSC1][7] = 0;
    }
}

bool applyPresetIndex(int16_t pg[kSynthPages][kParamsPerPage], int id) {
    if (id < 0) return false;
    if (id < kBasePresets) { applyPreset(pg, kPresets[id]); return true; }
    const CacheSlot* s = cacheFind(id);
    if (!s) return false;
    const PresetRecord& r = s->rec;
    const int16_t clock = pg[PG_GLOB][4];   // (the project's, not the sound's)
    for (int p = 0; p < kSynthPages; ++p)
        for (int k = 0; k < kParamsPerPage; ++k) pg[p][k] = r.v[p * kParamsPerPage + k];
    pg[PG_GLOB][4] = clock;
    for (int b = 0; b < 2; ++b)   // an oscillator on a bank: WAVE = 4 + the bank (a missing file plays the nearest basic wave: the part still sounds)
        if (r.bank[b] != 255) pg[b == 0 ? PG_OSC1 : PG_OSC2][0] = (int16_t)(kBasicWaves + r.bank[b]);
    migratePages(pg);
    return true;
}

void applyPreset(int16_t pg[kSynthPages][kParamsPerPage], const Preset& pr) {
    pg[PG_OSC1][0] = pr.osc1[1];
    pg[PG_FLT1][0] = pr.flt1[0];
    pg[PG_FLT1][1] = pr.flt1[1];
    pg[PG_FLT1][2] = pr.flt1[2];
    pg[PG_FLT1][5] = pr.flt1[3];
    pg[PG_AMP][0] = pr.amp[0];
    pg[PG_AMP][1] = pr.amp[1];
    pg[PG_AMP][2] = (int16_t)(pr.amp[2] * 12);
    pg[PG_AMP][3] = pr.amp[3];
    pg[PG_AMP][4] = (int16_t)(70 + pr.amp[4] > 127 ? 127 : 70 + pr.amp[4]);   // the mockup's presets go past 127 here: held to the knob's range
}

void defaultSynthPages(int16_t pg[kSynthPages][kParamsPerPage]) {
    for (int g = 0; g < kSynthPages; ++g)
        for (int i = 0; i < kParamsPerPage; ++i) pg[g][i] = kPages[g].p[i].def;
}

void defaultLanePages(int16_t pg[kLanePages][kParamsPerPage]) {
    for (int g = 0; g < kLanePages; ++g)
        for (int i = 0; i < kParamsPerPage; ++i) pg[g][i] = kPages[kLanePageIds[g]].p[i].def;
}

}  // namespace app
