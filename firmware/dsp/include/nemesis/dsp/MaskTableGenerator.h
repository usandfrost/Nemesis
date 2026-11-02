#pragma once

#include <nemesis/dsp/WavetableRecipe.h>

#include <cstdint>
#include <vector>

namespace nemesis::dsp
{
    // CATHODE §1.1 "Mask" procedural table family (T-MASK-02; docs/dev/bitmask-osc-analysis.md):
    // a one-integer 0..511 timbre space. Originally realised ONLY as a build-time table GENERATOR
    // in front of the untouched M1-04 Wavetable, with mask select riding the oscillator's existing
    // position/frame machinery (the mip pyramid carrying anti-aliasing, discrete jumps landing on
    // frame centers, the built linear frame blend as the crossfade).
    //
    // **ADR-037 (2026-08-04) REVERSED THAT for the SegmentOps law: it is now ALSO evaluated at
    // RENDER time by `dsp::BitmaskOscillator`, and that is the path the instrument plays.** The
    // build-time view below is retained — the `generate`/`layoutMasks`/ordering machinery is what
    // the three COMPLEX laws still ship through — but the sentence "nothing here runs at render
    // time" is no longer true of SegmentOps, and the two views are held together by a bit-exact
    // agreement test rather than by a convention. See the SEGMENTOPS LAW block further down for
    // the shared `segmentTap` the oscillator consumes.
    //
    // LAW (T-MASK-03 leaf B / ADR-037, superseding the 2026-07-16 `[s % 3]` pick): SegmentOps — a
    // generated pure-sine cycle splits into kNumSegments (9) near-equal segments and mask bit s,
    // when set, applies `opFor(s)` to segment s, drawn from {Flip, Zero, Mirror, Repeat, Scale}
    // via an AUTHORED nine-entry table (owner ruling 2, amplitude-reasoned — destructive ops where
    // the sine has amplitude to destroy, shape ops where it is steep). Mask 0 is a bit-exact pure
    // sine (the family §1 clean anchor, mirroring pdAmount = 0). Ops are segment-local IN INDEX
    // SPACE, so two masks differ in exactly the segments whose bits differ and all 512 frames are
    // pairwise distinct — but the emitted FRAME also carries a zero-mean correction, which is
    // frame-wide, so locality holds up to that constant (see `dcOffsetFor`). (Mirror can never be
    // a no-op: a segment [a,b) is sine-palindromic only if a+b-1 == frameLength/2 mod frameLength,
    // and no 9-way split boundary pair satisfies that.)
    //
    // ORDERING — how a bank lays masks onto frames (the layout is a scanning permutation;
    // direct mask entry maps through layoutMasks()):
    //   * Binary   — identity (frame i = maskFirst + i).
    //   * Gray     — binary-reflected Gray code (frame i = maskFirst | grayCode(i)): adjacent
    //                frames differ by exactly ONE mask bit, so position sweeps morph stepwise.
    //                Requires maskCount a power of two and maskFirst aligned to it (both hold
    //                for the default 2x256 banks); anything else falls back to Binary.
    //   * Centroid — ascending spectral-centroid sort (monotone brightness scans),
    //                deterministic tie-break on the mask value.
    //
    // COVERAGE (owner pick 2026-07-16): the default content convention is 2x256 banks
    // (masks 0-255 and 256-511) inside the OI-AE-10 256-frame cap; generate() itself accepts
    // any slice. Zero shipped assets: banks are generated from pure code (the OI-AE-10
    // "algorithmically generated" provenance rule); ~44 MB per built 256-frame bank, the
    // family's existing max-table envelope (budget rides T-B08).
    //
    // Build-time only, NEVER on the audio thread: generate() allocates and the .cpp uses libm
    // and an FFT (the Wavetable::build altitude). Deterministic — no RNG, same inputs give
    // bit-identical frames. Output layout is exactly what Wavetable::build() consumes.
    //
    // ===========================================================================================
    // THE THREE **COMPLEX** LAWS (T-WT-03 leaf G2; `family_overview.md` §2.3's sixth category,
    // owner ruling 2026-08-03) — Prism · Shatter · Weave, the shipped factory content of this
    // class. They live here rather than in a `ComplexTableGenerator` of their own because they are
    // the same MECHANISM as SegmentOps: a frame axis that IS a bit space, traversed by position.
    // What they are NOT is three slices of SegmentOps — the owner took the more expensive option
    // (2026-08-03, amending `docs/dev/bitmask-osc-analysis.md`'s single-law pick of 2026-07-16),
    // because three windows into one 512-mask space risk sounding like siblings rather than three
    // characters. So each is a fresh law with its own spectral claim:
    //
    //   * **Prism** — SPECTRAL octave-band gating. A saw is cut into bands
    //     [2^(s+1), 2^(s+2)) — harmonics 2..3, 4..7, 8..15, 16..31 — and bit s enables band s.
    //     The FUNDAMENTAL is unconditional, so mask 0 is a single sine and mask 15 is the whole
    //     31-harmonic saw. Additive, hence band-limited BY CONSTRUCTION (the `HarmonicFrame`
    //     kernel, shared with VOCAL/METALLIC/HARMONIC), so its alias claim is "nothing above the
    //     stated harmonic" rather than the mip keystone.
    //   * **Shatter** — TIME decimation. The frame splits into `kShatterSegments` (4 — its own
    //     mask width, not SegmentOps' 9) and bit s applies a zero-order hold inside segment s at
    //     that segment's own hold length (8 · 16 · 32 · 64 samples, coarsening across the frame).
    //     The held value is the window MEAN, which is the entire DC argument: a box mean preserves
    //     the window's sum exactly, so the frame's mean stays the carrier's zero. Holding the
    //     window's first sample instead would shift the mean by an amount nobody chose, and a DC
    //     offset survives the mip band-limiter (which never touches bin 0) and thumps on every note.
    //   * **Weave** — a PHASE-domain breakpoint weave. The phase axis is cut into 2^(s+1) blocks at
    //     level s and bit s SWAPS each adjacent pair, which is exactly an XOR of the sample index by
    //     the block length — and that is why it is a bijection: a Weave frame holds the carrier's
    //     2048 samples in a different order and nothing else, so its VALUE SET is the anchor's, bit
    //     for bit. The half-swap level (1024 samples) is deliberately EXCLUDED and the reason is a
    //     measurement, not taste: swapping the two halves of a sine cycle is a half-period rotation,
    //     i.e. an exact NEGATION — same magnitude spectrum, and a Gray step onto that bit would
    //     crossfade a frame with its own inverse, through SILENCE at the midpoint. The blocks
    //     therefore start one level down, at `kFrameLength / 4`.
    //
    // ALL THREE SHARE THE MASK-0 PURE-SINE ANCHOR, which is a shipped fact about the boot patch
    // rather than a nicety: frame 0 is the representative frame (`synth.osc{1,2}.pos` rests there
    // and the slot chip draws it), so the whole category is clean at rest.
    //
    // A LAW'S MASK WIDTH AND ITS FRAME COUNT ARE ONE NUMBER (§2.3, owner ruling 2026-08-03). A
    // consumer permutes POSITION over 2^b steps while the oscillator maps `fp = position ×
    // (frames − 1)`; those agree only when `frames == 2^b`. At 16 frames every one of the sixteen
    // stops lands on a real frame and 4-bit Gray really is one-bit-adjacent; at 32 frames fourteen
    // stops are blends of two frames, half the mask space is unreachable as a pure state, and the
    // property the orderings exist for is lost. Hence `kComplexBits == 4`, `kComplexFrames == 16`
    // (§2.3's factory FLOOR, 2.9 MB per table) and a PER-LAW mask space below.
    //
    // AND THEY ARE GENERATED IN GRAY ORDER (`renderFrame`), which is how the 2026-07-29 anti-Binary
    // ruling survives ordering becoming an orthogonal param that defaults to `Off` (T-PROTO-02):
    // the protection is the CONTENT's layout, so `Off` is already the musical traversal, `Gray` a
    // no-op restatement of it, and Binary an explicit user choice.
    // ===========================================================================================
    class MaskTableGenerator
    {
    public:
        enum class Law : std::uint8_t {
            SegmentOps = 0,
            Prism = 1,
            Shatter = 2,
            Weave = 3
        };

