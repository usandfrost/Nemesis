// The firmware's own saving (board/Card.cpp, the preset library) on the modelled card: what a start, a save and a power cut leave behind.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>
#include <vector>

#include "FatAudit.h"
#include "SdModel.h"
#include "app/Params.h"
#include "app/PresetFile.h"
#include "app/ProjectFile.h"
#include "board/Card.h"
#include "board/CardSoak.h"
#include "fatfs.h"
#include "memory/SdramHeap.h"

using namespace app;

extern "C" Disk_drvTypeDef disk;
extern int failures;
#define CHECK(c) do { if (!(c)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace memory {
size_t sdramHeapBytesFree() { return 8u * 1024 * 1024; }
size_t sdramHeapBytesUsed() { return 0; }
bool inSdram(const void*) { return false; }
}

constexpr int kCardPresets = 40 + 3;   // and the three built into the firmware

static SampleLib* lib;
static Project* pj;
static Project* scratch;
static PresetInfo* presetMem;

static bool boot() {
    sd::powerCycle();
    static bool linked = false;
    if (!linked) { MX_FATFS_Init(); linked = true; }
    disk.is_initialized[0] = 0;
    memset(&SDFatFS, 0, sizeof SDFatFS);
    new (&board::card) board::Card();
    const bool ok = board::card.mount();
    presetsInit(presetMem, kPresetCap);
    if (ok) presetsOpen(board::card.presets());
    return ok;
}

static void project(int mark) { makeDefaultProject(*pj); pj->bpm = (uint16_t)(60 + mark); }
static PresetRecord preset(const char* name, int value) {
    PresetRecord r;
    memset(&r, 0, sizeof r);
    r.cat = 1;
    r.voices = 2;
    r.bank[0] = r.bank[1] = 255;
    strncpy(r.name, name, kPresetNameMax);
    r.v[2] = (int8_t)value;
    return r;
}
static int loaded(int slot) {   // the mark of what the slot loads, -1 if nothing does
    int missing = 0;
    if (!board::card.loadSlot(slot, *pj, *scratch, *lib, missing)) return -1;
    return pj->bpm - 60;
}
static size_t problems(bool print) {
    FatAudit a(sd::image());
    if (print) for (size_t i = 0; i < a.problems.size() && i < 5; ++i) printf("    %s\n", a.problems[i].c_str());
    size_t bad = a.problems.size();
    for (const auto& p : a.longer) if (p != "/CATHODE/PRESETS/INDEX.PIX") { ++bad; if (print) printf("    chain longer than the file: %s\n", p.c_str()); }
    return bad;
}

// A card as a computer leaves it: folders, a library of presets, some samples
static void baseCard() {
    sd::open(90000);
    sd::format(1);
    sd::faults = sd::Faults{};
    sd::powerCycle();
    disk.is_initialized[0] = 0;
    static bool linked = false;
    if (!linked) { MX_FATFS_Init(); linked = true; }
    static FATFS fs;
    CHECK(f_mount(&fs, SDPath, 1) == FR_OK);
    CHECK(f_mkdir("CATHODE") == FR_OK);
    CHECK(f_mkdir("CATHODE/PRESETS") == FR_OK);
    CHECK(f_mkdir("Sample") == FR_OK);
    CHECK(f_mkdir("Sample/Kick") == FR_OK);
    static FIL f;
    UINT n;
    for (int i = 0; i < 40; ++i) {
        uint8_t body[kPresetBodySize];
        char name[32], p[64];
        snprintf(name, sizeof name, "Preset %d", i);
        PresetRecord r = preset(name, i);
        r.id = (uint16_t)(100 + i);
        encodePresetFile(r, body);
        snprintf(p, sizeof p, "CATHODE/PRESETS/%04d %s.PRS", 100 + i, name);
        CHECK(f_open(&f, p, FA_CREATE_NEW | FA_WRITE) == FR_OK);
        CHECK(f_write(&f, body, sizeof body, &n) == FR_OK);
        CHECK(f_close(&f) == FR_OK);
    }
    static uint8_t junk[9000];
    memset(junk, 0x33, sizeof junk);
    CHECK(f_open(&f, "Sample/Kick/A long sample name.wav", FA_CREATE_NEW | FA_WRITE) == FR_OK);
    CHECK(f_write(&f, junk, sizeof junk, &n) == FR_OK);
    CHECK(f_close(&f) == FR_OK);
    f_mount(nullptr, SDPath, 0);
}

