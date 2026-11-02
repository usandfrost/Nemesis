// The preset library on the card (firmware/src/app/PresetFile.h and the library part of Params.cpp): the file format and what is done with bad files, the
// index and when it is read again, and that a preset's file is read only when it is asked for.
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "PresetHost.h"
#include "app/Params.h"
#include "app/PresetFile.h"

using namespace app;

static int checks = 0, fails = 0;
static void ok(bool c, const std::string& m) {
    ++checks;
    if (!c) { ++fails; printf("FAIL %s\n", m.c_str()); }
}

// ---- helpers
static PresetRecord makeRec(int id, const char* name, int cat = 1, int voices = 2) {
    PresetRecord r;
    memset(&r, 0, sizeof(r));
    r.id = (uint16_t)id;
    r.cat = (uint8_t)cat;
    r.voices = (uint8_t)voices;
    r.bank[0] = r.bank[1] = 255;
    strncpy(r.name, name, kPresetNameMax);
    int16_t pg[kSynthPages][kParamsPerPage];
    defaultSynthPages(pg);
    for (int p = 0; p < kSynthPages; ++p)
        for (int k = 0; k < kParamsPerPage; ++k) r.v[p * kParamsPerPage + k] = (int8_t)(pg[p][k] > 127 ? 127 : pg[p][k]);
    r.send[0] = 40;
    r.send[1] = 20;
    r.mask = PM_DELAY | PM_AMP;
    for (int k = 0; k < kFxParams; ++k) {
        r.delay[k] = (uint8_t)fxDefault(FX_DELAY, k);
        r.amp[k] = (uint8_t)fxDefault(FX_AMP, k);
    }
    r.amp[0] = 2;
    return r;
}
static std::vector<uint8_t> bytesOf(const PresetRecord& r) {
    std::vector<uint8_t> b(kPresetBodySize);
    encodePresetFile(r, b.data());
    return b;
}
static void refix(std::vector<uint8_t>& b) { presetWr32(&b[kPresetBodySize - 4], presetCrc(b.data(), kPresetBodySize - 4)); }
static PresetParse parseBytes(const std::vector<uint8_t>& b, PresetRecord* out = nullptr) {
    PresetRecord tmp;
    return parsePresetFile(b.data(), b.size(), out ? *out : tmp);
}

// A card in memory.
struct MemStore : PresetStorage {
    struct F { std::vector<uint8_t> data; uint32_t stamp = 1; };
    std::map<std::string, F> files;
    std::vector<uint8_t> index;
    bool hasIndex = false, folder = true, failWrite = false, failRead = false;
    int fileReads = 0, indexWrites = 0;
    bool list(void (*fn)(void*, const char*, uint32_t, uint32_t), void* ctx) override {
        if (!folder) return false;
        for (auto& kv : files) fn(ctx, kv.first.c_str(), (uint32_t)kv.second.data.size(), kv.second.stamp);
        return true;
    }
    bool readFile(const char* name, uint8_t* buf, size_t cap, size_t& n) override {
        ++fileReads;
        n = 0;
        auto it = files.find(name);
        if (failRead || it == files.end()) return false;
        n = it->second.data.size() < cap ? it->second.data.size() : cap;
        memcpy(buf, it->second.data.data(), n);
        return it->second.data.size() <= cap;
    }
    bool indexSize(size_t& n) override { n = index.size(); return hasIndex; }
    bool readIndex(size_t off, uint8_t* buf, size_t len) override {
        if (!hasIndex || off + len > index.size()) return false;
        memcpy(buf, index.data() + off, len);
        return true;
    }
    bool failFileWrite = false;
    int fileWrites = 0;
    bool fileExists(const char* name) override { return files.count(name) != 0; }
    bool writeFile(const char* name, const uint8_t* buf, size_t n) override {
        if (failFileWrite) return false;
        ++fileWrites;
        files[name].data.assign(buf, buf + n);
        files[name].stamp += 1;
        return true;
    }
    bool writeIndex(size_t off, const uint8_t* buf, size_t len) override {
        if (off == 0) { ++indexWrites; index.clear(); hasIndex = true; if (failWrite) return false; }
        if (failWrite) return false;
        if (index.size() < off + len) index.resize(off + len);
        memcpy(index.data() + off, buf, len);
        return true;
    }
    void put(const PresetRecord& r, const std::string& file) { files[file].data = bytesOf(r); }
};

static PresetInfo* gMem = nullptr;
static void freshLibrary(int cap = kPresetCap) { presetsInit(gMem, cap); }

