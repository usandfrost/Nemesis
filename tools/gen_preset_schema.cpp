// The preset editor's data (web/www/preset-schema.js), made from the firmware's own tables: the pages, the knobs, their ranges and list choices, what the panel writes for every value,
// the effects, the categories, the wavetable banks and where each field sits in a .PRS file. The editor reads this and holds no copy of its own, so it cannot drift from the firmware.
//   tests/build/preset_schema content/data/wavetables > web/www/preset-schema.js
// (the folder holds the banks that exist: wtNNN.wt, the source of the wtNNN.wtp files that go on the card). tools/check_all.sh fails when the committed file differs from this output.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "app/PresetFile.h"

using namespace app;

namespace {

std::string quoted(const char* s) {
    std::string o = "\"";
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') o += '\\';
        o += *s;
    }
    return o + "\"";
}
std::string quoted(const std::string& s) { return quoted(s.c_str()); }

// What the panel writes for each value of a knob, kept once: many knobs share a table.
std::vector<std::string> gTables;
std::map<std::string, int> gTableAt;
int tableOf(const std::vector<std::string>& texts) {
    std::string row = "[";
    for (size_t i = 0; i < texts.size(); ++i) row += (i ? "," : "") + quoted(texts[i]);
    row += "]";
    const auto it = gTableAt.find(row);
    if (it != gTableAt.end()) return it->second;
    gTables.push_back(row);
    return gTableAt[row] = (int)gTables.size() - 1;
}

std::string sound(int page, int k) {
    const ParamDef& d = pageDef(page).p[k];
    char head[160];
    const bool wave = d.unit == Unit::Wave;
    // WAVE: the file holds a basic wave, and a bank is in the bank field (parsePresetFile), so the range here is the file's. The panel's is the same number with the banks after it.
    snprintf(head, sizeof head, "{\"name\":%s,\"min\":%d,\"max\":%d,\"def\":%d", quoted(d.name).c_str(), d.min, wave ? kBasicWaves - 1 : d.max, d.def);
    std::string o = head;
    if (wave) return o + ",\"wave\":true}";
    if (strcmp(d.name, "-") == 0) return o + ",\"unused\":true}";
    std::vector<std::string> texts;
    for (int v = d.min; v <= d.max; ++v) {
        Text t;
        formatParam(t, page, k, v, nullptr);
        texts.push_back(t.c_str());
    }
    if (d.list) o += ",\"list\":true";
    return o + ",\"text\":" + std::to_string(tableOf(texts)) + "}";
}

std::string effect(int view, int k) {
    const FxParamDef& d = fxDef(view, k);
    std::vector<std::string> texts;
    for (int v = 0, hi = fxMax(d); v <= hi; ++v) {
        Text t;
        formatFx(t, d, v);
        texts.push_back(t.c_str());
    }
    // (a send has no default of its own: defaultFx() starts them all at 0, as its table says)
    char head[160];
    snprintf(head, sizeof head, "{\"name\":%s,\"min\":0,\"max\":%d,\"def\":%d", quoted(d.name).c_str(), fxMax(d), fxDefault(view, k));
    std::string o = head;
    if (d.list) o += ",\"list\":true";
    return o + ",\"text\":" + std::to_string(tableOf(texts)) + "}";
}

// Where a field sits: the first byte that changes in the encoded file when the record's field does.
template <typename Poke>
int placeOf(Poke poke) {
    PresetRecord a;
    memset(&a, 0, sizeof(a));
    PresetRecord b = a;
    poke(b);
    uint8_t x[kPresetBodySize], y[kPresetBodySize];
    encodePresetFile(a, x);
    encodePresetFile(b, y);
    for (int i = 0; i < kPresetBodySize - 4; ++i)
        if (x[i] != y[i]) return i;
    fprintf(stderr, "a record's field does not reach the file\n");
    exit(1);
}

