#pragma once

#include <cstddef>
#include <cstdint>

// FIREFORK: switches for the Drumboy firmware. Every default equals upstream behaviour, so the default build stays bit-identical
// (bench/render_ref_* proves it).
//
// Compile-time (set by the build, docs/DSP_REVIEW.md):
//   NEMESIS_FORK_EXACT   optimizations that are bit-identical to the same build without them (proven by bench/render_cov +
//                       cov_diff --exact in double and float). Hot-path code, so no run-time branch.
//   NEMESIS_FORK_APPROX  optimizations that change the output by about 1 ulp .. 1e-8 (dropped near-zero FIR taps, reciprocal
//                       multiplies, fast pow). Checked spectrally (cov_diff) and for aliasing (alias_check).
//
// Run-time (the firmware flips them in main.cpp BEFORE prepare()):
//   idleMatrixSkip   Skip feeding and evaluating the mod matrix while it has no active slot. EXACT: evaluate() is then a no-op
//                    (rebuildTouched() already zeroed what left the route list), so output is bit-identical. Costs nothing.
//   skipSilentSlots  Do not compute an oscillator slot whose level is 0 AND that nothing couples to the other slot (no FM, ring,
//                    sync, cross-mod fade, unison stack). Output is identical while the level stays 0. The one visible difference:
//                    the skipped slot's phase freezes, so it comes back at a different (free-running anyway) phase.
//   pdOversample     Oversampling factor of the phase-distortion oscillator (upstream 4). Lower is cheaper and aliases a little more.
//   filterOversample Oversampling factor of the non-linear filter cores: Scream, Acid, Soviet, Reed, and Classic unless
//                    classicOversample is set (upstream 4).
//   classicOversample  0 = follow filterOversample. Classic's stability cap is on G, so at x2 its top cutoff at full resonance
//                    drops to ~15.5 kHz (x4: ~31 kHz). Set 4 to keep high-resonance sweeps reaching the top.
//   driveOversample  Oversampling of the filter drive, the output drive and the Ladder core (upstream 4).
//   stereoTwin       Prepare each voice's stereo right chain (upstream: always). Off saves its memory; width/Stereo route play mono.

// P4: where NEMESIS_FORK_FAST_DATA puts the hot shared tables. The firmware defines NEMESIS_FORK_FAST_DATA_DTCM (its linker script
// has a zero-filled .dtcm_bss section in the M7's 128 KB DTCM: zero wait states, never behind a cache).
#if defined(NEMESIS_FORK_FAST_DATA_DTCM) && !defined(NEMESIS_FORK_FAST_DATA)
#define NEMESIS_FORK_FAST_DATA __attribute__ ((section (".dtcm_bss")))
#endif

// P2: the engine's physical voice pool (upstream 32 = 8 notes x unison 4). The firmware plays 5 notes at engine unison 1, so 8
// voices; every voice is prepared up front, so this decides most of the engine's memory (~186 KB of heap per voice).
#ifndef NEMESIS_FORK_MAX_VOICES
#define NEMESIS_FORK_MAX_VOICES 32
#endif
// E21: leave out what the Drumboy panel cannot reach (0 = upstream): the filter cores no panel model selects, the modulation sources and drifters nothing routes
#ifndef NEMESIS_FORK_PANEL_FILTERS
#define NEMESIS_FORK_PANEL_FILTERS 0
#endif
#ifndef NEMESIS_FORK_PANEL_SOURCES
#define NEMESIS_FORK_PANEL_SOURCES 0
#endif
// E19: control rate of the named modulation destinations (1 = every sample = upstream) and of idle-skipped voices (default: the same)
#ifndef NEMESIS_FORK_CTL_RATE
#define NEMESIS_FORK_CTL_RATE 1
#endif
#ifndef NEMESIS_FORK_IDLE_CTL_RATE
#define NEMESIS_FORK_IDLE_CTL_RATE NEMESIS_FORK_CTL_RATE
#endif
// E18: bytes of padding at the end of every CathodeModulatedVoice (0 = upstream layout; the firmware sets it from CMake DBOY_VOICE_PAD)
#ifndef NEMESIS_FORK_VOICE_PAD
#define NEMESIS_FORK_VOICE_PAD 0
#endif
#ifndef NEMESIS_FORK_EXACT
#define NEMESIS_FORK_EXACT 0
#endif
#ifndef NEMESIS_FORK_APPROX
#define NEMESIS_FORK_APPROX 0
#endif

namespace nemesis::dsp::fork
{
    inline constexpr bool kExact = NEMESIS_FORK_EXACT != 0;
    inline constexpr bool kApprox = NEMESIS_FORK_APPROX != 0;
    inline constexpr bool kPanelFilters = NEMESIS_FORK_PANEL_FILTERS != 0;
    inline constexpr bool kPanelSources = NEMESIS_FORK_PANEL_SOURCES != 0;

    struct Config
    {
        static inline bool lanes = true;            // false: never prepare the NEON lane groups, every lane site off (the M7)
        static inline bool idleMatrixSkip = false;
        static inline bool skipSilentSlots = false;
#ifdef NEMESIS_FORK_DIAGNOSTICS
        static inline unsigned long slotSkips = 0; // diagnostic: how many slot-samples were skipped (proves the saving engages)
        static inline unsigned long lazyCleared = 0, lazySkipped = 0; // diagnostic E16: core resets still done / skipped
        static inline unsigned long pushCalls = 0, pushSkips = 0, expSkips = 0, cutCalls = 0; // diagnostic E17: pushes and exp2s done / skipped
#endif
        static inline int pdOversample = 4;
        static inline int filterOversample = 4;
        static inline int classicOversample = 0;
        static inline int driveOversample = 4;
        static inline bool stereoTwin = true;      // false: no voice prepares its stereo right chain (width and the Stereo route then play mono)
    };
}

namespace nemesis::dsp::fork
{
    // x / f for an oversampling factor. For f = 1, 2, 4, 8 the multiply by the exact reciprocal is bit-identical to the division
    // (a power of two only moves the exponent), and costs one multiply instead of a ~30-cycle double division on the M7.
    [[nodiscard]] inline double divByFactor (double x, int f) noexcept
    {
        switch (f)
        {
            case 1: return x;
            case 2: return x * 0.5;
            case 4: return x * 0.25;
            case 8: return x * 0.125;
            default: return x / static_cast<double> (f);
        }
    }
}

// Counts an event in diagnostic builds only (bench tools define NEMESIS_FORK_DIAGNOSTICS); compiles to nothing in the firmware.
#ifdef NEMESIS_FORK_DIAGNOSTICS
#define NEMESIS_FORK_COUNT(counter) (++::nemesis::dsp::fork::Config::counter)
#else
#define NEMESIS_FORK_COUNT(counter) ((void) 0)
#endif
