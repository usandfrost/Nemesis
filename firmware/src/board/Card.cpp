#include "board/Card.h"

#include <initializer_list>
#include <stdlib.h>
#include <string.h>

#include "app/PresetFile.h"
#include "app/ProjectFile.h"
#include "fatfs.h"
#include "main.h"
#include "memory/SdramHeap.h"
#include "sdmmc.h"
#include "storage/CardCheck.h"

namespace board {

Card card;

using namespace app;

constexpr size_t kReserveBytes = 1536u * 1024u;    // SDRAM left for the engines' lazy blocks (a Plaits slot is ~27 KB) and the rest
constexpr size_t kAuditionFrames = 512u * 1024u;   // each of the two audition buffers: ~11 s at 48 kHz
constexpr size_t kPathPool = 160u * 1024u;
constexpr size_t kSectorBytes = 512;
constexpr size_t kClusterMapBytes = 256u * 1024u;  // the card check's map of the clusters: 2 M of them, 64 GB at the 32 KB a computer formats that size with

static FIL gFile;                         // one, static, in AXI RAM (the SD DMA reaches it)
static FIL gExport;                       // an export stays open while the main loop goes on, and the card's other work with it
alignas(32) static uint8_t gBuf[4096];    // read buffer for sample conversion

static bool cardPresent() { return !(SDMMC2_DETECT_GPIO_Port->IDR & SDMMC2_DETECT_Pin); }   // the detect line is low with a card in

// ---------------------------------------------------------------------------------------------------------------------------------

namespace {
struct CardSectors : storage::SectorReader {
    bool read(uint32_t lba, uint8_t* buf, uint32_t count) override { return disk_read(0, buf, lba, count) == RES_OK; }
};
storage::CardFault gFault = storage::CardFault::None;

// FSInfo goes stale from the first write now that FatFs leaves it alone: unknown is the honest value, and a computer works it out again
void forgetFsInfo(uint32_t sector) {
    if (sector == 0 || disk_read(0, gBuf, sector, 1) != RES_OK) return;
    if (presetRd32(gBuf) != 0x41615252u || presetRd32(gBuf + 484) != 0x61417272u) return;
    if (presetRd32(gBuf + 488) == 0xFFFFFFFFu && presetRd32(gBuf + 492) == 0xFFFFFFFFu) return;
    presetWr32(gBuf + 488, 0xFFFFFFFFu);
    presetWr32(gBuf + 492, 0xFFFFFFFFu);
    disk_write(0, gBuf, sector, 1);
}
}  // namespace

bool Card::mount() {
    mounted_ = readOnly_ = checked_ = false;
    gFault = storage::CardFault::None;
    if (!cardPresent()) return false;
    // the stock settings (MX_SDMMC2_SD_Init without its Error_Handler: no card or a bad card must not stop the instrument)
    hsd2.Instance = SDMMC2;
    hsd2.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
    hsd2.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd2.Init.BusWide = SDMMC_BUS_WIDE_4B;
    hsd2.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd2.Init.ClockDiv = 32;
    static bool linked = false;
    if (!linked) { MX_FATFS_Init(); linked = true; }
    if (f_mount(&SDFatFS, SDPath, 1) != FR_OK) return false;
    mounted_ = true;
    finishPresetReplace();
    return true;
}

bool Card::mounted() {
    if (mounted_ && !cardPresent()) {   // pulled: every later call fails at once instead of waiting on the bus
        mounted_ = false;
        f_mount(nullptr, SDPath, 0);
    }
    if (mounted_ && !readOnly_ && SD_WritesBlocked()) {
        // a fault: FatFs may hold a dirty sector it can no longer write, and it would not read past that. Mounted again it has only what the card has.
        readOnly_ = true;
        f_mount(nullptr, SDPath, 0);
        mounted_ = f_mount(&SDFatFS, SDPath, 1) == FR_OK;
    }
    return mounted_;
}

namespace {
storage::CardFault readWholeCard(storage::CardReport& report) {
    static uint8_t* map = nullptr;
    static uint32_t* folders = nullptr;
    if (!map) {
        map = new uint8_t[kClusterMapBytes];
        folders = new uint32_t[storage::kCardCheckFolders];
    }
    DWORD sectors = 0;
    if (!map || !folders || disk_ioctl(0, GET_SECTOR_COUNT, &sectors) != RES_OK) return storage::CardFault::ReadFailed;
    CardSectors disk;
    return storage::checkCard(disk, sectors, (uint32_t)SDFatFS.volbase, gBuf, map, kClusterMapBytes, folders, report);
}
}  // namespace

const char* Card::checkAgain() {
    storage::CardReport report;
    const storage::CardFault f = readWholeCard(report);
    if (f != storage::CardFault::None) return storage::cardFaultName(f);
    return report.tailCount ? "OLD DATA AFTER A FOLDER" : nullptr;
}

bool Card::beginWrite() {
    if (!mounted() || readOnly_) return false;
    if (checked_) return true;
    storage::CardReport report;
    gFault = readWholeCard(report);
    if (gFault != storage::CardFault::None) {
        SD_BlockWrites();
        mounted();
        return false;
    }
    forgetFsInfo(report.fsInfoSector);
    // old data behind the end of a folder would come to life with the next new file. By the rules of FAT it is not there, so clearing it changes nothing a reader sees.
    for (int i = 0; i < report.tailCount; ++i) {
        const storage::FolderTail& t = report.tails[i];
        for (uint32_t s = 0; s < t.sectors; ++s) {
            memset(gBuf, 0, kSectorBytes);
            if (s == 0 && t.offset != 0 && disk_read(0, gBuf, t.lba, 1) != RES_OK) return false;
            if (s == 0) memset(gBuf + t.offset, 0, kSectorBytes - t.offset);
            if (disk_write(0, gBuf, t.lba + s, 1) != RES_OK) { mounted(); return false; }
        }
    }
    if (report.tailCount) {   // FatFs may have a sector of a folder in its window
        f_mount(nullptr, SDPath, 0);
        if (f_mount(&SDFatFS, SDPath, 1) != FR_OK) { mounted_ = false; return false; }
    }
    // (created here, not at the start: a card only ever read stays as it came)
    static const char* const kFolders[] = {"CATHODE", "CATHODE/WT", "CATHODE/PRESETS"};
    for (const char* d : kFolders) {
        const FRESULT r = f_mkdir(d);
        if (r != FR_OK && r != FR_EXIST) return false;
    }
    checked_ = true;
    return true;
}

const char* Card::writeProblem() {
    mounted();
    if (gFault != storage::CardFault::None) return storage::cardFaultName(gFault);
    return readOnly_ ? "WRITE ERROR, RESTART TO RETRY" : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// samples

static void upper(char* s) { for (; *s; ++s) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32); }
static bool isWav(const FILINFO& fi) {
    const char* n = fi.fname;
    const size_t l = strlen(n);
    if (n[0] == '.' || (fi.fattrib & (AM_DIR | AM_HID | AM_SYS))) return false;   // macOS "._" files, hidden and system files
    return l > 4 && (strcmp(n + l - 4, ".wav") == 0 || strcmp(n + l - 4, ".WAV") == 0);
}

static int byPath(const void* a, const void* b) {
    return strcmp(static_cast<const SampleItem*>(a)->path, static_cast<const SampleItem*>(b)->path);   // folder, then name
}

int Card::indexSamples(SampleLib& lib) {
    if (!mounted()) return 0;
    if (!paths_) { paths_ = new char[kPathPool]; pathsCap_ = kPathPool; }
    pathsUsed_ = 0;
    firstCardSample_ = lib.count;
    static DIR top, sub;
    static FILINFO fi;
    if (f_opendir(&top, "Sample") != FR_OK) return 0;
    char folder[64];
    while (f_readdir(&top, &fi) == FR_OK && fi.fname[0]) {
        if (!(fi.fattrib & AM_DIR) || fi.fname[0] == '.') continue;
        strncpy(folder, fi.fname, sizeof(folder) - 1);
        folder[sizeof(folder) - 1] = 0;
        char catName[12];
        strncpy(catName, folder, sizeof(catName) - 1);
        catName[sizeof(catName) - 1] = 0;
        upper(catName);
        const int cat = lib.addCategory(catName);
        char dirPath[80] = "Sample/";
        strncat(dirPath, folder, sizeof(dirPath) - strlen(dirPath) - 1);
        if (f_opendir(&sub, dirPath) != FR_OK) continue;
        while (f_readdir(&sub, &fi) == FR_OK && fi.fname[0]) {
            if (!isWav(fi) || lib.count >= kMaxSamples) continue;
            const size_t need = strlen(folder) + 1 + strlen(fi.fname) + 1;
            if (pathsUsed_ + need > pathsCap_) break;
            char* path = paths_ + pathsUsed_;
            strcpy(path, folder);
            strcat(path, "/");
            strcat(path, fi.fname);
            pathsUsed_ += need;
            SampleItem& it = lib.items[lib.count++];
            memset(&it, 0, sizeof(it));
            size_t n = strlen(fi.fname) - 4;   // the name without ".wav"
            if (n > kSampleNameLen - 1) n = kSampleNameLen - 1;
            memcpy(it.name, fi.fname, n);
            it.name[n] = 0;
            it.cat = (uint8_t)cat;
            it.path = path;
        }
        f_closedir(&sub);
    }
    f_closedir(&top);
    // sorted by folder and name, so the numbers a project stores mean the same samples while the card is the same
    qsort(&lib.items[firstCardSample_], (size_t)(lib.count - firstCardSample_), sizeof(SampleItem), byPath);
    lib.sortedEnd = lib.count;
    return lib.count - firstCardSample_;
}

void Card::allocate() {
    if (cache_) return;
    // after the instrument: what SDRAM has left, less a reserve, less the two audition buffers
    audition_ = new int16_t[2 * kAuditionFrames];
    auditionFrames_ = audition_ ? kAuditionFrames : 0;
    const size_t free = memory::sdramHeapBytesFree();
    const size_t bytes = free > kReserveBytes + 64u * 1024u ? free - kReserveBytes : 64u * 1024u;
    cache_ = new int16_t[bytes / 2];
    cacheFrames_ = cache_ ? bytes / 2 : 0;
}

void Card::openPool(size_t slotBytes, int& slots, void*& arena) {
    allocate();
    while (slots > 0 && (size_t)slots * slotBytes > cacheFrames_ * sizeof(int16_t)) --slots;
    pool_.init(cacheFrames_, slotBytes / sizeof(int16_t) + 8, slots);   // (the few frames over cover the arena being lined up to 8 bytes)
    const uintptr_t top = reinterpret_cast<uintptr_t>(cache_ + cacheFrames_) & ~(uintptr_t)7;
    arena = reinterpret_cast<void*>(top - (size_t)slots * slotBytes);
}

int16_t* Card::lendPool(size_t& frames) {
    allocate();
    frames = pool_.room();
    lent_ = true;
    return cache_ + cacheUsed_;
}

namespace { bool fileIs(const char* path, uint32_t size, uint32_t crc); }

bool Card::writeWav(const char* path, const int16_t* data, uint32_t frames, uint32_t rate) {
    FRESULT r = f_open(&gFile, path, FA_CREATE_NEW | FA_WRITE);
    if (r != FR_OK) { saveError_ = (int)r; return false; }
    const uint32_t bytes = frames * 2;
    uint8_t h[44];
    auto put32 = [&](int o, uint32_t v) { h[o] = (uint8_t)v; h[o + 1] = (uint8_t)(v >> 8); h[o + 2] = (uint8_t)(v >> 16); h[o + 3] = (uint8_t)(v >> 24); };
    auto put16 = [&](int o, uint32_t v) { h[o] = (uint8_t)v; h[o + 1] = (uint8_t)(v >> 8); };
    memcpy(h, "RIFF", 4); put32(4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8); put32(16, 16); put16(20, 1); put16(22, 1);
    put32(24, rate); put32(28, rate * 2); put16(32, 2); put16(34, 16); memcpy(h + 36, "data", 4); put32(40, bytes);
    UINT did = 0;
    r = f_write(&gFile, h, sizeof(h), &did);
    bool ok = r == FR_OK && did == sizeof(h);
    uint32_t crc = crc32(h, sizeof(h));
    for (uint32_t at = 0; ok && at < bytes;) {   // through the internal buffer (the SD DMA and the D-cache)
        const uint32_t n = bytes - at < sizeof(gBuf) ? bytes - at : (uint32_t)sizeof(gBuf);
        memcpy(gBuf, reinterpret_cast<const uint8_t*>(data) + at, n);
        crc = crc32(gBuf, n, crc);
        r = f_write(&gFile, gBuf, n, &did);
        ok = r == FR_OK && did == n;
        at += n;
    }
    const FRESULT closed = f_close(&gFile);
    if (ok && closed != FR_OK) { ok = false; r = closed; }
    if (!ok) { saveError_ = r != FR_OK ? (int)r : 100; f_unlink(path); return false; }   // (100: the card is full)
    if (fileIs(path, sizeof(h) + bytes, crc)) return true;
    saveError_ = 101;   // what came back is not what went in
    f_unlink(path);
    return false;
}

int Card::keepRecording(SampleLib& lib, uint32_t frames, uint32_t rate) {
    if (lib.count >= kMaxSamples || !frames) return -1;
    char file[24] = "";
    char path[48] = "";
    bool saved = false;
    saveError_ = -1;   // no card, or one that must not be written: writeProblem() says which
    if (beginWrite()) {
        saveError_ = 0;
        FILINFO fi;
        for (const char* d : {"Sample", "Sample/REC"}) {
            const FRESULT made = f_mkdir(d);
            if (made != FR_OK && made != FR_EXIST) saveError_ = (int)made;
        }
        for (int n = 1; n < 10000 && !saved && !saveError_; ++n) {
            strcpy(file, "REC0000.wav");
            file[3] = (char)('0' + n / 1000);
            file[4] = (char)('0' + n / 100 % 10);
            file[5] = (char)('0' + n / 10 % 10);
            file[6] = (char)('0' + n % 10);
            strcpy(path, "Sample/REC/");
            strcat(path, file);
            const FRESULT st = f_stat(path, &fi);
            if (st == FR_OK) continue;   // taken
            if (st != FR_NO_FILE) { saveError_ = (int)st; break; }
            saved = writeWav(path, cache_ + cacheUsed_, frames, rate);
            break;
        }
    }
    SampleItem& it = lib.items[lib.count];
    memset(&it, 0, sizeof(it));
    if (saved) {
        const size_t need = strlen("REC/") + strlen(file) + 1;
        if (paths_ && pathsUsed_ + need <= pathsCap_) {
            char* p = paths_ + pathsUsed_;
            strcpy(p, "REC/");
            strcat(p, file);
            pathsUsed_ += need;
            it.path = p;
        }
        memcpy(it.name, file, 7);
    } else {
        // not on the card: in memory until the next project load or the power goes. Numbered, because a project finds a sample by its name
        static int unsaved = 0;
        unsaved = unsaved % 99 + 1;
        memcpy(it.name, "UNSAV00", 7);
        it.name[5] = (char)('0' + unsaved / 10);
        it.name[6] = (char)('0' + unsaved % 10);
    }
    it.cat = (uint8_t)lib.addCategory("REC");
    it.length = frames;
    it.rate = rate;
    it.data = cache_ + cacheUsed_;
    cacheUsed_ += frames;
    pool_.useSamples(cacheUsed_);
    return lib.count++;
}

void Card::resetCache(SampleLib& lib) {
    for (int i = firstCardSample_; i < lib.count; ++i) {
        if (lib.items[i].data && i != auditionItem_) lib.items[i].data = nullptr;
    }
    cacheUsed_ = 0;
    pool_.useSamples(0);
}

namespace {
struct WavFormat {
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0, dataBytes = 0;
};
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// RIFF/WAVE: finds fmt and data, checks them; leaves the file at the first byte of the audio
bool readWavHeader(FIL& f, WavFormat& w) {
    uint8_t h[40];
    UINT got;
    if (f_read(&f, h, 12, &got) != FR_OK || got != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
    bool haveFmt = false;
    for (int guard = 0; guard < 32; ++guard) {
        if (f_read(&f, h, 8, &got) != FR_OK || got != 8) return false;
        const uint32_t size = rd32(h + 4);
        const FSIZE_t next = f_tell(&f) + size + (size & 1);
        if (next > f_size(&f) + 1 && memcmp(h, "data", 4)) return false;
        if (!memcmp(h, "fmt ", 4)) {
            const UINT n = size < sizeof(h) ? size : sizeof(h);
            if (n < 16 || f_read(&f, h, n, &got) != FR_OK || got != n) return false;
            w.format = rd16(h);
            w.channels = rd16(h + 2);
            w.rate = rd32(h + 4);
            w.bits = rd16(h + 14);
            if (w.format == 0xFFFE && n >= 26) w.format = rd16(h + 24);   // WAVE_FORMAT_EXTENSIBLE: the sub-format
            haveFmt = true;
            if (f_lseek(&f, next) != FR_OK) return false;
        } else if (!memcmp(h, "data", 4)) {
            w.dataBytes = size;
            // what the loader can read: PCM 8/16/24/32 or float 32, 1..8 channels, a sane rate
            return haveFmt && w.channels >= 1 && w.channels <= 8 && w.rate >= 4000 && w.rate <= 192000 &&
                   (((w.format == 1) && (w.bits == 8 || w.bits == 16 || w.bits == 24 || w.bits == 32)) || (w.format == 3 && w.bits == 32));
        } else {
            if (f_lseek(&f, next) != FR_OK) return false;
        }
    }
    return false;
}
}  // namespace

Card::Load Card::readWav(const SampleItem& it, int16_t* out, size_t capacity, uint32_t& frames, uint32_t& rate) {
    if (!mounted()) return Load::NoCard;
    char path[300] = "Sample/";
    strncat(path, it.path, sizeof(path) - strlen(path) - 1);
    if (f_open(&gFile, path, FA_READ) != FR_OK) return mounted() ? Load::Bad : Load::NoCard;
    WavFormat w;
    Load result = readWavHeader(gFile, w) ? Load::Ok : Load::Bad;
    const uint32_t bytesPerSample = w.bits / 8, frameBytes = bytesPerSample * w.channels;
    frames = result == Load::Ok ? w.dataBytes / frameBytes : 0;
    if (frames > 120u * w.rate) frames = 120u * w.rate;   // at most 120 s of a sample: a long take fits, and more than the pool is no use
    if (result == Load::Ok && frames < 2) result = Load::Bad;
    if (result == Load::Ok && frames > capacity) result = Load::Full;
    const uint32_t perRead = (uint32_t)(sizeof(gBuf) / frameBytes);
    uint32_t done = 0;
    while (result == Load::Ok && done < frames) {
        const uint32_t n = frames - done < perRead ? frames - done : perRead;
        UINT got;
        if (f_read(&gFile, gBuf, n * frameBytes, &got) != FR_OK || got != n * frameBytes) { result = mounted() ? Load::Bad : Load::NoCard; break; }
        for (uint32_t f = 0; f < n; ++f) {
            int32_t sum = 0;
            for (uint32_t c = 0; c < w.channels; ++c) {
                const uint8_t* p = gBuf + f * frameBytes + c * bytesPerSample;
                int32_t v;
                if (w.format == 3) { float x; memcpy(&x, p, 4); x = x > 1 ? 1 : x < -1 ? -1 : x; v = (int32_t)(x * 32767.0f); }
                else if (bytesPerSample == 1) v = ((int32_t)p[0] - 128) << 8;
                else if (bytesPerSample == 2) v = (int16_t)rd16(p);
                else if (bytesPerSample == 3) v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 16;
                else v = (int32_t)rd32(p) >> 16;
                sum += v;
            }
            out[done + f] = (int16_t)(sum / (int32_t)w.channels);
        }
        done += n;
    }
    rate = w.rate;
    f_close(&gFile);
    return result;
}

// the item's audio, published for the audio interrupt: the length and rate first, the pointer last
static void publish(SampleItem& it, const int16_t* data, uint32_t frames, uint32_t rate) {
    it.data = nullptr;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    it.length = frames;
    it.rate = rate;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    it.data = data;
}

Card::Load Card::ensureLoaded(SampleLib& lib, int i) {
    if (i < 0 || i >= lib.count) return Load::Bad;
    SampleItem& it = lib.items[i];
    const bool inAudition = audition_ && it.data >= audition_ && it.data < audition_ + 2 * auditionFrames_;
    if (it.data && !inAudition) return Load::Ok;   // built in, or in the cache
    if (!it.path) return Load::Bad;
    if (it.bad) return Load::Bad;
    allocate();
    if (lent_) return Load::Full;   // (a recording is in the free memory)
    if (inAudition) {   // auditioned before: copy it into the cache, the audition buffer is not for keeps
        if (!pool_.fitsSample(it.length)) return Load::Full;
        int16_t* out = cache_ + cacheUsed_;
        memcpy(out, it.data, it.length * sizeof(int16_t));
        cacheUsed_ += it.length;
        pool_.useSamples(cacheUsed_);
        publish(it, out, it.length, it.rate);
        if (auditionItem_ == i) auditionItem_ = -1;
        return Load::Ok;
    }
    uint32_t frames = 0, rate = 0;
    const Load r = readWav(it, cache_ + cacheUsed_, pool_.room(), frames, rate);
    if (r == Load::Bad) it.bad = 1;
    if (r != Load::Ok) return r;
    publish(it, cache_ + cacheUsed_, frames, rate);
    cacheUsed_ += frames;
    pool_.useSamples(cacheUsed_);
    return Load::Ok;
}

Card::Load Card::loadForAudition(SampleLib& lib, int i) {
    if (i < 0 || i >= lib.count) return Load::Bad;
    SampleItem& it = lib.items[i];
    if (it.data) return Load::Ok;
    if (!it.path || it.bad) return Load::Bad;
    allocate();
    if (!audition_) return Load::Full;
    // two buffers, alternating: the one being refilled is never the one a fading audition still reads
    static int half = 0;
    half ^= 1;
    int16_t* buf = audition_ + half * auditionFrames_;
    for (int k = firstCardSample_; k < lib.count; ++k)   // whatever lived in this half is gone now
        if (lib.items[k].data >= buf && lib.items[k].data < buf + auditionFrames_) { lib.items[k].data = nullptr; __atomic_signal_fence(__ATOMIC_SEQ_CST); }
    uint32_t frames = 0, rate = 0;
    const Load r = readWav(it, buf, auditionFrames_, frames, rate);
    if (r == Load::Bad) it.bad = 1;
    if (r != Load::Ok) return r;
    publish(it, buf, frames, rate);
    auditionItem_ = i;
    return Load::Ok;
}

bool Card::canEvict(const SampleLib& lib, const Project& pj) {
    if (lent_) return false;
    static bool used[kMaxSamples];
    samplesUsed(pj, used, lib.count);
    for (int i = 0; i < lib.count; ++i)
        if (!used[i] && lib.items[i].data >= cache_ && lib.items[i].data < cache_ + cacheUsed_) return true;
    return false;
}

bool Card::evictUnused(SampleLib& lib, const Project& pj) {
    static bool used[kMaxSamples];
    samplesUsed(pj, used, lib.count);
    allocate();
    const size_t before = cacheUsed_;
    cacheUsed_ = lib.compactCache(cache_, cacheUsed_, used);
    pool_.useSamples(cacheUsed_);
    return cacheUsed_ < before;
}

void Card::loadProjectSamples(SampleLib& lib, const Project& pj) {
    static bool used[kMaxSamples];
    samplesUsed(pj, used, lib.count);
    allocate();
    resetCache(lib);
    for (int i = firstCardSample_; i < lib.count; ++i) {
        if (!used[i]) continue;
        const Load r = ensureLoaded(lib, i);
        if (r == Load::Full || r == Load::NoCard) break;   // a bad file is skipped; a full cache or a missing card ends it
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the card's other files: wavetable banks, the preset library, project slots

namespace {
// A bank file for the wavetable store. The payload is read through the 4 KB bounce buffer (in AXI RAM, which the SD DMA reaches) and copied on
// into the arena in SDRAM, the way the samples are.
struct CardWaves : audio::WaveFile {
    bool open(int bank) override {
        char p[40] = "CATHODE/WT/wt000.wtp";
        p[13] = (char)('0' + bank / 100 % 10);
        p[14] = (char)('0' + bank / 10 % 10);
        p[15] = (char)('0' + bank % 10);
        return f_open(&gFile, p, FA_READ) == FR_OK;
    }
    bool read(void* dst, size_t n) override {
        uint8_t* d = static_cast<uint8_t*>(dst);
        while (n) {
            const UINT want = (UINT)(n > sizeof(gBuf) ? sizeof(gBuf) : n);
            UINT got;
            if (f_read(&gFile, gBuf, want, &got) != FR_OK || got != want) return false;
            memcpy(d, gBuf, got);
            d += got;
            n -= got;
        }
        return true;
    }
    void close() override { f_close(&gFile); }
} gWaves;

struct FileSink : ByteSink {
    FIL& f;
    uint32_t crc = 0, bytes = 0;
    explicit FileSink(FIL& x) : f(x) {}
    bool write(const void* d, size_t n) override {
        UINT put;
        if (f_write(&f, d, (UINT)n, &put) != FR_OK || put != n) return false;
        crc = crc32(d, n, crc);
        bytes += (uint32_t)n;
        return true;
    }
};
struct FileSource : ByteSource {
    FIL& f;
    explicit FileSource(FIL& x) : f(x) {}
    bool read(void* d, size_t n) override {
        UINT got;
        return f_read(&f, d, (UINT)n, &got) == FR_OK && got == n;
    }
};
void slotPath(char* out, int slot, const char* ext) {
    strcpy(out, "CATHODE/PROJ");
    const size_t l = strlen(out);
    size_t at = l;
    if (slot + 1 >= 10) out[at++] = (char)('0' + (slot + 1) / 10);
    out[at++] = (char)('0' + (slot + 1) % 10);
    out[at] = 0;
    strcat(out, ext);
}

// The first `covered` bytes of a file of `size`, against a CRC
bool fileHolds(const char* path, uint32_t size, uint32_t crc, uint32_t covered) {
    if (f_open(&gFile, path, FA_READ) != FR_OK) return false;
    bool ok = f_size(&gFile) == size;
    uint32_t c = 0, done = 0;
    while (ok && done < covered) {
        const UINT want = covered - done < sizeof(gBuf) ? (UINT)(covered - done) : (UINT)sizeof(gBuf);
        UINT got;
        ok = f_read(&gFile, gBuf, want, &got) == FR_OK && got == want;
        if (ok) c = crc32(gBuf, got, c);
        done += want;
    }
    f_close(&gFile);
    return ok && c == crc;
}

// What a write is judged by: the file as it now reads back from the card. A card that takes a write and hands back something else is not written to again.
bool fileIs(const char* path, uint32_t size, uint32_t crc) {
    if (fileHolds(path, size, crc, size)) return true;
    SD_BlockWrites();
    return false;
}
}  // namespace

audio::WaveFile& Card::waves() { return gWaves; }

// ---- exports: Export/<project>/...
namespace {
constexpr uint32_t kWavHeader = 44;
char gExportPath[160];
uint32_t gExportBytes = 0, gExportCrc = 0, gExportSkip = 0;
uint8_t gExportHead[kWavHeader];
bool gExportOpen = false;
}  // namespace

bool Card::exportOpen(const char* folder, const char* name) {
    if (!beginWrite() || gExportOpen) return false;
    saveError_ = 0;
    for (const char* d : {"Export", folder}) {
        const FRESULT made = f_mkdir(d);
        if (made != FR_OK && made != FR_EXIST) { saveError_ = (int)made; return false; }
    }
    const char* dot = strrchr(name, '.');
    const size_t stem = dot ? (size_t)(dot - name) : strlen(name);
    gExportSkip = dot && strcmp(dot, ".wav") == 0 ? kWavHeader : 0;
    for (int n = 1; n < 100; ++n) {
        Str<160> p;
        p.add(folder).ch('/');
        for (size_t i = 0; i < stem; ++i) p.ch(name[i]);
        if (n > 1) p.add(" (").num(n).ch(')');
        if (dot) p.add(dot);
        const FRESULT r = f_open(&gExport, p.c_str(), FA_CREATE_NEW | FA_WRITE);
        if (r == FR_EXIST) continue;
        if (r != FR_OK) { saveError_ = (int)r; return false; }
        strcpy(gExportPath, p.c_str());
        gExportBytes = 0;
        gExportCrc = 0;
        gExportOpen = true;
        return true;
    }
    saveError_ = 102;   // a hundred of them
    return false;
}

const char* Card::exportProblem() const {
    if (saveError_ == 100 || saveError_ == FR_DENIED) return "THE CARD IS FULL";   // (a new folder on a full card is refused with DENIED)
    if (saveError_ == 102) return "TOO MANY FILES OF THAT NAME";
    if (saveError_ == FR_NO_PATH || saveError_ == FR_EXIST) return "A FILE ON THE CARD HAS THE FOLDER'S NAME";
    return nullptr;
}

int Card::ensureProjectSamples(SampleLib& lib, const Project& pj) {
    static bool used[kMaxSamples];
    samplesUsed(pj, used, lib.count);
    allocate();
    int missing = 0;
    for (int i = firstCardSample_; i < lib.count; ++i) {
        if (!used[i] || lib.items[i].data) continue;
        if (ensureLoaded(lib, i) != Load::Ok) ++missing;
    }
    return missing;
}

bool Card::exportWrite(const void* data, size_t n) {
    if (!gExportOpen) return false;
    const uint8_t* d = static_cast<const uint8_t*>(data);
    while (n > 0) {   // through the internal buffer (the SD DMA and the D-cache)
        const size_t c = n < sizeof(gBuf) ? n : sizeof(gBuf);
        memcpy(gBuf, d, c);
        const uint32_t past = gExportBytes >= gExportSkip ? 0 : gExportSkip - gExportBytes;
        if (c > past) gExportCrc = crc32(gBuf + past, c - past, gExportCrc);
        UINT did = 0;
        const FRESULT r = f_write(&gExport, gBuf, (UINT)c, &did);
        if (r != FR_OK || did != c) { saveError_ = r != FR_OK ? (int)r : 100; return false; }   // (100: the card is full)
        gExportBytes += (uint32_t)c;
        d += c;
        n -= c;
    }
    return true;
}

bool Card::exportRewrite(uint32_t at, const void* data, size_t n) {
    if (!gExportOpen || n > sizeof(gBuf) || (uint64_t)at + n > gExportBytes) return false;
    if (n == kWavHeader) memcpy(gExportHead, data, n);
    memcpy(gBuf, data, n);
    UINT did = 0;
    const bool ok = f_lseek(&gExport, at) == FR_OK && f_write(&gExport, gBuf, (UINT)n, &did) == FR_OK && did == n && f_lseek(&gExport, gExportBytes) == FR_OK;
    if (!ok) saveError_ = 100;
    return ok;
}

bool Card::exportClose(bool keep) {
    if (!gExportOpen) return false;
    gExportOpen = false;
    const FRESULT closed = f_close(&gExport);
    if (closed != FR_OK && keep) { saveError_ = (int)closed; keep = false; }
    if (!keep) { f_unlink(gExportPath); return false; }
    // it counts once what came back is what went in
    bool ok = f_open(&gFile, gExportPath, FA_READ) == FR_OK;
    uint32_t crc = 0, at = 0;
    ok = ok && f_size(&gFile) == gExportBytes;
    while (ok && at < gExportBytes) {
        UINT got = 0;
        const uint32_t want = gExportBytes - at < sizeof(gBuf) ? gExportBytes - at : (uint32_t)sizeof(gBuf);
        ok = f_read(&gFile, gBuf, want, &got) == FR_OK && got == want;
        if (!ok) break;
        const uint32_t past = at >= gExportSkip ? 0 : gExportSkip - at;
        if (past && memcmp(gBuf, gExportHead + at, past < want ? past : want) != 0) ok = false;
        if (want > past) crc = crc32(gBuf + past, want - past, crc);
        at += want;
    }
    f_close(&gFile);
    if (ok && crc == gExportCrc) return true;
    saveError_ = 101;   // what came back is not what went in
    f_unlink(gExportPath);
    SD_BlockWrites();
    return false;
}

// ---- the preset library: /CATHODE/PRESETS/*.PRS and INDEX.PIX (app/PresetFile.h)
namespace {
const char kIndexPath[] = "CATHODE/PRESETS/INDEX.PIX";
// A preset is replaced where it lies, and its old bytes wait here until the new ones are read back. Beside the folder, so the folder's fingerprint never sees it.
const char kJournalPath[] = "CATHODE/PRESET.JNL";
struct PresetJournal {
    char name[kPresetFileMax + 1];
    uint32_t size;
    uint8_t body[kPresetBodySize];
    uint32_t crc;      // of everything before it
};
struct CardPresets : PresetStorage {
    bool indexOpen_ = false;
    bool list(void (*fn)(void*, const char*, uint32_t, uint32_t), void* ctx) override {
        static DIR dir;
        if (!card.mounted()) return false;
        FRESULT r = f_opendir(&dir, kPresetDir);
        if (r == FR_NO_PATH) return true;   // a card that has not been saved to: an empty library, which the first save makes
        if (r != FR_OK) return false;
        FILINFO fi;
        while ((r = f_readdir(&dir, &fi)) == FR_OK && fi.fname[0])
            if (!(fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) && fi.fname[0] != '.') fn(ctx, fi.fname, (uint32_t)fi.fsize, ((uint32_t)fi.fdate << 16) | fi.ftime);
        f_closedir(&dir);
        return r == FR_OK;   // a read error is not the end of the folder
    }
    bool readFile(const char* name, uint8_t* buf, size_t cap, size_t& n) override {
        char path[64];
        strcpy(path, kPresetDir);
        strcat(path, "/");
        strncat(path, name, kPresetFileMax);
        n = 0;
        if (!card.mounted() || f_open(&gFile, path, FA_READ) != FR_OK) return false;
        UINT got = 0;
        const bool ok = f_read(&gFile, buf, (UINT)cap, &got) == FR_OK && f_size(&gFile) <= cap;
        f_close(&gFile);
        n = got;
        return ok;
    }
    bool indexSize(size_t& n) override {
        FILINFO fi;
        if (!card.mounted() || f_stat(kIndexPath, &fi) != FR_OK) return false;
        n = (size_t)fi.fsize;
        return true;
    }
    bool readIndex(size_t offset, uint8_t* buf, size_t len) override {
        if (!card.mounted() || f_open(&gFile, kIndexPath, FA_READ) != FR_OK) return false;
        UINT got = 0;
        const bool ok = f_lseek(&gFile, offset) == FR_OK && f_read(&gFile, buf, (UINT)len, &got) == FR_OK && got == len;
        f_close(&gFile);
        return ok;
    }
    static void pathOf(char* path, const char* name) {
        strcpy(path, kPresetDir);
        strcat(path, "/");
        strncat(path, name, kPresetFileMax);
    }
    bool fileExists(const char* name) override {
        char path[64];
        pathOf(path, name);
        FILINFO fi;
        return card.mounted() && f_stat(path, &fi) == FR_OK && fi.fsize != 0;   // (an empty one is what a cut new file leaves: see writeFile)
    }
    // A new file is made under its name, and removed if it cannot be read back. A replace goes through the journal: the old bytes first, read back, then the new
    // ones over them. A power cut leaves one of the two whole: finishPresetReplace() sorts it out at the next start.
    bool writeFile(const char* name, const uint8_t* buf, size_t n) override {
        char path[64];
        pathOf(path, name);
        if (!card.beginWrite()) return false;
        FILINFO fi;
        FRESULT st = f_stat(path, &fi);
        if (st != FR_OK && st != FR_NO_FILE && st != FR_NO_PATH) return false;
        if (st == FR_OK && fi.fsize == 0) {   // a new file a power cut stopped before its first byte: it holds nothing, and the save is being tried again
            if (f_unlink(path) != FR_OK) return false;
            st = FR_NO_FILE;
        }
        const uint32_t crc = crc32(buf, n);
        UINT put = 0;
        if (st != FR_OK) {
            if (f_open(&gFile, path, FA_CREATE_NEW | FA_WRITE) != FR_OK) return false;
            bool ok = f_write(&gFile, buf, (UINT)n, &put) == FR_OK && put == n;
            ok = (f_close(&gFile) == FR_OK) && ok && fileIs(path, (uint32_t)n, crc);
            if (!ok) f_unlink(path);
            return ok;
        }
        if (n != (size_t)kPresetBodySize || fi.fsize != n) return false;
        static PresetJournal j;
        memset(&j, 0, sizeof(j));
        strncpy(j.name, name, kPresetFileMax);
        j.size = (uint32_t)n;
        UINT got = 0;
        if (f_open(&gFile, path, FA_READ) != FR_OK) return false;
        const bool have = f_read(&gFile, j.body, (UINT)n, &got) == FR_OK && got == n;
        f_close(&gFile);
        if (!have) return false;
        j.crc = crc32(&j, offsetof(PresetJournal, crc));
        if (f_open(&gFile, kJournalPath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
        bool ok = f_write(&gFile, &j, sizeof(j), &put) == FR_OK && put == sizeof(j);
        ok = (f_close(&gFile) == FR_OK) && ok && fileIs(kJournalPath, sizeof(j), crc32(&j, sizeof(j)));
        if (!ok) {
            f_unlink(kJournalPath);   // nothing has been replaced, and the next start has nothing to finish
            return false;
        }
        if (f_open(&gFile, path, FA_OPEN_EXISTING | FA_WRITE) != FR_OK) return false;
        ok = f_write(&gFile, buf, (UINT)n, &put) == FR_OK && put == n;
        ok = (f_close(&gFile) == FR_OK) && ok && fileIs(path, (uint32_t)n, crc);
        if (ok) return f_unlink(kJournalPath) == FR_OK;
        return false;   // the journal stays: the next start puts the old preset back
    }
    bool removeFile(const char* name) override {
        char path[64];
        pathOf(path, name);
        return card.beginWrite() && f_unlink(path) == FR_OK;
    }
    bool writeIndex(size_t offset, const uint8_t* buf, size_t len) override {
        if (!card.writesCleared()) return false;   // only ever alongside a save: starting up leaves the card as it is, and the index is rebuilt in memory
        if (offset == 0) {                         // one open for all of it: each one searches the folder, and a full folder is 200 sectors
            if (f_open(&gFile, kIndexPath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
            indexOpen_ = true;
        }
        UINT put = 0;
        return indexOpen_ && f_write(&gFile, buf, (UINT)len, &put) == FR_OK && put == len;
    }
    bool endIndex() override {
        if (!indexOpen_) return false;
        indexOpen_ = false;
        return f_close(&gFile) == FR_OK;
    }
} gPresets;
}  // namespace

app::PresetStorage& Card::presets() { return gPresets; }

// A preset replace that did not get to the end. The journal says what the file was; whichever of the two is whole stays. The only thing a start writes.
void Card::finishPresetReplace() {
    FILINFO fi;
    if (f_stat(kJournalPath, &fi) != FR_OK) return;
    static PresetJournal j;
    UINT got = 0;
    bool whole = fi.fsize == sizeof(j) && f_open(&gFile, kJournalPath, FA_READ) == FR_OK;
    if (whole) {
        whole = f_read(&gFile, &j, sizeof(j), &got) == FR_OK && got == sizeof(j) && j.crc == crc32(&j, offsetof(PresetJournal, crc)) && j.size == kPresetBodySize;
        f_close(&gFile);
    }
    if (!beginWrite()) return;
    if (whole) {
        j.name[kPresetFileMax] = 0;
        char path[64];
        CardPresets::pathOf(path, j.name);
        static uint8_t now[kPresetBodySize];
        static PresetRecord r;
        bool newOk = f_open(&gFile, path, FA_READ) == FR_OK;
        if (newOk) {
            newOk = f_read(&gFile, now, sizeof(now), &got) == FR_OK && got == sizeof(now);
            f_close(&gFile);
        }
        if (!newOk || parsePresetFile(now, sizeof(now), r) != PresetParse::Ok) {
            UINT put = 0;
            if (f_open(&gFile, path, FA_OPEN_ALWAYS | FA_WRITE) != FR_OK) return;
            const bool wrote = f_write(&gFile, j.body, sizeof(j.body), &put) == FR_OK && put == sizeof(j.body);
            if (f_close(&gFile) != FR_OK || !wrote || !fileIs(path, sizeof(j.body), crc32(j.body, sizeof(j.body)))) return;
        }
    }
    f_unlink(kJournalPath);
}

int Card::scanWaves() {
    for (bool& b : app::waveSet().present) b = false;
    if (!mounted()) return 0;
    static DIR dir;
    if (f_opendir(&dir, "CATHODE/WT") != FR_OK) return 0;
    int n = 0;
    FILINFO fi;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        const char* f = fi.fname;
        if ((fi.fattrib & AM_DIR) || strlen(f) != 9 || (f[0] | 32) != 'w' || (f[1] | 32) != 't' || (f[5] != '.') || (f[6] | 32) != 'w' || (f[7] | 32) != 't' || (f[8] | 32) != 'p') continue;
        if (f[2] < '0' || f[2] > '9' || f[3] < '0' || f[3] > '9' || f[4] < '0' || f[4] > '9') continue;
        const int b = (f[2] - '0') * 100 + (f[3] - '0') * 10 + (f[4] - '0');
        if (b < app::kWaveBanks && !app::waveSet().present[b]) { app::waveSet().present[b] = true; ++n; }
    }
    f_closedir(&dir);
    return n;
}

// ---- project slots: two files taken in turn, /CATHODE/PROJn.DBP and PROJn.BAK. A save writes over the older one, reads it back, and only then counts: the
// newest whole file is the project, and the one before it is what SHIFT + CLEAR goes back to. Nothing is renamed: FatFs's rename is two directory writes, and a power
// cut between them leaves two names on one chain. A mark at the end of the file (after what readProject reads) counts the saves; a file without one is from
// before this, and is older than anything with one.
namespace {
struct SlotMark {
    char magic[4];
    uint32_t generation;
    uint32_t projectBytes;
    uint32_t crc;       // of the project bytes
    uint32_t markCrc;   // of the mark so far: the count decides which file is the project, so one bad bit in it must not
};
constexpr const char* kSlotExt[2] = {".DBP", ".BAK"};

struct SlotFile {
    bool whole = false;     // it reads as a project, as far as a look at both ends can say
    bool marked = false;
    uint32_t generation = 0, projectBytes = 0, crc = 0;
};

// false: the card could not say, which is not the same as no file
bool lookAt(int slot, int which, SlotFile& s) {
    s = SlotFile{};
    char path[32];
    slotPath(path, slot, kSlotExt[which]);
    const FRESULT r = f_open(&gFile, path, FA_READ);
    if (r == FR_NO_FILE || r == FR_NO_PATH) return true;
    if (r != FR_OK) return false;
    FileSource src(gFile);
    ProjectHeader h;
    s.whole = readHeader(src, h);
    SlotMark m;
    UINT got = 0;
    const FSIZE_t size = f_size(&gFile);
    if (s.whole && size >= sizeof(m) && f_lseek(&gFile, size - sizeof(m)) == FR_OK && f_read(&gFile, &m, sizeof(m), &got) == FR_OK && got == sizeof(m) &&
        memcmp(m.magic, "DBGN", 4) == 0 && m.projectBytes + sizeof(m) == size && m.markCrc == crc32(&m, offsetof(SlotMark, markCrc))) {
        s.marked = true;
        s.generation = m.generation;
        s.projectBytes = m.projectBytes;
        s.crc = m.crc;
    }
    f_close(&gFile);
    return true;
}

// which of the two is the project: 0, 1, or -1 for neither
int newest(const SlotFile f[2]) {
    if (f[0].whole && (!f[1].whole || f[0].generation >= f[1].generation)) return 0;
    return f[1].whole ? 1 : -1;
}
}  // namespace

bool Card::slotInfo(int slot, SlotInfo& out) {
    if (!mounted()) return false;
    SlotFile f[2];
    if (!lookAt(slot, 0, f[0]) || !lookAt(slot, 1, f[1])) return false;
    const int which = newest(f);
    if (which < 0) return false;
    char path[32];
    slotPath(path, slot, kSlotExt[which]);
    if (f_open(&gFile, path, FA_READ) != FR_OK) return false;
    FileSource src(gFile);
    ProjectHeader h;
    const bool ok = readHeader(src, h);
    f_close(&gFile);
    if (ok) headerInfo(h, out);
    return ok;
}

bool Card::saveSlot(int slot, const Project& pj, const SampleLib& lib) {
    if (!beginWrite()) return false;
    SlotFile f[2];
    if (!lookAt(slot, 0, f[0]) || !lookAt(slot, 1, f[1])) return false;
    int current = newest(f);
    char path[32];
    // the file about to stay as the only copy has to be good all the way through; if it is not, it is the one to overwrite
    if (current >= 0 && f[current].marked) {
        slotPath(path, slot, kSlotExt[current]);
        if (!fileHolds(path, f[current].projectBytes + sizeof(SlotMark), f[current].crc, f[current].projectBytes)) f[current].whole = false;
    }
    const uint32_t latest = f[0].generation > f[1].generation ? f[0].generation : f[1].generation;
    current = newest(f);
    slotPath(path, slot, kSlotExt[current == 0 ? 1 : 0]);
    savedGeneration_[slot] = 0;
    if (f_open(&gFile, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    FileSink sink(gFile);
    bool ok = writeProject(pj, sink, &lib);
    SlotMark m;
    memcpy(m.magic, "DBGN", 4);
    m.generation = latest + 1;
    m.projectBytes = sink.bytes;
    m.crc = sink.crc;
    m.markCrc = crc32(&m, offsetof(SlotMark, markCrc));
    ok = ok && sink.write(&m, sizeof(m));
    ok = (f_close(&gFile) == FR_OK) && ok && fileIs(path, sink.bytes, sink.crc);
    if (!ok) {
        f_unlink(path);   // what was the project is still the project
        return false;
    }
    savedGeneration_[slot] = m.generation;
    return true;
}

bool Card::loadSlot(int slot, Project& pj, Project& scratch, const SampleLib& lib, int& missing) {
    if (!mounted()) return false;
    SlotFile f[2];
    if (!lookAt(slot, 0, f[0]) || !lookAt(slot, 1, f[1])) return false;
    // the newest first, and if it does not hold up in full (its CRC is only read here) the one before it
    fellBack_ = false;
    for (int tries = 0; tries < 2; ++tries) {
        const int which = newest(f);
        if (which < 0) return false;
        char path[32];
        slotPath(path, slot, kSlotExt[which]);
        if (f_open(&gFile, path, FA_READ) != FR_OK) return false;
        FileSource src(gFile);
        const bool ok = readProject(src, scratch, pj, &lib, &missing);
        f_close(&gFile);
        if (ok) return true;
        f[which].whole = false;
        fellBack_ = true;
    }
    return false;
}

bool Card::undoSave(int slot) {
    if (!beginWrite()) return false;
    SlotFile f[2];
    if (!lookAt(slot, 0, f[0]) || !lookAt(slot, 1, f[1])) return false;
    const int current = newest(f);
    // only the save that was made here: after one that failed, the newest file is the only copy there is
    if (current < 0 || savedGeneration_[slot] == 0 || f[current].generation != savedGeneration_[slot]) return false;
    char path[32];
    // and not onto a file that has gone bad: the one being taken back would be the last good copy
    const int before = current == 0 ? 1 : 0;
    if (f[before].marked) {
        slotPath(path, slot, kSlotExt[before]);
        if (!fileHolds(path, f[before].projectBytes + sizeof(SlotMark), f[before].crc, f[before].projectBytes)) return false;
    }
    slotPath(path, slot, kSlotExt[current]);
    savedGeneration_[slot] = 0;
    return f_unlink(path) == FR_OK;   // the one before it is the project again, or the slot is empty if the save made it
}

// The older file goes first: a power cut between the two leaves the project as it was, not the save before it in its place.
bool Card::deleteSlot(int slot) {
    if (!beginWrite()) return false;
    SlotFile f[2];
    if (!lookAt(slot, 0, f[0]) || !lookAt(slot, 1, f[1])) return false;
    const int current = newest(f);
    char path[32];
    // the file that stays longest has to be the one that loads, and only a read to the end can say which that is
    if (current >= 0 && f[current].marked) {
        slotPath(path, slot, kSlotExt[current]);
        if (!fileHolds(path, f[current].projectBytes + sizeof(SlotMark), f[current].crc, f[current].projectBytes)) f[current].whole = false;
    }
    const int older = newest(f) == 0 ? 1 : 0;
    for (const int which : {older, 1 - older}) {
        slotPath(path, slot, kSlotExt[which]);
        const FRESULT r = f_unlink(path);
        if (r == FR_NO_FILE) continue;
        if (r != FR_OK) {
            // a write can fail with the name already off the card. A card that has gone says nothing either way, and then the project is taken to be there
            return mounted() && lookAt(slot, 0, f[0]) && lookAt(slot, 1, f[1]) && newest(f) < 0;
        }
        savedGeneration_[slot] = 0;   // a save's undo would take the last file now; after a refusal it still has what it had
    }
    return true;
}

}  // namespace board