static void testFormat() {
    const PresetRecord base = makeRec(77, "Round Trip", 3, 4);
    std::vector<uint8_t> good = bytesOf(base);
    PresetRecord got;
    ok(parseBytes(good, &got) == PresetParse::Ok && got.id == 77 && strcmp(got.name, "Round Trip") == 0 && got.cat == 3 && got.voices == 4 &&
           got.bank[0] == 255 && got.send[0] == 40 && got.mask == (PM_DELAY | PM_AMP) && memcmp(got.v, base.v, sizeof(got.v)) == 0 &&
           memcmp(got.delay, base.delay, kFxParams) == 0 && memcmp(got.amp, base.amp, kFxParams) == 0,
       "a preset file reads back as it was written");
    ok(good[0] == 'D' && good[3] == 'R' && good.size() == 148, "the file is 148 bytes starting DBPR");

    int accepted = 0;
    for (size_t i = 0; i < good.size(); ++i) {
        std::vector<uint8_t> b = good;
        b[i] ^= 0x10;
        accepted += parseBytes(b) == PresetParse::Ok;
    }
    ok(accepted == 0, "a changed byte anywhere is refused (header or checksum): " + std::to_string(accepted));
    for (size_t n : {(size_t)0, (size_t)1, (size_t)147, (size_t)149, (size_t)296}) {
        std::vector<uint8_t> b(n, 0);
        ok(parsePresetFile(b.data(), n, got) == PresetParse::Size, "a file of " + std::to_string(n) + " bytes is refused");
    }
    {
        std::vector<uint8_t> b = good;
        b[0] = 'X'; refix(b);
        ok(parseBytes(b) == PresetParse::Magic, "wrong magic");
        b = good; b[4] = 2; refix(b);
        ok(parseBytes(b) == PresetParse::Version, "a newer version is refused");
    }

    struct Bad { const char* what; size_t off; uint8_t val; };
    const Bad bad[] = {
        {"id 9 (a factory number)", 6, 9}, {"id 0", 6, 0}, {"category 0 (ALL)", 8, 0}, {"category 12", 8, 12}, {"category 255", 8, 255},
        {"0 voices", 9, 0}, {"6 voices", 9, 6}, {"bank 200", 10, 200}, {"bank 254", 11, 254}, {"an empty name", 12, 0}, {"a name starting with a space", 12, ' '},
        {"a name with a control character", 14, 7}, {"a name with a high byte", 14, 0xC3}, {"a 21 character name", 32, 'x'}, {"stray bytes after the name's end", 12 + 11, 'z'},
        {"a byte in the last two name bytes", 34, 1}, {"an unknown effect flag", 111, 16},
    };
    for (const Bad& x : bad) {
        std::vector<uint8_t> b = good;
        b[x.off] = x.val;
        if (x.off == 6 && x.val == 0) b[7] = 0;
        if (x.off == 32) { for (int i = 12; i < 32; ++i) b[i] = 'a'; }
        if (x.off == 12 + 11) b[12 + 5] = 0;   // the name ends at 5, a stray byte follows
        refix(b);
        ok(parseBytes(b) == PresetParse::Field, std::string("refused with a good checksum: ") + x.what);
    }
    {
        std::vector<uint8_t> b = good;
        presetWr16(&b[6], 32001); refix(b);
        ok(parseBytes(b) == PresetParse::Field, "id 32001 is refused");
        presetWr16(&b[6], 32000); refix(b);
        ok(parseBytes(b) == PresetParse::Ok, "id 32000 is the largest");
        presetWr16(&b[6], 10); refix(b);
        ok(parseBytes(b) == PresetParse::Ok, "id 10 is the smallest");
    }
    {   // values are brought into their knob's range
        std::vector<uint8_t> b = good;
        int lowP = -1, highP = -1;
        for (int i = 0; i < kSynthPages * kParamsPerPage && highP < 0; ++i) {
            const ParamDef& d = pageDef(i / kParamsPerPage).p[i % kParamsPerPage];
            if (d.max < 100 && !(i % kParamsPerPage == 0 && (i / kParamsPerPage == PG_OSC1 || i / kParamsPerPage == PG_OSC2))) highP = i;
        }
        for (int i = 0; i < kSynthPages * kParamsPerPage && lowP < 0; ++i)
            if (pageDef(i / kParamsPerPage).p[i % kParamsPerPage].min >= 0 && i != 0) lowP = i;
        b[36 + highP] = 120;
        b[36 + lowP] = (uint8_t)-100;
        b[36] = 100;                // OSC 1 WAVE: a bank is the bank field's business
        b[108] = 255;
        b[112] = 250; b[121] = 200;
        refix(b);
        PresetRecord r;
        ok(parseBytes(b, &r) == PresetParse::Ok, "out-of-range values do not refuse the file");
        ok(r.v[highP] == pageDef(highP / 8).p[highP % 8].max, "a value above its knob's top is held to it");
        ok(r.v[lowP] == pageDef(lowP / 8).p[lowP % 8].min, "a value below its knob's bottom is held to it");
        ok(r.v[0] <= kBasicWaves - 1, "OSC 1 WAVE is a basic wave (0-3) in the file");
        ok(r.send[0] == 127, "a send above 127 is held to 127");
        ok(r.amp[0] <= fxMax(fxDef(FX_AMP, 0)) && r.delay[1] <= 127, "effect values are held to their parameters");
    }
    ok(sizeof(PresetInfo) <= (size_t)kPresetIndexEntry, "an index entry in memory is at most 64 bytes: " + std::to_string(sizeof(PresetInfo)));
    ok(presetIndexSize(kPresetCap) < 70000, "the largest index is about 64 KB");
}