        enum class Ordering : std::uint8_t {
            Binary,
            Gray,
            Centroid
        };

        static constexpr int kFrameLength = 2048; // family-locked single-cycle length (OI-AE-10)
        static constexpr int kNumBits = 9; // SegmentOps' mask integer is 0..511
        static constexpr int kNumSegments = kNumBits; // one segment per bit (SegmentOps law)
        static constexpr int kMaskSpace = 1 << kNumBits; // 512 — SegmentOps' space

        // The COMPLEX laws' width, and therefore their frame count (see the class note).
        static constexpr int kComplexBits = 4;
        static constexpr int kComplexFrames = 1 << kComplexBits; // 16

        // A law's mask WIDTH, and the space derived from it. Spelled as a function of the law
        // rather than as one constant because `wrapMask` has to wrap in the law's OWN space: mask
        // 16 is mask 0 under a 4-bit law and an ordinary mask under SegmentOps, and a shared 9-bit
        // wrap would silently give the COMPLEX laws 512 states they have no frames for.
        [[nodiscard]] static constexpr int bitsFor (Law law) noexcept
        {
            return law == Law::SegmentOps ? kNumBits : kComplexBits;
        }

        [[nodiscard]] static constexpr int maskSpaceFor (Law law) noexcept { return 1 << bitsFor (law); }

