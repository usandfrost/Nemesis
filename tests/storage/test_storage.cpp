// The storage stack on the host: the real sd_diskio.c, bsp_driver_sd.c and FatFs over a modelled card, DMA and cache (SdModel.h), judged by an independent
// reader of the image (FatAudit.h). A model that could not catch the fault this was all written for would prove nothing, so each fault is also run against
// a build with the protection taken away, and that run has to fail.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "FatAudit.h"
#include "SdModel.h"
#include "fatfs.h"

extern "C" Disk_drvTypeDef disk;

int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static uint8_t* gRaw = nullptr;
using Files = std::map<std::string, std::vector<uint8_t>>;

static uint32_t gRng = 1;
static uint32_t rnd() { gRng ^= gRng << 13; gRng ^= gRng >> 17; gRng ^= gRng << 5; return gRng; }
static std::vector<uint8_t> bytes(size_t n) { std::vector<uint8_t> v(n); for (auto& b : v) b = (uint8_t)rnd(); return v; }

static bool mountCard() {
    static bool linked = false;
    if (!linked) { MX_FATFS_Init(); linked = true; }
    disk.is_initialized[0] = 0;   // what a reset does to diskio.c
    return f_mount(&SDFatFS, SDPath, 1) == FR_OK;
}
static void powerCycle() { sd::powerCycle(); }

// a buffer not on a line boundary, like most of what FatFs is given: down the slow path
static FRESULT put(const std::string& path, const std::vector<uint8_t>& v, BYTE mode = FA_CREATE_ALWAYS | FA_WRITE) {
    static FIL f;
    static uint8_t mem[70000];
    FRESULT r = f_open(&f, path.c_str(), mode);
    if (r != FR_OK) return r;
    memcpy(mem + 7, v.data(), v.size());
    UINT n;
    r = f_write(&f, mem + 7, (UINT)v.size(), &n);
    const FRESULT c = f_close(&f);
    return r != FR_OK ? r : c;
}
static bool get(const std::string& path, std::vector<uint8_t>& v) {
    static FIL f;
    if (f_open(&f, path.c_str(), FA_READ) != FR_OK) return false;
    v.resize(f_size(&f));
    UINT n = 0;
    const FRESULT r = v.empty() ? FR_OK : f_read(&f, v.data(), (UINT)v.size(), &n);
    f_close(&f);
    return r == FR_OK && n == v.size();
}

// many small saves with reads in between: the pattern of a preset library
static size_t workload(Files& want, int rounds) {
    size_t bad = 0;
    if (f_mkdir("CATHODE") != FR_OK) ++bad;
    if (f_mkdir("CATHODE/PRESETS") != FR_OK) ++bad;
    for (int i = 0; i < rounds; ++i) {
        char p[64];
        snprintf(p, sizeof p, "CATHODE/PRESETS/%04d SOME NAME.PRS", i % 60);
        const auto v = bytes(148);
        if (put(p, v) == FR_OK) want["/CATHODE/PRESETS/" + std::string(p + 16)] = v;
        std::vector<uint8_t> back;
        if (get(p, back) && want.count("/CATHODE/PRESETS/" + std::string(p + 16)) && back != v) ++bad;
        if (i % 7 == 0) {
            const auto idx = bytes(600 + rnd() % 9000);
            if (put("CATHODE/PRESETS/INDEX.PIX", idx) == FR_OK) want["/CATHODE/PRESETS/INDEX.PIX"] = idx;
        }
        if (i % 11 == 0) {
            snprintf(p, sizeof p, "CATHODE/PRESETS/%04d SOME NAME.PRS", (int)(rnd() % 60));
            if (f_unlink(p) == FR_OK) want.erase("/CATHODE/PRESETS/" + std::string(p + 16));
        }
    }
    return bad;
}

static size_t differences(const Files& want, const FatAudit& a, bool print) {
    size_t bad = a.problems.size();
    if (print) for (size_t i = 0; i < a.problems.size() && i < 6; ++i) printf("    %s\n", a.problems[i].c_str());
    for (const auto& kv : want) {
        auto it = a.files.find(kv.first);
        if (it == a.files.end() || it->second != kv.second) { ++bad; if (print) printf("    %s: %s\n", kv.first.c_str(), it == a.files.end() ? "gone" : "wrong contents"); }
    }
    return bad;
}

// whole sectors into a line-aligned buffer the core has just written: FatFs hands such a read straight to the DMA
static size_t directReads() {
    alignas(32) static uint8_t buf[4096];
    static FIL f;
    size_t bad = 0;
    const auto v = bytes(sizeof buf);
    if (put("DIRECT.BIN", v) != FR_OK) return 1;
    for (int i = 0; i < 40; ++i) {
        memset(buf, 0xA5 + i, sizeof buf);
        UINT n = 0;
        if (f_open(&f, "DIRECT.BIN", FA_READ) != FR_OK || f_read(&f, buf, sizeof buf, &n) != FR_OK || n != sizeof buf) ++bad;
        f_close(&f);
        if (memcmp(buf, v.data(), sizeof buf)) ++bad;
    }
    return bad;
}

static size_t runCache(const sd::Faults& f, bool print) {
    sd::open(90000);
    sd::format(1);
    sd::faults = f;
    sd::powerCycle();
    sd::resetCounts();
    if (!mountCard()) return 1;
    Files want;
    size_t bad = workload(want, 400);
    bad += directReads();
    want.erase("/DIRECT.BIN");
    f_unlink("DIRECT.BIN");
    f_mount(nullptr, SDPath, 0);
    bad += differences(want, FatAudit(sd::image()), print);
    return bad;
}

