#include "SdModel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <vector>

#include "main.h"
#include "stm32h7xx_hal.h"

extern "C" void HAL_SD_RxCpltCallback(SD_HandleTypeDef*);
extern "C" void HAL_SD_TxCpltCallback(SD_HandleTypeDef*);
extern "C" void HAL_SD_ErrorCallback(SD_HandleTypeDef*);

namespace sd {

Faults faults;
Stats stats;

namespace {
constexpr int kLine = 32, kSector = 512;

std::vector<uint8_t> card;
struct Line { uint8_t ram[kLine]; };
std::map<uintptr_t, Line> lines;      // what RAM holds, for every line a transfer has touched

struct Transfer {
    bool active = false, write = false, hang = false;
    uint8_t* buf = nullptr;
    uint32_t sector = 0, count = 0;
    int ticks = 0;
} xfer;

uint32_t rng = 1, tick = 0;
int programming = 0;
bool stuck = false, powerOff = false;
uint32_t errorCode = 0, pendingR1 = 0;
long landed = 0;
std::vector<std::pair<uint32_t, std::vector<uint8_t>>> recent;   // what the last sectors overwrote, for lostOnCut

uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
bool chance(int percent) { return (int)(rnd() % 100) < percent; }

Line& lineAt(uintptr_t a) {
    auto it = lines.find(a);
    if (it == lines.end()) { it = lines.emplace(a, Line{}).first; memcpy(it->second.ram, (void*)a, kLine); }
    return it->second;
}
bool checkLine(uintptr_t a) { if (a & (kLine - 1)) { stats.misaligned++; return false; } return true; }
template <class F> void forLines(uintptr_t a, size_t n, F fn) {
    for (uintptr_t l = a & ~(uintptr_t)(kLine - 1); l < a + n; l += kLine) fn(l);
}
bool dirty(uintptr_t l) { return memcmp(lineAt(l).ram, (void*)l, kLine) != 0; }
void evict(uintptr_t l) { memcpy(lineAt(l).ram, (void*)l, kLine); }

void cut() {
    powerOff = true;
    for (int i = (int)recent.size() - 1; i >= 0 && i >= (int)recent.size() - faults.lostOnCut; --i)
        memcpy(card.data() + recent[(size_t)i].first * (size_t)kSector, recent[(size_t)i].second.data(), kSector);
}

// the DMA, RAM to card; dirty lines may fall out of the cache first
void landWrite() {
    for (uint32_t i = 0; i < xfer.count; ++i) {
        const uintptr_t base = (uintptr_t)xfer.buf + i * (size_t)kSector;
        if (faults.cutAfterSectors >= 0 && landed >= faults.cutAfterSectors) {
            if (faults.torn) {
                uint8_t* to = card.data() + (size_t)(xfer.sector + i) * kSector;
                const size_t keep = rnd() % kSector;
                for (uintptr_t l = base, o = 0; o < keep; ++l, ++o) to[o] = lineAt(l & ~(uintptr_t)(kLine - 1)).ram[l & (kLine - 1)];
            }
            cut();
            return;
        }
        uint8_t out[kSector];
        forLines(base, kSector, [&](uintptr_t l) {
            if (dirty(l) && chance(faults.evictPercent)) evict(l);
            if (dirty(l)) stats.staleToCard++;
            memcpy(out + (l - base), lineAt(l).ram, kLine);
        });
        uint8_t* to = card.data() + (size_t)(xfer.sector + i) * kSector;
        recent.emplace_back(xfer.sector + i, std::vector<uint8_t>(to, to + kSector));
        if (recent.size() > 64) recent.erase(recent.begin());
        memcpy(to, out, kSector);
        ++landed;
        stats.sectorsWritten++;
    }
    programming = faults.programmingPolls;
    if (faults.statusStuck == 0) stuck = true;
}

// the DMA, card to RAM: and a dirty line evicted now lands on top of it. This is what went into FAT sector 0.
void landRead() {
    for (uint32_t i = 0; i < xfer.count; ++i) {
        const uintptr_t base = (uintptr_t)xfer.buf + i * (size_t)kSector;
        const uint8_t* from = card.data() + (size_t)(xfer.sector + i) * kSector;
        forLines(base, kSector, [&](uintptr_t l) {
            const bool wasDirty = dirty(l);
            memcpy(lineAt(l).ram, from + (l - base), kLine);
            if (wasDirty) { stats.dirtyOverDma++; if (chance(faults.evictPercent)) evict(l); }
        });
        stats.sectorsRead++;
    }
}

void complete() {
    xfer.active = false;
    if (powerOff) return;
    if (xfer.write) {
        landWrite();
        if (powerOff) return;
        if (faults.stopFails == 0) { faults.stopFails = -1; errorCode = 1; HAL_SD_ErrorCallback(nullptr); }
        HAL_SD_TxCpltCallback(nullptr);
    } else {
        landRead();
        HAL_SD_RxCpltCallback(nullptr);
    }
}

void step() {
    ++tick;
    if (!xfer.active || --xfer.ticks > 0) return;
    if (xfer.hang) { if (xfer.ticks < -3000) { stats.lateLandings++; complete(); } return; }   // late: long after the driver's 1 s
    complete();
}

HAL_StatusTypeDef start(uint8_t* buf, uint32_t sector, uint32_t count, bool write) {
    if (powerOff || xfer.active) return HAL_BUSY;
    if (programming > 0 || stuck) stats.busyViolations++;
    if ((size_t)(sector + count) * kSector > card.size()) return HAL_ERROR;
    errorCode = 0;
    stats.transfers++;
    xfer = Transfer{true, write, faults.transferHangs == 0, buf, sector, count, 2 + (int)(rnd() % 4)};
    if (write && faults.writeRejected == 0) { xfer.active = false; pendingR1 = 2; HAL_SD_TxCpltCallback(nullptr); }
    else if (faults.transferFails == 0) { xfer.active = false; errorCode = 4; HAL_SD_ErrorCallback(nullptr); }
    if (faults.transferFails >= 0) faults.transferFails--;
    if (faults.transferHangs >= 0) faults.transferHangs--;
    if (write) {
        if (faults.writeRejected >= 0) faults.writeRejected--;
        if (faults.stopFails > 0) faults.stopFails--;
        if (faults.statusStuck > 0) faults.statusStuck--;
    }
    return HAL_OK;
}
}  // namespace

void open(uint32_t n) {
    card.assign((size_t)n * kSector, 0);
    faults = Faults{};
    stats = Stats{};
    powerCycle();
    rng = 0x9E3779B9u;
    landed = 0;
}
uint8_t* image() { return card.data(); }
uint32_t sectors() { return (uint32_t)(card.size() / kSector); }
bool poweredOff() { return powerOff; }
void powerCycle() {
    lines.clear();
    xfer = Transfer{};
    programming = 0;
    stuck = powerOff = false;
    errorCode = pendingR1 = 0;
    recent.clear();
    if (faults.seed) rng = faults.seed * 2654435761u | 1u;
    landed = 0;
}
void resetCounts() { stats = Stats{}; landed = 0; }
void saveImage(const char* path) { FILE* f = fopen(path, "wb"); fwrite(card.data(), 1, card.size(), f); fclose(f); }
void loadImage(const void* data, size_t bytes) { card.assign((const uint8_t*)data, (const uint8_t*)data + bytes); }

void format(uint32_t spc) {
    constexpr uint32_t kStart = 2048, kReserved = 32, kFats = 2;
    const uint32_t total = sectors() - kStart;
    uint32_t fatSize = (total / spc + 2) * 4 / kSector + 1;
    const uint32_t clusters = (total - kReserved - kFats * fatSize) / spc;
    auto w16 = [](uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); };
    auto w32 = [](uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); };
    uint8_t* mbr = card.data();
    uint8_t* e = mbr + 446;
    e[4] = 0x0C;
    w32(e + 8, kStart);
    w32(e + 12, total);
    w16(mbr + 510, 0xAA55);
    uint8_t* b = card.data() + (size_t)kStart * kSector;
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "MSDOS5.0", 8);
    w16(b + 11, kSector);
    b[13] = (uint8_t)spc;
    w16(b + 14, kReserved);
    b[16] = kFats;
    b[21] = 0xF8;
    w32(b + 32, total);
    w32(b + 36, fatSize);
    w32(b + 44, 2);
    w16(b + 48, 1);
    w16(b + 50, 6);
    b[66] = 0x29;
    memcpy(b + 71, "NO NAME    ", 11);
    memcpy(b + 82, "FAT32   ", 8);
    w16(b + 510, 0xAA55);
    uint8_t* si = b + kSector;
    w32(si, 0x41615252u);
    w32(si + 484, 0x61417272u);
    w32(si + 488, clusters - 1);
    w32(si + 492, 3);
    w16(si + 510, 0xAA55);
    memcpy(b + 6 * kSector, b, 2 * kSector);
    for (uint32_t f = 0; f < kFats; ++f) {
        uint8_t* fat = b + (size_t)(kReserved + f * fatSize) * kSector;
        w32(fat, 0x0FFFFFF8u);
        w32(fat + 4, 0x0FFFFFFFu);
        w32(fat + 8, 0x0FFFFFFFu);
    }
}

}  // namespace sd