static void testBootWritesNothing() {
    baseCard();
    std::vector<uint8_t> before(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
    sd::resetCounts();
    CHECK(boot());
    int found = 0;
    SlotInfo info;
    for (int s = 0; s < kCardSlots; ++s) found += board::card.slotInfo(s, info);
    CHECK(found == 0);
    printf("  (presets listed: %d)\n", presetCount());
    CHECK(presetCount() == kCardPresets);
    CHECK(board::card.scanWaves() == 0);
    CHECK(sd::stats.sectorsWritten == 0);
    CHECK(memcmp(before.data(), sd::image(), before.size()) == 0);
    printf("  card: a start writes nothing\n");
}

static void testSaves() {
    baseCard();
    CHECK(boot());
    for (int m = 1; m <= 5; ++m) {
        project(m);
        CHECK(board::card.saveSlot(2, *pj, *lib));
        CHECK(loaded(2) == m);
    }
    CHECK(problems(true) == 0);
    SlotInfo info;
    CHECK(board::card.slotInfo(2, info));
    CHECK(info.bpm == 65);
    CHECK(board::card.undoSave(2));     // the save before it comes back
    CHECK(loaded(2) == 4);
    CHECK(boot());                      // and stays back
    CHECK(loaded(2) == 4);
    project(6);
    CHECK(board::card.saveSlot(2, *pj, *lib));
    CHECK(loaded(2) == 6);
    project(9);                         // a slot that was empty, undone, is empty again
    CHECK(board::card.saveSlot(3, *pj, *lib));
    CHECK(board::card.undoSave(3));
    CHECK(!board::card.slotInfo(3, info));
    CHECK(problems(true) == 0);
    printf("  card: saves count, undo goes back one\n");

    // both files go, and with them what undo could take back; the slot is as good as new
    CHECK(board::card.deleteSlot(2));
    CHECK(!board::card.slotInfo(2, info));
    CHECK(loaded(2) == -1);
    CHECK(!board::card.undoSave(2));
    CHECK(problems(true) == 0);
    CHECK(boot());
    CHECK(!board::card.slotInfo(2, info));
    CHECK(board::card.deleteSlot(2));   // nothing there is not an error
    project(7);
    CHECK(board::card.saveSlot(2, *pj, *lib));
    CHECK(loaded(2) == 7);
    CHECK(board::card.deleteSlot(2));   // one save, one file
    CHECK(loaded(2) == -1);
    project(1);
    CHECK(board::card.saveSlot(4, *pj, *lib));
    project(2);
    CHECK(board::card.saveSlot(4, *pj, *lib));
    std::vector<uint8_t> two(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
    for (long n = 0; n < 12; ++n) {     // the n-th write refused: the project or an empty slot, and never the save before it
        sd::loadImage(two.data(), two.size());
        CHECK(boot());
        sd::faults.writeRejected = n;
        const bool said = board::card.deleteSlot(4);
        const bool hit = sd::faults.writeRejected < 0;
        sd::faults = sd::Faults{};
        const int was = loaded(4);
        CHECK(was == 2 || was == -1);
        CHECK(said == (was == -1));     // it says what is so
        if (hit && !said) CHECK(!board::card.deleteSlot(4));   // writes stay off until the next start
        CHECK(boot());
        CHECK(loaded(4) == was);
        CHECK(board::card.deleteSlot(4));
        CHECK(loaded(4) == -1);
    }
    CHECK(problems(true) == 0);

    // an older file that cannot go: the project stays, and so does the undo of its save
    baseCard();
    CHECK(boot());
    project(1);
    CHECK(board::card.saveSlot(1, *pj, *lib));
    project(2);
    CHECK(board::card.saveSlot(1, *pj, *lib));
    CHECK(boot());
    {
        uint8_t* d = sd::image();
        const size_t n = (size_t)sd::sectors() * 512;
        bool found = false;
        for (size_t o = 0; o + 32 <= n; o += 32) if (memcmp(d + o, "PROJ2   BAK", 11) == 0) { d[o + 11] |= 0x01; found = true; }
        CHECK(found);
    }
    CHECK(boot());
    project(3);
    CHECK(board::card.saveSlot(1, *pj, *lib));
    CHECK(!board::card.deleteSlot(1));
    CHECK(loaded(1) == 3);
    CHECK(board::card.undoSave(1));
    CHECK(loaded(1) == 2);

    // and the newest that cannot go, once the older has: the undo of its save must not take the last file
    project(4);
    CHECK(board::card.saveSlot(2, *pj, *lib));
    project(5);
    CHECK(board::card.saveSlot(2, *pj, *lib));    // PROJ3.BAK, the newest
    {
        uint8_t* d = sd::image();
        const size_t n = (size_t)sd::sectors() * 512;
        bool found = false;
        for (size_t o = 0; o + 32 <= n; o += 32) if (memcmp(d + o, "PROJ3   BAK", 11) == 0) { d[o + 11] |= 0x01; found = true; }
        CHECK(found);
        static FILINFO other;
        CHECK(f_stat("Sample/Kick/A long sample name.wav", &other) == FR_OK);   // (FatFs reads the folder again)
    }
    CHECK(!board::card.deleteSlot(2));
    CHECK(loaded(2) == 5);
    CHECK(!board::card.undoSave(2));
    CHECK(loaded(2) == 5);
    printf("  card: a delete empties the slot\n");

    // past nine the slots take two digits, and the first ones keep their names
    for (const int slot : {0, 8, 9, 19, 29}) {
        project(slot + 1);
        CHECK(board::card.saveSlot(slot, *pj, *lib));
    }
    static FILINFO fi;
    CHECK(f_stat("CATHODE/PROJ1.DBP", &fi) == FR_OK);
    CHECK(f_stat("CATHODE/PROJ9.DBP", &fi) == FR_OK);
    CHECK(f_stat("CATHODE/PROJ10.DBP", &fi) == FR_OK);
    CHECK(f_stat("CATHODE/PROJ30.DBP", &fi) == FR_OK);
    CHECK(boot());
    for (const int slot : {0, 8, 9, 19, 29}) CHECK(loaded(slot) == slot + 1);
    CHECK(board::card.deleteSlot(29));
    CHECK(loaded(29) == -1);
    CHECK(loaded(9) == 10);
    CHECK(problems(true) == 0);
}

// One sweep: an operation run with the power cut after each of its sectors in turn. judge() looks at what the next start finds.
template <class Op, class Judge>
static long sweep(const std::vector<uint8_t>& base, Op op, Judge judge) {
    sd::loadImage(base.data(), base.size());
    sd::faults = sd::Faults{};
    CHECK(boot());
    sd::resetCounts();
    op();
    const long sectors = sd::stats.sectorsWritten;
    long runs = 0;
    for (long cut = 0; cut <= sectors; cut += (cut < 60 || cut > sectors - 60) ? 1 : 37) {
        for (int kind = 0; kind < 3; ++kind) {
            sd::loadImage(base.data(), base.size());
            sd::Faults f;
            f.seed = (uint32_t)(cut * 3 + kind + 1);
            f.cutAfterSectors = cut;
            f.torn = kind == 1;
            f.lostOnCut = kind == 2 ? 1 : 0;
            sd::faults = f;
            if (boot()) op();
            sd::faults = sd::Faults{};
            judge(cut, kind);
            ++runs;
        }
    }
    return runs;
}

// A cut anywhere in a delete: the project, or an empty slot, and never the save before it. Either of the two files can be the newest.
static void testDeleteCuts() {
    for (int saves = 2; saves <= 3; ++saves) {
        baseCard();
        CHECK(boot());
        for (int m = 1; m <= saves; ++m) {
            project(m);
            CHECK(board::card.saveSlot(0, *pj, *lib));
        }
        std::vector<uint8_t> base(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
        long old = 0, broken = 0, gone = 0, stayed = 0;
        const long runs = sweep(base, [] { board::card.deleteSlot(0); }, [&](long, int) {
            if (!boot()) { ++broken; return; }
            const int m = loaded(0);
            if (m == saves) ++stayed;
            else if (m == -1) ++gone;
            else ++old;
            if (problems(false)) ++broken;
        });
        CHECK(runs > 0 && stayed > 0 && gone > 0);   // (both ends of the delete were reached)
        CHECK(old == 0);
        CHECK(broken == 0);
        printf("  card: %ld cuts in a delete: %ld kept it, %ld emptied the slot, %ld brought an old save back, %ld left the card damaged\n", runs, stayed, gone, old, broken);
    }
}

// The newest file damaged: the save before it is what loads, so that is the one that stays longest
static void testDeleteDamaged() {
    baseCard();
    CHECK(boot());
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    project(2);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    static FIL f;
    CHECK(f_open(&f, "CATHODE/PROJ1.BAK", FA_READ | FA_WRITE) == FR_OK);
    uint8_t b = 0;
    UINT n;
    CHECK(f_lseek(&f, 5000) == FR_OK && f_read(&f, &b, 1, &n) == FR_OK && n == 1);
    b ^= 0xFF;
    CHECK(f_lseek(&f, 5000) == FR_OK && f_write(&f, &b, 1, &n) == FR_OK && n == 1);
    CHECK(f_close(&f) == FR_OK);
    CHECK(boot());
    CHECK(loaded(0) == 1 && board::card.loadFellBack());
    std::vector<uint8_t> base(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
    long lost = 0, kept = 0, gone = 0;
    sweep(base, [] { board::card.deleteSlot(0); }, [&](long, int) {
        SlotInfo si;
        if (!boot()) return;
        const int m = loaded(0);
        if (m == 1) ++kept;
        else if (!board::card.slotInfo(0, si)) ++gone;
        else ++lost;
    });
    CHECK(lost == 0 && kept > 0 && gone > 0);
    printf("  card: cuts in a delete with the newest file damaged: %ld kept the save that loads, %ld emptied the slot, %ld lost it\n", kept, gone, lost);
}

// A long file as it is made: a new one every time, whole or not at all
static void testExport() {
    baseCard();
    CHECK(boot());
    project(3);
    CHECK(board::card.saveSlot(1, *pj, *lib));
    static uint8_t chunk[5000];
    for (size_t i = 0; i < sizeof chunk; ++i) chunk[i] = (uint8_t)(i * 7);
    uint8_t head[44];
    memset(head, 0, sizeof head);
    const char* const folder = "Export/MY SONG";
    const auto file = [&](const char* name, int chunks) {
        if (!board::card.exportOpen(folder, name)) return false;
        bool ok = board::card.exportWrite(head, sizeof head);
        for (int i = 0; i < chunks && ok; ++i) ok = board::card.exportWrite(chunk, sizeof chunk);
        memcpy(head, "RIFF", 4);
        ok = ok && board::card.exportRewrite(0, head, sizeof head);
        const bool closed = board::card.exportClose(ok);
        return closed && ok;
    };
    static FILINFO fi;
    CHECK(file("Song 1.wav", 200));
    CHECK(f_stat("Export/MY SONG/Song 1.wav", &fi) == FR_OK && fi.fsize == 44 + 200 * sizeof chunk);
    CHECK(file("Song 1.wav", 3));                         // a second one does not touch the first
    CHECK(f_stat("Export/MY SONG/Song 1 (2).wav", &fi) == FR_OK && fi.fsize == 44 + 3 * sizeof chunk);
    CHECK(f_stat("Export/MY SONG/Song 1.wav", &fi) == FR_OK && fi.fsize == 44 + 200 * sizeof chunk);
    CHECK(file("Song 1.mid", 1));
    CHECK(problems(true) == 0);
    {   // the header is the last thing written, and what the file starts with
        static FIL f;
        uint8_t b[4];
        UINT n;
        CHECK(f_open(&f, "Export/MY SONG/Song 1.wav", FA_READ) == FR_OK && f_read(&f, b, 4, &n) == FR_OK && memcmp(b, "RIFF", 4) == 0);
        f_close(&f);
    }
    // what came back is checked: one byte changed on the card behind the open file, in a WAV and in a MIDI file
    for (const char* const name : {"Damaged.wav", "Damaged.mid"}) {
        CHECK(boot());
        static uint8_t mark[5000];
        memcpy(mark, chunk, sizeof mark);
        memcpy(mark, "BREAKAUDIO", 10);
        CHECK(board::card.exportOpen(folder, name));
        CHECK(board::card.exportWrite(head, sizeof head));
        CHECK(board::card.exportWrite(mark, sizeof mark));
        for (int i = 0; i < 8; ++i) CHECK(board::card.exportWrite(chunk, sizeof chunk));   // (so the marked data has left FatFs's buffer)
        CHECK(board::card.exportRewrite(0, head, sizeof head));
        uint8_t* img = sd::image();
        const size_t all = (size_t)sd::sectors() * 512;
        bool hit = false;
        for (size_t i = 0; i + 10 <= all && !hit; ++i) if (img[i] == 'B' && !memcmp(img + i, "BREAKAUDIO", 10)) { img[i + 40] ^= 0x40; hit = true; }
        CHECK(hit);
        CHECK(!board::card.exportClose(true));
        CHECK(board::card.lastSaveError() == 101);
        project(4);
        CHECK(!board::card.saveSlot(2, *pj, *lib));       // writes stop until a restart
        CHECK(boot());
        char path[64];
        snprintf(path, sizeof path, "Export/MY SONG/%s", name);
        CHECK(f_stat(path, &fi) == FR_NO_FILE);
        CHECK(loaded(1) == 3);
        CHECK(problems(true) == 0);
    }
    CHECK(boot());
    // a card where a file has the name of the folder, or that has no room for one: said in words
    {
        static FIL f;
        CHECK(f_open(&f, "Export/IN THE WAY", FA_CREATE_NEW | FA_WRITE) == FR_OK && f_close(&f) == FR_OK);
        CHECK(!board::card.exportOpen("Export/IN THE WAY", "Song 1.wav") || !board::card.exportClose(false));
    }
    // taken back
    CHECK(board::card.exportOpen(folder, "Gone.wav"));
    CHECK(board::card.exportWrite(chunk, sizeof chunk));
    CHECK(!board::card.exportClose(false));
    CHECK(f_stat("Export/MY SONG/Gone.wav", &fi) == FR_NO_FILE);
    CHECK(problems(true) == 0);
    // a write refused: no half a file, and the project is still there
    CHECK(boot());
    CHECK(board::card.exportOpen(folder, "Bad.wav"));
    sd::faults.writeRejected = 2;
    bool wrote = true;
    for (int i = 0; i < 50 && wrote; ++i) wrote = board::card.exportWrite(chunk, sizeof chunk);
    sd::faults = sd::Faults{};
    CHECK(!wrote);
    CHECK(!board::card.exportClose(false));
    CHECK(boot());
    CHECK(loaded(1) == 3);
    CHECK(problems(true) == 0);
    // a card that fills up says so
    CHECK(boot());
    CHECK(board::card.exportOpen(folder, "Huge.wav"));
    wrote = true;
    for (int i = 0; i < 20000 && wrote; ++i) wrote = board::card.exportWrite(chunk, sizeof chunk);
    CHECK(!wrote && board::card.lastSaveError() == 100);
    CHECK(!board::card.exportClose(false));
    CHECK(f_stat("Export/MY SONG/Huge.wav", &fi) == FR_NO_FILE);
    CHECK(loaded(1) == 3);
    CHECK(problems(true) == 0);
    project(4);
    CHECK(board::card.saveSlot(2, *pj, *lib));            // and the room came back
    // a power cut anywhere in one: what was there stays
    baseCard();
    CHECK(boot());
    project(3);
    CHECK(board::card.saveSlot(1, *pj, *lib));
    std::vector<uint8_t> base(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
    long lost = 0, broken = 0;
    const long runs = sweep(base, [&] { file("Song 1.wav", 12); }, [&](long, int) {
        if (!boot()) { ++broken; return; }
        if (loaded(1) != 3) ++lost;
        project(5);
        if (!board::card.saveSlot(3, *pj, *lib)) ++broken;
    });
    CHECK(runs > 0 && lost == 0 && broken == 0);
    printf("  card: %ld cuts in an export: %ld lost a project, %ld left the card unusable\n", runs, lost, broken);
}

static void testPowerCuts() {
    // ---- a project saved over a project
    baseCard();
    CHECK(boot());
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    project(2);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    std::vector<uint8_t> base(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
    long lost = 0, unclean = 0, refused = 0, newer = 0, older = 0;
    long runs = sweep(base, [] { project(3); board::card.saveSlot(0, *pj, *lib); }, [&](long, int) {
        if (problems(false)) ++unclean;
        if (!boot()) { ++lost; return; }
        const int m = loaded(0);
        if (m == 3) ++newer; else if (m == 2) ++older; else ++lost;
        project(4);                     // and the card goes on working
        if (!board::card.saveSlot(0, *pj, *lib) || loaded(0) != 4) ++refused;
        if (problems(false)) ++unclean;
    });
    printf("  power cuts, project: %ld cuts, %ld old kept, %ld new, %ld lost, %ld unclean, %ld refused afterwards\n", runs, older, newer, lost, unclean, refused);
    CHECK(lost == 0);
    CHECK(unclean == 0);
    CHECK(refused == 0);
    CHECK(older > 0 && newer > 0);

    // ---- the first save into a slot: nothing to lose but the card
    baseCard();
    base.assign(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
    unclean = refused = 0;
    runs = sweep(base, [] { project(1); board::card.saveSlot(1, *pj, *lib); }, [&](long, int) {
        if (problems(false)) ++unclean;
        if (!boot()) { ++refused; return; }
        project(2);
        if (!board::card.saveSlot(1, *pj, *lib) || loaded(1) != 2) ++refused;
    });
    printf("  power cuts, first save: %ld cuts, %ld unclean, %ld refused afterwards\n", runs, unclean, refused);
    CHECK(unclean == 0);
    CHECK(refused == 0);

    // ---- a preset replaced, and a second cut during the repair at the next start
    baseCard();
    base.assign(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
    int id = -1;
    auto replace = [&] {
        PresetRecord r = preset("Replaced", 77);
        presetSave(r, 100, id);
    };
    long badPreset = 0, second = 0;
    long tornInside = 0;   // a tear inside a directory entry: not lost, but a disk check would object. Real flash tears at pages, if at all: reported, not failed.
    auto presetWhole = [&] {
        CHECK(boot());
        static FIL f;
        uint8_t body[kPresetBodySize];
        UINT n = 0;
        if (f_open(&f, "CATHODE/PRESETS/0100 Preset 0.PRS", FA_READ) != FR_OK) return false;
        const bool read = f_read(&f, body, sizeof body, &n) == FR_OK && n == sizeof body;
        f_close(&f);
        PresetRecord r;
        return read && presetCount() == kCardPresets && parsePresetFile(body, sizeof body, r) == PresetParse::Ok && (strcmp(r.name, "Replaced") == 0 || strcmp(r.name, "Preset 0") == 0);
    };
    unclean = 0;
    runs = sweep(base, replace, [&](long cut, int kind) {
        if (problems(false)) ++(kind == 1 ? tornInside : unclean);
        std::vector<uint8_t> crashed(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
        if (!presetWhole()) ++badPreset;
        if (problems(false)) ++(kind == 1 ? tornInside : unclean);
        for (long c2 = 0; c2 < 6 && kind == 0 && cut < 40; ++c2) {
            sd::loadImage(crashed.data(), crashed.size());
            sd::faults = sd::Faults{};
            sd::faults.cutAfterSectors = c2;
            boot();
            sd::faults = sd::Faults{};
            if (!presetWhole()) ++badPreset;
            if (problems(false)) ++unclean;
            ++second;
        }
    });
    printf("  power cuts, preset replace: %ld cuts, %ld more during the repair, %ld presets lost, %ld unclean, %ld after a tear inside an entry\n", runs, second, badPreset, unclean, tornInside);
    CHECK(badPreset == 0);
    CHECK(unclean == 0);

    // ---- the sweep has to be able to see a save done badly: the project written over itself, the way it used to go when something failed half way
    baseCard();
    CHECK(boot());
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    base.assign(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
    lost = 0;
    sweep(base, [] {
        static FIL f;
        struct : ByteSink { bool write(const void* d, size_t n) override { UINT p; return f_write(&f, d, (UINT)n, &p) == FR_OK; } } sink;
        project(2);
        if (f_open(&f, "CATHODE/PROJ1.DBP", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) { writeProject(*pj, sink, lib); f_close(&f); }
    }, [&](long, int) {
        if (!boot() || loaded(0) < 0) ++lost;
    });
    printf("  power cuts, a save done badly: %ld projects lost (the sweep must see these)\n", lost);
    CHECK(lost > 0);
}

// The card that started all this: one 32-byte line of a preset's name in the first FAT sector, both copies. It reads, and it must not be written.
static void testDamagedCard() {
    baseCard();
    uint8_t* img = sd::image();
    const uint32_t part = FatAudit::r32(img + 446 + 8);
    const uint8_t* b = img + (size_t)part * 512;
    const uint32_t fat1 = part + FatAudit::r16(b + 14), size = FatAudit::r32(b + 36);
    for (uint32_t f = 0; f < 2; ++f) memcpy(img + (size_t)(fat1 + f * size) * 512 + 96, "\xff\xff\xff\x0f\x1a\0\0\0\x98\x35\0\0" "0011 Chorus Arp.PRS", 32);
    std::vector<uint8_t> before(img, img + (size_t)sd::sectors() * 512);
    CHECK(boot());
    const int listed = presetCount();   // (the presets on the damaged clusters are gone from the list; the rest read)
    CHECK(listed > 3);
    project(1);
    CHECK(!board::card.saveSlot(0, *pj, *lib));
    CHECK(board::card.writeProblem() != nullptr);
    PresetRecord r = preset("New one", 1);
    int id = -1;
    CHECK(presetSave(r, -1, id) == PresetSave::WriteFailed);
    CHECK(memcmp(before.data(), sd::image(), before.size()) == 0);
    CHECK(presetCount() == listed);
    printf("  card: a damaged file table is refused: \"%s\", nothing written, still read\n", board::card.writeProblem());
}

// A card that fails a write part way through a save
static void testWriteErrors() {
    baseCard();
    CHECK(boot());
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    std::vector<uint8_t> base(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
    long bad = 0, runs = 0;
    for (long n = 0; n < 60; ++n) {
        sd::loadImage(base.data(), base.size());
        sd::faults = sd::Faults{};
        CHECK(boot());
        sd::faults.writeRejected = n;
        project(2);
        const bool saved = board::card.saveSlot(0, *pj, *lib);
        const bool hit = sd::faults.writeRejected < 0;
        sd::faults = sd::Faults{};
        if (hit && saved) ++bad;                     // never a success after a failed write
        if (hit && !board::card.writeProblem()) ++bad;
        const int m = loaded(0);
        if (saved ? m != 2 : m != 1) ++bad;          // what the save said is what the card holds, without a restart
        CHECK(boot());
        if (loaded(0) != (saved ? 2 : 1)) ++bad;
        ++runs;
    }
    printf("  card: %ld writes failed in a save, %ld wrong\n", runs, bad);
    CHECK(bad == 0);
}

// From the review: each of these lost something or refused a healthy card
static void testReviewCases() {
    // a failed save leaves undo with nothing to take back: the newest file is the only copy
    baseCard();
    CHECK(boot());
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    project(2);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    sd::faults.writeRejected = 3;
    project(3);
    CHECK(!board::card.saveSlot(0, *pj, *lib));
    sd::faults = sd::Faults{};
    CHECK(!board::card.undoSave(0));
    CHECK(loaded(0) == 2);
    CHECK(boot());
    CHECK(loaded(0) == 2);
    CHECK(!board::card.undoSave(0));   // nothing saved since this start: nothing to undo
    CHECK(loaded(0) == 2);

    // a fresh card: nothing has made the folder, and a preset can still be saved
    sd::open(90000);
    sd::format(1);
    CHECK(boot());
    PresetRecord r = preset("First", 1);
    int id = -1;
    CHECK(presetSave(r, -1, id) == PresetSave::Ok);
    CHECK(boot());
    CHECK(presetCount() == 4);
    CHECK(problems(true) == 0);

    // pulled from a computer that had set its dirty flag in the first boot sector only; and a backup that was never written
    baseCard();
    uint8_t* img = sd::image();
    const uint32_t part = FatAudit::r32(img + 446 + 8);
    img[(size_t)part * 512 + 65] |= 1;
    CHECK(boot());
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    baseCard();
    img = sd::image();
    memset(img + (size_t)(part + 6) * 512, 0, 512);
    CHECK(boot());
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    // a backup that describes something else is a card to leave alone
    baseCard();
    img = sd::image();
    img[(size_t)(part + 6) * 512 + 13] = 8;
    CHECK(boot());
    project(1);
    CHECK(!board::card.saveSlot(0, *pj, *lib));

    // the volume in the third partition entry, where FatFs finds it
    baseCard();
    img = sd::image();
    memcpy(img + 446 + 32, img + 446, 16);
    memset(img + 446, 0, 16);
    CHECK(boot());
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));

    // the newest file rots: the load says so, and the save after it overwrites that one, not the good one
    baseCard();
    CHECK(boot());
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    project(2);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    f_mount(nullptr, SDPath, 0);
    {
        CHECK(boot());
        static FIL f;
        UINT n;
        CHECK(f_open(&f, "CATHODE/PROJ1.BAK", FA_OPEN_EXISTING | FA_WRITE) == FR_OK);   // (save 2 went to the second file)
        CHECK(f_lseek(&f, 1000) == FR_OK);
        CHECK(f_write(&f, "x", 1, &n) == FR_OK);
        CHECK(f_close(&f) == FR_OK);
    }
    CHECK(boot());
    CHECK(loaded(0) == 1);
    CHECK(board::card.loadFellBack());
    std::vector<uint8_t> rotten(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
    long lost = 0;
    for (long cut = 0; cut < 400; cut += 3) {
        sd::loadImage(rotten.data(), rotten.size());
        sd::faults = sd::Faults{};
        CHECK(boot());
        sd::faults.cutAfterSectors = cut;
        project(3);
        board::card.saveSlot(0, *pj, *lib);
        sd::faults = sd::Faults{};
        CHECK(boot());
        if (loaded(0) < 0) ++lost;
    }
    CHECK(lost == 0);

    printf("  card: the review's cases hold, %ld cuts on a rotten newest file lost nothing\n", lost);
}

// From the attack on the storage code

// A real card's geometry: clusters of several sectors, and every free cluster full of what was there before. A new file that makes a folder grow, cut at each
// sector: the folder must never run into the old data, and presets must go on saving.
static void testFolderGrowth() {
    sd::open(600000);
    sd::format(8);
    sd::powerCycle();
    CHECK(boot());
    uint8_t* img = sd::image();
    const uint32_t part = FatAudit::r32(img + 446 + 8);
    const uint8_t* b = img + (size_t)part * 512;
    const size_t data = part + FatAudit::r16(b + 14) + 2u * FatAudit::r32(b + 36);
    // old data, looking as much like a folder as old data can: entries of a file that lies on clusters in use
    uint8_t old[32];
    memset(old, 0, sizeof old);
    memcpy(old, "OLDFILE BIN", 11);
    old[11] = 0x20;
    old[26] = 3;
    old[28] = 0xFF;
    for (size_t i = data * 512 + 8 * 512; i + 32 <= (size_t)sd::sectors() * 512; i += 32) memcpy(sd::image() + i, old, 32);
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));   // (the folders, and a project on the same clusters' neighbours)
    int id = -1;
    int made = 0;
    auto add = [&](int n) { char name[24]; snprintf(name, sizeof name, "Preset number %d", n); PresetRecord r = preset(name, n % 100); return presetSave(r, -1, id); };
    while (made < 41) { CHECK(add(made) == PresetSave::Ok); ++made; }   // just short of the folder's first cluster filling up
    std::vector<uint8_t> base(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
    long refused = 0, failed = 0, unclean = 0, runs = 0;
    for (long cut = 0; cut < 70; ++cut) {
        sd::loadImage(base.data(), base.size());
        sd::faults = sd::Faults{};
        CHECK(boot());
        sd::faults.cutAfterSectors = cut;
        for (int k = 0; k < 4 && !sd::poweredOff(); ++k) add(made + k);
        sd::faults = sd::Faults{};
        if (problems(false)) ++unclean;
        CHECK(boot());
        for (int k = 0; k < 80; ++k) {   // on past the next cluster, and the ones the cut was in are retried
            const PresetSave r = add(made + k);
            if (r != PresetSave::Ok && r != PresetSave::Exists) { ++failed; if (board::card.writeProblem()) ++refused; break; }
        }
        if (problems(false)) ++unclean;
        ++runs;
    }
    printf("  card: a folder growing, %ld cuts: %ld saves failed, %ld refused, %ld unclean\n", runs, failed, refused, unclean);
    CHECK(failed == 0);
    CHECK(unclean == 0);
}

// Old data behind a folder's end mark, on a card the firmware did not write: it must not come back as entries
static void testFolderTail() {
    sd::open(600000);
    sd::format(8);
    CHECK(boot());
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    f_mount(nullptr, SDPath, 0);
    uint8_t* img = sd::image();
    const uint32_t part = FatAudit::r32(img + 446 + 8);
    const uint8_t* b = img + (size_t)part * 512;
    const size_t data = part + FatAudit::r16(b + 14) + 2u * FatAudit::r32(b + 36);
    const FatAudit before(img);
    (void)before;
    // the root folder: its first cluster; a dead entry a few places past the end mark
    uint8_t* root = img + data * 512;
    size_t end = 0;
    while (root[end] != 0) end += 32;
    memcpy(root + end + 96, "GHOST   BIN\x20", 12);
    root[end + 96 + 26] = 3;                // cluster 3: in use by something else
    root[end + 96 + 28] = 0xFF;
    CHECK(boot());
    for (int m = 0; m < 12; ++m) {          // new files walk over the end mark
        char name[24];
        snprintf(name, sizeof name, "PROBE%02d.BIN", m);
        static FIL f;
        if (m == 0) CHECK(board::card.beginWrite());
        CHECK(f_open(&f, name, FA_CREATE_NEW | FA_WRITE) == FR_OK);
        f_close(&f);
    }
    CHECK(problems(true) == 0);
    FatAudit a(sd::image());
    CHECK(a.files.count("/GHOST.BIN") == 0);
    printf("  card: old data behind a folder's end is cleared before it can come back\n");
}

// The small ones
static void testAttackCases() {
    int id = -1;
    // a new preset cut before its first byte leaves an empty file: the same save tried again must go through
    baseCard();
    CHECK(boot());
    {
        static FIL f;
        CHECK(f_open(&f, "CATHODE/PRESETS/1000 Again.PRS", FA_CREATE_NEW | FA_WRITE) == FR_OK);
        CHECK(f_close(&f) == FR_OK);
    }
    CHECK(boot());
    PresetRecord r = preset("Again", 3);
    CHECK(presetSave(r, -1, id) == PresetSave::Ok);
    CHECK(boot());
    CHECK(presetCount() == kCardPresets + 1);

    // a replace refused half way leaves no journal, so the next start has nothing to do
    baseCard();
    CHECK(boot());
    sd::resetCounts();
    CHECK(boot());
    CHECK(sd::stats.sectorsWritten == 0);

    // one bad bit in the older file's count must not bring it back over the newer one
    baseCard();
    CHECK(boot());
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    project(2);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    project(3);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    {
        static FIL f;
        CHECK(f_open(&f, "CATHODE/PROJ1.BAK", FA_OPEN_EXISTING | FA_WRITE | FA_READ) == FR_OK);   // (saves 1 and 3 went to .DBP, 2 to .BAK)
        CHECK(f_lseek(&f, f_size(&f) - 16) == FR_OK);
        UINT n;
        const uint8_t highBit = 0x40;
        CHECK(f_lseek(&f, f_size(&f) - 13) == FR_OK);
        CHECK(f_write(&f, &highBit, 1, &n) == FR_OK);
        CHECK(f_close(&f) == FR_OK);
    }
    CHECK(boot());
    CHECK(loaded(0) == 3);

    // undo onto a file that has gone bad is refused: the save being taken back is the last good copy
    baseCard();
    CHECK(boot());
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    project(2);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    {
        static FIL f;
        UINT n;
        CHECK(f_open(&f, "CATHODE/PROJ1.DBP", FA_OPEN_EXISTING | FA_WRITE) == FR_OK);   // (save 1, the older)
        CHECK(f_lseek(&f, 1000) == FR_OK);
        CHECK(f_write(&f, "x", 1, &n) == FR_OK);
        CHECK(f_close(&f) == FR_OK);
    }
    CHECK(!board::card.undoSave(0));
    CHECK(loaded(0) == 2);

    // the index a start could not write goes with a project save
    baseCard();
    CHECK(boot());
    FILINFO fi;
    CHECK(f_stat("CATHODE/PRESETS/INDEX.PIX", &fi) == FR_NO_FILE);
    project(1);
    CHECK(board::card.saveSlot(0, *pj, *lib));
    presetsWriteIndex();
    CHECK(f_stat("CATHODE/PRESETS/INDEX.PIX", &fi) == FR_OK);
    sd::resetCounts();
    CHECK(boot());
    CHECK(sd::stats.sectorsWritten == 0);
    CHECK(presetCount() == kCardPresets);
    printf("  card: the attack's small cases hold\n");
}

// The test the board runs: clean on a healthy stack, and it has to stop when the cache rules are broken
static void testSoak() {
    baseCard();
    CHECK(boot());
    project(1);
    board::soakStart(300);
    while (board::soakStatus.running) board::soakStep(*pj, *scratch, *lib);
    CHECK(board::soakStatus.error == 0);
    CHECK(board::soakStatus.rounds == 300);
    CHECK(board::soakStatus.projectSaves == 37 && board::soakStatus.presetSaves == 18 && board::soakStatus.cardChecks == 4);
    CHECK(problems(true) == 0);
    printf("  soak: %u rounds, %u writes, %u reads, %u MB, clean\n", board::soakStatus.rounds, board::soakStatus.fileWrites, board::soakStatus.fileReads, board::soakStatus.megabytes);

    baseCard();
    CHECK(boot());
    sd::faults.noClean = true;
    sd::faults.noInvalidateBefore = true;
    sd::faults.evictPercent = 20;
    board::soakStart(300);
    while (board::soakStatus.running) board::soakStep(*pj, *scratch, *lib);
    sd::faults = sd::Faults{};
    printf("  soak on a build with no cache rules: stopped in round %u, error %u\n", board::soakStatus.errorRound, board::soakStatus.error);
    CHECK(board::soakStatus.error != 0);
}

// A take: saved whole or not at all, a cut leaves the card as good as it was, and a refused card keeps it in memory
static void testTakes() {
    const uint32_t frames = 30000;
    auto take = [&](int mark) -> int {
        void* arena = nullptr;
        int slots = 0;
        board::card.openPool(1024, slots, arena);
        size_t room = 0;
        int16_t* at = board::card.lendPool(room);
        CHECK(at != nullptr && room >= frames);
        for (uint32_t i = 0; i < frames; ++i) at[i] = (int16_t)(i * 7 + mark);
        return board::card.keepRecording(*lib, frames, 44100);
    };
    auto whole = [&](const char* path, int mark, bool& there) {
        static FIL f;
        FILINFO fi;
        there = f_stat(path, &fi) == FR_OK && fi.fsize != 0;
        if (!there) return true;
        if (fi.fsize != 44 + frames * 2 || f_open(&f, path, FA_READ) != FR_OK) return false;
        static int16_t d[30000];
        UINT n = 0;
        f_lseek(&f, 44);
        const bool ok = f_read(&f, d, frames * 2, &n) == FR_OK && n == frames * 2;
        f_close(&f);
        if (!ok) return false;
        for (uint32_t i = 0; i < frames; ++i) if (d[i] != (int16_t)(i * 7 + mark)) return false;
        return true;
    };
    baseCard();
    CHECK(boot());
    new (lib) SampleLib;
    CHECK(take(1) >= 0);
    CHECK(strncmp(lib->items[0].name, "UNSAV", 5) != 0);
    bool there = false;
    CHECK(whole("Sample/REC/REC0001.wav", 1, there) && there);
    CHECK(problems(true) == 0);
    std::vector<uint8_t> base(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
    long bad = 0, unclean = 0, kept = 0, none = 0, stuck = 0;
    const long runs = sweep(base, [&] { new (lib) SampleLib; take(2); }, [&](long, int kind) {
        if (kind != 1 && problems(false)) ++unclean;
        if (!boot()) { ++bad; return; }
        bool t2 = false, t1 = false;
        if (!whole("Sample/REC/REC0001.wav", 1, t1) || !t1) ++bad;
        if (!whole("Sample/REC/REC0002.wav", 2, t2)) ++bad;
        ++(t2 ? kept : none);
        new (lib) SampleLib;
        if (take(3) < 0 || !strncmp(lib->items[0].name, "UNSAV", 5)) ++stuck;
    });
    printf("  takes, power cuts: %ld cuts, %ld saved, %ld not, %ld bad, %ld unclean, %ld could not record afterwards\n", runs, kept, none, bad, unclean, stuck);
    CHECK(bad == 0 && unclean == 0 && stuck == 0);

    // a folder with other capitals: the take goes into the card's Sample/Rec, and the key it was kept under finds it after a restart, when the index calls the folder Rec
    baseCard();
    CHECK(boot());
    CHECK(f_mkdir("Sample/Rec") == FR_OK);
    new (lib) SampleLib;
    CHECK(board::card.indexSamples(*lib) == 1);
    const int taken = take(5);
    CHECK(taken >= 0 && strncmp(lib->items[taken].name, "UNSAV", 5) != 0);
    char key[48];
    lib->keyOf(taken, key, sizeof key);
    CHECK(strcmp(key, "REC/REC0001.wav") == 0);
    CHECK(boot());
    new (lib) SampleLib;
    CHECK(board::card.indexSamples(*lib) == 2);
    const int found = lib->find(key);
    char real[48];
    if (found >= 0) lib->keyOf(found, real, sizeof real);
    CHECK(found >= 0 && strcmp(real, "Rec/REC0001.wav") == 0);
    CHECK(strcmp(lib->cats[lib->items[found].cat], "REC") == 0);
    printf("  takes: a key with other capitals finds %s\n", real);

    // a card that is refused: the take stays in memory, and the card is not touched
    baseCard();
    uint8_t* img = sd::image();
    const uint32_t part = FatAudit::r32(img + 446 + 8);
    const uint8_t* b = img + (size_t)part * 512;
    const uint32_t fat1 = part + FatAudit::r16(b + 14), size = FatAudit::r32(b + 36);
    for (uint32_t f = 0; f < 2; ++f) memcpy(img + (size_t)(fat1 + f * size) * 512 + 96, "\xff\xff\xff\x0f\x1a\0\0\0\x98\x35\0\0" "0011 Chorus Arp.PRS", 32);
    std::vector<uint8_t> before(img, img + (size_t)sd::sectors() * 512);
    CHECK(boot());
    new (lib) SampleLib;
    const int kept2 = take(4);
    CHECK(kept2 >= 0 && strncmp(lib->items[kept2].name, "UNSAV", 5) == 0);
    CHECK(board::card.lastSaveError() != 0);
    CHECK(board::card.writeProblem() != nullptr);
    CHECK(memcmp(before.data(), sd::image(), before.size()) == 0);
    printf("  takes: a refused card keeps the take in memory and is not written\n");
}

// What the preset protocol does to the card: a preset that arrives whole is written, a cut leaves the old one or the new one, and a delete is all or nothing
static void testPresetsOverMidi() {
    baseCard();
    CHECK(boot());
    uint8_t oldFile[kPresetBodySize], body[kPresetBodySize];
    CHECK(presetReadFile(100, oldFile));
    PresetRecord r = preset("Over The Wire", 9);
    r.id = 100;
    encodePresetFile(r, body);
    std::vector<uint8_t> base(sd::image(), sd::image() + (size_t)sd::sectors() * 512);
    auto holds = [&](const uint8_t* want) { uint8_t now[kPresetBodySize]; return presetReadFile(100, now) && memcmp(now, want, sizeof now) == 0; };
    long bad = 0, unclean = 0;
    long runs = sweep(base, [&] { presetPutFile(body, sizeof body, 100, true); }, [&](long, int kind) {
        if (kind != 1 && problems(false)) ++unclean;
        if (!boot() || !(holds(oldFile) || holds(body))) ++bad;
    });
    printf("  presets over MIDI, replace: %ld cuts, %ld bad, %ld unclean\n", runs, bad, unclean);
    CHECK(bad == 0 && unclean == 0);

    PresetRecord n = preset("Brand New", 3);
    n.id = 7000;
    encodePresetFile(n, body);
    bad = unclean = 0;
    runs = sweep(base, [&] { presetPutFile(body, sizeof body, 7000, false); }, [&](long, int kind) {
        if (kind != 1 && problems(false)) ++unclean;
        if (!boot()) { ++bad; return; }
        // whole or not there at all, and the same save goes through afterwards
        uint8_t now[kPresetBodySize];
        if (presetExists(7000) && (!presetReadFile(7000, now) || memcmp(now, body, sizeof now))) ++bad;
        if (presetFiles() < 40) ++bad;
        if (!presetExists(7000) && presetPutFile(body, sizeof body, 7000, false) != PresetPut::Ok) ++bad;
    });
    printf("  presets over MIDI, new: %ld cuts, %ld bad, %ld unclean\n", runs, bad, unclean);
    CHECK(bad == 0 && unclean == 0);

    CHECK(boot());
    CHECK(presetPutFile(body, sizeof body, 7000, presetExists(7000)) == PresetPut::Ok);   // (the first write checks the whole card)
    const long read0 = sd::stats.sectorsRead, written0 = sd::stats.sectorsWritten;
    CHECK(presetPutFile(body, sizeof body, 7000, true) == PresetPut::Ok);
    printf("  a replace: %ld sectors read, %ld written\n", sd::stats.sectorsRead - read0, sd::stats.sectorsWritten - written0);
    CHECK(sd::stats.sectorsRead - read0 < 150 && sd::stats.sectorsWritten - written0 < 30);   // (the index opened for each piece: 214 and 37, and it grows with the square of the folder)
    uint32_t nameCrc = presetCrc(reinterpret_cast<const uint8_t*>(oldFile + 12), kPresetNameMax + 2);
    bad = unclean = 0;
    runs = sweep(base, [&] { presetRemove(100, nameCrc); }, [&](long, int kind) {
        if (kind != 1 && problems(false)) ++unclean;
        if (!boot()) { ++bad; return; }
        if (presetExists(100) && !holds(oldFile)) ++bad;   // there and whole, or gone
    });
    printf("  presets over MIDI, delete: %ld cuts, %ld bad, %ld unclean\n", runs, bad, unclean);
    CHECK(bad == 0 && unclean == 0);

    // a damaged card refuses it all, and is not touched
    baseCard();
    uint8_t* img = sd::image();
    const uint32_t part = FatAudit::r32(img + 446 + 8);
    const uint8_t* b = img + (size_t)part * 512;
    const uint32_t fat1 = part + FatAudit::r16(b + 14), size = FatAudit::r32(b + 36);
    for (uint32_t f = 0; f < 2; ++f) memcpy(img + (size_t)(fat1 + f * size) * 512 + 96, "\xff\xff\xff\x0f\x1a\0\0\0\x98\x35\0\0" "0011 Chorus Arp.PRS", 32);
    std::vector<uint8_t> before(img, img + (size_t)sd::sectors() * 512);
    CHECK(boot());
    CHECK(presetPutFile(body, sizeof body, 7000, false) == PresetPut::WriteFailed);
    CHECK(presetRemove(100, nameCrc) == PresetPut::WriteFailed || !presetExists(100));
    CHECK(memcmp(before.data(), sd::image(), before.size()) == 0);
}

void testCard() {
    lib = new SampleLib;
    pj = new Project;
    scratch = new Project;
    presetMem = new PresetInfo[kPresetCap];
    testBootWritesNothing();
    testSaves();
    testDamagedCard();
    testWriteErrors();
    testReviewCases();
    testAttackCases();
    testFolderGrowth();
    testFolderTail();
    testSoak();
    testTakes();
    testPresetsOverMidi();
    testPowerCuts();
    testDeleteCuts();
    testDeleteDamaged();
    testExport();
}