static void testCache() {
    for (uint32_t seed : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u}) {
        sd::Faults f;
        f.seed = seed;
        f.evictPercent = 30;
        f.directAll = seed & 1;
        gRng = seed;
        CHECK(runCache(f, true) == 0);
        CHECK(sd::stats.staleToCard == 0);
        CHECK(sd::stats.dirtyOverDma == 0);
        CHECK(sd::stats.busyViolations == 0);
        CHECK(sd::stats.misaligned == 0);
    }
    printf("  cache: 8 runs clean, %ld sectors written in the last\n", sd::stats.sectorsWritten);
    struct { const char* what; bool sd::Faults::*off; } broken[] = {{"no clean before a write", &sd::Faults::noClean},
                                                                    {"no invalidate before a read", &sd::Faults::noInvalidateBefore},
                                                                    {"no invalidate after a read", &sd::Faults::noInvalidateAfter}};
    for (auto& b : broken) {
        size_t bad = 0;
        for (uint32_t seed : {1u, 2u, 3u, 4u}) {
            sd::Faults f;
            f.seed = seed;
            f.evictPercent = 30;
            f.directAll = seed & 1;
            f.*(b.off) = true;
            gRng = seed;
            bad += runCache(f, false);
        }
        printf("  cache: %s: the model finds %zu problems\n", b.what, bad);
        CHECK(bad > 0);
    }
}

// a fresh card with one file on it, mounted
static void freshCard(std::vector<uint8_t>& v) {
    sd::open(90000);
    sd::format(1);
    sd::powerCycle();
    CHECK(mountCard());
    v = bytes(5000);
    CHECK(put("KEEP.BIN", v) == FR_OK);
    f_mount(nullptr, SDPath, 0);
    CHECK(mountCard());
}
// FatFs cannot get past a dirty sector it cannot write, so after a fault the firmware mounts again; the driver stays blocked (the card is not reinitialised)
static void afterFault() {
    f_mount(nullptr, SDPath, 0);
    CHECK(f_mount(&SDFatFS, SDPath, 1) == FR_OK);
}
static bool canWrite() {
    if (put("PROBE.BIN", bytes(10)) != FR_OK) return false;
    return f_unlink("PROBE.BIN") == FR_OK;
}

static void testFaults() {
    std::vector<uint8_t> v, back;

    // one bad read is read again, and costs nothing
    freshCard(v);
    sd::faults.transferFails = 0;
    CHECK(get("KEEP.BIN", back) && back == v);
    CHECK(canWrite());

    // a read that keeps failing is an error, not old data, and after it nothing is written; reading goes on
    freshCard(v);
    sd::faults.transferFails = 0;
    sd::faults.statusStuck = -1;
    {
        // two in a row: the retry fails as well
        static FIL f;
        CHECK(f_open(&f, "KEEP.BIN", FA_READ) == FR_OK);
        UINT n;
        static uint8_t b[512];
        sd::faults.transferFails = 0;
        sd::faults.transferHangs = 1;
        const FRESULT r = f_read(&f, b, sizeof b, &n);
        CHECK(r != FR_OK);
        f_close(&f);
    }
    sd::faults.transferHangs = -1;
    CHECK(SD_WritesBlocked());
    CHECK(put("NEW.BIN", bytes(10)) == FR_WRITE_PROTECTED);
    CHECK(get("KEEP.BIN", back) && back == v);

    // a transfer that hangs is stopped, and cannot complete into a later read
    freshCard(v);
    sd::faults.transferHangs = 0;
    CHECK(get("KEEP.BIN", back) && back == v);
    for (int i = 0; i < 5000; ++i) HAL_GetTick();
    CHECK(sd::stats.lateLandings == 0);
    CHECK(get("KEEP.BIN", back) && back == v);

    // a write the card turns down is seen in R1 at the next status poll: the save fails, and nothing more goes to the card
    freshCard(v);
    sd::faults.writeRejected = 0;
    CHECK(put("NEW.BIN", bytes(3000)) != FR_OK);
    CHECK(SD_WritesBlocked());
    afterFault();
    CHECK(SD_WritesBlocked());
    CHECK(get("KEEP.BIN", back) && back == v);
    long written = sd::stats.sectorsWritten;
    CHECK(put("MORE.BIN", bytes(3000)) == FR_WRITE_PROTECTED);
    CHECK(sd::stats.sectorsWritten == written);

    // the stop command fails after the data went: the same
    freshCard(v);
    sd::faults.stopFails = 0;
    CHECK(put("NEW.BIN", bytes(3000)) != FR_OK);
    CHECK(SD_WritesBlocked());

    // a card that never finishes programming: an error within the timeout, not a hang
    freshCard(v);
    sd::faults.statusStuck = 0;
    CHECK(put("NEW.BIN", bytes(3000)) != FR_OK);
    CHECK(SD_WritesBlocked());

    // a card that is merely busy is not a dead volume
    freshCard(v);
    sd::faults.programmingPolls = 400;
    CHECK(canWrite());
    CHECK(canWrite());
    CHECK(get("KEEP.BIN", back) && back == v);
    CHECK(sd::stats.busyViolations == 0);

    // a failure on the last write of a save, which FatFs does not look at, still fails the save
    freshCard(v);
    int bad = 0;
    for (long n = 0; n < 12; ++n) {
        freshCard(v);
        sd::faults.writeRejected = n;
        if (put("NEW.BIN", bytes(600)) == FR_OK && !SD_WritesBlocked() && sd::faults.writeRejected < 0) ++bad;
    }
    CHECK(bad == 0);
    printf("  faults: all reported, nothing written after one\n");
}

void testCard();   // test_card.cpp

int main() {
    testCache();
    testFaults();
    testCard();
    if (failures) { printf("%d failed\n", failures); return 1; }
    printf("storage: all passed\n");
    return 0;
}
