#ifndef __STORAGE_CARD_CHECK_H
#define __STORAGE_CARD_CHECK_H

#include <stddef.h>
#include <stdint.h>

// Is this card safe to write to? FatFs believes the FAT: a cluster marked free is free, whatever a directory says. A card an earlier build damaged, or one that
// went bad somewhere else, therefore gets more damaged by a healthy firmware (a take written into a folder's clusters is how one lost its presets). So before
// the first write after a start the whole file system is read, with no code shared with FatFs and nothing written: every FAT entry, every folder, every chain.
// The firmware then refuses to write to a card that fails, keeps reading from it, and never repairs anything. That is a job for a computer.
//
// What is refused is what makes allocation dangerous or shows a card is not what it says. What is harmless passes, and has to, or one power cut would end saving
// on a card for good: clusters allocated to nothing (lost), a chain longer than its file, the second FAT behind the first.
//
// One thing is handed back instead: old data after the end mark of a folder. Every reader stops at that mark, but a new file overwrites it, and what lay behind
// it comes to life as entries. Nothing a reader can see is there, so the caller clears it (the report's tails) before the first write.
namespace storage {

enum class CardFault : uint8_t {
    None,
    ReadFailed,      // a sector could not be read
    NoVolume,        // no FAT32 volume where FatFs found one (FAT12/16, on cards of 2 GB and less, is not checked and so not written), or a boot sector that makes no sense
    VolumeTooBig,    // the volume claims more sectors than the card has
    BootCopyDiffers, // the backup boot sector describes a different volume
    HardError,       // the FAT's flag says a disk error was seen and not fixed
    BadFatEntry,     // a FAT entry points outside the volume
    BadFolder,       // a folder's or a file's start cluster is outside the volume, or more folders hold old data past their end than the report has room for
    FreeInChain,     // a file runs through a cluster marked free: the next write would land on it
    CrossLink,       // two files share a cluster
    ChainTooShort,   // a file's chain ends before its size says
    TooManyClusters, // the map of the clusters does not fit (cards of 64 GB and up come as exFAT, which is not read at all)
    TooManyFolders,
};

const char* cardFaultName(CardFault f);   // for the screen and the debug mailbox

class SectorReader {
   public:
    virtual ~SectorReader() {}
    virtual bool read(uint32_t lba, uint8_t* buf, uint32_t count) = 0;   // false: not read
};

constexpr int kCardCheckTails = 16;

struct FolderTail {          // sectors of one cluster to clear, from a byte of the first
    uint32_t lba = 0, sectors = 0;
    uint32_t offset = 0;
};

struct CardReport {
    uint32_t clusters = 0, files = 0, folders = 0, lost = 0;
    bool dirty = false;       // the FAT's clean-shutdown flag is off: a computer lost the card while it was mounted
    bool fatsDiffer = false;
    uint32_t where = 0;       // the cluster or sector the fault was found at
    uint32_t fsInfoSector = 0;   // 0: the volume has none
    FolderTail tails[kCardCheckTails];
    int tailCount = 0;
};

constexpr size_t kCardCheckBuffer = 4096;      // whole sectors, 32-byte aligned for the SD driver
constexpr uint32_t kCardCheckFolders = 4096;   // folders the walk can hold at once

// `volume` is where FatFs mounted: what is checked has to be what gets written. `buffer` is kCardCheckBuffer bytes. `map` is one bit per cluster, `folders`
// kCardCheckFolders entries; both come from the caller, who has SDRAM.
CardFault checkCard(SectorReader& disk, uint32_t cardSectors, uint32_t volume, uint8_t* buffer, uint8_t* map, size_t mapBytes, uint32_t* folders, CardReport& report);

}  // namespace storage

#endif