        // First sample index of segment s (s in [0, kNumSegments]; s == kNumSegments returns
        // kFrameLength). Integer floor split — segments differ in length by at most one sample.
        // SegmentOps' NINE-way split; Shatter's own is below, and they are two functions on
        // purpose: the number is the law's mask width, and the two laws do not share one.
        [[nodiscard]] static constexpr int segmentBegin (int s) noexcept
        {
            return s * kFrameLength / kNumSegments;
        }

        // SHATTER's split: four segments of exactly kFrameLength / 4, one per mask bit.
        static constexpr int kShatterSegments = kComplexBits;

        [[nodiscard]] static constexpr int shatterSegmentBegin (int s) noexcept
        {
            return s * kFrameLength / kShatterSegments;
        }

        // Shatter's hold length inside segment s: 8, 16, 32, 64 samples — coarsening across the
        // frame, so the four bits are four audibly different decimations rather than one repeated.
        // Each DIVIDES the 512-sample segment, so the last window is whole (a partial window would
        // be a shorter hold no claim covers). Clamped: s outside [0, kShatterSegments) answers with
        // the nearest end, like every other accessor here.
        [[nodiscard]] static constexpr int shatterHold (int s) noexcept
        {
            const int c = s < 0 ? 0 : (s >= kShatterSegments ? kShatterSegments - 1 : s);
            return 8 << c;
        }

        // WEAVE's block length at level s: kFrameLength >> (s + 2), i.e. 512 · 256 · 128 · 64. The
        // `+ 2` rather than `+ 1` is the excluded half-swap (see the class note) — a level whose
        // XOR would negate the carrier instead of weaving it.
        [[nodiscard]] static constexpr int weaveBlock (int s) noexcept
        {
            const int c = s < 0 ? 0 : (s >= kComplexBits ? kComplexBits - 1 : s);
            return kFrameLength >> (c + 2);
        }

        // The index XOR one Weave mask means: the OR of the enabled levels' block lengths.
        [[nodiscard]] static constexpr int weavePattern (int mask) noexcept
        {
            int pattern = 0;
            for (int s = 0; s < kComplexBits; ++s)
                if (((mask >> s) & 1) != 0)
                    pattern |= weaveBlock (s);
            return pattern;
        }

