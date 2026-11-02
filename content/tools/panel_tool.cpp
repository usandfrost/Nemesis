// panel_tool: facts about the Drumboy panel for the Python tools, taken from the firmware sources themselves.
//   panel_tool ranges            JSON: per page, per knob: name, min, max, def, list names
//   panel_tool format            stdin: "P <page 0..8> v0..v7" (a synth page) or "F <view 0..4> v0..v7" (DELAY REVERB CHORUS COMP AMP); stdout: per line a JSON array of the
//                                8 knob values as the panel shows them (formatParam / formatFx), and the FX knobs' names for F lines
//   panel_tool factory           the three factory presets: per line 72 integers (the nine pages after applyPreset on the default pages)
//   panel_tool cost [voices]     stdin: one preset per line, 72 integers (9 pages x 8, page order OSC1..GLOB); stdout: per-voice cost
//                                (app::descOf + cost::voiceCost with the firmware's settings), the heavy-rule verdict, voices that fit
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/CostBridge.h"
#include "app/FxParams.h"
#include "app/Model.h"
#include "app/Params.h"

using namespace app;

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "ranges")) {
        printf("{\"pages\":[\n");
        for (int g = 0; g < kSynthPages; ++g) {
            const PageDef& pd = pageDef(g);
            printf(" {\"page\":\"%s\",\"key\":\"%s\",\"p\":[", pd.title, pageKey(g));
            for (int i = 0; i < kParamsPerPage; ++i) {
                const ParamDef& p = pd.p[i];
                printf("%s{\"name\":\"%s\",\"min\":%d,\"max\":%d,\"def\":%d", i ? "," : "", p.name, p.min, p.max, p.def);
                if (p.list) {
                    printf(",\"list\":[");
                    for (int k = p.min; k <= p.max; ++k) printf("%s\"%s\"", k > p.min ? "," : "", p.list[k]);
                    printf("]");
                }
                printf("}");
            }
            printf("]}%s\n", g + 1 < kSynthPages ? "," : "");
        }
        printf("],\"cats\":[");
        for (int i = 0; i < kPresetCatCount; ++i) printf("%s\"%s\"", i ? "," : "", kPresetCats[i]);
        printf("],\"budget\":%d}\n", kVoiceBudget);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "format")) {
        char line[1024];
        while (fgets(line, sizeof line, stdin)) {
            char kind = 0;
            int idx = 0, v[8];
            char* s = line;
            while (*s == ' ') ++s;
            kind = *s++;
            char* e;
            idx = (int)strtol(s, &e, 10);
            s = e;
            int n = 0;
            for (; n < 8; ++n) {
                const long x = strtol(s, &e, 10);
                if (e == s) break;
                v[n] = (int)x;
                s = e;
            }
            if (n != 8 || (kind == 'P' && (idx < 0 || idx >= kSynthPages)) || (kind == 'F' && (idx < 0 || idx > FX_AMP)) || (kind != 'P' && kind != 'F')) { printf("ERR\n"); continue; }
            printf("[");
            for (int i = 0; i < 8; ++i) {
                Text t;
                if (kind == 'P') formatParam(t, idx, i, v[i], nullptr);
                else formatFx(t, fxDef(idx, i), v[i]);
                printf("%s\"%s\"", i ? "," : "", t.c_str());
            }
            printf("]\n");
        }
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "factory")) {
        for (int k = 0; k < kBasePresets; ++k) {
            int16_t pg[kSynthPages][kParamsPerPage];
            defaultSynthPages(pg);
            applyPresetIndex(pg, k);
            for (int g = 0; g < kSynthPages; ++g)
                for (int i = 0; i < kParamsPerPage; ++i) printf("%d ", pg[g][i]);
            printf("\n");
        }
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "cost")) {
        static Project pj;
        makeDefaultProject(pj);
        char line[4096];
        while (fgets(line, sizeof line, stdin)) {
            int v[72];
            char* s = line;
            int n = 0;
            for (; n < 72; ++n) {
                char* e;
                const long x = strtol(s, &e, 10);
                if (e == s) break;
                v[n] = (int)x;
                s = e;
            }
            if (n != 72) { printf("ERR\n"); continue; }
            Part& p = pj.parts[0];
            for (int g = 0; g < kSynthPages; ++g)
                for (int i = 0; i < kParamsPerPage; ++i) p.pages[g][i] = (int16_t)v[g * 8 + i];
            const cost::VoiceDesc d = descOf(pj, 0);
            const float c = cost::voiceCost(d, kFwCost);
            const int fit = (int)(kVoiceBudget / c + 1e-4f);
            printf("%.4f %d %d\n", c, cost::heavyRuleOk(d) ? 1 : 0, fit > 8 ? 8 : fit);
        }
        return 0;
    }
    fprintf(stderr, "usage: panel_tool ranges | format | factory | cost\n");
    return 2;
}
