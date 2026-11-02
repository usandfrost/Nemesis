#include "board/CardSoak.h"

#include <stdio.h>
#include <string.h>

#include "app/Params.h"
#include "app/PresetFile.h"
#include "board/Card.h"
#include "fatfs.h"

namespace board {

SoakStatus soakStatus;

namespace {
constexpr int kFiles = 64, kPresetSlot = 7;
constexpr uint32_t kSizes[] = {148, 512, 513, 4096, 33000, 70000, 200000};
constexpr uint32_t kChunk = 4096;

struct SoakFile { uint32_t seed, size; };
SoakFile gTable[kFiles];
FIL gSoak;
alignas(32) uint8_t gBuf[kChunk + 64];     // a chunk starts anywhere in the first 32 bytes, so every alignment goes down the driver's paths
uint32_t gRandom = 1, gBytes = 0;

uint32_t rnd(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
void fill(uint8_t* p, uint32_t n, uint32_t& s) { for (uint32_t i = 0; i < n; ++i) p[i] = (uint8_t)rnd(s); }
void fail(SoakError e, uint32_t detail) {
    if (soakStatus.error == 0) { soakStatus.error = (uint32_t)e; soakStatus.errorRound = soakStatus.rounds; soakStatus.errorDetail = detail; }
    soakStatus.running = 0;
}
void soakPath(char* out, int slot) { snprintf(out, 32, "CATHODE/SOAK/F%03d.BIN", slot); }

bool writeOne(const char* path, uint32_t seed, uint32_t size) {
    FRESULT r = f_open(&gSoak, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (r != FR_OK) { fail(SoakError::Open, (uint32_t)r); return false; }
    uint32_t s = seed, done = 0;
    while (done < size) {
        const uint32_t room = kChunk - (rnd(gRandom) % 512), n = size - done < room ? size - done : room, at = rnd(gRandom) % 32;
        fill(gBuf + at, n, s);
        UINT put = 0;
        r = f_write(&gSoak, gBuf + at, (UINT)n, &put);
        if (r != FR_OK || put != n) { fail(SoakError::Write, (uint32_t)r); f_close(&gSoak); return false; }
        done += n;
    }
    r = f_close(&gSoak);
    if (r != FR_OK) { fail(SoakError::Close, (uint32_t)r); return false; }
    ++soakStatus.fileWrites;
    gBytes += size;
    return true;
}

bool readOne(const char* path, uint32_t seed, uint32_t size, SoakError bad) {
    FRESULT r = f_open(&gSoak, path, FA_READ);
    if (r != FR_OK || f_size(&gSoak) != size) { fail(SoakError::ReadBack, r != FR_OK ? (uint32_t)r : 1000u); if (r == FR_OK) f_close(&gSoak); return false; }
    uint32_t s = seed, done = 0;
    while (done < size) {
        const uint32_t room = kChunk - (rnd(gRandom) % 512), n = size - done < room ? size - done : room, at = rnd(gRandom) % 32;
        UINT got = 0;
        r = f_read(&gSoak, gBuf + at, (UINT)n, &got);
        if (r != FR_OK || got != n) { fail(SoakError::ReadBack, (uint32_t)r); f_close(&gSoak); return false; }
        for (uint32_t i = 0; i < n; ++i) {
            if (gBuf[at + i] != (uint8_t)rnd(s)) { fail(bad, done + i); f_close(&gSoak); return false; }
        }
        done += n;
    }
    f_close(&gSoak);
    ++soakStatus.fileReads;
    gBytes += size;
    return true;
}
}  // namespace

void soakStart(uint32_t rounds) {
    memset(&soakStatus, 0, sizeof(soakStatus));
    memset(gTable, 0, sizeof(gTable));
    soakStatus.target = rounds;
    soakStatus.running = 1;
    gRandom = 0x9E3779B9u;
    gBytes = 0;
    if (!card.beginWrite()) { fail(SoakError::Refused, 0); return; }
    const FRESULT r = f_mkdir("CATHODE/SOAK");
    if (r != FR_OK && r != FR_EXIST) fail(SoakError::Open, (uint32_t)r);
}

void soakStep(const app::Project& pj, app::Project& scratch, const app::SampleLib& lib) {
    if (!soakStatus.running) return;
    if (soakStatus.rounds >= soakStatus.target) { soakStatus.running = 0; return; }
    const uint32_t round = soakStatus.rounds + 1;
    char path[32];

    const int slot = (int)(rnd(gRandom) % kFiles);
    const uint32_t seed = rnd(gRandom) | 1u, size = kSizes[rnd(gRandom) % (sizeof(kSizes) / sizeof(kSizes[0]))];
    soakPath(path, slot);
    gTable[slot].size = 0;
    if (!writeOne(path, seed, size) || !readOne(path, seed, size, SoakError::Mismatch)) return;
    gTable[slot] = {seed, size};

    const int other = (int)(rnd(gRandom) % kFiles);
    if (gTable[other].size) {
        soakPath(path, other);
        if (!readOne(path, gTable[other].seed, gTable[other].size, SoakError::EarlierFile)) return;
    }
    if (round % 5 == 0) {
        const int gone = (int)(rnd(gRandom) % kFiles);
        if (gTable[gone].size) {
            soakPath(path, gone);
            const FRESULT r = f_unlink(path);
            if (r != FR_OK) { fail(SoakError::Remove, (uint32_t)r); return; }
            gTable[gone].size = 0;
        }
    }
    if (round % 8 == 0) {
        if (!card.saveSlot(kPresetSlot, pj, lib)) { fail(SoakError::ProjectSave, 0); return; }
        ++soakStatus.projectSaves;
        app::SlotInfo info;
        if (!card.slotInfo(kPresetSlot, info)) { fail(SoakError::ProjectLoad, 1); return; }
        static app::Project* out = new app::Project;
        int missing = 0;
        if (!card.loadSlot(kPresetSlot, *out, scratch, lib, missing)) { fail(SoakError::ProjectLoad, 2); return; }
        if (card.loadFellBack()) { fail(SoakError::ProjectLoad, 3); return; }
        if (out->bpm != pj.bpm) { fail(SoakError::ProjectDiffers, out->bpm); return; }
    }
    if (round % 16 == 0) {
        static int id = -1;
        app::PresetRecord r;
        memset(&r, 0, sizeof(r));
        r.cat = 1;
        r.voices = 2;
        r.bank[0] = r.bank[1] = 255;
        strcpy(r.name, "SOAK TEST");
        r.v[2] = (int8_t)(round % 100);
        int got = -1;
        const app::PresetSave s = app::presetSave(r, id, got);
        if (s != app::PresetSave::Ok) { fail(SoakError::PresetSave, (uint32_t)s); return; }
        id = got;
        ++soakStatus.presetSaves;
    }
    if (round % 64 == 0) {
        const char* why = card.checkAgain();
        ++soakStatus.cardChecks;
        if (why) { fail(SoakError::CardCheck, (uint32_t)why[0]); return; }
    }
    soakStatus.rounds = round;
    soakStatus.megabytes = gBytes >> 20;
}

}  // namespace board