static void testNames() {
    ok(presetFileNameOk("0010 Acid Roll.PRS") && presetFileNameOk("a.prs") && presetFileNameOk("MY_BASS-2 (b).PRS"), "ordinary file names");
    ok(!presetFileNameOk(".PRS") && !presetFileNameOk(".hidden.PRS") && !presetFileNameOk("x.TXT") && !presetFileNameOk("a/b.PRS") && !presetFileNameOk("a\\b.PRS") &&
           !presetFileNameOk("a:b.PRS") && !presetFileNameOk("caf\xC3\xA9.PRS") && !presetFileNameOk("") && !presetFileNameOk("0123456789012345678901234567890.PRS"),
       "names that are refused: no stem, hidden, wrong extension, separators, non-ASCII, empty, too long");
    PresetInfo e;
    memset(&e, 0, sizeof(e));
    e.id = 500; e.cat = 2; e.voices = 3; e.bank[0] = 12; e.bank[1] = 255;
    strcpy(e.name, "Entry"); strcpy(e.file, "0500 Entry.PRS");
    uint8_t buf[kPresetIndexEntry];
    encodePresetIndexEntry(e, buf);
    PresetInfo g;
    ok(decodePresetIndexEntry(buf, g) && g.id == 500 && strcmp(g.name, "Entry") == 0 && strcmp(g.file, "0500 Entry.PRS") == 0 && g.bank[0] == 12, "an index entry reads back");
    buf[2] = 0;
    ok(!decodePresetIndexEntry(buf, g), "an entry with category 0 is refused");
    encodePresetIndexEntry(e, buf);
    buf[28] = '/';
    ok(!decodePresetIndexEntry(buf, g), "an entry whose file name has a separator is refused");
    uint8_t h[kPresetIndexHeader];
    encodePresetIndexHeader(131, 0xDEADBEEF, h);
    int c = 0;
    uint32_t fp = 0;
    ok(decodePresetIndexHeader(h, c, fp) && c == 131 && fp == 0xDEADBEEF, "the index header reads back");
    encodePresetIndexHeader(kPresetCap + 1, 1, h);
    ok(!decodePresetIndexHeader(h, c, fp), "a count above the limit is refused");
    h[0] = 'Q';
    ok(!decodePresetIndexHeader(h, c, fp), "wrong index magic");
}

static std::string fname(int id, const char* n) { char b[64]; snprintf(b, sizeof b, "%04d %s.PRS", id, n); return b; }