        // PRISM's octave band s: harmonics [prismBandFirst, prismBandLast] = [2^(s+1), 2^(s+2) − 1].
        // The bands TILE 2..kPrismTopHarmonic with no gap and no overlap, which is what makes "the
        // spectrum reads back the mask" a complete statement rather than a claim about some
        // harmonics.
        [[nodiscard]] static constexpr int prismBandFirst (int s) noexcept
        {
            const int c = s < 0 ? 0 : (s >= kComplexBits ? kComplexBits - 1 : s);
            return 1 << (c + 1);
        }

        [[nodiscard]] static constexpr int prismBandLast (int s) noexcept
        {
            const int c = s < 0 ? 0 : (s >= kComplexBits ? kComplexBits - 1 : s);
            return (1 << (c + 2)) - 1;
        }

        static constexpr int kPrismTopHarmonic = (1 << (kComplexBits + 1)) - 1; // 31

        // Binary-reflected Gray code of i (the Gray-ordering permutation kernel).
        [[nodiscard]] static constexpr int grayCode (int i) noexcept { return i ^ (i >> 1); }

        // =======================================================================================
        // THE SEGMENTOPS LAW ITSELF (T-MASK-03 leaf B, ADR-037). It lives here — its original home
        // — and `dsp::BitmaskOscillator` consumes it at RENDER time, so there is exactly ONE law
        // with two views: this class renders it into frames at the 2048 integer positions, the
        // oscillator evaluates it per sample at fractional positions. ADR-037 makes their agreement
        // a TEST rather than a convention, because two copies of a law is the failure mode it was
        // written to avoid.
        //
        // The ops are all SEGMENT-LOCAL transforms of the pure-sine carrier, which is the whole
        // reason a render-time oscillator is affordable at all: 512 masks cost ONE resident sine
        // table plus index/sign/gain arithmetic, not 512 x 2048 floats of pre-rendered frames.
        // =======================================================================================

        enum class Op : std::uint8_t {
            Flip = 0, // negate the segment
            Zero = 1, // silence it
            Mirror = 2, // time-reverse within it (std::reverse's index map)
            Repeat = 3, // its content at 2x, so it fits twice (T-MASK-03; the reference's "repeated")
            Scale = 4 // amplitude only (T-MASK-03; the reference's "scaled")
        };

        static constexpr int kNumOps = 5;

        // Scale's gain. Exact in binary, which is load-bearing: it keeps the render-time view
        // BIT-identical to the build-time one (a non-representable gain would differ by an ulp
        // between a float and a double multiply).
        static constexpr float kScaleGain = 0.5f;

        // THE AUTHORED 9-ENTRY OP TABLE (owner ruling 2, 2026-08-03). Destructive ops (Zero,
        // Scale) land where the sine has amplitude to destroy; shape ops (Repeat, Mirror) where it
        // is steep and monotone so they change the waveform most. Segment s covers phase
        // [s·2π/9, (s+1)·2π/9), so the peak falls in segment 2 and the trough in segment 7.
        //
        // **THIS TABLE IS CONTENT, NOT A LAW.** It is a starting point T-MASK-03 leaf H's WAV may
        // revise, so the tests pin each OP's behaviour and the table's SHAPE (nine entries, every
        // op present, the ruling-2 counts) and deliberately do NOT fix a segment→op pairing — a
        // sound-design revision must not read as a regression.
        [[nodiscard]] static constexpr Op opFor (int s) noexcept
        {
            constexpr Op table[kNumSegments] = {
                Op::Repeat, // 0 — rise, near zero
                Op::Mirror, // 1 — rise to peak
                Op::Zero, //   2 — PEAK
                Op::Scale, //  3 — fall from peak
                Op::Flip, //   4 — fall, mid
                Op::Repeat, // 5 — fall to zero
                Op::Mirror, // 6 — to trough
                Op::Zero, //   7 — TROUGH
                Op::Scale //   8 — rise from trough
            };
            const int c = s < 0 ? 0 : (s >= kNumSegments ? kNumSegments - 1 : s);
            return table[c];
        }

