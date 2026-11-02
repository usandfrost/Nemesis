#ifndef __APP_SAMPLE_LIB_H
#define __APP_SAMPLE_LIB_H

#include <stddef.h>
#include <stdint.h>

// The samples the sampler parts can play: the SD card's (the board indexes every WAV in /Sample/<category>/ at boot and loads a sample's
// audio into an SDRAM cache when a project or the panel needs it), or the twelve drum sounds the mockup synthesises
// (buildFactorySamples), so the instrument always has drums. Mono, 16 bit. A sample whose audio is not loaded has data == nullptr and
// plays nothing.
namespace app {

constexpr int kMaxSamples = 3072;     // the stock card has 2500
constexpr int kMaxSampleCats = 48;    // and 19 folders, each one with no built-in namesake on top of the built-in six and ALL (past the end a folder lands in the last category; the place after that is REC's)
constexpr int kSampleNameLen = 24;

struct SampleItem {
    char name[kSampleNameLen];
    uint8_t cat;                 // index into SampleLib::cats (never 0: 0 is ALL)
    uint32_t length;             // frames
    uint32_t rate;               // Hz
    const int16_t* data;         // nullptr: not loaded (yet)
    const char* path;            // the file on the card (nullptr: built in)
    uint8_t bad;                 // the file could not be read: not tried again until the card is indexed again
};

struct SampleLib {
    int count = 0;
    int sortedEnd = 0;                   // where the card's sorted run ends: what comes later (takes) is in no order. 0: all of it is sorted
    SampleItem items[kMaxSamples];
    int catCount = 0;            // cats[0] is "ALL"
    char cats[kMaxSampleCats][12];

    int addCategory(const char* name);   // returns its index (existing or new)
    // A sample's lasting name, for a project file: its path on the card ("Kick/Altair_1.wav"), or "@" + the name of a built-in one.
    void keyOf(int i, char* out, int size) const;
    int find(const char* key) const;     // the sample with that key now, or -1. Capitals do not count, as on the card: Rec/ is found for REC/
    // The categories (the card's folders), walked by .cat in list order: a category's samples need not sit together (built-in ones, takes).
    int catSize(int c) const;            // how many samples category c has (0, ALL: every one)
    int catPlace(int i) const;           // sample i's place among its category's, from 0
    int catFirst(int c) const;           // the category's first sample, or -1
    int catNext(int i, int dir) const;   // the next sample of i's category that way, or -1 at its end
    int catStep(int c, int dir, bool all) const;   // the nearest category that way that has samples, or c at the end. `all`: ALL is one of them
    int catsInUse(int c, int& place) const;        // how many categories have samples, and c's place among them, from 0
    // Makes room in a sample cache (one block, filled from `base`, `used` frames so far): the samples kept (keep[i]) move down to close
    // the gaps, the others are dropped (data = nullptr). Returns the frames now used. Nothing may be playing from the cache meanwhile.
    size_t compactCache(int16_t* base, size_t used, const bool* keep);
    float seconds(int i) const { return items[i].rate ? (float)items[i].length / (float)items[i].rate : 0.0f; }
};

// The mockup's twelve synthesised drum samples (same seed, same formulas). `storage` must hold kFactoryFrames int16 values.
constexpr uint32_t kFactoryRate = 44100;
constexpr size_t kFactoryFrames = 285500;
void buildFactorySamples(SampleLib& lib, int16_t* storage, size_t capacity);

}  // namespace app

#endif
