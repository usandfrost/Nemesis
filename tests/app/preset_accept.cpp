// What the firmware says about preset files a program made: the file holds records of 148 bytes, one after the other, and each must pass parsePresetFile and come back unchanged from
// encodePresetFile, with a file name the firmware would list. web/test/preset-edit.mjs writes every value the page's editor can produce into such a file.
//   preset_accept FILE              exit status 1 when a record is refused or comes back changed
//   preset_accept --refused FILE    the other way round: every record must be refused or changed (the check that the check can fail)
//   preset_accept --names FILE      prints the file name the firmware gives each record, for the test to compare with the page's
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "app/PresetFile.h"

using namespace app;

static const char* const kWhy[] = {"ok", "size", "magic", "version", "crc", "field"};

int main(int argc, char** argv) {
    const char* mode = argc == 3 ? argv[1] : "";
    const char* path = argc == 3 ? argv[2] : argc == 2 ? argv[1] : nullptr;
    const bool refused = !strcmp(mode, "--refused"), names = !strcmp(mode, "--names");
    if (!path || (argc == 3 && !refused && !names)) { fprintf(stderr, "usage: preset_accept [--refused|--names] FILE\n"); return 2; }
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    std::vector<uint8_t> all;
    uint8_t chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) > 0;) all.insert(all.end(), chunk, chunk + n);
    fclose(f);
    if (all.empty() || all.size() % kPresetBodySize) { fprintf(stderr, "%zu bytes is not a whole number of records\n", all.size()); return 2; }
    const size_t count = all.size() / kPresetBodySize;
    size_t good = 0, shown = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* d = all.data() + i * kPresetBodySize;
        PresetRecord r;
        const PresetParse p = parsePresetFile(d, kPresetBodySize, r);
        bool ok = p == PresetParse::Ok;
        const char* why = kWhy[(int)p];
        if (ok) {
            uint8_t back[kPresetBodySize];
            encodePresetFile(r, back);
            if (memcmp(back, d, kPresetBodySize) != 0) { ok = false; why = "came back changed"; }
        }
        if (ok) {
            char file[kPresetFileMax + 1];
            presetFileNameFor(r.id, r.name, file);
            if (!presetFileNameOk(file)) { ok = false; why = "its file name would not be listed"; }
            if (names) puts(file);
        }
        good += ok;
        if (ok == refused && shown < 10) { fprintf(stderr, "record %zu: %s\n", i, refused ? "was accepted" : why); ++shown; }
    }
    printf("%zu records, %zu accepted and unchanged\n", count, good);
    return (refused ? good == 0 : good == count) ? 0 : 1;
}
