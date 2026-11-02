#ifndef __APP_PARAMS_H
#define __APP_PARAMS_H

#include <stddef.h>
#include <stdint.h>

#include "app/Fmt.h"
#include "app/FxParams.h"

// The sound pages: eight labelled parameters each, as in the mockup's soundPages() and samplerPages(). A CATHODE part has the nine
// synth pages; a sampler part has seven pages per lane (each lane is its own sample) plus the part's GLOBAL page.
namespace app {

enum PageId : uint8_t {
    PG_OSC1, PG_OSC2, PG_SUB, PG_FLT1, PG_FLT2, PG_GRIT, PG_AMP, PG_MOD, PG_GLOB,   // synth (a part's pages, in this order)
    PG_SMP, PG_SFLT, PG_SAMP, PG_SLC, PG_LOOP,                                       // sampler-only pages
    PG_COUNT
};
constexpr int kSynthPages = 9;    // PG_OSC1 .. PG_GLOB
constexpr int kLanePages = 7;     // a sampler lane: SMP, SFLT, SAMP, MOD, GRIT, SLC, LOOP (a new page goes last: the file converter adds it at the end)
constexpr int kParamsPerPage = 8;
constexpr int kSlices = 32;       // a sample holds at most this many (a loop of 32 steps can give each its own)

// Where a sampler page lives inside a lane (-1: not a lane page).
inline int lanePageIndex(int page) {
    switch (page) {
        case PG_SMP: return 0;
        case PG_SFLT: return 1;
        case PG_SAMP: return 2;
        case PG_MOD: return 3;
        case PG_GRIT: return 4;
        case PG_SLC: return 5;
        case PG_LOOP: return 6;
        default: return -1;
    }
}
// A step can also lock its part's effects: the three SENDS and the AMP's knobs. They live in two more key pages past the sound pages; the sequencer
// hands them to the effects section for the step's length (Sequencer.h), not to a note. Not lockable: anything shared by all the parts (the effects' own
// settings, the returns, the compressor), and the amp's MODEL and CAB (a new model is a fade; an amp that is off stays off).
constexpr int PG_FXSEND = PG_COUNT;       // param 0..2: the DELAY, REVERB, CHORUS send
constexpr int PG_FXAMP = PG_COUNT + 1;    // param 0..7: the amp's eight knobs as on its FX view
static const uint8_t kLanePageIds[kLanePages] = {PG_SMP, PG_SFLT, PG_SAMP, PG_MOD, PG_GRIT, PG_SLC, PG_LOOP};
// The SLICE page's knobs (a step can lock SLICE, RANDOM and RND REV; the others are the lane's). SLC_MODE: OFF plays the sample as ever; STEP, a step that fires plays a slice; SYNC, the
// loop plays through by itself at the tempo and each slice falls at its own place in it.
enum SlicePage : uint8_t { SLC_MODE, SLC_SLICE, SLC_RANDOM, SLC_BEATS, SLC_FIT, SLC_REVRND, SLC_SENS, SLC_GRID };
enum SliceMode : uint8_t { SLICE_OFF, SLICE_STEP, SLICE_SYNC };
enum SliceFit : uint8_t { FIT_SLICES, FIT_PITCH };   // at another tempo: the slices keep their pitch and gap or get cut, or the loop speeds up like a record
constexpr int kSliceBeatChoices = 6;
static const uint8_t kSliceBeats[kSliceBeatChoices] = {1, 2, 4, 8, 16, 32};
// The LOOP page's knobs, for a lane whose PLAY is LOOP. A step can lock XFADE.
enum LoopPage : uint8_t { LP_AUTO, LP_XFADE, LP_UNIT, LP_CURVE, LP_SFINE, LP_EFINE, LP_SEARCH };
enum LoopAuto : uint8_t { AUTO_OFF, AUTO_AMP, AUTO_POWER };   // find the points: matching zero crossings, or matching loudness
enum LoopUnit : uint8_t { LOOP_MS, LOOP_PCT };
enum LoopCurve : uint8_t { CURVE_LINEAR, CURVE_POWER, CURVE_LOG, CURVE_MATCH };   // MATCH: worked out from how alike the two ends are (AUTO measures that)
constexpr double kLoopFadeMaxMs = 2000.0;
constexpr int kSliceGridChoices = 5;
static const uint8_t kSliceGrid[kSliceGridChoices] = {0, 4, 8, 16, 32};   // 0: at the transients

enum class Unit : uint8_t { Plain, Pct, Cutoff, Hpf, Ms, Pan, Oct, Semi, Cents, Bend, Bits, Rate, LfoRate, Unison, MidiCh, Sample, Wave, Slice };

struct ParamDef {
    const char* name;
    int16_t min, max, def;
    const char* const* list;   // names for a list parameter, else null
    Unit unit;
};

struct PageDef {
    const char* title;
    uint32_t acc;                // accent colour, 0xRRGGBB
    ParamDef p[kParamsPerPage];
};

const PageDef& pageDef(int page);
const char* pageKey(int page);     // "OSC1", "SMP", ... (the mockup's page names, used in toasts and the lock badge)

// A lock key packs page and parameter (page * 8 + param), the C version of the mockup's "PAGE:i" strings.
inline uint8_t lockKey(int page, int param) { return (uint8_t)(page * kParamsPerPage + param); }
inline int lockPage(uint8_t key) { return key / kParamsPerPage; }
inline int lockParam(uint8_t key) { return key % kParamsPerPage; }
inline bool lockIsFx(uint8_t key) { return lockPage(key) >= PG_COUNT; }
inline bool fxLockableParam(int page, int param) { return page == PG_FXSEND ? param >= 0 && param < 3 : page == PG_FXAMP && param != 0 && param != 6 && param >= 0 && param < 8; }
// The lock key of a knob on the FX screen (-1: not lockable): SENDS view knobs 0-2, AMP view knobs except MODEL and CAB.
inline int fxLockKey(int view, int param) {
    if (view == FX_SENDS) return fxLockableParam(PG_FXSEND, param) ? lockKey(PG_FXSEND, param) : -1;
    if (view == FX_AMP) return fxLockableParam(PG_FXAMP, param) ? lockKey(PG_FXAMP, param) : -1;
    return -1;
}

// The value in the units a player thinks in (Hz, ms, %, L/C/R, ...). `sampleName` resolves Unit::Sample.
struct SampleLib;
void formatParam(Text& out, int page, int param, int v, const SampleLib* lib);

// Curves shared by the screen, the units and the sound.
double cutHz(int v);     // 0..127 -> 30 Hz .. 16 kHz, exponential
double envMs(int v);     // 0..127 -> 2 ms .. 2.5 s
void fmtHz(Text& out, double hz);
void fmtMs(Text& out, double ms);

// ---- lists
extern const char* const kWaves[];
extern const char* const kModels[];      // the synth MODEL knob
constexpr int kModelCount = 8;

// ---- factory presets (three of the mockup's ten): only the values a preset sets.
struct Preset {
    const char* name;
    uint8_t cat;                 // index into kPresetCats
    int8_t osc1[2];              // {unused, WAVE}
    int8_t flt1[4];              // MODEL, CUTOFF, RESO, ENV AMT
    int8_t amp[5];               // ATTACK, DECAY, SUSTAIN/12, RELEASE, VEL SENS-70
};
extern const Preset kPresets[];
constexpr int kBasePresets = 3;   // the factory's three (above): ids 0..2; ids 3..9 are not used (once the factory's other seven); the library on the card starts at 10
constexpr int kBootSoundCount = 3;
extern const Preset kBootSounds[];   // what a new project's parts A, B and C start with (not on the preset list); D, E and F take the same three, G the first again
extern const char* const kPresetCats[];
constexpr int kPresetCatCount = 12;   // ALL BASS LEAD PAD PLUCK STAB (the factory's) POLY KEYS FX SEQ TEXTURE BELL

// Writes a preset's values into a part's nine synth pages.
void applyPreset(int16_t pages[kSynthPages][kParamsPerPage], const Preset& pr);

// The presets as one numbered list: the three factory presets (built in), then the library on the card (/CATHODE/PRESETS, app/PresetFile.h: 131 converted
// from the original CATHODE and rated on the listening page, and whatever else was put there). A preset's NUMBER is its id: the factory's are 0..2, the
// library's start at 10, a project stores it (Part::preset), and a number never changes meaning. The numbers are not contiguous, so walk the list with
// presetCount() / presetIdAt(k). The names, categories, voices and banks come from the index and cost nothing to ask for; a preset's sound is read from its
// file by presetLoad() (main loop; it may take a few milliseconds), after which applyPresetIndex / applyPresetFx can use it (also from the audio interrupt).
int presetCount();
int presetIdAt(int k);                         // the k-th preset of the list, in id order
bool presetExists(int id);
const char* presetName(int id);                // "" for a number that is not on the list
int presetCat(int id);
int presetVoices(int id);                      // 0: leave the part's voices as they are (the factory's three)
void presetBanks(int id, int out[2]);          // the banks it plays, -1 none
bool presetLoad(int id);                       // makes the preset's sound ready (false: its file is missing or damaged). Main loop only.
bool presetReady(int id);                      // true when applyPresetIndex / applyPresetFx will find it
// false (nothing changed): the preset is not ready.
bool applyPresetIndex(int16_t pages[kSynthPages][kParamsPerPage], int id);
void migratePages(int16_t pages[kSynthPages][kParamsPerPage]);   // a file's pages in today's layout (a retired knob's value moves to the knob that took it over)
// A preset's effects: how much of its part goes to the delay, reverb and chorus (the sends), its amp, and the character of the shared effects it
// was made with. Loading a preset sets the part's sends and amp completely; a shared effect takes the preset's settings only if no OTHER part
// sends to it (the project's delay time or reverb decay is not overwritten while another part uses it).
// (Generated from the upstream preset's own effects chain: content/out/FX_MAPPING.md.)
bool applyPresetFx(FxSettings& fx, int part, int id);

// The library's storage, behind the card (board/Card.cpp) or a folder (the host tests). presetsOpen reads the index, or the files when the index no
// longer matches the folder, and writes a new index.
struct PresetInfo;
struct PresetStorage {
    virtual ~PresetStorage() {}
    // calls fn for every file in the preset folder (any name), false when the folder does not exist. `stamp`: its date and time.
    virtual bool list(void (*fn)(void* ctx, const char* name, uint32_t size, uint32_t stamp), void* ctx) = 0;
    virtual bool readFile(const char* name, uint8_t* buf, size_t cap, size_t& n) = 0;   // the whole file (false: not readable or larger than cap)
    virtual bool indexSize(size_t& n) = 0;                                   // false: no index file
    virtual bool readIndex(size_t offset, uint8_t* buf, size_t len) = 0;     // false: shorter than that
    virtual bool writeIndex(size_t offset, const uint8_t* buf, size_t len) = 0;   // offset 0 starts a new file (the old one is dropped); the pieces come in order
    virtual bool endIndex() { return true; }                                 // the last piece has been given, or a piece failed
    virtual bool writeFile(const char* name, const uint8_t* buf, size_t n) = 0;   // a preset file, whole: replaces one of that name; a failure leaves the old one
    virtual bool fileExists(const char* name) = 0;
    virtual bool removeFile(const char* name) { return false; }
};
struct PresetOpen {
    int count = 0;        // presets on the card's list
    int ignored = 0;      // files left out: unreadable, damaged, an id already taken, or beyond the list's size
    bool rebuilt = false; // the files were read (the index was missing or out of date)
    bool wrote = false;   // ... and a new index was written
    int readErrors = 0;   // ... but the card would not give some files (then no index is written, and the next start tries again)
};
// `mem`: room for `capacity` entries (the list lives there; 64 bytes each). Call once, then presetsOpen again to re-read (a card changed).
void presetsInit(PresetInfo* mem, int capacity);
PresetOpen presetsOpen(PresetStorage& st);
void presetsWriteIndex();                      // after a save, when the card may be written: the index a start could not write
void presetsForget();                          // the library off the list (no card)

// Saves a sound as a preset: `rec` is the sound (everything but the id). overwriteId < 0: a new preset, the next id from 1000; >= 10: the library
// preset with that id is replaced (its file is rewritten under the same file name). The preset goes on the list at once and the index is
// updated. Main loop only. The factory three (ids 0-2) cannot be replaced.
struct PresetRecord;
enum class PresetSave : uint8_t { Ok, NoCard, Full, BadPreset, Missing, Exists, WriteFailed };
PresetSave presetSave(PresetRecord& rec, int overwriteId, int& idOut);

// ---- the library from outside (midi/PresetLink.h, the preset protocol): whole files, under the id they carry
enum class PresetPut : uint8_t { Ok, NoCard, Full, BadPreset, BuiltIn, InUse, WriteFailed, Missing, WrongName };
bool presetLibraryOpen();                      // there is a card with a library to talk to
int presetFiles();                             // presets on the card (not the built-in three)
bool presetReadFile(int id, uint8_t out[148]); // the file as it is on the card
// A checked file under the id it carries, `id` being what the sender announced. `replace`: over a preset with that id.
PresetPut presetPutFile(const uint8_t* body, size_t n, int id, bool replace);
// Off the card. `nameCrc`: of the 22-byte name field, so a list gone stale removes nothing.
PresetPut presetRemove(int id, uint32_t nameCrc);
int presetNextId();                            // the id the next new preset would get, -1 when there is no card or no room
// Every page at its default.
void defaultSynthPages(int16_t pages[kSynthPages][kParamsPerPage]);
void defaultLanePages(int16_t pages[kLanePages][kParamsPerPage]);

}  // namespace app

#endif