        // The segment holding index-space position x in [0, kFrameLength). Fractional x is the
        // render-time case.
        //
        // NOT `floor(x·kNumSegments / kFrameLength)`: `segmentBegin` is an integer-FLOOR split
        // (0·227·455·682·910·1137·1365·1592·1820) and the equal-width formula disagrees at every
        // boundary — x = 227 answers 0 while it belongs to segment 1. The naive index is provably
        // s−1 or s and never further (x >= floor(2048s/9) gives 9x/2048 > s − 9/2048, and
        // x < floor(2048(s+1)/9) gives 9x/2048 < s+1), so ONE correcting compare is exactly
        // enough — and the tests probe {lo−1, lo, hi, hi+1} on all nine segments because an
        // off-by-one here is invisible at every interior position.
        [[nodiscard]] static constexpr int segmentAt (double x) noexcept
        {
            int s = static_cast<int> (x * static_cast<double> (kNumSegments)
                                      / static_cast<double> (kFrameLength));
            if (s < 0)
                s = 0;
            if (s > kNumSegments - 1)
                s = kNumSegments - 1;
            if (s < kNumSegments - 1 && x >= static_cast<double> (segmentBegin (s + 1)))
                ++s;
            return s;
        }

        // Where a masked read of position x lands in the carrier, and with what gain. This IS the
        // law: `renderMask` walks it over the integer positions, the oscillator evaluates it per
        // sample. Pure arithmetic — no libm, no allocation, safe on the audio thread.
        struct Tap
        {
            double index; // carrier read position, in [0, kFrameLength)
            float gain; // ±1, 0 or kScaleGain — every value exact in binary
        };

        [[nodiscard]] static constexpr Tap segmentTap (int mask, double x) noexcept
        {
            const int s = segmentAt (x);
            if (((mask >> s) & 1) == 0)
                return { x, 1.0f }; // an unset bit is the exact identity — the mask-0 anchor

            const double begin = static_cast<double> (segmentBegin (s));
            const double end = static_cast<double> (segmentBegin (s + 1));
            const double len = end - begin;

            switch (opFor (s))
            {
                case Op::Flip:
                    return { x, -1.0f };
                case Op::Zero:
                    return { x, 0.0f };
                case Op::Mirror:
                {
                    // begin + end − 1 − x is exactly std::reverse's index map at integral x. At a
                    // FRACTIONAL x approaching `end` the reflection runs off the low end of the
                    // segment (negative outright, for segment 0), so it wraps periodically inside
                    // the segment — a sub-sample sliver the build-time view never visits.
                    double idx = begin + end - 1.0 - x;
                    if (idx < begin)
                        idx += len;
                    return { idx, 1.0f };
                }
                case Op::Repeat:
                {
                    // A rate doubling INSIDE the segment: the content fits twice and the read never
                    // leaves [begin, end). Without the wrap the second half would read into the
                    // NEXT segment, which is audible and invisible to a first-half-only probe.
                    double u = 2.0 * (x - begin);
                    if (u >= len)
                        u -= len;
                    return { begin + u, 1.0f };
                }
                case Op::Scale:
                    return { x, kScaleGain };
            }
            return { x, 1.0f };
        }