using namespace sd;

extern "C" {
uint32_t HAL_GetTick(void) { step(); return tick; }
HAL_StatusTypeDef HAL_SD_Init(SD_HandleTypeDef*) { return powerOff ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_SD_ConfigWideBusOperation(SD_HandleTypeDef*, uint32_t) { return HAL_OK; }
HAL_StatusTypeDef HAL_SD_ReadBlocks(SD_HandleTypeDef*, uint8_t*, uint32_t, uint32_t, uint32_t) { return HAL_ERROR; }
HAL_StatusTypeDef HAL_SD_WriteBlocks(SD_HandleTypeDef*, const uint8_t*, uint32_t, uint32_t, uint32_t) { return HAL_ERROR; }
HAL_StatusTypeDef HAL_SD_Erase(SD_HandleTypeDef*, uint32_t, uint32_t) { return HAL_ERROR; }
HAL_StatusTypeDef HAL_SD_ReadBlocks_DMA(SD_HandleTypeDef*, uint8_t* d, uint32_t add, uint32_t n) { return start(d, add, n, false); }
HAL_StatusTypeDef HAL_SD_WriteBlocks_DMA(SD_HandleTypeDef*, const uint8_t* d, uint32_t add, uint32_t n) { return start((uint8_t*)d, add, n, true); }
void BSP_SD_Abort(void) { xfer.active = false; }   // (the real one is register work; the model has only the transfer to stop)
HAL_SD_CardStateTypeDef HAL_SD_GetCardState(SD_HandleTypeDef*) {
    if (powerOff) { errorCode |= 8; return HAL_SD_CARD_PROGRAMMING; }
    if (pendingR1) { errorCode |= pendingR1; pendingR1 = 0; }
    if (stuck) return HAL_SD_CARD_PROGRAMMING;
    if (programming > 0) { --programming; return HAL_SD_CARD_PROGRAMMING; }
    return HAL_SD_CARD_TRANSFER;
}
HAL_StatusTypeDef HAL_SD_GetCardInfo(SD_HandleTypeDef*, HAL_SD_CardInfoTypeDef* info) { info->LogBlockNbr = sectors(); info->LogBlockSize = kSector; return HAL_OK; }
uint32_t HAL_SD_GetError(const SD_HandleTypeDef*) { return errorCode; }
int sd_model_reaches(const void*) { return faults.directAll; }
uint8_t BSP_PlatformIsDetected(void) { return 1; }
SD_HandleTypeDef hsd2;
GpioStub gDetectPort;

void SCB_CleanDCache_by_Addr(volatile void* p, int32_t n) {
    if (faults.noClean) return;
    forLines((uintptr_t)p, (size_t)n, [](uintptr_t l) { if (checkLine(l)) evict(l); });
    if ((uintptr_t)p & (kLine - 1)) stats.misaligned++;
}
void SCB_InvalidateDCache_by_Addr(volatile void* p, int32_t n) {
    if ((uintptr_t)p & (kLine - 1)) stats.misaligned++;
    if (faults.noInvalidateAfter) return;
    forLines((uintptr_t)p, (size_t)n, [](uintptr_t l) { memcpy((void*)l, lineAt(l).ram, kLine); });
}
void SCB_CleanInvalidateDCache_by_Addr(volatile void* p, int32_t n) {
    if ((uintptr_t)p & (kLine - 1)) stats.misaligned++;
    if (faults.noInvalidateBefore) return;
    forLines((uintptr_t)p, (size_t)n, [](uintptr_t l) { evict(l); });
}
}
