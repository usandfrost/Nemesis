#pragma once

#include <cstdint>

namespace nemesis::dsp
{
    // [T-CMP-11] The seeded, deterministic per-note draw Organic's four dimensions share.
    //
    // ONE hash function, four independent callers (one per dimension: Pitch/Timing/Amp/Cutoff),
    // distinguished by `dimension` so no two dimensions ever draw the same bits — a shared draw
    // would correlate Pitch and Amp offsets, which is not what "each dimension is an independent
    // seeded draw" (family_overview.md §2.1) means.
    //
    // INPUTS: `seed` is the per-track `organicSeed` (0 default, re-rolled by Re-seed); `noteOrdinal`
    // identifies THIS note event — the sequencer derives it from (step, retrig-within-loop) for a
    // sequenced note, or a monotonic live-note counter for a host-MIDI note (`EngineCore`); `voice`
    // is the physical voice index within a unison stroke (0 for a single voice), so each member of
    // a unison stack draws its OWN offset rather than all moving together; `dimension` selects
    // Pitch/Timing/Amp/Cutoff (the `kOrganicDim*` constants below).
    //
    // DETERMINISM (bounce-parity, DSP-03): same four inputs -> bit-identical output, always — no
    // clock read, no static/mutable state, no allocation. A render replays the exact same sequence
    // of note events in the exact same order, so the SAME (seed, ordinal, voice, dimension) tuple
    // recurs identically across any two renders of the same document.
    //
    // splitmix64-shaped (Vigna, public domain construction) — cheap (a handful of xor/mul/add), well
    // distributed, and NOT `<random>`: its engines are not specified to produce identical sequences
    // across standard-library versions/vendors, which would silently break bounce-parity for anyone
    // building against a different libc++ — the same reasoning `NoiseSource`'s own LCG (not
    // `<random>`) documents for the audio-rate case.
    [[nodiscard]] inline std::uint64_t organicHash (std::uint32_t seed, std::uint32_t noteOrdinal, std::uint32_t voice, std::uint32_t dimension) noexcept
    {
        std::uint64_t x = static_cast<std::uint64_t> (seed) ^ 0x9E3779B97F4A7C15ULL;
        x += static_cast<std::uint64_t> (noteOrdinal) * 0xBF58476D1CE4E5B9ULL;
        x += static_cast<std::uint64_t> (voice) * 0x94D049BB133111EBULL;
        x += static_cast<std::uint64_t> (dimension) * 0xD6E8FEB86659FD93ULL;
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
        return x;
    }

    // The hash mapped to a BIPOLAR draw in [-1, 1) — the shape every dimension's law scales from (a
    // bipolar deviation about the dialed value, per §2.1's "perturbs ... by a small ... draw").
    // Uses the top 24 bits: a float's mantissa is 24 bits, so more would just be rounded away — the
    // same "how many bits a float draw actually needs" idiom `NoiseSource::nextWhite` already uses.
    [[nodiscard]] inline float organicDrawBipolar (std::uint32_t seed, std::uint32_t noteOrdinal, std::uint32_t voice, std::uint32_t dimension) noexcept
    {
        const std::uint64_t h = organicHash (seed, noteOrdinal, voice, dimension);
        const auto top24 = static_cast<std::uint32_t> (h >> 40); // top 24 bits of a 64-bit hash
        return static_cast<float> (top24) * (2.0f / 16777216.0f) - 1.0f; // [0, 2^24) -> [-1, 1)
    }

    // Dimension indices — one hash call per, so no two ever draw the same bits. Order matches
    // family_overview.md §2.1's table (Pitch, Timing, Amp, Cutoff/tone).
    inline constexpr std::uint32_t kOrganicDimPitch = 0;
    inline constexpr std::uint32_t kOrganicDimTiming = 1;
    inline constexpr std::uint32_t kOrganicDimAmp = 2;
    inline constexpr std::uint32_t kOrganicDimCutoff = 3;
}