static void testLibrary() {
    MemStore st;
    // files named so that the name order is not the id order
    st.put(makeRec(30, "Thirty"), "a_thirty.PRS");
    st.put(makeRec(10, "Ten"), "b_ten.PRS");
    st.put(makeRec(1000, "User One", 5, 1), "c_user.PRS");
    st.put(makeRec(20, "Twenty"), "d_twenty.PRS");
    st.files["readme.txt"].data = {1, 2, 3};

    freshLibrary();
    PresetOpen r = presetsOpen(st);
    ok(r.rebuilt && r.wrote && r.count == 4 && r.ignored == 0, "the first open reads the files and writes the index");
    ok(st.fileReads == 4 && st.indexWrites == 1, "each file is read once to build the index: " + std::to_string(st.fileReads));
    ok(presetCount() == kBasePresets + 4 && presetIdAt(2) == 2 && presetIdAt(3) == 10 && presetIdAt(4) == 20 && presetIdAt(5) == 30 && presetIdAt(6) == 1000 && presetIdAt(7) == -1,
       "the list is the three factory presets, then the card's in id order");
    ok(presetExists(1000) && presetExists(2) && !presetExists(3) && !presetExists(9) && !presetExists(500) && !presetExists(-1) && !presetExists(11), "which numbers exist");
    ok(strcmp(presetName(1000), "User One") == 0 && presetCat(1000) == 5 && presetVoices(1000) == 1 && strcmp(presetName(500), "") == 0 && presetCat(500) == 0 && presetVoices(500) == 0,
       "the name, category and voices come from the index; an unknown number has none");
    int b[2];
    presetBanks(500, b);
    ok(b[0] == -1 && b[1] == -1, "an unknown number plays no banks");

    // opening again: the index is enough, no file is read
    st.fileReads = 0;
    freshLibrary();
    r = presetsOpen(st);
    ok(!r.rebuilt && r.count == 4 && st.fileReads == 0 && st.indexWrites == 1, "the next open uses the index and reads no preset file");
    ok(presetCount() == kBasePresets + 4 && presetIdAt(6) == 1000 && strcmp(presetName(20), "Twenty") == 0, "the same list");

    // lazy: a file is read when the preset is asked for, once
    int16_t pg[kSynthPages][kParamsPerPage], keep[kSynthPages][kParamsPerPage];
    defaultSynthPages(pg);
    memcpy(keep, pg, sizeof(pg));
    FxSettings fx;
    defaultFx(fx);
    const FxSettings fx0 = fx;
    ok(!presetReady(20) && !applyPresetIndex(pg, 20) && memcmp(pg, keep, sizeof(pg)) == 0 && !applyPresetFx(fx, 0, 20) && memcmp(&fx, &fx0, sizeof(fx)) == 0,
       "a preset whose file has not been read changes nothing when applied");
    ok(presetLoad(20) && st.fileReads == 1 && presetReady(20), "presetLoad reads that one file");
    ok(presetLoad(20) && st.fileReads == 1, "and not again");
    ok(applyPresetIndex(pg, 20) && pg[PG_OSC1][0] == makeRec(20, "x").v[0] && applyPresetFx(fx, 1, 20) && fx.send[1][0] == 40 && fx.send[1][1] == 20 && fx.amp[1][0] == 2,
       "then it applies: its pages, sends and amp");
    ok(presetReady(2) && presetLoad(2) && st.fileReads == 1, "a factory preset is always ready, with no file");
    ok(!presetLoad(500) && !presetLoad(-1) && !presetLoad(11), "a number not on the list cannot be loaded");

    // the cache: the least recently used goes
    {
        MemStore s2;
        for (int i = 0; i < 20; ++i) s2.put(makeRec(2000 + i, "Many"), fname(2000 + i, "Many"));
        freshLibrary();
        presetsOpen(s2);
        s2.fileReads = 0;
        for (int i = 0; i < 16; ++i) presetLoad(2000 + i);
        ok(s2.fileReads == 16 && presetReady(2000) && presetReady(2015), "sixteen presets fit the cache");
        presetLoad(2000);   // the first becomes the most recent
        presetLoad(2016);
        ok(presetReady(2000) && !presetReady(2001) && presetReady(2016) && s2.fileReads == 17, "a seventeenth replaces the least recently used, not the one just touched");
        const int before = s2.fileReads;
        ok(presetLoad(2001) && s2.fileReads == before + 1, "and the replaced one reads again when asked");
        bool all = true;
        for (int i = 0; i < 20; ++i) all = all && presetLoad(2000 + i) && presetReady(2000 + i);
        ok(all, "every one of twenty presets can be loaded in turn");
    }

    // the folder changes: the index is built again
    auto reopen = [&](MemStore& s) { freshLibrary(); return presetsOpen(s); };
    st.put(makeRec(40, "Forty"), "e_forty.PRS");
    r = reopen(st);
    ok(r.rebuilt && r.count == 5 && presetExists(40) && presetIdAt(6) == 40, "a file added: found at the next open");
    r = reopen(st);
    ok(!r.rebuilt && r.count == 5, "and the index was rewritten");
    st.put(makeRec(40, "Forty Two"), "e_forty.PRS");
    st.files["e_forty.PRS"].stamp += 1;   // writing a file sets its date
    r = reopen(st);
    ok(r.rebuilt && strcmp(presetName(40), "Forty Two") == 0, "a file replaced (its date changed): read again");
    st.files["e_forty.PRS"].stamp += 7;
    r = reopen(st);
    ok(r.rebuilt && r.count == 5, "a changed date alone is enough");
    st.files.erase("e_forty.PRS");
    r = reopen(st);
    ok(r.rebuilt && r.count == 4 && !presetExists(40), "a file removed: gone at the next open");
    {   // renamed: same content, another name
        auto node = st.files.extract("d_twenty.PRS");
        node.key() = "z_twenty.PRS";
        st.files.insert(std::move(node));
        r = reopen(st);
        ok(r.rebuilt && r.count == 4 && presetExists(20), "a file renamed: read again");
    }

    // a damaged index is rebuilt, never trusted
    for (int what = 0; what < 5; ++what) {
        reopen(st);
        std::vector<uint8_t> saved = st.index;
        const char* why = "";
        if (what == 0) { st.index[st.index.size() / 2] ^= 0x01; why = "a flipped byte"; }
        if (what == 1) { st.index.resize(st.index.size() - 10); why = "a short file"; }
        if (what == 2) { st.index.push_back(0); why = "a long file"; }
        if (what == 3) { st.hasIndex = false; why = "no file"; }
        if (what == 4) { st.index.assign(st.index.size(), 0xFF); why = "garbage"; }
        r = reopen(st);
        ok(r.rebuilt && r.count == 4 && presetExists(1000) && strcmp(presetName(30), "Thirty") == 0, std::string("an index with ") + why + " is built again");
        (void)saved;
    }
    {   // an index that checks out but lists ids out of order, or a name that fails: refused
        reopen(st);
        // swap two entries and fix the checksum
        std::vector<uint8_t> ix = st.index;
        uint8_t tmp[kPresetIndexEntry];
        memcpy(tmp, &ix[kPresetIndexHeader], kPresetIndexEntry);
        memcpy(&ix[kPresetIndexHeader], &ix[kPresetIndexHeader + kPresetIndexEntry], kPresetIndexEntry);
        memcpy(&ix[kPresetIndexHeader + kPresetIndexEntry], tmp, kPresetIndexEntry);
        presetWr32(&ix[ix.size() - 4], presetCrc(ix.data(), ix.size() - 4));
        st.index = ix;
        r = reopen(st);
        ok(r.rebuilt && presetIdAt(3) == 10 && presetIdAt(4) == 20, "an index whose ids are out of order is built again");
    }

    // a card that cannot be written: the list still works, the files are read at each start
    {
        MemStore s3;
        s3.put(makeRec(50, "Fifty"), fname(50, "Fifty"));
        s3.failWrite = true;
        r = reopen(s3);
        ok(r.rebuilt && !r.wrote && r.count == 1 && presetExists(50), "a card that cannot be written still gives its presets");
        r = reopen(s3);
        ok(r.rebuilt && r.count == 1, "and reads them again next time");
    }

    // bad and duplicate files are left out, and counted
    {
        MemStore s4;
        s4.put(makeRec(60, "Good"), fname(60, "Good"));
        s4.put(makeRec(61, "Dup B"), "0061 b.PRS");
        s4.put(makeRec(61, "Dup A"), "0061 a.PRS");
        std::vector<uint8_t> crc = bytesOf(makeRec(62, "Bad Crc"));
        crc[50] ^= 1;
        s4.files["0062 crc.PRS"].data = crc;
        s4.files["0063 short.PRS"].data = std::vector<uint8_t>(100, 0);
        s4.put(makeRec(64, "Bad File Name"), "0064|x.PRS");
        std::vector<uint8_t> low = bytesOf(makeRec(65, "Low"));
        low[6] = 2; low[7] = 0; refix(low);
        s4.files["0065 factory id.PRS"].data = low;
        s4.files["notes.txt"].data = std::vector<uint8_t>(500, 'x');
        s4.files["INDEX.OLD"].data = std::vector<uint8_t>(500, 'x');
        r = reopen(s4);
        ok(r.count == 2 && r.ignored == 5 && presetExists(60) && presetExists(61) && !presetExists(62) && !presetExists(64) && !presetExists(3 + 1000), "bad files are left out and counted: " + std::to_string(r.count) + "/" + std::to_string(r.ignored));
        ok(strcmp(presetName(61), "Dup A") == 0, "of two files with one id, the one with the smaller name keeps it");
        ok(strcmp(presetName(2), "Dimension Bloom") == 0, "a file claiming a factory number does not replace it");
        s4.fileReads = 0;
        r = reopen(s4);
        ok(!r.rebuilt && r.count == 2 && s4.fileReads == 0, "the bad files do not cause a rebuild at every start");
    }

    // a full list
    {
        MemStore s5;
        for (int i = 0; i < 8; ++i) s5.put(makeRec(70 + i, "Cap"), fname(70 + i, "Cap"));
        freshLibrary(3);
        r = presetsOpen(s5);
        ok(r.count == 3 && r.ignored == 5 && presetCount() == kBasePresets + 3, "the list holds what it has room for, the rest is counted: " + std::to_string(r.count) + "/" + std::to_string(r.ignored));
        freshLibrary(3);
        r = presetsOpen(s5);
        ok(!r.rebuilt && r.count == 3, "and the index is good for that list");
        freshLibrary(2);
        r = presetsOpen(s5);
        ok(r.rebuilt && r.count == 2, "a smaller list than the index holds rebuilds it");
    }

    // no folder, no card
    {
        MemStore s6;
        s6.folder = false;
        freshLibrary();
        r = presetsOpen(s6);
        ok(r.count == 0 && !r.rebuilt && presetCount() == kBasePresets && presetExists(2) && !presetExists(5) && !presetLoad(10), "no folder: the three factory presets");
        presetsForget();
        ok(presetCount() == kBasePresets, "forgotten");
    }

    // files that cannot be read when the preset is asked for; or that changed under the index
    {
        MemStore s7;
        s7.put(makeRec(90, "Reader"), fname(90, "Reader"));
        s7.put(makeRec(91, "Mover"), fname(91, "Mover"));
        freshLibrary();
        presetsOpen(s7);
        freshLibrary();
        presetsOpen(s7);   // from the index; the cache is empty
        s7.failRead = true;
        int16_t p2[kSynthPages][kParamsPerPage];
        defaultSynthPages(p2);
        ok(!presetLoad(90) && !presetReady(90) && !applyPresetIndex(p2, 90), "a file that cannot be read: not loaded, nothing applied");
        s7.failRead = false;
        // replaced with another preset's content but the same size and date: the index is stale; the id in the file must match
        s7.files[fname(91, "Mover")].data = bytesOf(makeRec(92, "Other"));
        ok(!presetLoad(91) && !presetReady(91) && !presetReady(92), "a file whose id is not the index's is refused");
        ok(presetLoad(90), "and the others still load");
    }
}