std::vector<int> existingBanks(const char* folder) {
    std::vector<int> out;
    DIR* d = opendir(folder);
    if (!d) { fprintf(stderr, "no folder %s\n", folder); exit(1); }
    while (const dirent* e = readdir(d)) {
        int n = 0, used = 0;
        if (sscanf(e->d_name, "wt%3d.wt%n", &n, &used) == 1 && used == (int)strlen(e->d_name) && n >= 0 && n < kWaveBanks) out.push_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

void strings(const char* key, int n, const char* (*get)(int), const char* end) {
    printf("\"%s\":[", key);
    for (int i = 0; i < n; ++i) printf("%s%s", i ? "," : "", quoted(get(i)).c_str());
    printf("]%s\n", end);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: preset_schema <folder of wtNNN.wt>\n"); return 2; }
    const std::vector<int> banks = existingBanks(argv[1]);
    if (banks.empty()) { fprintf(stderr, "no banks in %s\n", argv[1]); return 1; }

    std::vector<std::string> pages;
    for (int p = 0; p < kSynthPages; ++p) {
        std::string o = "{\"key\":" + quoted(pageKey(p)) + ",\"title\":" + quoted(pageDef(p).title) + ",\"knobs\":[\n";
        for (int k = 0; k < kParamsPerPage; ++k) o += "  " + sound(p, k) + (k + 1 < kParamsPerPage ? ",\n" : "]}");
        pages.push_back(o);
    }
    struct Fx { int view; const char* field; int mask; int at; };
    const Fx fx[] = {
        {FX_DELAY, "delay", PM_DELAY, placeOf([](PresetRecord& r) { r.delay[0] = 1; })},
        {FX_REVERB, "reverb", PM_REVERB, placeOf([](PresetRecord& r) { r.reverb[0] = 1; })},
        {FX_CHORUS, "chorus", PM_CHORUS, placeOf([](PresetRecord& r) { r.chorus[0] = 1; })},
        {FX_AMP, "amp", PM_AMP, placeOf([](PresetRecord& r) { r.amp[0] = 1; })},
        {FX_SENDS, "sends", 0, placeOf([](PresetRecord& r) { r.send[0] = 1; })},
    };
    std::vector<std::string> effects;
    for (const Fx& e : fx) {
        std::string key;
        for (const char* c = fxViewName(e.view); *c; ++c) if (*c != ' ') key += *c;   // ("AMP SIM": a key, and AMP is a sound page)
        std::string o = "{\"key\":" + quoted(key) + ",\"title\":" + quoted(fxViewName(e.view)) + ",\"field\":" + quoted(e.field) + ",\"mask\":" + std::to_string(e.mask) + ",\"at\":" + std::to_string(e.at) + ",\"knobs\":[\n";
        const int n = fxCount(e.view);
        for (int k = 0; k < n; ++k) o += "  " + effect(e.view, k) + (k + 1 < n ? ",\n" : "]}");
        effects.push_back(o);
    }

    printf("// GENERATED by tools/gen_preset_schema.cpp from the firmware's tables. Do not edit: tools/check_all.sh fails when this differs from what the tool makes.\n");
    printf("globalThis.PresetSchema = {\n");
    printf("\"format\":{\"size\":%d,\"idMin\":%d,\"idMax\":%d,\"firstUser\":%d,\"cap\":%d,\"nameMax\":%d,\"nameField\":%d,\"voicesMax\":%d,\"banks\":%d,\"basicWaves\":%d,\"noBank\":255,\"pages\":%d,\"perPage\":%d,\n",
           kPresetBodySize, kPresetIdMin, kPresetIdMax, kPresetFirstUser, kPresetCap, kPresetNameMax, kPresetNameMax + 2, kPresetVoicesMax, kWaveBanks, kBasicWaves, kSynthPages, kParamsPerPage);
    printf(" \"at\":{\"id\":%d,\"cat\":%d,\"voices\":%d,\"bank\":%d,\"name\":%d,\"values\":%d,\"mask\":%d,\"crc\":%d}},\n",
           placeOf([](PresetRecord& r) { r.id = 1; }), placeOf([](PresetRecord& r) { r.cat = 1; }), placeOf([](PresetRecord& r) { r.voices = 1; }), placeOf([](PresetRecord& r) { r.bank[0] = 1; }),
           placeOf([](PresetRecord& r) { r.name[0] = 1; }), placeOf([](PresetRecord& r) { r.v[0] = 1; }), placeOf([](PresetRecord& r) { r.mask = 1; }), kPresetBodySize - 4);
    strings("cats", kPresetCatCount, [](int i) { return kPresetCats[i]; }, ",");
    printf("\"wave\":{");
    strings("basic", kBasicWaves, [](int i) { static Text t; t.clear(); formatWave(t, i); return t.c_str(); }, ",");
    strings("bankNames", kWaveBanks, waveBankName, ",");
    printf("\"bankCat\":[");
    for (int b = 0; b < kWaveBanks; ++b) printf("%s%d", b ? "," : "", waveBankCategory(b));
    printf("],\n");
    strings("cats", 6, waveCategoryName, ",");
    printf("\"exist\":[");
    for (size_t i = 0; i < banks.size(); ++i) printf("%s%d", i ? "," : "", banks[i]);
    printf("]},\n\"pages\":[\n");
    for (size_t i = 0; i < pages.size(); ++i) printf("%s%s\n", pages[i].c_str(), i + 1 < pages.size() ? "," : "");
    printf("],\n\"fx\":[\n");
    for (size_t i = 0; i < effects.size(); ++i) printf("%s%s\n", effects[i].c_str(), i + 1 < effects.size() ? "," : "");
    printf("],\n\"texts\":[\n");
    for (size_t i = 0; i < gTables.size(); ++i) printf("%s%s\n", gTables[i].c_str(), i + 1 < gTables.size() ? "," : "");
    printf("]};\n");
    return 0;
}
