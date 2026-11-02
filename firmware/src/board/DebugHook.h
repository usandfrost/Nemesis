#ifndef __BOARD_DEBUG_HOOK_H
#define __BOARD_DEBUG_HOOK_H

#include <stddef.h>
#include <stdint.h>

#include "app/Ui.h"
#include "gfx/Canvas.h"

// A mailbox in RAM for a script on the computer that drives the board through the debugger (SWD), so a change can be tried without
// anyone pressing buttons: tools/board_script.py. Three things go through it:
//   events   the script writes key / layer-key / knob events; the main loop hands them to the Ui exactly as Input::poll does
//   status   the main loop writes what it knows (mode, part, playing, audio load, degrade level, the toast ...) for the script to read
//   screen   the script asks for a strip of the screen; the main loop rasterises that strip of the last frame into `cap`
// Nothing here listens on any port: the only way in is a debug probe on the SWD header. CMake option DBOY_DEBUG_HOOK (on while the firmware
// is being brought up); off compiles all of it out.
//
// The layout is fixed (the script reads it by word index, see static_asserts in DebugHook.cpp); change both together and bump kDebugVersion.
namespace board {

constexpr uint32_t kDebugMagic = 0x4442474Du;   // "DBGM"
constexpr uint32_t kDebugVersion = 4;
constexpr int kDebugEvents = 32;                // the script never leaves more than this many unread
constexpr int kCapRows = 20;                    // rows in one captured strip (540 / 20 = 27 strips)

// Event word: type | a << 8 | b << 16 (b signed)
enum DebugEvent : uint8_t {
    DbgNone = 0, DbgKeyDown = 1, DbgKeyUp = 2, DbgLayerDown = 3, DbgLayerUp = 4, DbgTurn = 5,   // a = Key / layer key / knob, b = detents
    DbgMuteLane = 6,    // a = lane, b = 1 mute / 0 unmute
    DbgSoloPart = 7,    // a = part: every lane that plays on another part is muted (a = 255: unmute every lane)
    DbgGovernor = 8,    // b = 1 the degrade governor runs (the default) / 0 it is off, so a measurement keeps its voices
    DbgLoadPreset = 9,  // a = part, b = preset id: load it into that part as the BROWSE list's OK does
    DbgMidiNote = 10,   // a = note, b = velocity (0 = off): a note on MIDI channel 16, which plays the part on screen
    DbgPartVoices = 11, // a = part, b = VOICES (1..8): the MIXER knob, set directly
    DbgLean = 12,       // b = 1 the CATHODE parts play through the lean voice / 0 through the CATHODE engine (only in a DBOY_LEAN build): A/B on the board
    DbgLanePart = 13,   // a = lane, b = part: the lane plays on that part, in the pattern on screen
    DbgSoak = 14        // b = rounds: the card test (board/CardSoak.h). A TEST CARD ONLY: it writes
};

struct DebugMailbox {
    uint32_t magic;                  // word 0
    uint32_t version;                // 1
    volatile uint32_t head;          // 2   the script's write position (free-running)
    volatile uint32_t tail;          // 3   the firmware's read position
    uint32_t ev[kDebugEvents];       // 4..35
    // status, written by the main loop (words 36..)
    uint32_t loops;                  // 36  main-loop passes
    uint32_t revision;               // 37  Ui::revision()
    uint32_t nowMs;                  // 38
    uint32_t mode;                   // 39  app::Mode
    uint32_t part;                   // 40
    uint32_t playing;                // 41
    uint32_t songPlaying;            // 42
    uint32_t pattern;                // 43
    uint32_t step;                   // 44  the step sounding now (-1 = none, as an unsigned word)
    uint32_t degrade;                // 45  steps of the degrade ladder applied
    uint32_t loadLastPct10;          // 46  tenths of a percent of one audio block's cycle budget: the last block
    uint32_t loadAvgPct10;           // 47  the average over the last second
    uint32_t loadMaxPct10;           // 48  the worst block since reset
    uint32_t overruns;               // 49  blocks over budget since reset
    uint32_t sdramFreeKb;            // 50
    uint32_t tilesSent;              // 51  Lcd: tiles the last frame sent
    uint32_t presentUs;              // 52  Lcd: microseconds the last frame took
    uint32_t toastOn;                // 53
    char toast[96];                  // 54..77
    // screen capture
    volatile uint32_t capRequest;    // 78  the script: strip number + 1
    volatile uint32_t capDone;       // 79  the firmware: the strip number + 1 now in `cap`
    // where the audio block's cycles go (words 80..96), then how long a note start takes (97..100): voices sounding per part now, then tenths of a percent of the block budget per section,
    // averaged over the last second
    uint32_t voices[app::kParts];    // 80..86  parts A to G
    uint32_t partPct10[app::kParts]; // 87..93
    uint32_t samplerPct10;           // 94
    uint32_t fxPct10;                // 95
    uint32_t seqPct10;               // 96
    uint32_t startOnMaxKc;           // 97  the slowest note start so far, in thousands of cycles: the engine's noteOn
    uint32_t startPatchMaxKc;        // 98  ... and applying the patch to the voices of the note
    uint32_t startOnSumKc;           // 99  all the engine noteOns so far added up, thousands of cycles (differences over a run give the average)
    uint32_t starts;                 // 100 note starts counted
    uint32_t cfgVoices[app::kParts]; // 101..107  the VOICES setting of parts A to G (what the engine runs, sounding or not)
    int32_t partPreset[app::kParts]; // 108..114  the preset id each part holds (-1: none, a boot sound or edited)
    uint32_t waveSlots;              // 115  wavetable banks the SDRAM has room for at once (0: none, and a preset on a bank is silent)
    uint32_t waveResident;           // 116  banks in memory now
    int32_t waveRefused;             // 117  a bank that could not be loaded for lack of a slot (-1: none)
    // written by the AUDIO interrupt itself after every block (so they stay current when the main loop is starved by an overloaded audio): running
    // totals since boot, low 32 bits (a script reads them twice and divides the differences)
    uint32_t isrBlocks;              // 118  blocks rendered
    uint32_t isrTotalCyc;            // 119  cycles spent in the blocks
    uint32_t isrPartCyc[app::kParts];// 120..126  ... in each synth part's engine
    uint32_t isrSamplerCyc;          // 127
    uint32_t isrFxCyc;               // 128
    uint32_t isrVoices[app::kParts]; // 129..135  voices sounding in parts A to G at the last block
    uint32_t soak[12];               // 136..147  board::SoakStatus
    uint16_t cap[gfx::kWidth * kCapRows];   // 148..
};

extern DebugMailbox debugMail;

#ifndef DBOY_DEBUG_HOOK
#define DBOY_DEBUG_HOOK 0
#endif

#if DBOY_DEBUG_HOOK
void debugHookInit();
// Main loop, once a pass: runs the pending events and the strip request. `canvas` holds the last frame drawn.
// `setGovernor` switches the degrade governor (a function pointer, so this header stays free of the DSP headers, which want their own compile flags).
using DebugGovernorFn = void (*)(bool on);
using DebugLeanFn = void (*)(bool on);   // null when the lean voice is not built in
void debugHookService(app::Ui& ui, DebugGovernorFn setGovernor, DebugLeanFn setLean, const gfx::Canvas& canvas);
#else
inline void debugHookInit() {}
using DebugGovernorFn = void (*)(bool);
using DebugLeanFn = void (*)(bool);
inline void debugHookService(app::Ui&, DebugGovernorFn, DebugLeanFn, const gfx::Canvas&) {}
#endif

}  // namespace board

#endif
