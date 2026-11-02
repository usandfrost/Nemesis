// Draws the firmware's screen on the host, in the same states as the mockup reference shots (scratchpad ref/mockref.py), through the
// same strip rasteriser the board uses. Writes <out>/<state>.ppm.   render_screens <out dir> [state ...]
#include <stdio.h>
#include <string.h>

#include <functional>
#include <string>
#include <vector>

#include "CardLib.h"
#include "PresetHost.h"
#include "HostServices.h"
#include "app/Model.h"
#include "app/SampleLib.h"
#include "app/Ui.h"
#include "app/WaveLib.h"
#include "gfx/Canvas.h"
#include "gfx/Screen.h"

using namespace app;

struct Rig {
    Project* pj = new Project;
    Transport tp;
    SampleLib* lib = new SampleLib;
    std::vector<int16_t> store = std::vector<int16_t>(kFactoryFrames);
    HostServices sv;
    Ui* ui;
    Rig() {
        makeDefaultProject(*pj);
        buildFactorySamples(*lib, store.data(), store.size());
        ui = new Ui(*pj, tp, *lib, sv);
    }
    void key(Key k) { ui->keyDown(k); ui->keyUp(k); }
    Lane& lane(int i) { return pj->patterns[tp.pat].lanes[i]; }
    // a stock card's samples, on lane 8 (the sample browser's shots)
    std::deque<std::string> paths;
    void card() { fillCard(*lib, paths); ui->layerDown(7); ui->layerUp(7); }
    void folder(int enc, int n) { ui->keyDown(Key::Shift); for (int i = 0; i < n; ++i) ui->turn(enc, 1); }   // (SHIFT stays down)
    void pick(int enc, int n) { ui->keyUp(Key::Shift); for (int i = 0; i < n; ++i) ui->turn(enc, 1); }
};

static void write(const char* path, const std::vector<uint16_t>& fb) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", gfx::kWidth, gfx::kHeight);
    for (uint16_t p : fb) {
        const uint8_t r = (uint8_t)(((p >> 11) & 31) * 255 / 31), g = (uint8_t)(((p >> 5) & 63) * 255 / 63), b = (uint8_t)((p & 31) * 255 / 31);
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
}

