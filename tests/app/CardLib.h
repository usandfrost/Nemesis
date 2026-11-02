#pragma once
// A card-sized sample list for the tests: what Card::indexSamples gives on a stock card, 2,500 samples in 20 folders sorted by path, after the built-in twelve.
#include <stdio.h>
#include <string.h>

#include <deque>
#include <string>

#include "app/SampleLib.h"

// `empty`: one more folder with no WAV in it, as a card can have. The paths live in `paths`.
inline void fillCard(app::SampleLib& lib, std::deque<std::string>& paths, int total = 2500, bool empty = true) {
    static const char* const kFolders[20] = {"Kick", "Snare", "Hihat", "Clap", "Perc", "Tom", "Cymbal", "Bass", "Synth", "Pad", "Lead", "Vocal", "FX", "Loop", "Break", "Noise", "Foley", "Stab", "Vinyl", "Texture"};
    int done = 0;
    for (int f = 0; f < 20; ++f) {
        char cat[12];
        snprintf(cat, sizeof cat, "%s", kFolders[f]);
        for (char* c = cat; *c; ++c) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
        const int c = lib.addCategory(cat), n = (total - done) / (20 - f);
        for (int i = 0; i < n; ++i) {
            char name[64];
            snprintf(name, sizeof name, "%s_%03d", kFolders[f], i + 1);
            paths.push_back(std::string(kFolders[f]) + "/" + name + ".wav");
            app::SampleItem& it = lib.items[lib.count++];
            memset(&it, 0, sizeof(it));
            snprintf(it.name, sizeof it.name, "%s", name);
            it.cat = (uint8_t)c;
            it.path = paths.back().c_str();
        }
        done += n;
    }
    if (empty) lib.addCategory("EMPTY");
    lib.sortedEnd = lib.count;
}
