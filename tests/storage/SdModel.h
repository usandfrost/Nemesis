#pragma once
#include <stddef.h>
#include <stdint.h>

// The hardware under the real SD driver and FatFs: a card in memory, the SDMMC's DMA, and the Cortex-M7 data cache, built to be as unhelpful as the real ones
// are allowed to be.
//
// The cache: host memory is what the core sees, and every 32-byte line a transfer touches has a second copy, what RAM holds. They differ wherever a line is
// dirty. The DMA works on RAM only. A line is assumed cached until invalidated (the M7 fetches ahead, so a buffer can be in the cache at any time), and a dirty
// line may be evicted at any moment, including on top of what a DMA has just delivered.
namespace sd {

struct Faults {
    uint32_t seed = 1;
    int evictPercent = 25;         // of the dirty lines, how many fall out of the cache during a transfer
    int programmingPolls = 3;      // status polls for which the card is still programming after a write
    bool directAll = false;        // every aligned buffer goes direct (the firmware decides by address; the host cannot)
    // cut the power after this many sectors have landed (<0: never); the next sector lands torn at a random byte if `torn`
    long cutAfterSectors = -1;
    bool torn = false;
    int lostOnCut = 0;             // sectors the card had acknowledged but not committed when the power went: they revert
    long transferFails = -1;       // the n-th transfer from now fails with a CRC error, once
    long transferHangs = -1;       // the n-th transfer from now never completes unless aborted; it lands late if it is not
    long writeRejected = -1;       // the n-th write from now is refused: no data lands and R1 says so at the next status poll
    long stopFails = -1;           // the n-th write from now fails at its stop command, after the data has gone
    long statusStuck = -1;         // the card never leaves programming, from the n-th write from now
    // a build that forgot its cache maintenance: the regression the model has to catch
    bool noClean = false, noInvalidateBefore = false, noInvalidateAfter = false;
};

extern Faults faults;

struct Stats {
    long transfers = 0, sectorsWritten = 0, sectorsRead = 0, dirtyOverDma = 0, staleToCard = 0;
    long busyViolations = 0;       // a command sent while the card was programming
    long misaligned = 0;           // cache maintenance on an address that is not a line boundary: it would hit a neighbour
    long lateLandings = 0;         // a transfer that completed after the driver had given up on it
};
extern Stats stats;

void open(uint32_t sectors);       // an empty card, everything reset
uint8_t* image();
uint32_t sectors();
void powerCycle();                 // RAM and cache gone, the card stays
bool poweredOff();
void resetCounts();
void saveImage(const char* path);
void loadImage(const void* data, size_t bytes);

// A blank FAT32 volume on the card, in a partition, the way a card is sold. Nothing of FatFs is used for it.
void format(uint32_t sectorsPerCluster);

}  // namespace sd
