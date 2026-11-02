// The preset protocol's device side on the host, for web/test/link.mjs: the website's real client talks to the real firmware code, with a folder for a card.
//   link_bridge FOLDER    lines in: a message in hex, or "tick MS". Lines out: what the device sends, in hex.
// Every second send finds the socket busy, as the real one often will, so the waiting path runs too.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../app/PresetHost.h"
#include "midi/PresetLink.h"

using namespace app;

static bool busyNext = false;
static bool sendOut(const uint8_t* b, int n) {
    busyNext = !busyNext;
    if (busyNext) return false;
    for (int i = 0; i < n; ++i) printf("%02x", b[i]);
    printf("\n");
    fflush(stdout);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    static DirPresets store(argv[1], std::string(argv[1]) + ".pix");
    static PresetInfo mem[kPresetCap];
    presetsInit(mem, kPresetCap);
    presetsOpen(store);
    midi::PresetLink link(sendOut, [](const char*) {});
    link.setSongMidi([](int song, uint32_t& size) -> const uint8_t* {   // (the file itself is tested on its own, app_test: this is the road)
        static uint8_t file[1000];
        if (song > 7) return nullptr;
        for (size_t i = 0; i < sizeof file; ++i) file[i] = (uint8_t)(i * 3 + song);
        size = sizeof file;
        return file;
    });
    uint32_t now = 1000;
    static char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        if (!strncmp(line, "tick ", 5)) {
            now += (uint32_t)atoi(line + 5);
        } else {
            std::vector<uint8_t> m;
            for (const char* p = line; p[0] && p[1] && p[0] != '\n'; p += 2) { char h[3] = {p[0], p[1], 0}; m.push_back((uint8_t)strtol(h, nullptr, 16)); }
            link.receive(m.data(), (int)m.size(), now);
        }
        for (int i = 0; i < 5000; ++i) link.step(now);
    }
    return 0;
}