// a read error while the files are read for the index must not become a permanent hole in the list
static void testReadErrors() {
    MemStore st;
    for (int i = 0; i < 5; ++i) st.put(makeRec(100 + i, "Item"), fname(100 + i, "Item"));
    freshLibrary();
    st.failRead = true;
    PresetOpen r = presetsOpen(st);
    ok(r.rebuilt && r.count == 0 && r.readErrors == 5 && !r.wrote && !st.hasIndex, "a card that gives no file: no index is written (" + std::to_string(r.readErrors) + " errors)");
    st.failRead = false;
    freshLibrary();
    r = presetsOpen(st);
    ok(r.rebuilt && r.count == 5 && r.readErrors == 0 && r.wrote, "the next start reads them all and writes the index");
    // a file the card fails on among good ones
    st.index.clear(); st.hasIndex = false;
    struct Flaky : PresetStorage {
        MemStore& m; int failOn;
        Flaky(MemStore& s, int f) : m(s), failOn(f) {}
        int n = 0;
        bool list(void (*fn)(void*, const char*, uint32_t, uint32_t), void* c) override { return m.list(fn, c); }
        bool readFile(const char* name, uint8_t* b, size_t cap, size_t& k) override { return ++n == failOn ? (k = 0, false) : m.readFile(name, b, cap, k); }
        bool indexSize(size_t& k) override { return m.indexSize(k); }
        bool readIndex(size_t o, uint8_t* b, size_t l) override { return m.readIndex(o, b, l); }
        bool writeIndex(size_t o, const uint8_t* b, size_t l) override { return m.writeIndex(o, b, l); }
        bool writeFile(const char* a, const uint8_t* b, size_t l) override { return m.writeFile(a, b, l); }
        bool fileExists(const char* a) override { return m.fileExists(a); }
    } flaky(st, 3);
    freshLibrary();
    r = presetsOpen(flaky);
    ok(r.count == 4 && r.readErrors == 1 && !r.wrote && !st.hasIndex, "one file the card failed on: the others are listed, no index written");
    flaky.failOn = 0;
    freshLibrary();
    r = presetsOpen(flaky);
    ok(r.count == 5 && r.wrote, "and the next start has all five");
}