        // THE DC CORRECTION, and it is why both views agree on more than the tap.
        //
        // Repeat changes a segment's mean (its content is resampled) and Scale changes it (a
        // non-zero-mean segment scaled is a different mean); Zero and Flip change it too. A DC
        // offset SURVIVES any band-limiter — nothing touches bin 0 — and thumps on every note, so
        // the law subtracts the frame mean.
        //
        // Stored as a DIFFERENCE from mask 0's own sum rather than as an absolute mean, so the
        // mask-0 anchor is bit-exact BY CONSTRUCTION: the same value minus itself is exactly 0.0f
        // whatever the carrier sums to.
        //
        // **BATTERY SURVIVOR, LABELLED — category (c), unprovable by construction** (T-CORE-03's
        // three-kinds rule). Replacing `(sum - sum0)` with `(sum)` kills nothing, and the reason is
        // a measurement rather than a guess: for THIS carrier accumulated in THIS order, `sum0` is
        // **exactly 0.0** in double. Not approximately — exactly. The `sin(pi)` term at i = 1024 is
        // ~1.22e-16 and is ABSORBED by the running partial sum (which is O(10^2) by then), after
        // which the antisymmetric halves cancel term for term. So the absolute-mean version happens
        // to be exact too, and no test can tell the two apart.
        //
        // KEPT, because that exactness is an accident of summation ORDER and MAGNITUDE, not a
        // property of the law: summed pairwise, or with Kahan compensation, or over any carrier
        // whose samples are not exactly antisymmetric, `sum0` becomes ~1e-16 and the absolute
        // version leaves mask 0 an ulp off a pure sine — while still passing every Approx
        // comparison in the suite. The subtraction costs one flop at build time and makes the
        // anchor independent of all of that. (T-CORE-02: keep the asymmetry and document why it is
        // not an oversight, or someone restores it.)
        //
        // The 512 offsets are computed ONCE into a shared table (2 KB), because the mask is a
        // modulation destination: the render-time oscillator changes it at audio rate, and taking
        // a fresh 2048-position mean per change would not be RT-safe. SegmentOps' space only —
        // the COMPLEX laws are zero-mean by construction (see their notes above).
        [[nodiscard]] static float dcOffsetFor (int mask) noexcept;

        // Renders the single-cycle frame for one mask value under `law` into frame
        // (kFrameLength floats). mask is wrapped into [0, maskSpaceFor (law)). Deterministic.
        static void renderMask (Law law, int mask, float* frame);

        // THE GRAY-NATIVE FRAME VIEW, and the `WavetableBank::RenderFrame` the catalogue captures
        // for the three COMPLEX banks: frame f holds mask `grayCode (f)`. `frame` is clamped into
        // [0, maskSpaceFor (law)) because the callers drive this with computed indices (the preview
        // bakes, the L4 pane). Equivalent to generate (law, Ordering::Gray, 0, maskSpaceFor (law))
        // frame for frame — one law, two views, held together by the tests.
        static void renderFrame (Law law, int frame, float* out);
        // THE RECIPE BEHIND A COMPLEX LAW, or `nullptr` for `SegmentOps` — which is ADR-037's
        // render-time bitmask view rather than a factory bank, so it has no recipe and no roster
        // row. Public for the same reason the five category generators' accessor is (T-WT-12): the
        // ADR-041 validator walks the SHIPPED recipes, and a nullptr here is how it tells a factory
        // bank from the one law that is not one, without a list of laws to keep in step.
        [[nodiscard]] static const WavetableRecipe* recipeFor (Law law) noexcept;

        // The mask rendered into each frame index of a bank covering masks
        // [maskFirst, maskFirst + maskCount) under `ordering` (see class note). `law` matters
        // only for Centroid (it sorts rendered frames). Inputs are clamped to the mask space.
        [[nodiscard]] static std::vector<int> layoutMasks (Law law, Ordering ordering, int maskFirst, int maskCount);

        // Generates the bank: layoutMasks() rendered contiguously, frame f at
        // [f * kFrameLength, (f+1) * kFrameLength) — the exact layout Wavetable::build()
        // consumes. Not RT-safe (allocates; libm/FFT inside).
        [[nodiscard]] static std::vector<float> generate (Law law, Ordering ordering, int maskFirst, int maskCount);

        // Spectral centroid of a single-cycle frame: the harmonic-magnitude-weighted mean
        // harmonic number over 1..length/2 (0 for an all-zero frame). The Centroid ordering
        // key; exposed so the monotone keystone is testable.
        [[nodiscard]] static double spectralCentroid (const float* frame, int length);
    };
}
