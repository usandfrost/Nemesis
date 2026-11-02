#include "storage/CardCheck.h"

#include <string.h>

namespace storage {

namespace {
constexpr uint32_t kSector = 512, kBufferSectors = kCardCheckBuffer / kSector, kEntry = 32;
constexpr uint32_t kBad = 0x0FFFFFF7u, kEnd = 0x0FFFFFF8u;

uint32_t rd16(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// The FAT through a window of a few sectors: chains mostly run forward
struct Fat {
    SectorReader& disk;
    uint8_t* buf;
    uint32_t start, sectors, loaded = 0xFFFFFFFFu;
    bool get(uint32_t cluster, uint32_t& value) {
        const uint32_t sector = cluster / (kSector / 4), first = sector & ~(kBufferSectors - 1);
        if (sector >= sectors) return false;
        if (first != loaded) {
            uint32_t n = sectors - first;
            if (n > kBufferSectors) n = kBufferSectors;
            if (!disk.read(start + first, buf, n)) { loaded = 0xFFFFFFFFu; return false; }
            loaded = first;
        }
        value = rd32(buf + (cluster - first * (kSector / 4)) * 4) & 0x0FFFFFFFu;
        return true;
    }
};

struct Map {
    uint8_t* bits;
    bool test(uint32_t c) const { return (bits[c >> 3] >> (c & 7)) & 1; }
    void set(uint32_t c) { bits[c >> 3] = (uint8_t)(bits[c >> 3] | 1 << (c & 7)); }
};

struct Walk {
    Fat fat;
    Map used;
    uint32_t clusters;
    CardReport& report;
    CardFault fault(CardFault f, uint32_t at) { report.where = at; return f; }

    // Marks a chain used, and says how long it is
    CardFault chain(uint32_t first, uint32_t& length) {
        length = 0;
        for (uint32_t c = first;;) {
            if (used.test(c)) return fault(CardFault::CrossLink, c);
            used.set(c);
            ++length;
            uint32_t next;
            if (!fat.get(c, next)) return fault(CardFault::ReadFailed, c);
            if (next == 0) return fault(CardFault::FreeInChain, c);
            if (next >= kEnd) return CardFault::None;
            if (next < 2 || next >= clusters) return fault(CardFault::BadFatEntry, c);
            c = next;
        }
    }
};
}  // namespace

const char* cardFaultName(CardFault f) {
    switch (f) {
        case CardFault::None: return "OK";
        case CardFault::ReadFailed: return "READ ERROR";
        case CardFault::NoVolume: return "CARD MUST BE FAT32";
        case CardFault::VolumeTooBig: return "WRONG SIZE";
        case CardFault::BootCopyDiffers: return "BOOT SECTOR DAMAGED";
        case CardFault::HardError: return "DISK ERROR FLAGGED";
        case CardFault::BadFatEntry: return "FILE TABLE DAMAGED";
        case CardFault::BadFolder: return "FOLDER DAMAGED";
        case CardFault::FreeInChain: return "FILE TABLE DAMAGED";
        case CardFault::CrossLink: return "FILES OVERLAP";
        case CardFault::ChainTooShort: return "FILE CUT SHORT";
        case CardFault::TooManyClusters: return "CARD TOO BIG";
        case CardFault::TooManyFolders: return "TOO MANY FOLDERS";
    }
    return "?";
}

CardFault checkCard(SectorReader& disk, uint32_t cardSectors, uint32_t part, uint8_t* buffer, uint8_t* map, size_t mapBytes, uint32_t* folders, CardReport& report) {
    report = CardReport{};
    // ---- the volume
    if (part >= cardSectors) return CardFault::VolumeTooBig;
    if (!disk.read(part, buffer, 1)) return CardFault::ReadFailed;
    const uint32_t bps = rd16(buffer + 11), spc = buffer[13], reserved = rd16(buffer + 14), nfats = buffer[16], total = rd32(buffer + 32), fatSize = rd32(buffer + 36);
    const uint32_t root = rd32(buffer + 44), backup = rd16(buffer + 50), fsInfo = rd16(buffer + 48);
    if (rd16(buffer + 510) != 0xAA55 || bps != kSector || spc == 0 || (spc & (spc - 1)) || reserved == 0 || nfats < 1 || nfats > 2 || fatSize == 0 || total == 0 ||
        rd16(buffer + 22) != 0 || rd16(buffer + 19) != 0)
        return CardFault::NoVolume;
    if ((uint64_t)part + total > cardSectors) return CardFault::VolumeTooBig;
    if ((uint64_t)reserved + (uint64_t)nfats * fatSize >= total) return CardFault::NoVolume;
    if (backup != 0 && backup < reserved) {
        // the shape of the volume only: an operating system sets a dirty flag in the first copy alone, and a backup never written is no fault
        if (!disk.read(part + backup, buffer, 1)) return CardFault::ReadFailed;
        if (rd16(buffer + 510) == 0xAA55 && (rd16(buffer + 11) != bps || buffer[13] != spc || rd16(buffer + 14) != reserved || buffer[16] != nfats ||
                                             rd32(buffer + 32) != total || rd32(buffer + 36) != fatSize || rd32(buffer + 44) != root))
            return CardFault::BootCopyDiffers;
    }
    if (fsInfo != 0 && fsInfo < reserved) report.fsInfoSector = part + fsInfo;
    const uint32_t fat1 = part + reserved, data = fat1 + nfats * fatSize;
    const uint32_t clusters = (total - reserved - nfats * fatSize) / spc + 2;
    report.clusters = clusters;
    if (clusters <= 65525) return CardFault::NoVolume;
    if ((uint64_t)fatSize * (kSector / 4) < clusters) return CardFault::NoVolume;
    if (clusters / 8 + 1 > mapBytes) return CardFault::TooManyClusters;
    if (root < 2 || root >= clusters) return CardFault::NoVolume;

    // ---- every entry of the FAT, the only pass that sees all of it. The second copy is compared as it comes.
    if (!disk.read(fat1, buffer, 1)) return CardFault::ReadFailed;
    const uint32_t flags = rd32(buffer + 4);
    report.dirty = !((flags >> 27) & 1);
    if (!((flags >> 26) & 1)) return CardFault::HardError;
    const uint32_t half = kBufferSectors / 2;   // half the buffer each way
    for (uint32_t s = 0; s < fatSize; s += half) {
        const uint32_t n = fatSize - s < half ? fatSize - s : half;
        if (!disk.read(fat1 + s, buffer, n)) { report.where = fat1 + s; return CardFault::ReadFailed; }
        for (uint32_t i = 0; i < n * (kSector / 4); ++i) {
            const uint32_t c = s * (kSector / 4) + i;
            if (c < 2 || c >= clusters) continue;
            const uint32_t v = rd32(buffer + i * 4) & 0x0FFFFFFFu;
            if (v != 0 && v != kBad && v < kEnd && (v < 2 || v >= clusters)) { report.where = c; return CardFault::BadFatEntry; }
        }
        if (nfats == 2 && !report.fatsDiffer) {
            if (!disk.read(fat1 + fatSize + s, buffer + half * kSector, n)) { report.where = fat1 + fatSize + s; return CardFault::ReadFailed; }
            report.fatsDiffer = memcmp(buffer, buffer + half * kSector, n * kSector) != 0;
        }
    }

    // ---- every folder from the root, every file in them. The folders wait in a ring.
    memset(map, 0, clusters / 8 + 1);
    Walk w{Fat{disk, buffer, fat1, fatSize}, Map{map}, clusters, report};
    static uint8_t sector[kSector];
    uint32_t head = 0, tail = 0;
    folders[tail++ % kCardCheckFolders] = root;
    while (head != tail) {
        const uint32_t first = folders[head++ % kCardCheckFolders];
        ++report.folders;
        uint32_t length = 0;
        CardFault f = w.chain(first, length);
        if (f != CardFault::None) return f;
        bool end = false;
        uint32_t endCluster = 0, endSector = 0, endByte = 0;
        for (uint32_t c = first; !end;) {
            for (uint32_t s = 0; s < spc && !end; ++s) {
                if (!disk.read(data + (c - 2) * spc + s, sector, 1)) return w.fault(CardFault::ReadFailed, c);
                for (uint32_t e = 0; e < kSector; e += kEntry) {
                    const uint8_t* d = sector + e;
                    if (d[0] == 0) { end = true; endCluster = c; endSector = s; endByte = e; break; }
                    if (d[0] == 0xE5 || (d[11] & 0x0F) == 0x0F || (d[11] & 0x08)) continue;   // deleted, a piece of a long name, the volume label
                    if (d[0] == '.' && (d[1] == ' ' || (d[1] == '.' && d[2] == ' '))) continue;
                    const uint32_t start = rd16(d + 20) << 16 | rd16(d + 26), size = rd32(d + 28);
                    if (d[11] & 0x10) {
                        if (start < 2 || start >= clusters) return w.fault(CardFault::BadFolder, c);
                        if (tail - head >= kCardCheckFolders) return CardFault::TooManyFolders;
                        folders[tail++ % kCardCheckFolders] = start;
                    } else {
                        ++report.files;
                        if (start == 0) {
                            if (size != 0) return w.fault(CardFault::ChainTooShort, c);
                            continue;
                        }
                        if (start < 2 || start >= clusters) return w.fault(CardFault::BadFolder, c);
                        f = w.chain(start, length);
                        if (f != CardFault::None) return f;
                        if ((uint64_t)length * spc * kSector < size) return w.fault(CardFault::ChainTooShort, start);
                    }
                }
            }
            if (end) break;
            uint32_t next;
            if (!w.fat.get(c, next)) return w.fault(CardFault::ReadFailed, c);
            if (next >= kEnd) break;
            c = next;   // (w.chain above has seen this chain)
        }
        // from the end mark to the end of the chain: all zeros, or a job for the caller
        for (uint32_t c = endCluster, s = endSector, skip = endByte; end;) {
            bool dirty = false;
            for (uint32_t k = s; k < spc && !dirty; k += kBufferSectors) {
                const uint32_t n = spc - k < kBufferSectors ? spc - k : kBufferSectors;
                w.fat.loaded = 0xFFFFFFFFu;   // (the same buffer)
                if (!disk.read(data + (c - 2) * spc + k, buffer, n)) return w.fault(CardFault::ReadFailed, c);
                for (uint32_t i = k == s ? skip : 0; i < n * kSector && !dirty; ++i) dirty = buffer[i] != 0;
            }
            if (dirty) {
                if (report.tailCount == kCardCheckTails) return w.fault(CardFault::BadFolder, c);
                FolderTail& t = report.tails[report.tailCount++];
                t.lba = data + (c - 2) * spc + s;
                t.sectors = spc - s;
                t.offset = skip;
            }
            uint32_t next;
            if (!w.fat.get(c, next)) return w.fault(CardFault::ReadFailed, c);
            if (next >= kEnd) break;
            c = next;
            s = skip = 0;
        }
    }
    return CardFault::None;
}

}  // namespace storage