// a full list keeps the smallest ids whatever order the files come in
static void testCapacity() {
    std::vector<int> ids = {500, 20, 300, 40, 100, 60, 90, 11};
    for (int round = 0; round < 3; ++round) {
        MemStore st;
        for (size_t i = 0; i < ids.size(); ++i) {
            const size_t k = round == 0 ? i : round == 1 ? ids.size() - 1 - i : (i * 3) % ids.size();
            st.put(makeRec(ids[k], "Cap"), "f" + std::to_string(round == 2 ? (int)i : (int)k) + " " + std::to_string(ids[k]) + ".PRS");
        }
        freshLibrary(4);
        PresetOpen r = presetsOpen(st);
        ok(r.count == 4 && presetIdAt(kBasePresets) == 11 && presetIdAt(kBasePresets + 1) == 20 && presetIdAt(kBasePresets + 2) == 40 && presetIdAt(kBasePresets + 3) == 60 && r.ignored == 4,
           "four places, eight files (order " + std::to_string(round) + "): the four smallest ids stay, " + std::to_string(r.ignored) + " counted");
    }
}

// files that are not presets (hidden, system, macOS's ._ copies) are not part of the folder
static void testJunk() {
    MemStore st;
    st.put(makeRec(10, "Good"), fname(10, "Good"));
    freshLibrary();
    PresetOpen r = presetsOpen(st);
    const int fpFiles = r.count;
    st.put(makeRec(11, "Junk"), "._0011 Junk.PRS");
    st.files["._0011 Junk.PRS"].data.assign(4096, 0);
    st.put(makeRec(12, "Dot"), ".hidden.PRS");
    freshLibrary();
    r = presetsOpen(st);
    ok(!r.rebuilt && r.count == fpFiles && r.ignored == 0, "a dot file is neither listed nor does it change the folder");
}

