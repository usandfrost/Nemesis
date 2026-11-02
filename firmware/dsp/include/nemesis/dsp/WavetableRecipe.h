#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace nemesis::dsp
{
    // THE RECIPE SEAM (T-WT-11; ADR-041 decision 2, `family_overview.md` §2.3,
    // `docs/specs/specs/synth/wavetable-{kernels,roster}.md`).
    //
    // A bank is a KERNEL plus a RECIPE, and a recipe is DATA. Behind the six category generator
    // classes — which stay exactly as they are, so `WavetableCatalogue` and the three engines see
    // no change — sits a table of recipes over a set of parameterised kernels.
    // `XTableGenerator::renderFrame` becomes: look up the recipe for the `Bank` enum value,
    // dispatch to the kernel. Adding a bank is then a one-line data edit rather than a new switch
    // arm inside a category class.
    //
    // WHY THE SEAM EXISTS AT ALL, stated here because the cost only makes sense against it: the
    // pattern it replaces is one class per category with a `Bank` enum and a `switch` in
    // `renderFrame`, and `BasicTableGenerator` spent 89 header + 180 implementation lines on
    // THREE banks. Linearly extended to ADR-041's 200 that is ~12,000 lines of near-duplicated
    // switch arms — and every one of them is a place a DC convention or a normalisation factor
    // drifts apart between banks the distinctness tests then compare against each other, which is
    // the exact failure `HarmonicFrame` was written once to prevent.
    //
    // ═══ OWNER RULING R1 (2026-08-14): THE SHIPPED C++ LAW IS THE KERNEL ═══════════════════════
    // `Tools/wavetable-proto/` is ADR-041's validated reference implementation and is NOT a build
    // input — and it is also NOT the source of truth for the eighteen banks that already ship.
    // The two disagree, and the plan gate measured where:
    //   * `pulse` — the proto counts high samples as ceil(d·N) (103 at d = 0.05); the shipped
    //     `renderAnalog` uses lround(d·N) = 102, and each derives its two plateaus from ITS OWN
    //     count, so the banks differ in the edge position AND in both levels.
    //   * `quantize` — the proto is 32 → 3 levels at floor(v·L + 0.5)/L; the shipped law is
    //     64 → 3 at a mid-tread (L−1)/2 half-scale. Not a tie: `BasicTableGenerator.cpp` records
    //     that 32 → 3 WAS the first draft, that it duplicated the level count at frames 7 and 8
    //     so the bank morphed across nothing, and that 64 → 3 is the fix its monotone-coarsening
    //     and pairwise-distinct tests now hold. The proto carries a defect we already fixed.
    //   * `shape_morph` — the proto's triangle is 4·|t − 0.5| − 1 (a trough at t = 0.5); ours is
    //     t < 0.5 ? 4t − 1 : 3 − 4t (a peak there). Inverted, so every crossfade frame differs.
    // The ruling: the kernels below ARE the shipped code, parameterised. This is what ADR-041
    // decision 1 already ordered ("the existing eighteen keep their ids, labels, laws") and the
    // only reading under which the card's own risk line — "if a golden moves, the seam is WRONG"
    // — is satisfiable. `wavetable-kernels.md` is amended to the law actually implemented; the
    // roster's measured travel/mono columns were computed with the proto's kernels and are stale
    // for every kernel this card re-states (ledgered for T-WT-13/14, which must re-run the proto
    // against the corrected kernels before trusting a measurement).
    // ══════════════════════════════════════════════════════════════════════════════════════════
    //
    // SHARED FAMILY DATA IS SELECTED, NEVER COPIED INTO A RECIPE. Three shipped laws read their
    // numbers from somewhere else on purpose — `Bell`'s partials are `MetallicResonator`'s own
    // mode ratios, `Vox`/`Choir`'s vowels are the Peterson-Barney rows `FormantFilter` ships, and
    // each carries a comment saying a private copy of shared family data is a copy that drifts in
    // silence. So a recipe param that names such a table is an ENUM SELECTOR (`ModeSet::Metallic`,
    // `VowelPair::UtoA`), not a transcription of its values: the static_asserts that tie the two
    // together keep working, and the recipe stays five numbers wide.

    // The six §2.3 categories. Provenance and category are the only two facts a bank carries
    // (ADR-041 decision 12) — the kernel FAMILY that the browser's second level will group by is
    // derived from `WavetableRecipe::kernel`, never stored separately.
    enum class WavetableCategory : std::uint8_t {
        Basic = 0,
        Vocal = 1,
        Metallic = 2,
        Harmonic = 3,
        Chip = 4,
        Complex = 5
    };

    // The thirteen kernels the eighteen shipped banks need. ADR-041 names 32; the other nineteen
    // are T-WT-13's, and adding one here is deliberately a compile-time enum change so a recipe
    // cannot name a kernel that does not exist.
    enum class WavetableKernel : std::uint8_t {
        ShapeMorph = 0,
        Pulse = 1,
        Quantize = 2,
        Formants = 3,
        FormantSweep = 4,
        Comb = 5,
        Modal = 6,
        HarmonicCount = 7,
        OctaveStack = 8,
        Drawbars = 9,
        JustTriad = 10,
        Fm2Op = 11,
        MaskComplex = 12,

        // ─── T-WT-13's ports (ADR-041's 35, minus the thirteen above) ─────────────────────────
        // Leaf A — spectral, no phase law.
        OddEven = 13,
        NotchSweep = 14,
        Stretch = 15,
        TiltShelf = 16,
        Ringmod = 17,
        TiltSweep = 18, // NOT the shipped `Harm` — that is `OctaveStack`, T-WT-12b's split

        // Leaf B — spectral, WITH a phase law. ADR-041 decision 6 binds all four: a phase-only law
        // cannot move the centroid, so each carries a MAGNITUDE gesture alongside its phase one or
        // its declared frame ordering would be sorting on a measurement that never moves.
        NoiseSpectrum = 19,
        Scramble = 20,
        PhaseSpread = 21,
        Chord = 22, // NOT the shipped triad — that is `JustTriad`, T-WT-12b's split

        // Leaf C — time domain, symmetric by construction.
        DoublePulse = 23,
        Skew = 24,
        Sync = 25,
        Fold = 26,
        SampleHold = 27,

        // Leaf D — time domain, ITERATIVE. ADR-041 decision 9 is the constraint every one of
        // these is written around: two runs of the same binary must produce a bit-identical bank,
        // which rules out a real feedback loop (its output depends on where you started it), a
        // free-running accumulator, and any `static` mutable state. Each is a FIXED number of
        // steps from a STATED start.
        FmFeedback = 28,
        Chaos = 29,
        Bitwise = 30,
        PhaseDistort = 31,
        Walsh = 32,
        VectorBlend = 33,
        MaskSpace = 34 // NOT the shipped three — those are `MaskComplex`, T-WT-12b's split
    };

    // THE KERNEL'S OWN DISPLAY NAME (ADR-041 decision 12 / T-UI-69). DS Sheet-27's browser groups its
    // SECOND level by a bank's kernel — this is the label that band draws, derived here rather than
    // duplicated: an EXHAUSTIVE switch with no default arm, so a 36th kernel fails to compile here
    // instead of silently drawing a blank family header (the same discipline
    // `WavetableCatalogue.cpp`'s `categoryOf` and `BankBrowser::categoryLabel` already use for the
    // adjacent category switch). Spaced and upper-cased for the DS mono header role's convention
    // (`categoryLabel` draws "BASIC" the same way); the underlying id this transcribes is
    // `wavetable-kernels.md`'s own `##` heading, snake_case — this is that name's DISPLAY spelling,
    // never a second source of what the kernel IS.
    [[nodiscard]] inline const char* wavetableKernelLabel (WavetableKernel kernel) noexcept
    {
        switch (kernel)
        {
            case WavetableKernel::ShapeMorph:
                return "SHAPE MORPH";
            case WavetableKernel::Pulse:
                return "PULSE";
            case WavetableKernel::Quantize:
                return "QUANTIZE";
            case WavetableKernel::Formants:
                return "FORMANTS";
            case WavetableKernel::FormantSweep:
                return "FORMANT SWEEP";
            case WavetableKernel::Comb:
                return "COMB";
            case WavetableKernel::Modal:
                return "MODAL";
            case WavetableKernel::HarmonicCount:
                return "HARMONIC COUNT";
            case WavetableKernel::OctaveStack:
                return "OCTAVE STACK";
            case WavetableKernel::Drawbars:
                return "DRAWBARS";
            case WavetableKernel::JustTriad:
                return "JUST TRIAD";
            case WavetableKernel::Fm2Op:
                return "FM 2-OP";
            case WavetableKernel::MaskComplex:
                return "MASK COMPLEX";
            case WavetableKernel::OddEven:
                return "ODD/EVEN";
            case WavetableKernel::NotchSweep:
                return "NOTCH SWEEP";
            case WavetableKernel::Stretch:
                return "STRETCH";
            case WavetableKernel::TiltShelf:
                return "TILT SHELF";
            case WavetableKernel::Ringmod:
                return "RING MOD";
            case WavetableKernel::TiltSweep:
                return "TILT SWEEP";
            case WavetableKernel::NoiseSpectrum:
                return "NOISE SPECTRUM";
            case WavetableKernel::Scramble:
                return "SCRAMBLE";
            case WavetableKernel::PhaseSpread:
                return "PHASE SPREAD";
            case WavetableKernel::Chord:
                return "CHORD";
            case WavetableKernel::DoublePulse:
                return "DOUBLE PULSE";
            case WavetableKernel::Skew:
                return "SKEW";
            case WavetableKernel::Sync:
                return "SYNC";
            case WavetableKernel::Fold:
                return "FOLD";
            case WavetableKernel::SampleHold:
                return "SAMPLE & HOLD";
            case WavetableKernel::FmFeedback:
                return "FM FEEDBACK";
            case WavetableKernel::Chaos:
                return "CHAOS";
            case WavetableKernel::Bitwise:
                return "BITWISE";
            case WavetableKernel::PhaseDistort:
                return "PHASE DISTORT";
            case WavetableKernel::Walsh:
                return "WALSH";
            case WavetableKernel::VectorBlend:
                return "VECTOR BLEND";
            case WavetableKernel::MaskSpace:
                return "MASK SPACE";
        }
        return "";
    }

    // FRAME ORDERING IS DECLARED PER BANK (ADR-041 decision 5). §2.3 calls a bank "laid out in
    // generation order because nobody thought about it" a spec gap, not a default.
    //   * `Morph`      — the law's own sweep IS the intended order. All fifteen non-mask banks.
    //   * `Brightness` — frames sorted by MEASURED spectral centroid. No shipped bank uses it yet
    //                    (`MaskTableGenerator::Ordering::Centroid` is the measurement it will
    //                    reach for); declared here because a recipe must be able to say it.
    //   * `Gray`       — a mask space in Gray order, never re-sorted. The three COMPLEX banks.
    enum class FrameOrdering : std::uint8_t {
        Morph = 0,
        Brightness = 1,
        Gray = 2
    };

    // KERNEL PARAMETERS: a fixed-size POD, read through PER-KERNEL ACCESSOR STRUCTS.
    //
    // Not a map and not heap-allocated. `generate()` is off the audio thread — it allocates a
    // frame buffer and calls libm, and the class notes say so — but a per-frame map lookup would
    // still be a surprise in the one place a 200-bank catalogue is walked, and it would make the
    // recipe table non-constexpr for no gain.
    //
    // EIGHT scalars and four selectors. It was six until T-WT-14, and the reason it was six is
    // worth keeping next to the reason it is not: at thirteen kernels the widest was `Pulse`, which
    // expresses three shipped banks agreeing on being "a pulse" and on nothing else (a linear duty
    // sweep with no partner, a four-step register staircase with a rising partner, an integer-64ths
    // ramp with a fixed one) in five scalars and four selectors. At thirty-five kernels and 200
    // recipes the widest is `Formants`, which needs seven: an off-resonance floor, a width SCALE
    // and a uniform width OVERRIDE, two detunes, a side weight, and a free source exponent.
    //
    // Raising it costs two doubles per recipe — 3.2 KB across the whole table, in read-only memory
    // — and the alternative was overlaying two meanings on one slot, which is the thing the named
    // accessor structs exist to prevent. The count is a budget, not a law; what IS a law is that
    // `s[]` and `sel[]` are never indexed outside those structs, so a slot's meaning is stated
    // exactly once.
    struct KernelParams
    {
        static constexpr int kScalarCount = 8;
        static constexpr int kSelectorCount = 4;

        std::array<double, kScalarCount> s {};
        std::array<int, kSelectorCount> sel {};
    };

    // A recipe: `{ id, label, category, frames, kernel, params, ordering }` and nothing else
    // (ADR-041 decision 2). `id` and `label` are `string_view` over string literals in the table,
    // so a recipe stays a literal type and the whole table can live in read-only memory.
    struct WavetableRecipe
    {
        std::string_view id;
        std::string_view label;
        WavetableCategory category {};
        int frames { 0 };
        WavetableKernel kernel {};
        KernelParams params {};
        FrameOrdering ordering {};

        // [T-WT-14 leaf 4] THE BAKED FRAME ORDER — a permutation of [0, frames), and how
        // `FrameOrdering::Brightness` is realised. Null for `Morph` and `Gray`, which need none.
        //
        // WHY IT IS DATA AND NOT A SORT AT RENDER TIME. Decision 5 defines a brightness bank as its
        // frames sorted by MEASURED spectral centroid — a whole-bank fact, while `render()` is
        // per-frame. Measuring it inside `render()` costs 32 renders and 32 spectra PER FRAME, and
        // the preview bakes call `render()` about seventeen times a bank; caching it is `static`
        // mutable state, which ADR-041 decision 9 forbids outright and which would make a bank
        // depend on what was rendered before it.
        //
        // But the permutation is a DETERMINISTIC FUNCTION OF THE RECIPE — same kernel, same params,
        // same centroids, same order — so it can be computed once, off-line, and carried as five
        // hundred bytes of recipe data. That keeps `render()` per-frame, stateless and free, and it
        // keeps `generate()` exactly `renderFrame` laid out contiguously, which is the invariant
        // T-WT-11's fixture pins.
        //
        // THE COST IS THAT IT CAN DRIFT FROM THE LAW, and the answer is not care but a test: the
        // suite recomputes every brightness bank's permutation live and requires it equal to the
        // table. A kernel change that reorders a bank is then a red, not a silent wrong layout.
        const std::uint8_t* frameOrder { nullptr };
    };

    // ─── The per-kernel parameter views ───────────────────────────────────────────────────────
    //
    // Each names its own slots and is built from a `KernelParams` by value. Reading a recipe is
    // then `const auto p = kernel::Pulse::from (r.params);` and every use of `p.dutyFirst` says
    // what it is. The `make` helpers are the only place a slot INDEX appears on the write side,
    // so the table below cannot fill slot 2 while the kernel reads slot 3.
    namespace kernel
    {
        // The four classic shapes, in the order `shapeAt` saturates through. The set is a
        // SELECTOR rather than a list in the params because every shipped shape_morph bank walks
        // the same four; T-WT-13's `Saw Pulse` / `Tri Circle` / `Soft Hard` add sets, each a new
        // enumerator, which keeps the shape list out of the numeric params entirely.
        enum class ShapeSet : std::uint8_t {
            SawSquareTriSine = 0, // the shipped `Basics` — rich to pure, and frame 0 is the saw
            SawSquarePara = 1,
            TriSineHalfsineTri = 2, // a ROUND TRIP: it returns home, so its travel is 0 by design
            SineParaTriSawSquare = 3
        };

        // Which duty LAW drives a pulse bank. Three shipped banks are all "a pulse", and all
        // three move their duty differently — a linear sweep (`Analog`), a four-step register
        // staircase (`Chip`), and an integer-64ths ramp (`PWM`) — so the law is part of the
        // recipe rather than three near-copies of one kernel.
        enum class DutyLaw : std::uint8_t {
            Sweep = 0, // duty = lerp(first, last, morph)
            Staircase = 1, // duty = the register set, held for frames/steps frames each
            SixtyFourths = 2 // duty = (top − frame) / 64, an exact integer numerator per frame
        };

        // The quantiser's source wave.
        enum class Source : std::uint8_t {
            Sine = 0,
            Saw = 1
        };

        // ON PEAK-NORMALISATION (§2.3 / ADR-006's per-frame normalize), and on a claim this file
        // made and its own battery REFUTED — kept because the refutation is the useful part.
        //
        // Two spellings shipped: BASIC divided each sample by the frame peak, CHIP multiplied each
        // by the reciprocal. The seam originally carried a `Normalise` selector to preserve the
        // difference, on the reasoning that `x / p` and `x · (1/p)` are different floats and that
        // unifying them would therefore move `Analog` or both CHIP pulse banks — which the card
        // forbids. That reasoning was plausible, written into this header and into
        // `wavetable-kernels.md`, and **wrong**.
        //
        // The T-WT-11 battery aimed two mutations at it — force everyone onto one spelling, then
        // onto the other — expecting both to be KILLED. **Both SURVIVED**, and the digest fixture
        // over every frame of all eighteen banks is what they survived, so this is not a weak-test
        // survivor. The arithmetic says why: the two spellings differ by at most an ulp of DOUBLE,
        // and every frame is then cast to FLOAT, which has twenty-nine bits of headroom over that.
        // The difference cannot reach the output.
        //
        // So the selector was DEAD CODE (ADR-033 survivor category (b)) and is deleted, the two
        // laws are one law, and the "owner decision" this card had ledgered turned out not to
        // exist. `finishTimeDomain` divides — the spelling that carries the documented degenerate
        // guard.

        // The duty values a `Staircase` pulse steps through. A register set, not a sweep — which
        // is the whole point of the `Chip` bank.
        enum class DutySet : std::uint8_t {
            ChipRegisters = 0 // 12.5 / 25 / 50 / 75 %, held for frames/4 frames each
        };

        // The GLOTTAL SOURCE a spectral formant law sits on: the amplitudes are divided by this
        // before the resonances are applied. Two ship, and the difference is load-bearing rather
        // than cosmetic — `Formant`'s peak could not travel under a 1/k source, because harmonic
        // 27 would start 27× down and the fundamental would stay the loudest partial for the
        // whole top half of the sweep while every frame still sounded plausible.
        enum class SourceLaw : std::uint8_t {
            OverK = 0, // 1/k — the glottal tilt. VOCAL's `Vox` and `Choir`.
            OverSqrtK = 1, // 1/√k — the flatter source. VOCAL's `Formant`.
            // [T-WT-14] A FREE exponent, k^−tilt, read from the recipe. The two spellings above
            // stay because they are the shipped ones and `pow` is not obliged to reproduce `k` or
            // `sqrt(k)` to the last bit — a bank frozen on `OverK` must not start going through
            // `pow(k, -1.0)`. So this is a third arm, not a generalisation of the other two.
            Free = 2
        };

        // A vowel PAIR from the shared Peterson-Barney rows (`filter.md` §12.1) — selected, never
        // copied. See the shared-family-data note at the top of this file.
        //
        // [T-WT-14] A PAIR rather than two vowel selectors, and that is a budget decision stated
        // rather than hidden: two selectors plus phases, voices and source is five, and there are
        // four. The roster uses ten distinct pairs, so enumerating the pairs costs nothing and
        // keeps every slot's meaning single.
        //
        // `UtoA` IS NOT `U`→`A`. It is the SHIPPED tract — the same two centre triples, but with
        // `FormantFilter::kGain`'s fixed taper {1, 0.63, 0.4} at both ends, where the roster's own
        // /u/ and /a/ rows carry per-vowel gains. `Vox` and `Choir` are frozen on the former and
        // the nine roster pairs use the latter, so they are different data under one geometry and
        // are named apart.
        enum class VowelPair : std::uint8_t {
            UtoA = 0, // the shipped tract — `Vox` and `Choir`
            AtoE = 1,
            OoToA = 2,
            ItoU = 3,
            AtoU = 4,
            ErToI = 5,
            AtoO = 6,
            UtoEr = 7,
            ItoE = 8,
            AeToOo = 9
        };

        // A shared inharmonic MODE SET. `Metallic` is `MetallicResonator`'s own ratios, read
        // through its accessor and rounded, with a static_assert tying the counts together.
        //
        // [T-WT-14] THE OTHER EIGHT ARE REAL-VALUED, and that is the difference `addBin` exists
        // for: a modal partial is inharmonic by definition, so it lands BETWEEN bins, and rounding
        // it to one is what makes a bank freeze across consecutive frames. `Metallic` keeps its
        // rounded integers — the shipped `Bell` is frozen on them — and it reaches the same result
        // through `addBin` anyway, because a deposit at an integer position puts the whole gain on
        // one bin and exactly zero on its neighbour.
        enum class ModeSet : std::uint8_t {
            Metallic = 0,
            Bell = 1,
            Tube = 2,
            Bar = 3,
            Glass = 4,
            Gong = 5,
            Plate = 6,
            Cymbal = 7,
            Marimba = 8
        };

        // A partial SET for the spectral stacks that do not run over every harmonic.
        enum class PartialSet : std::uint8_t {
            Octaves = 0, // 1, 2, 4, … 2^6 — HARMONIC's `Harm`
            JustTriad = 1 // 20, 24, 25, 30, 40 — HARMONIC's `Chord`
        };

        // An organ REGISTRATION: the drawbar footages plus their gains and which are quints.
        //
        // [T-WT-14] `Classic` IS THE SHIPPED LAW AND KEEPS ITS OWN ARM — one registration whose
        // three QUINTS fade in — while the eight below are the reference implementation's
        // registrations, morphed as a PAIR. T-WT-12b ruled the two equivalent and they are, as
        // algebra: `g·(0.05 + 0.95m)` against `g·0.05 + (g − g·0.05)·m`. They are NOT equivalent
        // as floats — the operation order differs — so routing `Organ` through the pair arm would
        // move a frozen bank by an ulp for no gain. The asymmetry is kept deliberately and pinned
        // by a test that the two agree to float precision, so they cannot drift apart as LAWS
        // while being allowed to differ in their last bit (T-CORE-02: keep the asymmetry and
        // document why it is not an oversight, or someone restores it).
        enum class Registration : std::uint8_t {
            Classic = 0, // the shipped `Organ` — bit-frozen, its own arm
            Flute = 1,
            Full = 2,
            Reed = 3,
            Jazz = 4,
            Gospel = 5,
            Tibia = 6,
            Principal = 7,
            Celeste = 8
        };

        // A phase law over the harmonic stack. `Schroeder` is φ_k = −π·k²/K, which spreads each
        // harmonic's peak to a different point in the cycle and so lowers the crest factor for a
        // FIXED magnitude spectrum — how `Choir` and `Vox` share a spectrum without sounding
        // alike.
        enum class PhaseLaw : std::uint8_t {
            Zero = 0,
            Schroeder = 1
        };

        // Which 4-bit mask LAW a COMPLEX bank is. `SegmentOps` is deliberately absent: it is
        // ADR-037's render-time bitmask view, not a factory bank, and it has no recipe.
        enum class MaskLaw : std::uint8_t {
            Prism = 0,
            Shatter = 1,
            Weave = 2
        };

        struct ShapeMorph
        {
            ShapeSet shapes {};

            [[nodiscard]] static constexpr ShapeMorph from (const KernelParams& p) noexcept
            {
                return { static_cast<ShapeSet> (p.sel[0]) };
            }
            [[nodiscard]] static constexpr KernelParams make (ShapeSet shapes) noexcept
            {
                KernelParams p {};
                p.sel[0] = static_cast<int> (shapes);
                return p;
            }
        };

        struct Pulse
        {
            double dutyFirst {}; // `Sweep` only
            double dutyLast {}; // `Sweep` only
            double partnerLevelFirst {}; // the octave-up square partner; 0 = no partner at all
            double partnerLevelLast {};
            double dutyTop {}; // `SixtyFourths` only: the numerator at frame 0
            DutyLaw law {};
            DutySet dutySet {}; // `Staircase` only
            int partnerRatio {}; // cycles per frame of the square partner

            [[nodiscard]] static constexpr Pulse from (const KernelParams& p) noexcept
            {
                return { p.s[0],
                    p.s[1],
                    p.s[2],
                    p.s[3],
                    p.s[4],
                    static_cast<DutyLaw> (p.sel[0]),
                    static_cast<DutySet> (p.sel[1]),
                    p.sel[2] };
            }
            [[nodiscard]] static constexpr KernelParams make (DutyLaw law,
                double dutyFirst,
                double dutyLast,
                double dutyTop,
                DutySet dutySet,
                double partnerLevelFirst,
                double partnerLevelLast,
                int partnerRatio) noexcept
            {
                KernelParams p {};
                p.s[0] = dutyFirst;
                p.s[1] = dutyLast;
                p.s[2] = partnerLevelFirst;
                p.s[3] = partnerLevelLast;
                p.s[4] = dutyTop;
                p.sel[0] = static_cast<int> (law);
                p.sel[1] = static_cast<int> (dutySet);
                p.sel[2] = partnerRatio;
                return p;
            }
        };

        struct Quantize
        {
            double levelsFirst {};
            double levelsLast {};
            Source source {};

            [[nodiscard]] static constexpr Quantize from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], static_cast<Source> (p.sel[0]) };
            }
            [[nodiscard]] static constexpr KernelParams make (double levelsFirst, double levelsLast, Source source) noexcept
            {
                KernelParams p {};
                p.s[0] = levelsFirst;
                p.s[1] = levelsLast;
                p.sel[0] = static_cast<int> (source);
                return p;
            }
        };

        struct Formants
        {
            double floorLevel {}; // the OFF-RESONANCE floor — load-bearing, not a fudge
            double widthScale {}; // 1 = one tract, 2 = the ensemble smear
            // THE TWO OUTER VOICES' DETUNE FACTORS, STATED RATHER THAN DERIVED FROM A ± SPREAD.
            // `{0.94, 1.06}` and `{1 − 0.06, 1 + 0.06}` are the same intent and not the same
            // doubles — `1.0 − 0.06` lands exactly halfway between two representable values and
            // resolves by ties-to-even, which is not a coin this seam gets to flip. The shipped
            // law writes the endpoints as literals, so the recipe carries the literals.
            double detuneLow {};
            double detuneHigh {};
            double sideWeight {}; // the two outer voices' weight; unused with voices == 1
            // [T-WT-14] A UNIFORM width for all three formants, overriding the per-formant table.
            // **ZERO MEANS "use the table"**, and that sentinel is sound rather than convenient: a
            // zero-width resonance is a division by zero and `HarmonicFrame::resonance` already
            // answers 0 for any non-positive width, so 0 is outside the parameter's domain and
            // cannot collide with a real value. The shipped banks pass 0 and take the table.
            double uniformWidth {};
            double tilt {}; // `SourceLaw::Free` only
            VowelPair vowels {};
            PhaseLaw phases {};
            int voices {};
            SourceLaw source {};

            [[nodiscard]] static constexpr Formants from (const KernelParams& p) noexcept
            {
                return { p.s[0],
                    p.s[1],
                    p.s[2],
                    p.s[3],
                    p.s[4],
                    p.s[5],
                    p.s[6],
                    static_cast<VowelPair> (p.sel[0]),
                    static_cast<PhaseLaw> (p.sel[1]),
                    p.sel[2],
                    static_cast<SourceLaw> (p.sel[3]) };
            }
            [[nodiscard]] static constexpr KernelParams make (VowelPair vowels,
                int voices,
                double floorLevel,
                double widthScale,
                double detuneLow,
                double detuneHigh,
                double sideWeight,
                SourceLaw source,
                PhaseLaw phases,
                double uniformWidth = 0.0,
                double tilt = 0.0) noexcept
            {
                KernelParams p {};
                p.s[0] = floorLevel;
                p.s[1] = widthScale;
                p.s[2] = detuneLow;
                p.s[3] = detuneHigh;
                p.s[4] = sideWeight;
                p.s[5] = uniformWidth;
                p.s[6] = tilt;
                p.sel[0] = static_cast<int> (vowels);
                p.sel[1] = static_cast<int> (phases);
                p.sel[2] = voices;
                p.sel[3] = static_cast<int> (source);
                return p;
            }
        };

        struct FormantSweep
        {
            double centreFirstHz {};
            double centreLastHz {};
            double floorLevel {};
            double width {}; // octaves
            double peakGain {}; // the resonance's own weight over the floor
            double tilt {}; // `SourceLaw::Free` only
            SourceLaw source {};

            [[nodiscard]] static constexpr FormantSweep from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2], p.s[3], p.s[4], p.s[5], static_cast<SourceLaw> (p.sel[0]) };
            }
            [[nodiscard]] static constexpr KernelParams make (double centreFirstHz,
                double centreLastHz,
                double floorLevel,
                double width,
                double peakGain,
                SourceLaw source,
                double tilt = 0.0) noexcept
            {
                KernelParams p {};
                p.s[0] = centreFirstHz;
                p.s[1] = centreLastHz;
                p.s[2] = floorLevel;
                p.s[3] = width;
                p.s[4] = peakGain;
                p.s[5] = tilt;
                p.sel[0] = static_cast<int> (source);
                return p;
            }
        };

        struct Comb
        {
            double spacingFirst {}; // non-integer on purpose — see the kernel note
            double spacingLast {};
            double sourceTilt {}; // k^−tilt source
            double combExponent {};
            // [T-WT-14] The source tilt may MOVE across the bank. Two roster banks need it and
            // `Metal` does not — it holds its tilt fixed at k^−0.3, so its recipe passes 0 and the
            // arithmetic is `tilt − 0·m`, which is the identity. The shipped bank is unmoved by
            // the widening, which is the property every widening in this card has to have.
            double tiltMove {};

            [[nodiscard]] static constexpr Comb from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2], p.s[3], p.s[4] };
            }
            [[nodiscard]] static constexpr KernelParams make (double spacingFirst,
                double spacingLast,
                double sourceTilt,
                double combExponent,
                double tiltMove = 0.0) noexcept
            {
                KernelParams p {};
                p.s[0] = spacingFirst;
                p.s[1] = spacingLast;
                p.s[2] = sourceTilt;
                p.s[3] = combExponent;
                p.s[4] = tiltMove;
                return p;
            }
        };

        struct Modal
        {
            double tiltFloor {}; // the tilt at the last frame; mode i is scaled by tilt^i
            // [T-WT-14] The harmonic the mode set is anchored on. A mode set is a set of RATIOS,
            // so where it sits in the series is a separate number — the roster's thirteen modal
            // banks reuse eight ratio sets at eight different bases, which is most of why the
            // category has thirty-five banks and eight tables.
            double base {};
            ModeSet modes {};

            [[nodiscard]] static constexpr Modal from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], static_cast<ModeSet> (p.sel[0]) };
            }
            [[nodiscard]] static constexpr KernelParams make (ModeSet modes, double tiltFloor, double base = 1.0) noexcept
            {
                KernelParams p {};
                p.s[0] = tiltFloor;
                p.s[1] = base;
                p.sel[0] = static_cast<int> (modes);
                return p;
            }
        };

        struct HarmonicCount
        {
            double countFirst {}; // partial count at frame 0
            double countStep {}; // added per frame — an integer count, kept as a double slot
            double tilt {}; // k^−tilt over the partials that are present

            [[nodiscard]] static constexpr HarmonicCount from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2] };
            }
            [[nodiscard]] static constexpr KernelParams make (double countFirst, double countStep, double tilt) noexcept
            {
                KernelParams p {};
                p.s[0] = countFirst;
                p.s[1] = countStep;
                p.s[2] = tilt;
                return p;
            }
        };

        struct OctaveStack
        {
            double tiltFirst {};
            double tiltLast {};
            PartialSet partials {};

            [[nodiscard]] static constexpr OctaveStack from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], static_cast<PartialSet> (p.sel[0]) };
            }
            [[nodiscard]] static constexpr KernelParams make (PartialSet partials, double tiltFirst, double tiltLast) noexcept
            {
                KernelParams p {};
                p.s[0] = tiltFirst;
                p.s[1] = tiltLast;
                p.sel[0] = static_cast<int> (partials);
                return p;
            }
        };

        struct Drawbars
        {
            double quintFadeFirst {}; // `Classic` only
            double quintFadeLast {}; // `Classic` only
            double tiltMove {}; // the PAIR arm only — two similar registrations barely morph
            Registration registration {}; // `Classic`, or the FROM of a pair
            Registration registrationTo {}; // the TO of a pair; ignored under `Classic`

            [[nodiscard]] static constexpr Drawbars from (const KernelParams& p) noexcept
            {
                return { p.s[0],
                    p.s[1],
                    p.s[2],
                    static_cast<Registration> (p.sel[0]),
                    static_cast<Registration> (p.sel[1]) };
            }
            [[nodiscard]] static constexpr KernelParams make (Registration registration,
                double quintFadeFirst,
                double quintFadeLast) noexcept
            {
                KernelParams p {};
                p.s[0] = quintFadeFirst;
                p.s[1] = quintFadeLast;
                p.sel[0] = static_cast<int> (registration);
                return p;
            }
            // The PAIR form, for the registrations that morph between two drawn sets.
            [[nodiscard]] static constexpr KernelParams makePair (Registration from,
                Registration to,
                double tiltMove = 0.0) noexcept
            {
                KernelParams p {};
                p.s[2] = tiltMove;
                p.sel[0] = static_cast<int> (from);
                p.sel[1] = static_cast<int> (to);
                return p;
            }
        };

        struct JustTriad
        {
            double rootGain {};
            double thirdGain {};
            double fifthGain {};
            double octaveGain {};
            PartialSet partials {};

            [[nodiscard]] static constexpr JustTriad from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2], p.s[3], static_cast<PartialSet> (p.sel[0]) };
            }
            [[nodiscard]] static constexpr KernelParams make (PartialSet partials,
                double rootGain,
                double thirdGain,
                double fifthGain,
                double octaveGain) noexcept
            {
                KernelParams p {};
                p.s[0] = rootGain;
                p.s[1] = thirdGain;
                p.s[2] = fifthGain;
                p.s[3] = octaveGain;
                p.sel[0] = static_cast<int> (partials);
                return p;
            }
        };

        struct Fm2Op
        {
            double indexFirst {};
            double indexLast {};
            double ratio {}; // modulator : carrier. Integer keeps the frame exactly periodic.

            [[nodiscard]] static constexpr Fm2Op from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2] };
            }
            [[nodiscard]] static constexpr KernelParams make (double indexFirst, double indexLast, double ratio) noexcept
            {
                KernelParams p {};
                p.s[0] = indexFirst;
                p.s[1] = indexLast;
                p.s[2] = ratio;
                return p;
            }
        };

        struct MaskComplex
        {
            MaskLaw law {};

            [[nodiscard]] static constexpr MaskComplex from (const KernelParams& p) noexcept
            {
                return { static_cast<MaskLaw> (p.sel[0]) };
            }
            [[nodiscard]] static constexpr KernelParams make (MaskLaw law) noexcept
            {
                KernelParams p {};
                p.sel[0] = static_cast<int> (law);
                return p;
            }
        };

        // THE SHAPE SET, extended for T-WT-13's time-domain kernels. `Para` and `HalfSine` are
        // new and both are stated ZERO-MEAN AT THE SOURCE rather than corrected per bank: the mean
        // of 1 − 2v² over a cycle is exactly 1/3 and the mean of 2·sin(πt) is exactly 4/π, so each
        // carries its own subtraction as a literal. That is what lets `vector_blend` interpolate
        // four of them and stay zero-mean by construction instead of by a DC pass — a correction
        // nobody chose is how a DC convention drifts between banks the distinctness tests then
        // compare against each other.
        //
        // Separate from `ShapeSet`, which names a LIST the shape-morph crossfade walks; this names
        // ONE shape, which is what a source parameter needs.
        enum class Shape : std::uint8_t {
            Saw = 0,
            Square = 1,
            Triangle = 2,
            Sine = 3,
            Para = 4,
            HalfSine = 5
        };

        // A bounded iterated MAP for `Chaos`. Every one is clamped into [0, 1] each step, which
        // is what makes "never divergent" a property of the code rather than a hope about the
        // parameters a recipe happens to carry.
        enum class ChaosMap : std::uint8_t {
            Logistic = 0,
            Tent = 1,
            Sine = 2
        };

        // The integer op a `Bitwise` kernel runs over its phase counter.
        enum class BitwiseOp : std::uint8_t {
            Xor = 0,
            And = 1,
            Mod = 2,
            Mul = 3
        };

        // The four CORNERS of a vector blend, in the VS order the interpolation reads them:
        // top-left, top-right, bottom-right, bottom-left.
        enum class CornerSet : std::uint8_t {
            SawSquareTriSine = 0,
            ParaHalfsineSawTri = 1,
            SquareSawSquarePara = 2,
            SineHalfsineTriPara = 3
        };

        // The path a vector blend walks across the square. `Circle` is the ROUND-TRIP case
        // ADR-041 decision 6's travel-OR-motion disjunction exists for: it ends where it started,
        // so its spectral travel is 0 BY DESIGN and only its waveform motion means anything.
        enum class VectorPath : std::uint8_t {
            Diagonal = 0,
            Circle = 1,
            Cross = 2
        };

        // Which harmonic each of the four bits addresses under the generic `MaskSpace` law.
        // `MaskComplex` — the shipped Prism/Shatter/Weave — has no bit list at all; its bands,
        // holds and blocks ARE the law. That difference is the whole of T-WT-12b's third split.
        enum class MaskBits : std::uint8_t {
            OneTwoThreeFive = 0,
            Odd = 1,
            Octaves = 2,
            WideThree = 3,
            Fibonacci = 4,
            OddNine = 5,
            Primes = 6,
            Sparse = 7
        };

        // The generic mask space's three laws share NAMES with `MaskComplex`'s and nothing else,
        // which is exactly what the split found — so they are their own enum rather than a reuse
        // of `MaskLaw`, and a recipe cannot accidentally name the wrong family.
        enum class MaskSpaceLaw : std::uint8_t {
            Prism = 0,
            Shatter = 1,
            Weave = 2
        };

        // A CHORD VOICING for the `Chord` kernel: two interval sets and their two weight sets,
        // morphed. An ENUM SELECTOR rather than params because the lists are variable-length and a
        // list does not fit six scalars — the same shared-family-data rule the thirteen already
        // follow, applied to data that is this kernel's own rather than another module's.
        enum class ChordVoicing : std::uint8_t {
            MinorToMajor = 0,
            MajorToOctave = 1,
            MinorToWide = 2,
            Fifths = 3,
            Sus = 4,
            Seventh = 5,
            Octaves = 6
        };

        // ═══ T-WT-13 leaf A — the SPECTRAL kernels with no phase law ═══════════════════════════
        //
        // THE HARMONIC DEPTH IS 256 HERE, NOT 64, AND THE ASYMMETRY IS DELIBERATE. The five
        // SHIPPED spectral kernels write their laws over their category's `kMaxHarmonic`, which is
        // 64 in all three, and they must keep doing so — the T-WT-11 digests freeze the eighteen.
        // The new kernels cannot: the roster specifies `Tilt Full` at a centroid of 139.98, and a
        // magnitude-weighted mean is bounded by the largest index carrying weight, so 64 harmonics
        // cannot reach it. New spectral laws therefore run to `HarmonicFrame::kMaxHarmonics` (256),
        // which is also the reference implementation's `MAXH`. Do not "unify" the two depths — the
        // difference is what keeps the eighteen bit-identical.

        struct OddEven
        {
            double balanceFirst {}; // 1 = odd only, 0.5 = equal
            double balanceLast {};
            double tilt {};
            double tiltMove {}; // the SECOND axis: balance alone barely moves the spectrum, and a
                // bank that does not travel is one the validator fails

            [[nodiscard]] static constexpr OddEven from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2], p.s[3] };
            }
            [[nodiscard]] static constexpr KernelParams make (double balanceFirst,
                double balanceLast,
                double tilt,
                double tiltMove) noexcept
            {
                KernelParams p {};
                p.s[0] = balanceFirst;
                p.s[1] = balanceLast;
                p.s[2] = tilt;
                p.s[3] = tiltMove;
                return p;
            }
        };

        struct NotchSweep
        {
            double centreFirst {}; // in HARMONIC index, not Hz: the notch's identity is which
            double centreLast {}; // partial it sits on, and the roster sweeps it over the series
            double width {}; // octaves
            double depth {};
            double tilt {};

            [[nodiscard]] static constexpr NotchSweep from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2], p.s[3], p.s[4] };
            }
            [[nodiscard]] static constexpr KernelParams make (double centreFirst,
                double centreLast,
                double width,
                double depth,
                double tilt) noexcept
            {
                KernelParams p {};
                p.s[0] = centreFirst;
                p.s[1] = centreLast;
                p.s[2] = width;
                p.s[3] = depth;
                p.s[4] = tilt;
                return p;
            }
        };

        struct Stretch
        {
            double stretchFirst {}; // partial i sits at i^s; s = 1 is a harmonic series
            double stretchLast {};
            double tilt {};
            int partials {};

            [[nodiscard]] static constexpr Stretch from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2], p.sel[0] };
            }
            [[nodiscard]] static constexpr KernelParams make (double stretchFirst,
                double stretchLast,
                double tilt,
                int partials) noexcept
            {
                KernelParams p {};
                p.s[0] = stretchFirst;
                p.s[1] = stretchLast;
                p.s[2] = tilt;
                p.sel[0] = partials;
                return p;
            }
        };

        struct TiltShelf
        {
            double hingeFirst {}; // a MOVING hinge — the gesture is where the spectrum bends,
            double hingeLast {}; // not how steeply it falls overall
            double slopeLow {};
            double slopeHigh {};

            [[nodiscard]] static constexpr TiltShelf from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2], p.s[3] };
            }
            [[nodiscard]] static constexpr KernelParams make (double hingeFirst,
                double hingeLast,
                double slopeLow,
                double slopeHigh) noexcept
            {
                KernelParams p {};
                p.s[0] = hingeFirst;
                p.s[1] = hingeLast;
                p.s[2] = slopeLow;
                p.s[3] = slopeHigh;
                return p;
            }
        };

        struct Ringmod
        {
            double modFirst {};
            double modLast {};
            double tilt {};
            int partials {};

            [[nodiscard]] static constexpr Ringmod from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2], p.sel[0] };
            }
            [[nodiscard]] static constexpr KernelParams make (double modFirst,
                double modLast,
                double tilt,
                int partials) noexcept
            {
                KernelParams p {};
                p.s[0] = modFirst;
                p.s[1] = modLast;
                p.s[2] = tilt;
                p.sel[0] = partials;
                return p;
            }
        };

        // `tilt_sweep` PROPER — k^-tilt over the whole series, the plainest brightness law there
        // is. The shipped `Harm` is `OctaveStack` (T-WT-12b's split); this is the law the roster's
        // three `Tilt *` banks are written in, and the one that can reach a centroid of 140.
        struct TiltSweep
        {
            double tiltFirst {};
            double tiltLast {};

            [[nodiscard]] static constexpr TiltSweep from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1] };
            }
            [[nodiscard]] static constexpr KernelParams make (double tiltFirst, double tiltLast) noexcept
            {
                KernelParams p {};
                p.s[0] = tiltFirst;
                p.s[1] = tiltLast;
                return p;
            }
        };

        // ═══ T-WT-13 leaf B — the SPECTRAL kernels that carry a PHASE law ══════════════════════
        //
        // ADR-041 DECISION 6, AND IT IS A CONSTRAINT ON THE LAW RATHER THAN ON THE TEST. A
        // phase-only law cannot move the spectral centroid, because the centroid is a MAGNITUDE
        // statistic and a phase law leaves magnitudes untouched. Such a bank still morphs audibly
        // — the peak-normalised waveform changes completely — but it has no brightness DIRECTION,
        // and a frame ordering sorted by a measurement that does not move is an arbitrary
        // ordering, i.e. exactly the spec gap decision 5 exists to close. So every law here
        // carries a magnitude gesture beside its phase gesture, and each one names which field
        // that is.

        struct NoiseSpectrum
        {
            double tiltFirst {}; // the magnitude gesture
            double tiltLast {};
            double densityFirst {};
            double densityLast {};
            std::uint32_t seed {}; // a recipe CONSTANT — decision 9's only permitted randomness

            [[nodiscard]] static constexpr NoiseSpectrum from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2], p.s[3], static_cast<std::uint32_t> (p.sel[0]) };
            }
            [[nodiscard]] static constexpr KernelParams make (std::uint32_t seed,
                double tiltFirst,
                double tiltLast,
                double densityFirst,
                double densityLast) noexcept
            {
                KernelParams p {};
                p.s[0] = tiltFirst;
                p.s[1] = tiltLast;
                p.s[2] = densityFirst;
                p.s[3] = densityLast;
                p.sel[0] = static_cast<int> (seed);
                return p;
            }
        };

        struct Scramble
        {
            double depthFirst {};
            double depthLast {};
            double tilt {};
            double tiltMove {}; // the magnitude gesture decision 6 requires
            std::uint32_t seed {};

            [[nodiscard]] static constexpr Scramble from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2], p.s[3], static_cast<std::uint32_t> (p.sel[0]) };
            }
            [[nodiscard]] static constexpr KernelParams make (std::uint32_t seed,
                double depthFirst,
                double depthLast,
                double tilt,
                double tiltMove) noexcept
            {
                KernelParams p {};
                p.s[0] = depthFirst;
                p.s[1] = depthLast;
                p.s[2] = tilt;
                p.s[3] = tiltMove;
                p.sel[0] = static_cast<int> (seed);
                return p;
            }
        };

        struct PhaseSpread
        {
            double spreadFirst {};
            double spreadLast {};
            double tilt {};
            double ripple {};
            double slide {}; // the magnitude gesture: the ripple COMB slides across the spectrum
            int voices {};

            [[nodiscard]] static constexpr PhaseSpread from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2], p.s[3], p.s[4], p.sel[0] };
            }
            [[nodiscard]] static constexpr KernelParams make (double spreadFirst,
                double spreadLast,
                int voices,
                double tilt,
                double ripple,
                double slide) noexcept
            {
                KernelParams p {};
                p.s[0] = spreadFirst;
                p.s[1] = spreadLast;
                p.s[2] = tilt;
                p.s[3] = ripple;
                p.s[4] = slide;
                p.sel[0] = voices;
                return p;
            }
        };

        // `chord` PROPER — several harmonic SERIES at just ratios, with voices ENTERING on a
        // weight morph. The shipped bank is `JustTriad` (T-WT-12b's split), and the difference is
        // a measurement rather than a preference: moving the INTERVALS alone shifts each partial
        // by a fraction of a bin and the centroid barely stirs, which is why `JustTriad` measures
        // the lowest travel in its whole category. Voices entering is what moves the spectrum.
        // (No phase law of its own; it lives in this leaf because it is the fourth spectral port.)
        struct Chord
        {
            double tilt {};
            double tiltMove {};
            ChordVoicing voicing {};
            int partials {}; // partials per voice — a SERIES, which is the whole distinction

            [[nodiscard]] static constexpr Chord from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], static_cast<ChordVoicing> (p.sel[0]), p.sel[1] };
            }
            [[nodiscard]] static constexpr KernelParams make (ChordVoicing voicing,
                int partials,
                double tilt,
                double tiltMove) noexcept
            {
                KernelParams p {};
                p.s[0] = tilt;
                p.s[1] = tiltMove;
                p.sel[0] = static_cast<int> (voicing);
                p.sel[1] = partials;
                return p;
            }
        };

        // ═══ T-WT-13 leaf C — the TIME-domain kernels that are symmetric by construction ═══════

        struct DoublePulse
        {
            double widthFirst {};
            double widthLast {};
            double gap {}; // where the second pulse starts, in normalised phase

            [[nodiscard]] static constexpr DoublePulse from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2] };
            }
            [[nodiscard]] static constexpr KernelParams make (double widthFirst, double widthLast, double gap) noexcept
            {
                KernelParams p {};
                p.s[0] = widthFirst;
                p.s[1] = widthLast;
                p.s[2] = gap;
                return p;
            }
        };

        struct Skew
        {
            double skewFirst {}; // 0.5 is a triangle; the rails are saws either way up
            double skewLast {};

            [[nodiscard]] static constexpr Skew from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1] };
            }
            [[nodiscard]] static constexpr KernelParams make (double skewFirst, double skewLast) noexcept
            {
                KernelParams p {};
                p.s[0] = skewFirst;
                p.s[1] = skewLast;
                return p;
            }
        };

        struct Sync
        {
            double ratioFirst {};
            double ratioLast {};

            [[nodiscard]] static constexpr Sync from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1] };
            }
            [[nodiscard]] static constexpr KernelParams make (double ratioFirst, double ratioLast) noexcept
            {
                KernelParams p {};
                p.s[0] = ratioFirst;
                p.s[1] = ratioLast;
                return p;
            }
        };

        struct Fold
        {
            double gainFirst {};
            double gainLast {};
            Shape source {};
            int stages {};

            [[nodiscard]] static constexpr Fold from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], static_cast<Shape> (p.sel[0]), p.sel[1] };
            }
            [[nodiscard]] static constexpr KernelParams make (Shape source,
                double gainFirst,
                double gainLast,
                int stages) noexcept
            {
                KernelParams p {};
                p.s[0] = gainFirst;
                p.s[1] = gainLast;
                p.sel[0] = static_cast<int> (source);
                p.sel[1] = stages;
                return p;
            }
        };

        struct SampleHold
        {
            double stepsFirst {};
            double stepsLast {};
            Shape source {};

            [[nodiscard]] static constexpr SampleHold from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], static_cast<Shape> (p.sel[0]) };
            }
            [[nodiscard]] static constexpr KernelParams make (Shape source, double stepsFirst, double stepsLast) noexcept
            {
                KernelParams p {};
                p.s[0] = stepsFirst;
                p.s[1] = stepsLast;
                p.sel[0] = static_cast<int> (source);
                return p;
            }
        };

        // ═══ T-WT-13 leaf D — the TIME-domain kernels that ITERATE ═════════════════════════════

        struct FmFeedback
        {
            double indexFirst {};
            double indexLast {};
            int iterations {}; // FIXED, never a loop — decision 9

            [[nodiscard]] static constexpr FmFeedback from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.sel[0] };
            }
            [[nodiscard]] static constexpr KernelParams make (double indexFirst, double indexLast, int iterations) noexcept
            {
                KernelParams p {};
                p.s[0] = indexFirst;
                p.s[1] = indexLast;
                p.sel[0] = iterations;
                return p;
            }
        };

        struct Chaos
        {
            double rateFirst {};
            double rateLast {};
            double seedValue {}; // x0 — a recipe constant, so the orbit starts in the same place
            ChaosMap map {};

            [[nodiscard]] static constexpr Chaos from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], p.s[2], static_cast<ChaosMap> (p.sel[0]) };
            }
            [[nodiscard]] static constexpr KernelParams make (ChaosMap map,
                double rateFirst,
                double rateLast,
                double seedValue) noexcept
            {
                KernelParams p {};
                p.s[0] = rateFirst;
                p.s[1] = rateLast;
                p.s[2] = seedValue;
                p.sel[0] = static_cast<int> (map);
                return p;
            }
        };

        struct Bitwise
        {
            double shiftFirst {}; // fractional ON PURPOSE — the two bracketing integers crossfade
            double shiftLast {};
            BitwiseOp op {};
            int mask {};
            int multiplier {};

            [[nodiscard]] static constexpr Bitwise from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], static_cast<BitwiseOp> (p.sel[0]), p.sel[1], p.sel[2] };
            }
            [[nodiscard]] static constexpr KernelParams make (BitwiseOp op,
                double shiftFirst,
                double shiftLast,
                int mask,
                int multiplier) noexcept
            {
                KernelParams p {};
                p.s[0] = shiftFirst;
                p.s[1] = shiftLast;
                p.sel[0] = static_cast<int> (op);
                p.sel[1] = mask;
                p.sel[2] = multiplier;
                return p;
            }
        };

        struct PhaseDistort
        {
            double breakFirst {};
            double breakLast {};
            Shape source {};

            [[nodiscard]] static constexpr PhaseDistort from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1], static_cast<Shape> (p.sel[0]) };
            }
            [[nodiscard]] static constexpr KernelParams make (Shape source, double breakFirst, double breakLast) noexcept
            {
                KernelParams p {};
                p.s[0] = breakFirst;
                p.s[1] = breakLast;
                p.sel[0] = static_cast<int> (source);
                return p;
            }
        };

        struct Walsh
        {
            double countFirst {};
            double countLast {};

            [[nodiscard]] static constexpr Walsh from (const KernelParams& p) noexcept
            {
                return { p.s[0], p.s[1] };
            }
            [[nodiscard]] static constexpr KernelParams make (double countFirst, double countLast) noexcept
            {
                KernelParams p {};
                p.s[0] = countFirst;
                p.s[1] = countLast;
                return p;
            }
        };

        struct VectorBlend
        {
            CornerSet corners {};
            VectorPath path {};

            [[nodiscard]] static constexpr VectorBlend from (const KernelParams& p) noexcept
            {
                return { static_cast<CornerSet> (p.sel[0]), static_cast<VectorPath> (p.sel[1]) };
            }
            [[nodiscard]] static constexpr KernelParams make (CornerSet corners, VectorPath path) noexcept
            {
                KernelParams p {};
                p.sel[0] = static_cast<int> (corners);
                p.sel[1] = static_cast<int> (path);
                return p;
            }
        };

        struct MaskSpace
        {
            MaskSpaceLaw law {};
            MaskBits bits {};

            [[nodiscard]] static constexpr MaskSpace from (const KernelParams& p) noexcept
            {
                return { static_cast<MaskSpaceLaw> (p.sel[0]), static_cast<MaskBits> (p.sel[1]) };
            }
            [[nodiscard]] static constexpr KernelParams make (MaskSpaceLaw law, MaskBits bits) noexcept
            {
                KernelParams p {};
                p.sel[0] = static_cast<int> (law);
                p.sel[1] = static_cast<int> (bits);
                return p;
            }
        };
    } // namespace kernel
} // namespace nemesis::dsp