int main(int argc, char** argv) {
    shippedPresets();   // the library, as the card would give it
    if (argc < 2) { fprintf(stderr, "usage: render_screens <out dir> [state ...]\n"); return 2; }
    // the states of the mockup reference shots, reached through the UI where the panel would reach them
    std::vector<std::pair<std::string, std::function<void(Rig&)>>> states = {
        {"play", [](Rig&) {}},
        {"play_sel", [](Rig& r) {
             r.ui->layerDown(3); r.ui->layerUp(3);
             r.lane(3).st[2].ov = 1; r.lane(3).st[1].setLock(lockKey(PG_FLT1, 1), 90); r.lane(3).st[4].chord = 2; r.lane(1).mute = 1;
         }},
        {"step1", [](Rig& r) {
             r.key(Key::StepK); r.lane(0).st[2].ov = 1; r.lane(0).st[0].setLock(lockKey(PG_FLT1, 1), 90); r.lane(0).st[4].chord = 1;
             r.key(Key::Right); r.key(Key::Right);
         }},
        {"step2", [](Rig& r) { r.key(Key::StepK); r.key(Key::StepK); r.lane(0).st[0].chord = 1; r.lane(0).st[4].chord = 8; r.lane(0).st[4].strum = 20; }},
        {"drum2", [](Rig& r) {
             r.ui->layerDown(7); r.ui->layerUp(7); r.key(Key::StepK); r.key(Key::StepK); r.key(Key::Right); r.key(Key::Right);
             r.lane(7).st[2].setLock(lockKey(PG_SMP, 0), 2);
         }},
        {"tracker", [](Rig& r) {
             for (int i = 0; i < 3; ++i) r.key(Key::StepK);
             r.lane(0).st[4].setLock(lockKey(PG_FLT1, 1), 90); r.lane(3).st[0].chord = 8; r.lane(7).st[4].setLock(lockKey(PG_SMP, 0), 2);
             r.lane(1).mute = 1; r.key(Key::Down); r.key(Key::Down);
         }},
        {"tracker_play", [](Rig& r) { for (int i = 0; i < 3; ++i) r.key(Key::StepK); r.tp.playing = 1; r.tp.shownStep = 5; r.ui->tick(); }},
        {"tracker_pop", [](Rig& r) {
             for (int i = 0; i < 3; ++i) r.key(Key::StepK);
             r.ui->layerDown(3); r.ui->layerUp(3);
             for (int i = 0; i < 5; ++i) r.key(Key::Down);
             r.key(Key::Ok); r.ui->turn(4, 3); r.ui->turn(6, 8); r.ui->turn(7, 1);
         }},
        {"tracker_flip", [](Rig& r) {
             for (int i = 0; i < 3; ++i) r.key(Key::StepK);
             r.ui->layerDown(6); r.ui->layerUp(6);
             for (int i = 0; i < 13; ++i) r.key(Key::Down);
             r.key(Key::Ok); r.ui->turn(6, 1);
         }},
        {"tracker_stop", [](Rig& r) {
             for (int i = 0; i < 3; ++i) r.key(Key::StepK);
             r.key(Key::Right); r.key(Key::Down);
             r.ui->keyDown(Key::Ok); for (int i = 0; i < 3; ++i) r.key(Key::Up); r.ui->keyUp(Key::Ok);
         }},
        {"tracker_sampler", [](Rig& r) {
             r.card(); for (int i = 0; i < 3; ++i) r.key(Key::StepK);
             r.key(Key::Down); r.key(Key::Ok); r.ui->turn(4, 3); r.ui->turn(6, 4);
         }},
        {"tracker_32", [](Rig& r) {   // lanes of 32, 24, 16, 12, 7 and 1 steps: the repeats are dim
             static const int kLens[kLanes] = {32, 24, 16, 16, 12, 7, 1, 16, 32, 16};
             for (int i = 0; i < kLanes; ++i) { Lane& l = r.lane(i); l.n = (uint8_t)kLens[i]; if (l.k > l.n) l.k = l.n; l.r = 0; }
             for (int i = 0; i < 3; ++i) r.key(Key::StepK);
             for (int i = 0; i < 18; ++i) r.key(Key::Down);
             r.tp.playing = 1; r.tp.shownStep = 21; r.ui->tick();
         }},
        {"tracker_lock", [](Rig& r) {
             for (int i = 0; i < 3; ++i) r.key(Key::StepK);
             r.key(Key::Down); r.key(Key::Down);
             r.ui->layerDown(0); r.sv.now += 400; r.ui->tick(); r.ui->layerUp(0);
         }},
        {"tracker_toast", [](Rig& r) { for (int i = 0; i < 3; ++i) r.key(Key::StepK); }},
        {"step_tabs", [](Rig& r) { r.key(Key::StepK); r.pj->key = 9; r.pj->scale = 8; r.lane(0).st[0].chord = 10; }},
        {"flt1", [](Rig& r) { r.key(Key::Flt1); }},
        {"amp", [](Rig& r) { r.key(Key::Amp); }},
        {"osc1", [](Rig& r) { r.pj->parts[0].pages[PG_OSC1][2] = 80; r.key(Key::Osc1); }},
        {"smp", [](Rig& r) { r.ui->layerDown(7); r.ui->layerUp(7); r.key(Key::Osc1); }},
        {"lock", [](Rig& r) {
             r.key(Key::StepK); r.ui->layerDown(4); r.sv.now += 400; r.ui->tick(); r.ui->layerUp(4);
             r.lane(0).st[4].setLock(lockKey(PG_FLT1, 1), 100); r.key(Key::Flt1); r.sv.now += 2000;
         }},
        {"mix1", [](Rig& r) { r.key(Key::Mixer); }},
        {"mix2", [](Rig& r) { r.key(Key::Mixer); r.key(Key::Mixer); r.sv.now += 2000; }},
        {"rec", [](Rig& r) { r.sv.recOn = true; r.key(Key::Rec); r.key(Key::Rec); static std::vector<int16_t> take(66150); for (size_t i = 0; i < take.size(); ++i) take[i] = (int16_t)(22000.0 * exp(-(double)(i % 22050) / 4000.0) * sin(i * 0.05)); r.sv.recNow.state = app::REC_TAKING; r.sv.recNow.data = take.data(); r.sv.recNow.frames = 66150; r.sv.recNow.peak = 0.5f; r.sv.now += 100; r.ui->tick(); }},
        {"mix4", [](Rig& r) { for (int i = 0; i < 4; ++i) r.key(Key::Mixer); r.sv.now += 2000; }},
        {"browse", [](Rig& r) { r.key(Key::File); r.key(Key::Down); r.key(Key::Down); }},
        {"project", [](Rig& r) { r.pj->key = 9; r.pj->scale = 2; r.key(Key::Info); }},
        {"osc-bank", [](Rig& r) { waveSet().present[4] = true; r.ui->layerDown(0); r.ui->layerUp(0); r.key(Key::Osc1); r.pj->parts[r.ui->part()].pages[PG_OSC1][0] = (int16_t)(kBasicWaves + 4); r.pj->parts[r.ui->part()].pages[PG_OSC1][1] = 70; }},
        {"osc-bank-missing", [](Rig& r) { r.ui->layerDown(0); r.ui->layerUp(0); r.key(Key::Osc1); r.pj->parts[r.ui->part()].pages[PG_OSC1][0] = (int16_t)(kBasicWaves + 77); }},
        {"fx-delay", [](Rig& r) { r.key(Key::Spare); }},
        {"fx-reverb", [](Rig& r) { r.key(Key::Spare); r.key(Key::Spare); }},
        {"fx-chorus", [](Rig& r) { for (int i = 0; i < 3; ++i) r.key(Key::Spare); }},
        {"fx-comp", [](Rig& r) { for (int i = 0; i < 4; ++i) r.key(Key::Spare); r.ui->turn(0, 1); r.ui->turn(1, 8); }},
        {"fx-amp", [](Rig& r) { for (int i = 0; i < 5; ++i) r.key(Key::Spare); for (int i = 0; i < 2; ++i) r.ui->turn(0, 1); r.ui->turn(2, 6); r.ui->turn(4, 8); }},
        {"fx-sends", [](Rig& r) { for (int i = 0; i < 6; ++i) r.key(Key::Spare); r.ui->turn(1, 40); r.ui->turn(2, 20); r.key(Key::PartK); r.ui->turn(3, 60); }},
        {"project-midi", [](Rig& r) { r.key(Key::Info); r.key(Key::Info); for (int i = 0; i < 8; ++i) r.ui->turn(0, 1); r.ui->tick(); }},
        {"save-new", [](Rig& r) {
             r.key(Key::File);
             int row = 0;
             for (int i = 0; i < r.ui->browseCount(); ++i) if (r.ui->browseItem(i) == 12) row = i;
             while (r.ui->browseIdx() < row) r.key(Key::Down);
             r.key(Key::Ok);
             r.pj->fx.send[r.ui->part()][0] = 40; r.pj->fx.amp[r.ui->part()][0] = 2;
             r.key(Key::Flt1);
             r.ui->keyDown(Key::Shift); r.key(Key::Ok); r.ui->keyUp(Key::Shift);
             for (int i = 0; i < 6; ++i) r.key(Key::Right);
         }},
        {"save-replace", [](Rig& r) {
             r.key(Key::File);
             int row = 0;
             for (int i = 0; i < r.ui->browseCount(); ++i) if (r.ui->browseItem(i) == 12) row = i;
             while (r.ui->browseIdx() < row) r.key(Key::Down);
             r.key(Key::Ok);
             r.key(Key::Flt1);
             r.ui->keyDown(Key::Shift); r.key(Key::Ok); r.ui->keyUp(Key::Shift);
             r.ui->turn(3, 1);
             r.key(Key::Ok);
         }},
        {"fx-lock", [](Rig& r) {
             r.key(Key::StepK);
             r.ui->layerDown(2); r.sv.now += 400; r.ui->tick(); r.ui->layerUp(2);
             r.key(Key::Spare);
             r.ui->turn(2, 40); r.ui->turn(1, 12);
         }},
        {"fx-lock-amp", [](Rig& r) {
             r.key(Key::StepK);
             r.pj->fx.amp[r.ui->part()][0] = 2;
             r.ui->layerDown(2); r.sv.now += 400; r.ui->tick(); r.ui->layerUp(2);
             r.key(Key::Spare); r.key(Key::Spare);
             r.ui->turn(1, 30);
         }},
        {"picker-play", [](Rig& r) { r.card(); r.folder(4, 1); r.pick(4, 12); }},
        {"picker-folder", [](Rig& r) { r.card(); r.folder(4, 3); }},
        {"picker-moved", [](Rig& r) { r.card(); r.folder(4, 3); r.pick(4, 0); }},
        {"picker-end", [](Rig& r) { r.card(); r.folder(4, 2); r.pick(4, 500); }},
        {"picker-sound", [](Rig& r) { r.card(); r.key(Key::Osc1); r.folder(0, 1); r.pick(0, 12); }},
        {"picker-step", [](Rig& r) {
             r.card(); r.key(Key::StepK); r.key(Key::StepK);
             r.ui->layerDown(2); r.ui->layerUp(2); r.lane(7).st[2].ov = 0;
             r.folder(4, 2); r.pick(4, 5);
         }},
        {"picker-lock", [](Rig& r) {
             r.card(); r.key(Key::StepK);
             r.ui->layerDown(2); r.sv.now += 400; r.ui->tick(); r.ui->layerUp(2);
             r.key(Key::Osc1); r.folder(0, 1); r.pick(0, 4);
         }},
        {"picker-file", [](Rig& r) { r.card(); r.folder(4, 2); r.pick(4, 12); r.key(Key::File); }},
        {"song", [](Rig& r) { r.key(Key::SongK); r.key(Key::Down); }},
        {"toast", [](Rig& r) { r.key(Key::Mute); r.key(Key::Mute); r.ui->say("LANE 1 MUTED"); }},
        {"step_empty", [](Rig& r) { r.key(Key::StepK); r.lane(0).eu = 0; r.key(Key::Right); }},
        {"cpu", [](Rig& r) {   // the default project with its effects in use: what the CPU view is there for
             r.pj->fx.send[0][1] = 40; r.pj->fx.send[1][0] = 50; r.pj->fx.send[1][1] = 60; r.pj->fx.send[2][1] = 20;
             for (int i = 0; i < 4; ++i) r.key(Key::Mixer);
             r.sv.now += 2000;
         }},
        {"slice", [](Rig& r) {   // a two-bar loop mixed from the built-in drums, cut by the firmware's own slicer at its attacks
             const int kick = r.lib->find("@Kick 808"), snare = r.lib->find("@Snare Crack"), hat = r.lib->find("@Hat Closed");
             const SampleItem kItem = r.lib->items[kick < 0 ? 0 : kick], sItem = r.lib->items[snare < 0 ? 1 : snare], hItem = r.lib->items[hat < 0 ? 2 : hat];
             const uint32_t rate = kItem.rate, stepLen = rate / 8, n = stepLen * 32;   // 120 BPM: an eighth of a second a step
             static std::vector<int16_t> loop;
             loop.assign(n, 0);
             auto hit = [&](const SampleItem& it, uint32_t at, float gain) { for (uint32_t i = 0; i < it.length && at + i < n; ++i) loop[at + i] = (int16_t)(loop[at + i] + it.data[i] * gain); };
             static const char* const kPattern = "K.h.S.h.K.hKS.h.K.h.S.hhK.hKS.hS";
             for (uint32_t s = 0; s < 32; ++s) { const char c = kPattern[s]; if (c == 'K') hit(kItem, s * stepLen, 0.8f); else if (c == 'S') hit(sItem, s * stepLen, 0.7f); else if (c == 'h') hit(hItem, s * stepLen, 0.35f); }
             SampleItem& it = r.lib->items[r.lib->count];
             it = kItem;
             snprintf(it.name, sizeof(it.name), "Drum Loop");
             it.length = n; it.data = loop.data();
             const int index = r.lib->count++;
             r.ui->layerDown(7); r.ui->layerUp(7);
             r.lane(7).smpParam(PG_SMP, 0) = (int16_t)index;
             r.key(Key::Osc2);
             r.ui->turn(0, 2);        // MODE: SYNC
             r.sv.now += 200; r.ui->tick();
         }},
    };
    static uint8_t arena[192 * 1024];
    static uint16_t strip[gfx::kWidth * 20];
    std::vector<uint16_t> fb(gfx::kWidth * gfx::kHeight);
    int done = 0;
    for (auto& [name, setup] : states) {
        bool want = argc == 2;
        for (int i = 2; i < argc; ++i) want = want || name == argv[i];
        if (!want) continue;
        Rig r;
        setup(r);
        if (name.rfind("picker", 0) != 0 && name != "tracker_toast") r.sv.now += 1500;   // past the toasts the setup raised (the "toast" state raises its own last; the picker's are part of the picture)
        if (name == "toast") r.ui->say("LANE 1 MUTED");
        gfx::Canvas cv(arena, sizeof(arena));
        gfx::drawScreen(cv, *r.ui);
        if (cv.overflowed()) fprintf(stderr, "%s: display list overflowed\n", name.c_str());
        for (int y = 0; y < gfx::kHeight; y += 20) {   // strip by strip, as on the board
            const int rows = gfx::kHeight - y < 20 ? gfx::kHeight - y : 20;
            cv.raster(strip, y, rows);
            memcpy(&fb[(size_t)y * gfx::kWidth], strip, sizeof(uint16_t) * gfx::kWidth * rows);
        }
        write((std::string(argv[1]) + "/" + name + ".ppm").c_str(), fb);
        printf("%s: %zu bytes of display list\n", name.c_str(), cv.used());
        ++done;
    }
    return done ? 0 : 1;
}