// a stale index (a file replaced with the same name, size and date) takes the file's description when the preset is loaded
static void testStale() {
    MemStore st;
    st.put(makeRec(30, "Old Name", 2, 3), fname(30, "x"));
    freshLibrary();
    presetsOpen(st);
    PresetRecord changed = makeRec(30, "New Name", 6, 1);
    changed.bank[0] = 12;
    st.files[fname(30, "x")].data = bytesOf(changed);   // (same size, same date)
    freshLibrary();
    presetsOpen(st);
    ok(strcmp(presetName(30), "Old Name") == 0 && presetCat(30) == 2 && presetVoices(30) == 3, "the list still shows the old description");
    ok(presetLoad(30) && strcmp(presetName(30), "New Name") == 0 && presetCat(30) == 6 && presetVoices(30) == 1, "loading takes the file's name, category and voices");
    int b[2];
    presetBanks(30, b);
    ok(b[0] == 12 && b[1] == -1, "and its banks");
}

static void testSave() {
    MemStore st;
    st.put(makeRec(10, "Lib A"), fname(10, "Lib A"));
    st.put(makeRec(11, "Lib B"), fname(11, "Lib B"));
    freshLibrary();
    presetsOpen(st);
    const int n0 = presetCount();
    ok(presetNextId() == 1000, "the first new id is 1000");
    PresetRecord rec = makeRec(0, "First Save", 4, 3);
    rec.bank[1] = 33;
    int id = -1;
    ok(presetSave(rec, -1, id) == PresetSave::Ok && id == 1000 && presetCount() == n0 + 1 && presetIdAt(presetCount() - 1) == 1000, "a new preset gets 1000 and goes last");
    ok(st.files.count("1000 First Save.PRS") == 1, "its file is '<id> <name>.PRS'");
    ok(strcmp(presetName(1000), "First Save") == 0 && presetCat(1000) == 4 && presetVoices(1000) == 3, "the list has its name, category and voices");
    int b[2];
    presetBanks(1000, b);
    ok(b[0] == -1 && b[1] == 33, "and its banks");
    ok(presetNextId() == 1001, "the next one is 1001");
    // the index was brought up to date: the next start does not read the files
    st.fileReads = 0;
    freshLibrary();
    PresetOpen r = presetsOpen(st);
    ok(!r.rebuilt && r.count == 3 && st.fileReads == 0 && presetExists(1000), "the index already lists it");
    // the saved file is a good preset file: it loads, and holds what was saved
    int16_t pg[kSynthPages][kParamsPerPage];
    defaultSynthPages(pg);
    ok(presetLoad(1000) && applyPresetIndex(pg, 1000) && pg[PG_OSC2][0] == kBasicWaves + 33, "it loads, with its bank on OSC 2");
    FxSettings fx;
    defaultFx(fx);
    ok(applyPresetFx(fx, 0, 1000) && fx.send[0][0] == 40 && fx.send[0][1] == 20 && fx.amp[0][0] == 2, "and its effects");
    // names with characters a file name cannot have
    PresetRecord odd = makeRec(0, "A/B:C*D?E", 1, 1);
    ok(presetSave(odd, -1, id) == PresetSave::Ok && id == 1001 && st.files.count("1001 A_B_C_D_E.PRS") == 1, "odd characters become '_' in the file name only");
    ok(strcmp(presetName(1001), "A/B:C*D?E") == 0, "(the name itself is kept as typed)");
    // replace: same id, new name; the file keeps its name
    PresetRecord again = makeRec(0, "Renamed", 5, 2);
    int id2 = -1;
    ok(presetSave(again, 1000, id2) == PresetSave::Ok && id2 == 1000 && presetCount() == n0 + 2, "replacing keeps the id and the count");
    ok(st.files.count("1000 First Save.PRS") == 1 && st.files.count("1000 Renamed.PRS") == 0, "the file keeps its name (no second file)");
    ok(strcmp(presetName(1000), "Renamed") == 0 && presetCat(1000) == 5, "the list follows");
    ok(presetLoad(1000) && presetReady(1000), "it loads again");
    applyPresetIndex(pg, 1000);
    ok(pg[PG_OSC2][0] != kBasicWaves + 33, "from the new file, not from a cache");
    // a library preset can be replaced; a factory one and an unknown one cannot
    PresetRecord lib = makeRec(0, "Lib A Edit", 3, 2);
    ok(presetSave(lib, 10, id) == PresetSave::Ok && id == 10 && strcmp(presetName(10), "Lib A Edit") == 0, "a library preset can be replaced");
    PresetRecord fac = makeRec(0, "Nope");
    ok(presetSave(fac, 2, id) == PresetSave::Missing && presetSave(fac, 0, id) == PresetSave::Missing && presetSave(fac, 555, id) == PresetSave::Missing, "a factory preset or an unknown id cannot be replaced");
    ok(strcmp(presetName(2), "Dimension Bloom") == 0, "the factory preset is as it was");
    // bad presets
    PresetRecord bad = makeRec(0, "Bad");
    bad.cat = 0;
    ok(presetSave(bad, -1, id) == PresetSave::BadPreset, "category 0 is refused");
    bad = makeRec(0, "Bad"); bad.name[0] = 0;
    ok(presetSave(bad, -1, id) == PresetSave::BadPreset, "an empty name is refused");
    bad = makeRec(0, "Bad"); bad.voices = 9;
    ok(presetSave(bad, -1, id) == PresetSave::BadPreset, "9 voices is refused");
    // the card cannot be written
    const int before = presetCount();
    st.failFileWrite = true;
    PresetRecord w = makeRec(0, "Unwritten");
    ok(presetSave(w, -1, id) == PresetSave::WriteFailed && presetCount() == before && presetNextId() == 1002, "a failed write changes nothing");
    st.failFileWrite = false;
    // a name that is already a file: not overwritten
    st.files["1002 Taken.PRS"].data = bytesOf(makeRec(7777, "Other"));
    PresetRecord tk = makeRec(0, "Taken");
    ok(presetSave(tk, -1, id) == PresetSave::Exists && presetCount() == before, "an existing file of that name is not overwritten");
    st.files.erase("1002 Taken.PRS");
    // the list is full
    freshLibrary(3);
    presetsOpen(st);
    ok(presetCount() == kBasePresets + 3 && presetNextId() == -1, "no room: no id");
    PresetRecord full = makeRec(0, "Full");
    ok(presetSave(full, -1, id) == PresetSave::Full, "a new preset is refused");
    ok(presetSave(full, 10, id) == PresetSave::Ok, "but one can still be replaced");
    // no card
    presetsForget();
    ok(presetSave(full, -1, id) == PresetSave::NoCard && presetNextId() == -1, "no card");
    // the highest ids
    freshLibrary();
    MemStore st2;
    st2.put(makeRec(31999, "High"), fname(31999, "High"));
    presetsOpen(st2);
    PresetRecord h = makeRec(0, "H1");
    ok(presetSave(h, -1, id) == PresetSave::Ok && id == 32000, "id 32000 is the last");
    PresetRecord h2 = makeRec(0, "H2");
    ok(presetSave(h2, -1, id) == PresetSave::Ok && id == 1000, "at the top, the first free id from 1000 is used");
    ok(st2.files.count("32000 H1.PRS") == 1, "five-digit ids keep all their digits in the file name");
    {   // a hand-made preset at the very top does not block saving
        MemStore st3;
        st3.put(makeRec(32000, "Top"), fname(32000, "Top"));
        freshLibrary();
        presetsOpen(st3);
        PresetRecord x = makeRec(0, "Below");
        ok(presetNextId() == 1000 && presetSave(x, -1, id) == PresetSave::Ok && id == 1000, "one preset at id 32000 does not make the list full");
        // ids of 10000 and more do not collide with ids below
        PresetRecord a1 = makeRec(12345, "Same"), a2 = makeRec(2345, "Same");
        st3.put(a1, "12345 Same.PRS");
        st3.put(a2, "2345 Same.PRS");
        freshLibrary();
        presetsOpen(st3);
        PresetRecord rep = makeRec(0, "Same", 2, 1);
        ok(presetSave(rep, 12345, id) == PresetSave::Ok && presetLoad(2345) && presetLoad(12345), "replacing id 12345 leaves id 2345 alone");
    }
    // an index written for a small list is not used for a bigger one
    {
        MemStore st4;
        for (int i = 0; i < 10; ++i) st4.put(makeRec(200 + i, "Many"), fname(200 + i, "Many"));
        freshLibrary(4);
        presetsOpen(st4);
        freshLibrary(12);
        const PresetOpen r4 = presetsOpen(st4);
        ok(r4.rebuilt && r4.count == 10, "a larger list than the index was made for reads the files again: " + std::to_string(r4.count));
    }
}

int main() {
    gMem = new PresetInfo[kPresetCap];
    testFormat();
    testNames();
    testLibrary();
    testReadErrors();
    testCapacity();
    testJunk();
    testStale();
    testSave();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
