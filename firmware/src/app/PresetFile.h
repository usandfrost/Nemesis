#ifndef __APP_PRESET_FILE_H
#define __APP_PRESET_FILE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app/FxParams.h"
#include "app/Params.h"
#include "app/WaveLib.h"

// The preset library on the card (docs/PRESETS_AND_WAVETABLES.md): one small file per preset in /CATHODE/PRESETS (*.PRS, 148 bytes) and an
// INDEX.PIX that lists them (id, category, voices, banks, name, file name: 64 bytes each). The firmware keeps only the index in memory and reads
// a preset's file when it is previewed or loaded. The index is a cache of the folder: it records a fingerprint of the folder's listing (names,
// sizes, dates), and when that no longer matches (a file was added, replaced or removed) the firmware reads the files again and rewrites it.
// tools/prs.py makes and reads the files. Every number is little endian. Everything read from the card is checked: a file that fails is skipped.
namespace app {

constexpr int kPresetBodySize = 148;
constexpr int kPresetIndexHeader = 16;
constexpr int kPresetIndexEntry = 64;
constexpr int kPresetCap = 1024;           // presets on the card the firmware lists (64 KB of the index)
constexpr int kPresetIdMin = 10;           // 0..2 are the factory's, built in (3..9 stay unused)
constexpr int kPresetIdMax = 32000;        // a project stores the id in an int16
constexpr int kPresetFirstUser = 1000;     // a preset saved on the Drumboy gets the next id from here
constexpr int kPresetNameMax = 20;
constexpr int kPresetFileMax = 31;         // the file name, with its .PRS
constexpr int kPresetVoicesMax = 5;        // CATHODE parts have 1..5 voices
constexpr const char* kPresetDir = "CATHODE/PRESETS";
constexpr const char* kPresetIndexName = "INDEX.PIX";

// What the list needs: BROWSE draws these without opening any file.
struct PresetInfo {
    uint16_t id;
    uint8_t cat, voices;
    uint8_t bank[2];                       // the wavetable bank OSC 1 / OSC 2 plays (catalogue number), 255 none
    char name[kPresetNameMax + 2];
    char file[kPresetFileMax + 1];
};

// A preset in full, as the file holds it.
struct PresetRecord {
    uint16_t id;
    uint8_t cat, voices;
    uint8_t bank[2];
    char name[kPresetNameMax + 2];
    int8_t v[kSynthPages * kParamsPerPage];   // the nine pages' values; a bank's position is in the PD SHAPE place
    uint8_t send[3];                       // DELAY, REVERB, CHORUS sends
    uint8_t mask;                          // PM_*: which of the effect settings below the preset carries
    uint8_t amp[kFxParams], delay[kFxParams], reverb[kFxParams], chorus[kFxParams];
};
enum : uint8_t { PM_DELAY = 1, PM_REVERB = 2, PM_CHORUS = 4, PM_AMP = 8, PM_ALL = 15 };

enum class PresetParse : uint8_t { Ok, Size, Magic, Version, Crc, Field };

// CRC-32 (zlib's), in steps: state = presetCrcUpdate(state, bytes...) from 0xFFFFFFFF, the CRC is the complement.
inline uint32_t presetCrcUpdate(uint32_t c, const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}
inline uint32_t presetCrc(const uint8_t* d, size_t n) { return ~presetCrcUpdate(0xFFFFFFFFu, d, n); }

inline uint16_t presetRd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t presetRd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline void presetWr16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void presetWr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

// A name: 1..20 printable ASCII characters, then zeros to the end of the field.
inline bool presetNameOk(const char* f, int cap) {
    int n = 0;
    while (n < cap && f[n]) {
        if (f[n] < 0x20 || f[n] > 0x7E) return false;
        ++n;
    }
    if (n < 1 || n > kPresetNameMax || f[0] == ' ') return false;
    for (int i = n; i < cap; ++i) if (f[i]) return false;
    return true;
}

// Whether a file name may be listed: 1..27 characters of A-Z a-z 0-9 space _ - . ( ) before ".PRS".
inline bool presetFileNameOk(const char* f) {
    const size_t n = strlen(f);
    if (n < 5 || n > (size_t)kPresetFileMax) return false;
    const char* e = f + n - 4;
    if (e[0] != '.' || (e[1] | 32) != 'p' || (e[2] | 32) != 'r' || (e[3] | 32) != 's') return false;
    for (const char* c = f; c < e; ++c) {
        const char ch = *c;
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == ' ' || ch == '_' || ch == '-' || ch == '.' || ch == '(' || ch == ')';
        if (!ok) return false;
    }
    return f[0] != '.';
}

// The file name a new preset is saved under: "<id> <name>.PRS", the name's characters outside the allowed set replaced by "_". At most 30 characters.
inline void presetFileNameFor(int id, const char* name, char out[kPresetFileMax + 1]) {
    int n = 0;
    if (id >= 10000) out[n++] = (char)('0' + (id / 10000) % 10);   // (four digits at least; the largest id has five)
    out[n++] = (char)('0' + (id / 1000) % 10);
    out[n++] = (char)('0' + (id / 100) % 10);
    out[n++] = (char)('0' + (id / 10) % 10);
    out[n++] = (char)('0' + id % 10);
    out[n++] = ' ';
    for (int i = 0; name[i] && i < kPresetNameMax && n < kPresetFileMax - 4; ++i) {
        const char ch = name[i];
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == ' ' || ch == '_' || ch == '-' || ch == '.' || ch == '(' || ch == ')';
        out[n++] = ok ? ch : '_';
    }
    memcpy(out + n, ".PRS", 5);
}

// The fields every file and index entry shares, checked.
inline bool presetHeadOk(int id, int cat, int voices, const uint8_t bank[2], const char* name) {
    return id >= kPresetIdMin && id <= kPresetIdMax && cat >= 1 && cat < kPresetCatCount && voices >= 1 && voices <= kPresetVoicesMax &&
           (bank[0] == 255 || bank[0] < kWaveBanks) && (bank[1] == 255 || bank[1] < kWaveBanks) && presetNameOk(name, kPresetNameMax + 2);
}

// ---- a preset file: magic "DBPR", version 1, id, category, voices, banks, name[24], 72 values, sends, mask, amp[8], delay[8], reverb[8], chorus[8], CRC-32
// The values are brought into their knobs' ranges (a hand-made file may stray); the effects into their parameters'.
inline PresetParse parsePresetFile(const uint8_t* d, size_t n, PresetRecord& out) {
    if (n != (size_t)kPresetBodySize) return PresetParse::Size;
    if (memcmp(d, "DBPR", 4) != 0) return PresetParse::Magic;
    if (d[4] != 1 || d[5] != 0) return PresetParse::Version;
    if (presetCrc(d, kPresetBodySize - 4) != presetRd32(d + kPresetBodySize - 4)) return PresetParse::Crc;
    PresetRecord r;
    memset(&r, 0, sizeof(r));
    r.id = presetRd16(d + 6);
    r.cat = d[8];
    r.voices = d[9];
    r.bank[0] = d[10];
    r.bank[1] = d[11];
    memcpy(r.name, d + 12, kPresetNameMax + 2);
    if (!presetHeadOk(r.id, r.cat, r.voices, r.bank, r.name)) return PresetParse::Field;
    if (d[34] != 0 || d[35] != 0) return PresetParse::Field;   // the name field is 24 bytes: the last two are zero
    for (int i = 0; i < kSynthPages * kParamsPerPage; ++i) {
        const int page = i / kParamsPerPage, k = i % kParamsPerPage;
        const ParamDef& pd = pageDef(page).p[k];
        int v = (int8_t)d[36 + i];
        const int hi = (page == PG_OSC1 || page == PG_OSC2) && k == 0 ? kBasicWaves - 1 : pd.max;   // a bank is set by the bank field, not here
        v = v < pd.min ? pd.min : v > hi ? hi : v;
        r.v[i] = (int8_t)v;
    }
    for (int i = 0; i < 3; ++i) r.send[i] = d[108 + i] > 127 ? 127 : d[108 + i];
    r.mask = d[111];
    if (r.mask & ~PM_ALL) return PresetParse::Field;
    FxSettings fx;
    memset(&fx, 0, sizeof(fx));
    for (int i = 0; i < kFxParams; ++i) {
        fx.amp[0][i] = d[112 + i];
        fx.delay[i] = d[120 + i];
        fx.reverb[i] = d[128 + i];
        fx.chorus[i] = d[136 + i];
    }
    sanitizeFx(fx);
    for (int i = 0; i < kFxParams; ++i) {
        r.amp[i] = (uint8_t)fx.amp[0][i];
        r.delay[i] = (uint8_t)fx.delay[i];
        r.reverb[i] = (uint8_t)fx.reverb[i];
        r.chorus[i] = (uint8_t)fx.chorus[i];
    }
    out = r;
    return PresetParse::Ok;
}

inline void encodePresetFile(const PresetRecord& r, uint8_t out[kPresetBodySize]) {
    memset(out, 0, kPresetBodySize);
    memcpy(out, "DBPR", 4);
    out[4] = 1;
    presetWr16(out + 6, r.id);
    out[8] = r.cat;
    out[9] = r.voices;
    out[10] = r.bank[0];
    out[11] = r.bank[1];
    memcpy(out + 12, r.name, kPresetNameMax + 2);
    for (size_t i = 0; i < sizeof(r.v); ++i) out[36 + i] = (uint8_t)r.v[i];
    for (int i = 0; i < 3; ++i) out[108 + i] = r.send[i];
    out[111] = r.mask;
    memcpy(out + 112, r.amp, kFxParams);
    memcpy(out + 120, r.delay, kFxParams);
    memcpy(out + 128, r.reverb, kFxParams);
    memcpy(out + 136, r.chorus, kFxParams);
    presetWr32(out + kPresetBodySize - 4, presetCrc(out, kPresetBodySize - 4));
}

// ---- the index: a header ("DBPX", version 1, count, the folder's fingerprint), the entries (id, category, voices, banks, name[22], file[32], 4 spare),
// then the CRC-32 of all of it. Written and read one piece at a time, so the 64 KB it can reach never sits in memory.
inline size_t presetIndexSize(int count) { return (size_t)kPresetIndexHeader + (size_t)count * kPresetIndexEntry + 4; }

inline void encodePresetIndexHeader(int count, uint32_t fingerprint, uint8_t out[kPresetIndexHeader]) {
    memset(out, 0, kPresetIndexHeader);
    memcpy(out, "DBPX", 4);
    out[4] = 1;
    presetWr16(out + 6, (uint32_t)count);
    presetWr32(out + 8, fingerprint);
}
// false: not an index this firmware reads, or a count that is too big
inline bool decodePresetIndexHeader(const uint8_t* d, int& count, uint32_t& fingerprint) {
    if (memcmp(d, "DBPX", 4) != 0 || d[4] != 1 || d[5] != 0) return false;
    const int c = presetRd16(d + 6);
    if (c > kPresetCap) return false;
    count = c;
    fingerprint = presetRd32(d + 8);
    return true;
}
inline void encodePresetIndexEntry(const PresetInfo& e, uint8_t out[kPresetIndexEntry]) {
    memset(out, 0, kPresetIndexEntry);
    presetWr16(out, e.id);
    out[2] = e.cat;
    out[3] = e.voices;
    out[4] = e.bank[0];
    out[5] = e.bank[1];
    memcpy(out + 6, e.name, kPresetNameMax + 2);
    memcpy(out + 28, e.file, kPresetFileMax + 1);
}
// false: the fields are not valid
inline bool decodePresetIndexEntry(const uint8_t* p, PresetInfo& x) {
    memset(&x, 0, sizeof(x));
    x.id = presetRd16(p);
    x.cat = p[2];
    x.voices = p[3];
    x.bank[0] = p[4];
    x.bank[1] = p[5];
    memcpy(x.name, p + 6, kPresetNameMax + 2);
    memcpy(x.file, p + 28, kPresetFileMax + 1);
    x.file[kPresetFileMax] = 0;
    return presetHeadOk(x.id, x.cat, x.voices, x.bank, x.name) && presetFileNameOk(x.file);
}

// One file's share of the folder's fingerprint (FNV-1a over its name, size and date). The shares are added, so the order the folder lists does not matter.
inline uint32_t presetFileStamp(const char* name, uint32_t size, uint32_t stamp) {
    uint32_t h = 2166136261u;
    for (const char* c = name; *c; ++c) { h ^= (uint8_t)*c; h *= 16777619u; }
    const uint32_t w[2] = {size, stamp};
    for (int i = 0; i < 2; ++i)
        for (int k = 0; k < 4; ++k) { h ^= (w[i] >> (8 * k)) & 0xFF; h *= 16777619u; }
    return h;
}

}  // namespace app

#endif
