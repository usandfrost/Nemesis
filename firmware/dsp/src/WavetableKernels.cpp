#include <nemesis/dsp/WavetableKernels.h>

#include <nemesis/dsp/ChipTableGenerator.h>
#include <nemesis/dsp/HarmonicFrame.h>
#include <nemesis/dsp/HarmonicTableGenerator.h>
#include <nemesis/dsp/MaskTableGenerator.h>
#include <nemesis/dsp/MetallicTableGenerator.h>
#include <nemesis/dsp/VocalTableGenerator.h>
#include <nemesis/dsp/WavetableBank.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

// THE THIRTEEN KERNELS, each the law that already shipped (owner ruling R1 — see
// `WavetableRecipe.h`). This file is a LIFT, not a rewrite: every expression below is the
// expression its category class evaluated, in the same order, with its constants promoted to
// recipe parameters. Expression order is load-bearing and not stylistic — `a + (b − a) · t` and
// `a·(1 − t) + b·t` are the same algebra and different floats, and the card's acceptance test is
// bit-identity over all eighteen banks.
//
// SHARED FAMILY DATA IS READ THROUGH THE OWNING CLASS'S PUBLIC LAW ACCESSOR, never copied here:
// `MetallicTableGenerator::bellPartials()` is `MetallicResonator`'s own mode set (with the
// static_assert that ties them together), `HarmonicTableGenerator::organDrawbars()` is the
// registration, `VocalTableGenerator::vowelFormantsHz()` is the Peterson-Barney row pair
// `FormantFilter` ships. A recipe SELECTS one of these by enum; it does not transcribe it.

namespace nemesis::dsp
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        constexpr int kFrameLength = WavetableBank::kFrameLength;

        // ─── shared shape + normalisation primitives ──────────────────────────────────────────

        // The four classic shapes as functions of normalised phase t in [0,1). Each peaks at
        // exactly 1.0 and is zero-mean in the continuum, which is what makes the `Basics` frame-0
        // keystone exact rather than approximate: saw at t = 0 is −1.0, so max|saw| == 1.0 and the
        // normalising divide is by 1.0.
        //
        // The saw's literal is deliberately the same expression `buildBootWavetable` uses
        // (EngineCore.cpp:97) — same operations in the same order, so the float result is
        // bit-identical rather than merely equal to within a rounding.
        [[nodiscard]] double shapeSaw (double t) noexcept { return 2.0 * t - 1.0; }
        [[nodiscard]] double shapeSquare (double t) noexcept { return t < 0.5 ? 1.0 : -1.0; }
        [[nodiscard]] double shapeTriangle (double t) noexcept { return t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t; }
        [[nodiscard]] double shapeSine (double t) noexcept { return std::sin (kTwoPi * t); }

        // SATURATES at the last shape rather than wrapping, and that is the ONE mechanism handling
        // the far end of the last segment: at morph 1.0 the segment index is exactly kShapes−1, so
        // the upper operand asks for index kShapes and must answer sine. An earlier draft ALSO
        // clamped the segment index in the caller; that clamp was decorative — with this
        // saturation, index 3 pairs with index 4 at a fade weight of exactly 0 and yields sine
        // either way, so no mutation could kill it.
        // THE SHAPE LISTS a crossfade walks. `SawSquareTriSine` is the shipped `Basics` and its
        // ORDER is load-bearing rather than cosmetic — `synth.osc{1,2}.pos` defaults to frame 0,
        // and a pure frame at the front would make the boot timbre a sine, on which a filter-cutoff
        // change is very nearly inaudible. [T-WT-14] The other three are the roster's.
        struct ShapeList
        {
            int count {};
            std::array<kernel::Shape, 5> shapes {};
        };

        [[nodiscard]] ShapeList shapeListOf (kernel::ShapeSet set) noexcept
        {
            using S = kernel::Shape;
            switch (set)
            {
                case kernel::ShapeSet::SawSquarePara:
                    return { 3, { S::Saw, S::Square, S::Para } };
                case kernel::ShapeSet::TriSineHalfsineTri:
                    // A ROUND TRIP — it ends where it started, so its spectral travel is 0 by
                    // design and ADR-041 decision 6's waveform-motion half is what judges it.
                    return { 4, { S::Triangle, S::Sine, S::HalfSine, S::Triangle } };
                case kernel::ShapeSet::SineParaTriSawSquare:
                    return { 5, { S::Sine, S::Para, S::Triangle, S::Saw, S::Square } };
                case kernel::ShapeSet::SawSquareTriSine:
                default:
                    return { 4, { S::Saw, S::Square, S::Triangle, S::Sine } };
            }
        }

        [[nodiscard]] int shapeCountOf (kernel::ShapeSet set) noexcept
        {
            return shapeListOf (set).count;
        }

        // ONE shape, as a function of normalised phase — the source parameter several time-domain
        // kernels take. `shapeAt` above walks a LIST for the crossfade; this names a single member,
        // and it covers the two extra shapes T-WT-13 needs.
        //
        // `Para` and `HalfSine` carry their mean subtraction as LITERALS: the mean of 1 − 2v² over
        // a cycle is exactly 1/3 and the mean of 2·sin(πt) is exactly 4/π. Stated at the source, so
        // the shape set is zero-mean member by member and `VectorBlend` can interpolate four of
        // them without needing a DC pass to clean up after the interpolation.
        [[nodiscard]] double shapeOf (kernel::Shape s, double t) noexcept
        {
            switch (s)
            {
                case kernel::Shape::Saw:
                    return shapeSaw (t);
                case kernel::Shape::Square:
                    return shapeSquare (t);
                case kernel::Shape::Triangle:
                    return shapeTriangle (t);
                case kernel::Shape::Para:
                {
                    const double v = 2.0 * t - 1.0;
                    return (1.0 - 2.0 * v * v) - (1.0 / 3.0);
                }
                case kernel::Shape::HalfSine:
                    return 2.0 * std::sin (kPi * t) - 4.0 / kPi;
                case kernel::Shape::Sine:
                default:
                    return shapeSine (t);
            }
        }

        [[nodiscard]] double shapeAt (kernel::ShapeSet set, int which, double t) noexcept
        {
            const auto list = shapeListOf (set);
            const int c = which < 0 ? 0 : (which >= list.count ? list.count - 1 : which);
            return shapeOf (list.shapes[static_cast<std::size_t> (c)], t);
        }

        // The quantiser's source wave. A switch rather than a ternary even though `Source` has one
        // enumerator today: a `cond ? f(x) : f(x)` placeholder is a branch clone, which clang-tidy
        // rejects and is right to — it reads as a choice while making none. The switch says the same
        // thing honestly and grows an arm when T-WT-13's `Crush Saw` needs a saw source.
        [[nodiscard]] double quantizeSource (kernel::Source source, double t) noexcept
        {
            switch (source)
            {
                case kernel::Source::Saw:
                    return shapeSaw (t);
                case kernel::Source::Sine:
                default:
                    return shapeSine (t);
            }
        }

        // PEAK-NORMALISE TO UNITY, then cast to float. ONE spelling for all six time-domain banks,
        // and the fact that one is enough is a battery result rather than an assumption: BASIC
        // shipped `work[i] /= peak` and CHIP shipped `out[i] = work[i] · (1/peak)`, this seam
        // originally preserved both behind a selector on the reasoning that they are different
        // floats, and two mutations forcing everyone onto each spelling BOTH SURVIVED the eighteen
        // bit-identity digests. The two differ by at most an ulp of double; the cast to float has
        // twenty-nine bits of headroom over that, so the difference cannot reach the output. See
        // the note in `WavetableRecipe.h` where the selector used to be.
        //
        // The degenerate guard is a COST/SAFETY guard and is labelled as such rather than dressed
        // up as a tested law — no law here can produce an all-zero frame (the crossfade's two
        // operands are never antiphase everywhere, the pulse always has both plateaus, and the
        // quantised sine always reaches ±1), so a mutation deleting it kills nothing. It stays
        // because the alternative on a future law is a frame of NaNs rather than a silent one.
        void finishTimeDomain (double* work, float* out) noexcept
        {
            double peak = 0.0;
            for (int i = 0; i < kFrameLength; ++i)
                peak = std::max (peak, std::abs (work[i]));

            if (peak > 1.0e-12) // see the note above — the else branch is unreachable for these laws
                for (int i = 0; i < kFrameLength; ++i)
                    work[i] /= peak;

            for (int i = 0; i < kFrameLength; ++i)
                out[i] = static_cast<float> (work[static_cast<std::size_t> (i)]);
        }

        // ─── the time-domain kernels ──────────────────────────────────────────────────────────

        void renderShapeMorph (const kernel::ShapeMorph& p, double morph, double* work)
        {
            const int shapes = shapeCountOf (p.shapes);
            const double seg = morph * static_cast<double> (shapes - 1);
            const int k = static_cast<int> (seg);
            const double frac = seg - static_cast<double> (k);

            for (int i = 0; i < kFrameLength; ++i)
            {
                const double t = static_cast<double> (i) / static_cast<double> (kFrameLength);
                const double a = shapeAt (p.shapes, k, t);
                const double b = shapeAt (p.shapes, k + 1, t);
                work[i] = a + (b - a) * frac;
            }
        }

        // The duty a `Staircase` bank holds at `frame`. The register set, not a sweep.
        [[nodiscard]] double staircaseDuty (kernel::DutySet set, int frame, int frames) noexcept
        {
            constexpr std::array<double, 4> kChipRegisters { 0.125, 0.25, 0.5, 0.75 };
            const auto steps = static_cast<int> (kChipRegisters.size());
            switch (set)
            {
                case kernel::DutySet::ChipRegisters:
                default:
                {
                    const int step = frame / (frames / steps);
                    return kChipRegisters[static_cast<std::size_t> (std::clamp (step, 0, steps - 1))];
                }
            }
        }

        // DUTY IS AN INTEGER SAMPLE COUNT, and the two plateaus are derived FROM THAT COUNT rather
        // than from the real-valued duty: (1 − n1/N) and (−n1/N), whose weighted sum is
        // n1(1 − n1/N) + (N − n1)(−n1/N) = 0 exactly. Computing them from the real duty instead
        // leaves an offset of up to one sample's worth, which the mip band-limiter keeps (it zeroes
        // bins ABOVE the top harmonic and never touches bin 0) and every note then thumps.
        void addPulse (double* acc, int hiSamples, double level) noexcept
        {
            const double d = static_cast<double> (hiSamples) / static_cast<double> (kFrameLength);
            for (int i = 0; i < kFrameLength; ++i)
                acc[i] += level * ((i < hiSamples) ? (1.0 - d) : -d);
        }

        // The octave-up partner: a 50 % square at `ratio` cycles per frame, so its own mean is
        // zero whenever `ratio` divides N/2 — which it does at N = 2048.
        void addSquarePartner (double* acc, int ratio, double level) noexcept
        {
            for (int i = 0; i < kFrameLength; ++i)
            {
                const int phase = (i * ratio) % kFrameLength;
                acc[i] += level * ((phase * 2 < kFrameLength) ? 1.0 : -1.0);
            }
        }

        void renderPulse (const kernel::Pulse& p, const WavetableRecipe& recipe, int frame, double* work)
        {
            const double duty = WavetableKernels::dutyAt (recipe, frame);
            const int hi = static_cast<int> (std::lround (duty * static_cast<double> (kFrameLength)));

            for (int i = 0; i < kFrameLength; ++i)
                work[i] = 0.0;

            addPulse (work, std::clamp (hi, 1, kFrameLength - 1), 1.0);

            // BATTERY SURVIVOR, LABELLED — category (c), equivalent and PREDICTED as such before
            // the run. Opening this guard is a no-op: `Analog` is the only bank with no partner,
            // its `partnerRatio` is 0 so the partner's phase is `(i·0) % N` = 0 for every i, and
            // its partner LEVEL is 0 too — so the pass would add +0.0 to every sample, and no
            // sample here is −0.0 (both plateaus are strictly signed). The guard buys a reader's
            // clarity, not a byte, and is kept on that basis rather than dressed up as a law.
            if (p.partnerRatio > 0)
                addSquarePartner (work, p.partnerRatio, WavetableKernels::partnerLevelAt (recipe, frame));
        }

        // Mid-tread quantiser: the source is scaled by (L−1)/2, rounded, and scaled back, so the
        // realised set is the L-step ladder. `lround` is odd-symmetric and the sine is odd about
        // t = 0.5, so the quantised frame stays zero-mean without any correction. No sample lands
        // exactly on a decision boundary for any L in use (the boundaries fall at non-integer
        // sample positions), so the rounding has no knife-edge and determinism is not resting on a
        // tie-break rule.
        //
        // THE LEVEL SPAN IS FORCED BY THE FRAME COUNT, not chosen for taste. A ladder of L levels
        // laid across `frames` frames must lose at least one level per frame or two adjacent frames
        // come out BIT-IDENTICAL and the bank morphs across nothing: `Digital`'s first draft ran
        // 32 → 3, i.e. 30 steps over 32 frames, and duplicated L at frames 7 and 8 (both 25).
        // 61 steps over 32 frames gives ~1.97 per frame, so the rounded sequence is strictly
        // decreasing. (The Python reference implementation still carries the 32 → 3 draft; ruling
        // R1 is why this file does not.)
        void renderQuantize (const kernel::Quantize& p, double morph, double* work)
        {
            const auto levels = static_cast<int> (std::lround (p.levelsFirst + (p.levelsLast - p.levelsFirst) * morph));
            const double half = static_cast<double> (levels - 1) * 0.5;

            for (int i = 0; i < kFrameLength; ++i)
            {
                const double t = static_cast<double> (i) / static_cast<double> (kFrameLength);
                work[i] = static_cast<double> (std::lround (quantizeSource (p.source, t) * half)) / half;
            }
        }

        // Phase modulation, not frequency modulation: the modulator displaces the carrier's PHASE,
        // which is what every digital "FM" chip actually did and what keeps the frame exactly
        // periodic (an integer ratio in, an integer harmonic set out).
        //
        // [T-WT-14] DC-CORRECTED FOR A NON-INTEGER RATIO, AND THE SHIPPED `2-Op` IS UNMOVED BY
        // CONSTRUCTION — MEASURED, NOT ASSUMED. An integer ratio makes the frame exactly periodic
        // at the fundamental, and an EARLIER version of this note argued from that periodicity that
        // its discrete-sample mean must be analytically zero — which the digest pin below disproves:
        // subtracting even that tiny a mean moved `2-Op`'s bit-exact frozen hash. So the correction
        // is scoped to where the law's own periodicity guarantee does NOT hold: a NON-integer ratio
        // (only METALLIC's six inharmonic FM banks use one) has no such guarantee and measurably
        // carries real DC — up to 0.485 uncorrected. `ratio` is a recipe CONSTANT, never a per-frame
        // value, so this branch is static per bank, not a runtime wobble.
        void renderFm2Op (double index, double ratio, double* work)
        {
            const bool integerRatio = std::abs (ratio - std::round (ratio)) < 1.0e-9;

            double sum = 0.0;
            for (int i = 0; i < kFrameLength; ++i)
            {
                const double phase = static_cast<double> (i) / static_cast<double> (kFrameLength);
                const double mod = std::sin (kTwoPi * ratio * phase);
                const double v = std::sin (kTwoPi * phase + index * mod);
                work[static_cast<std::size_t> (i)] = v;
                sum += v;
            }
            if (integerRatio)
                return; // the shipped law — `2-Op` and CHIP's whole fm2op family — unmoved

            const double mean = sum / static_cast<double> (kFrameLength);
            for (int i = 0; i < kFrameLength; ++i)
                work[static_cast<std::size_t> (i)] -= mean;
        }

        // ─── the spectral kernels ─────────────────────────────────────────────────────────────

        [[nodiscard]] double harmonicHz (int k) noexcept
        {
            return static_cast<double> (k) * HarmonicFrame::kReferenceHz;
        }

        // The source divisor for harmonic k under a formant law. Spelled as two branches rather
        // than one `pow (k, tilt)` because that is how both shipped laws are written and `pow` is
        // not obliged to reproduce `k` or `sqrt(k)` to the last bit.
        [[nodiscard]] double sourceDivisor (kernel::SourceLaw law, int k, double tilt = 0.0) noexcept
        {
            if (law == kernel::SourceLaw::OverSqrtK)
                return std::sqrt (static_cast<double> (k));
            // [T-WT-14] A FREE exponent for the roster banks that name one. A third arm rather
            // than a generalisation: routing `OverK` through `pow (k, -1.0)` could move a frozen
            // bank by an ulp, and the two spellings above are the shipped ones.
            if (law == kernel::SourceLaw::Free)
                return std::pow (static_cast<double> (k), tilt);
            return static_cast<double> (k);
        }

        // The vocal-tract response at `hz` for one vowel state: three resonances at the family's
        // per-formant gain taper, over an OFF-RESONANCE FLOOR.
        //
        // THE FLOOR IS LOAD-BEARING, NOT A FUDGE. With F1 at 300 Hz over a 110 Hz fundamental, a
        // pure product model leaves harmonic 1 some twelve orders of magnitude down — a vowel with
        // no voice in it. Real vocal tracts pass off-resonance energy; this is that, and it is why
        // the fundamental is audible while the FORMANT is still the loudest partial.
        [[nodiscard]] double vowelResponse (double hz,
            const std::array<double, 3>& formants,
            const std::array<double, 3>& gains,
            double detune,
            double widthScale,
            double uniformWidth,
            double floorLevel) noexcept
        {
            // F3 is broader than F1/F2: it is the "naturalness" formant rather than an identity
            // one, and a narrow third peak reads as a whistle. [T-WT-14] `uniformWidth` overrides
            // the table when a recipe names one — zero means "use the table", which is a sound
            // sentinel because a non-positive width is outside the parameter's domain.
            constexpr double kWidth[3] { 0.22, 0.22, 0.26 };

            double env = floorLevel;
            for (int i = 0; i < 3; ++i)
            {
                const auto j = static_cast<std::size_t> (i);
                const double w = uniformWidth > 0.0 ? uniformWidth : kWidth[j] * widthScale;
                env += gains[j] * HarmonicFrame::resonance (hz, formants[j] * detune, w);
            }
            return env;
        }

        // A VOWEL: three formant centres and their gains. The roster's rows carry a per-vowel gain
        // taper; the SHIPPED tract carries `FormantFilter::kGain`'s fixed one at both ends, which
        // is why `UtoA` is not `U`→`A` and is handled apart.
        struct VowelRow
        {
            std::array<double, 3> centresHz {};
            std::array<double, 3> gains {};
        };

        [[nodiscard]] VowelRow vowelRow (int which) noexcept
        {
            switch (which)
            {
                case 1:
                    return { { 530.0, 1840.0, 2480.0 }, { 1.0, 0.55, 0.25 } }; // e
                case 2:
                    return { { 270.0, 2290.0, 3010.0 }, { 1.0, 0.60, 0.28 } }; // i
                case 3:
                    return { { 570.0, 840.0, 2410.0 }, { 1.0, 0.48, 0.18 } }; // o
                case 4:
                    return { { 300.0, 870.0, 2240.0 }, { 1.0, 0.42, 0.15 } }; // u
                case 5:
                    return { { 660.0, 1720.0, 2410.0 }, { 1.0, 0.55, 0.24 } }; // ae
                case 6:
                    return { { 490.0, 1350.0, 1690.0 }, { 1.0, 0.50, 0.30 } }; // er
                case 7:
                    return { { 320.0, 900.0, 2200.0 }, { 1.0, 0.40, 0.14 } }; // oo
                case 0:
                default:
                    return { { 730.0, 1090.0, 2440.0 }, { 1.0, 0.50, 0.22 } }; // a
            }
        }

        // The pair's two endpoints, as vowel-table indices. `UtoA` never reaches here.
        [[nodiscard]] std::array<int, 2> vowelPairEnds (kernel::VowelPair pair) noexcept
        {
            switch (pair)
            {
                case kernel::VowelPair::AtoE:
                    return { 0, 1 };
                case kernel::VowelPair::OoToA:
                    return { 7, 0 };
                case kernel::VowelPair::ItoU:
                    return { 2, 4 };
                case kernel::VowelPair::AtoU:
                    return { 0, 4 };
                case kernel::VowelPair::ErToI:
                    return { 6, 2 };
                case kernel::VowelPair::AtoO:
                    return { 0, 3 };
                case kernel::VowelPair::UtoEr:
                    return { 4, 6 };
                case kernel::VowelPair::ItoE:
                    return { 2, 1 };
                case kernel::VowelPair::AeToOo:
                    return { 5, 7 };
                case kernel::VowelPair::UtoA:
                default:
                    return { 4, 0 };
            }
        }

        // The vowel STATE at `morph`: centres and gains, both interpolated.
        //
        // `UtoA` KEEPS ITS OWN PATH, through `VocalTableGenerator::vowelFormantsHz` — the shipped
        // accessor, which is where the shared Peterson-Barney rows live and where the
        // don't-copy-family-data note is written. Its gains are constant, so interpolating them
        // is `g + (g − g)·t`, exactly `g`, and `Vox` and `Choir` are unmoved.
        [[nodiscard]] VowelRow vowelsAt (kernel::VowelPair pair, double morph) noexcept
        {
            if (pair == kernel::VowelPair::UtoA)
                return { VocalTableGenerator::vowelFormantsHz (morph), { 1.0, 0.63, 0.4 } };

            const auto ends = vowelPairEnds (pair);
            const auto a = vowelRow (ends[0]);
            const auto b = vowelRow (ends[1]);
            const double t = std::clamp (morph, 0.0, 1.0);

            VowelRow out {};
            for (std::size_t i = 0; i < 3; ++i)
            {
                out.centresHz[i] = a.centresHz[i] + t * (b.centresHz[i] - a.centresHz[i]);
                out.gains[i] = a.gains[i] + t * (b.gains[i] - a.gains[i]);
            }
            return out;
        }

        [[nodiscard]] std::vector<double> ampsFormants (const kernel::Formants& p, double morph)
        {
            const auto vowel = vowelsAt (p.vowels, morph);
            const auto& formants = vowel.centresHz;
            const int maxHarmonic = VocalTableGenerator::kMaxHarmonic;

            std::vector<double> amps (static_cast<std::size_t> (maxHarmonic));

            if (p.voices <= 1)
            {
                for (int k = 1; k <= maxHarmonic; ++k)
                    amps[static_cast<std::size_t> (k - 1)] =
                        vowelResponse (harmonicHz (k), formants, vowel.gains, 1.0, p.widthScale, p.uniformWidth, p.floorLevel)
                        / sourceDivisor (p.source, k, p.tilt);
                return amps;
            }

            // THE ENSEMBLE. Three formant sets ±`voiceDetune` apart, each formant `widthScale`
            // times as WIDE, over a higher off-resonance floor.
            //
            // THE WIDTH IS THE BANK, AND MEASUREMENT IS WHY. The first version of `Choir` was
            // detune + spread phases alone, and it was very nearly the same bank as `Vox`: the
            // measured centroid came out 10.68 against Vox's 10.71 and the significant-partial
            // counts differed by one, because a ±6 % shift of a 0.22-octave resonance barely moves
            // the harmonic envelope. Two banks in one catalogue that differ only in PHASE is a
            // content defect and not a test problem — a sustained single-cycle tone is heard
            // through its magnitude spectrum, so the ear would have found them the same wave under
            // two names. What actually distinguishes an ensemble is that many tracts SMEAR the
            // formants: broader peaks, filled valleys, more air between them.
            const std::array<double, 3> detune { p.detuneLow, 1.0, p.detuneHigh };
            const std::array<double, 3> weight { p.sideWeight, 1.0, p.sideWeight };

            for (int k = 1; k <= maxHarmonic; ++k)
            {
                const double hz = harmonicHz (k);
                double env = 0.0;
                for (int v = 0; v < 3; ++v)
                    env += weight[static_cast<std::size_t> (v)]
                           * vowelResponse (hz,
                               formants,
                               vowel.gains,
                               detune[static_cast<std::size_t> (v)],
                               p.widthScale,
                               p.uniformWidth,
                               p.floorLevel);
                amps[static_cast<std::size_t> (k - 1)] = env / sourceDivisor (p.source, k, p.tilt);
            }
            return amps;
        }

        // Schroeder: φ_k = −π·k²/K over K harmonics. The quadratic ramp spreads each harmonic's
        // peak to a different point in the cycle, which is what lowers the crest factor for a
        // FIXED magnitude spectrum — so `Choir` and `Vox` can share a spectrum and still not sound
        // alike. (An earlier golden-ratio spread was replaced after measuring: it left the crest
        // within 0.5 % of Vox's, i.e. no decorrelation at all.)
        [[nodiscard]] std::vector<double> phasesFor (kernel::PhaseLaw law, int harmonics)
        {
            if (law != kernel::PhaseLaw::Schroeder)
                return {};

            std::vector<double> phases (static_cast<std::size_t> (harmonics));
            for (int k = 1; k <= harmonics; ++k)
                phases[static_cast<std::size_t> (k - 1)] =
                    -kPi * static_cast<double> (k) * static_cast<double> (k) / static_cast<double> (harmonics);
            return phases;
        }

        // Interpolates a resonance centre GEOMETRICALLY (constant ratio per frame). A sweep
        // spanning nearly three octaves is heard in octaves: a linear ramp from 400 to 3000 Hz
        // crawls at the start and races at the end.
        [[nodiscard]] double glide (double fromValue, double toValue, double morph) noexcept
        {
            return fromValue * std::pow (toValue / fromValue, morph);
        }

        [[nodiscard]] std::vector<double> ampsFormantSweep (const kernel::FormantSweep& p, double morph)
        {
            const double centre = glide (p.centreFirstHz, p.centreLastHz, morph);
            const int maxHarmonic = VocalTableGenerator::kMaxHarmonic;

            std::vector<double> amps (static_cast<std::size_t> (maxHarmonic));
            for (int k = 1; k <= maxHarmonic; ++k)
            {
                const double hz = harmonicHz (k);
                const double env = p.floorLevel + p.peakGain * HarmonicFrame::resonance (hz, centre, p.width);
                amps[static_cast<std::size_t> (k - 1)] = env / sourceDivisor (p.source, k, p.tilt);
            }
            return amps;
        }

        [[nodiscard]] std::vector<double> ampsComb (const kernel::Comb& p, double spacing, double morph)
        {
            const int maxHarmonic = MetallicTableGenerator::kMaxHarmonic;
            const double tilt = p.sourceTilt - p.tiltMove * morph;

            std::vector<double> amps (static_cast<std::size_t> (maxHarmonic));
            for (int k = 1; k <= maxHarmonic; ++k)
            {
                const double kd = static_cast<double> (k);
                const double comb = std::abs (std::cos (kPi * kd / spacing));
                amps[static_cast<std::size_t> (k - 1)] = std::pow (kd, -tilt) * std::pow (comb, p.combExponent);
            }
            return amps;
        }

        // SELECTED, not copied: these are `MetallicResonator`'s own inharmonic mode ratios, read
        // through the accessor that carries the static_assert tying the two counts together, so a
        // mode added to the resonator is still a partial owed here. Same switch-not-ternary reason
        // as `quantizeSource` above.
        // A MODE SET: real partial ratios and their gains. The widest is `Cymbal` at twelve.
        struct ModeSetData
        {
            int count {};
            std::array<double, 12> ratios {};
            std::array<double, 12> gains {};
        };

        // The default gain taper the reference implementation uses when a set names none: 1/(i+1),
        // i.e. the partials thin from the fundamental up. `Metallic` overrides it with the
        // resonator's own measured gains, which is the point of it being SELECTED and not copied.
        [[nodiscard]] ModeSetData modeSetFrom (std::initializer_list<double> ratios) noexcept
        {
            ModeSetData d {};
            for (const double r : ratios)
            {
                d.ratios[static_cast<std::size_t> (d.count)] = r;
                d.gains[static_cast<std::size_t> (d.count)] = 1.0 / static_cast<double> (d.count + 1);
                ++d.count;
            }
            return d;
        }

        [[nodiscard]] ModeSetData modeSetOf (kernel::ModeSet modes) noexcept
        {
            switch (modes)
            {
                case kernel::ModeSet::Bell:
                    return modeSetFrom ({ 1, 2, 2.4, 3, 4.5, 5.33, 6.8, 8.2 });
                case kernel::ModeSet::Tube:
                    return modeSetFrom ({ 1, 2.76, 5.40, 8.93, 13.34, 18.64 });
                case kernel::ModeSet::Bar:
                    return modeSetFrom ({ 1, 2.72, 5.15, 8.43, 12.56, 17.55, 23.4 });
                case kernel::ModeSet::Glass:
                    return modeSetFrom ({ 1, 2.32, 4.25, 6.63, 9.38, 13.2, 17.1 });
                case kernel::ModeSet::Gong:
                    return modeSetFrom ({ 1, 1.52, 2.13, 2.96, 3.83, 4.97, 6.35, 7.9, 9.6 });
                case kernel::ModeSet::Plate:
                    return modeSetFrom ({ 1, 1.59, 2.14, 2.30, 2.65, 2.92, 3.5, 4.1, 4.9, 5.7 });
                case kernel::ModeSet::Cymbal:
                    return modeSetFrom ({ 1, 1.37, 1.83, 2.41, 3.02, 3.77, 4.63, 5.62, 6.75, 8.1, 9.7, 11.5 });
                case kernel::ModeSet::Marimba:
                    return modeSetFrom ({ 1, 3.9, 9.2, 16.1, 24.3 });
                case kernel::ModeSet::Metallic:
                default:
                {
                    // SELECTED, not copied: `MetallicResonator`'s own mode set, read through the
                    // accessor that carries the static_assert tying the two counts together, so a
                    // mode added to the resonator is still a partial owed here. Its ratios are the
                    // ROUNDED integers and stay that way — `Bell` is frozen on them.
                    ModeSetData d {};
                    d.count = MetallicTableGenerator::kBellPartialCount;
                    const auto partials = MetallicTableGenerator::bellPartials();
                    for (int i = 0; i < d.count; ++i)
                    {
                        const auto j = static_cast<std::size_t> (i);
                        d.ratios[j] = static_cast<double> (partials[j]);
                        d.gains[j] = MetallicTableGenerator::bellPartialGain (i);
                    }
                    return d;
                }
            }
        }

        [[nodiscard]] std::vector<double> ampsModal (const kernel::Modal& p, double tilt)
        {
            // Depth is the full 256 rather than METALLIC's 64: a real ratio at a high base reaches
            // past harmonic 64 (`Cymbal` at base 16 tops out near 184). `Bell` is unaffected — its
            // partials stop at 14 and `renderAdditive` skips a zero amplitude, so the longer vector
            // is bit-identical for it.
            const auto set = modeSetOf (p.modes);

            std::vector<double> amps (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics));
            for (int i = 0; i < set.count; ++i)
            {
                const auto j = static_cast<std::size_t> (i);
                HarmonicFrame::addBin (amps,
                    set.ratios[j] * p.base,
                    set.gains[j] * std::pow (tilt, static_cast<double> (i)));
            }
            return amps;
        }

        [[nodiscard]] std::vector<double> ampsHarmonicCount (const kernel::HarmonicCount& p, int n)
        {
            const int maxHarmonic = MetallicTableGenerator::kMaxHarmonic;

            std::vector<double> amps (static_cast<std::size_t> (maxHarmonic));
            for (int k = 1; k <= n; ++k)
                amps[static_cast<std::size_t> (k - 1)] = std::pow (static_cast<double> (k), -p.tilt);
            return amps;
        }

        [[nodiscard]] std::vector<double> ampsTiltSweep (const kernel::OctaveStack& p, double tilt)
        {
            const int maxHarmonic = HarmonicTableGenerator::kMaxHarmonic;
            const auto partials = HarmonicTableGenerator::harmPartials();

            std::vector<double> amps (static_cast<std::size_t> (maxHarmonic));
            for (int j = 0; j < HarmonicTableGenerator::kHarmPartialCount; ++j)
            {
                const int k = partials[static_cast<std::size_t> (j)];
                amps[static_cast<std::size_t> (k - 1)] = std::pow (tilt, static_cast<double> (j));
            }
            return amps;
        }

        // The reference implementation's nine drawbar FOOTAGES and its eight registrations. Only
        // the pair arm uses them; `Classic` reads `HarmonicTableGenerator`'s own eight-footage set
        // through its accessors, which is what keeps the shipped `Organ` on the shipped numbers.
        constexpr std::array<int, 9> kProtoFootages { 1, 2, 3, 4, 6, 8, 10, 12, 16 };

        [[nodiscard]] std::array<double, 9> registrationOf (kernel::Registration r) noexcept
        {
            switch (r)
            {
                case kernel::Registration::Full:
                    return { 1.0, 0.9, 0.85, 0.7, 0.6, 0.5, 0.4, 0.3, 0.25 };
                case kernel::Registration::Reed:
                    return { 0.4, 0.3, 1.0, 0.8, 0.6, 0.4, 0.3, 0.2, 0.1 };
                case kernel::Registration::Jazz:
                    return { 1.0, 0.7, 0.5, 0.2, 0.1, 0.0, 0.0, 0.0, 0.0 };
                case kernel::Registration::Gospel:
                    return { 1.0, 1.0, 1.0, 0.6, 0.4, 0.6, 0.8, 1.0, 1.0 };
                case kernel::Registration::Tibia:
                    return { 1.0, 0.0, 0.0, 0.5, 0.0, 0.0, 0.0, 0.0, 0.3 };
                case kernel::Registration::Principal:
                    return { 1.0, 0.5, 0.35, 0.6, 0.2, 0.3, 0.15, 0.1, 0.2 };
                case kernel::Registration::Celeste:
                    return { 0.8, 0.6, 0.2, 0.9, 0.3, 0.1, 0.0, 0.0, 0.0 };
                case kernel::Registration::Flute:
                case kernel::Registration::Classic:
                default:
                    return { 1.0, 0.0, 0.8, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
            }
        }

        // A registration crossfade between two SIMILAR sets is not a morph — the partials barely
        // move and the bank fails the travel floor. `tiltMove` brightens or darkens the whole
        // stack across the bank so the sweep has a direction even between close neighbours.
        [[nodiscard]] std::vector<double> ampsDrawbarPair (const kernel::Drawbars& p, double morph)
        {
            const auto a = registrationOf (p.registration);
            const auto b = registrationOf (p.registrationTo);

            std::vector<double> amps (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics));
            for (std::size_t i = 0; i < kProtoFootages.size(); ++i)
            {
                const int k = kProtoFootages[i];
                if (k < 1 || k > HarmonicFrame::kMaxHarmonics)
                    continue;
                const double level = a[i] + (b[i] - a[i]) * morph;
                amps[static_cast<std::size_t> (k - 1)] +=
                    level * std::pow (static_cast<double> (k), -p.tiltMove * morph);
            }
            return amps;
        }

        [[nodiscard]] std::vector<double> ampsDrawbars (const kernel::Drawbars& p, double fade)
        {
            const int maxHarmonic = HarmonicTableGenerator::kMaxHarmonic;
            const auto set = HarmonicTableGenerator::organDrawbars();

            std::vector<double> amps (static_cast<std::size_t> (maxHarmonic));
            for (int i = 0; i < HarmonicTableGenerator::kOrganDrawbarCount; ++i)
            {
                const auto j = static_cast<std::size_t> (i);
                const double gain = HarmonicTableGenerator::organDrawbarGain (i)
                                    * (HarmonicTableGenerator::organIsQuint (set[j]) ? fade : 1.0);
                amps[static_cast<std::size_t> (set[j] - 1)] = gain;
            }
            return amps;
        }

        [[nodiscard]] std::vector<double> ampsChord (const kernel::JustTriad& p, double blend)
        {
            const int maxHarmonic = HarmonicTableGenerator::kMaxHarmonic;
            const auto partials = HarmonicTableGenerator::chordPartials();

            // EQUAL-POWER crossfade between the two thirds: cos/sin of a quarter turn, so
            // a24² + a25² is constant and the third keeps its weight all the way across. A linear
            // blend would lose 3 dB at the midpoint, i.e. the chord's colour would go hollow
            // exactly where the morph is most interesting.
            const double minor = std::cos (0.5 * kPi * blend);
            const double major = std::sin (0.5 * kPi * blend);

            std::vector<double> amps (static_cast<std::size_t> (maxHarmonic));
            amps[static_cast<std::size_t> (partials[0] - 1)] = p.rootGain;
            amps[static_cast<std::size_t> (partials[1] - 1)] = p.thirdGain * minor;
            amps[static_cast<std::size_t> (partials[2] - 1)] = p.thirdGain * major;
            amps[static_cast<std::size_t> (partials[3] - 1)] = p.fifthGain;
            amps[static_cast<std::size_t> (partials[4] - 1)] = p.octaveGain;
            return amps;
        }

        // ─── T-WT-13 leaf A: the SPECTRAL kernels with no phase law ───────────────────────────
        //
        // Depth is `HarmonicFrame::kMaxHarmonics` (256), not the shipped categories' 64 — see the
        // note over the parameter views in `WavetableRecipe.h` for why the two must stay apart.

        [[nodiscard]] std::vector<double> ampsOddEven (const kernel::OddEven& p, double morph)
        {
            // The BALANCE alone barely moves the spectrum — a narrow odd/even shift redistributes
            // energy without changing where it sits — so the tilt is the second axis rather than a
            // decoration. Without it the bank fails the travel floor while sounding fine.
            const double bal = p.balanceFirst + (p.balanceLast - p.balanceFirst) * morph;
            const double tilt = p.tilt - p.tiltMove * morph;

            std::vector<double> amps (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics));
            for (int k = 1; k <= HarmonicFrame::kMaxHarmonics; ++k)
            {
                const double w = (k % 2 == 1) ? bal : (1.0 - bal);
                amps[static_cast<std::size_t> (k - 1)] = w * std::pow (static_cast<double> (k), -tilt);
            }
            return amps;
        }

        [[nodiscard]] std::vector<double> ampsNotchSweep (const kernel::NotchSweep& p, double morph)
        {
            // ONE moving notch over a tilted source. The centre interpolates LINEARLY in harmonic
            // index rather than geometrically: unlike a formant or a comb, this law's identity is
            // which partial the notch is standing on, and the roster's banks walk it across the
            // whole series rather than over octaves.
            const double centre = p.centreFirst + (p.centreLast - p.centreFirst) * morph;

            std::vector<double> amps (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics));
            for (int k = 1; k <= HarmonicFrame::kMaxHarmonics; ++k)
            {
                const double d = std::log2 (static_cast<double> (k) / centre);
                const double notch = 1.0 - p.depth * std::exp (-(d * d) / (2.0 * p.width * p.width));
                amps[static_cast<std::size_t> (k - 1)] =
                    std::pow (static_cast<double> (k), -p.tilt) * std::max (notch, 0.0);
            }
            return amps;
        }

        [[nodiscard]] std::vector<double> ampsStretch (const kernel::Stretch& p, double morph)
        {
            // Progressive partial-ratio STRETCH, i^s: s = 1 is a harmonic series and s > 1 pulls it
            // bell-wards. Deposited through `addBin` because a stretched partial lands BETWEEN
            // bins, and rounding it to one is the defect that freezes a bank across frames.
            const double s = p.stretchFirst + (p.stretchLast - p.stretchFirst) * morph;

            std::vector<double> amps (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics));
            for (int i = 1; i <= p.partials; ++i)
                HarmonicFrame::addBin (amps,
                    std::pow (static_cast<double> (i), s),
                    std::pow (static_cast<double> (i), -p.tilt));
            return amps;
        }

        [[nodiscard]] std::vector<double> ampsTiltShelf (const kernel::TiltShelf& p, double morph)
        {
            // Two slopes with a MOVING HINGE — a shelf, not a single exponent. The two are audibly
            // different gestures: a tilt sweep changes how fast the spectrum falls everywhere at
            // once, a shelf keeps both slopes and moves the corner between them.
            const double hinge = p.hingeFirst + (p.hingeLast - p.hingeFirst) * morph;

            std::vector<double> amps (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics));
            for (int k = 1; k <= HarmonicFrame::kMaxHarmonics; ++k)
            {
                const double kd = static_cast<double> (k);
                amps[static_cast<std::size_t> (k - 1)] =
                    kd <= hinge ? std::pow (kd, -p.slopeLow)
                                : std::pow (hinge, -p.slopeLow) * std::pow (kd / hinge, -p.slopeHigh);
            }
            return amps;
        }

        [[nodiscard]] std::vector<double> ampsRingmod (const kernel::Ringmod& p, double morph)
        {
            // Sum AND difference partials against a moving modulator index — the classic ring-mod
            // sideband pair, at half weight each so the pair carries the source partial's energy
            // rather than doubling it.
            //
            // A difference partial legitimately runs off the bottom of the series, and `addBin`
            // DROPS anything outside [1, kMaxHarmonics] rather than clamping or reflecting it.
            // That is the law: clamping would pile every negative sideband onto harmonic 1 and
            // invent a fundamental this kernel does not have.
            const double cm = p.modFirst + (p.modLast - p.modFirst) * morph;

            std::vector<double> amps (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics));
            for (int i = 1; i <= p.partials; ++i)
            {
                const double a = std::pow (static_cast<double> (i), -p.tilt);
                HarmonicFrame::addBin (amps, static_cast<double> (i) + cm, 0.5 * a);
                HarmonicFrame::addBin (amps, static_cast<double> (i) - cm, 0.5 * a);
            }
            return amps;
        }

        [[nodiscard]] std::vector<double> ampsSeriesTilt (const kernel::TiltSweep& p, double morph)
        {
            const double tilt = p.tiltFirst + (p.tiltLast - p.tiltFirst) * morph;

            std::vector<double> amps (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics));
            for (int k = 1; k <= HarmonicFrame::kMaxHarmonics; ++k)
                amps[static_cast<std::size_t> (k - 1)] = std::pow (static_cast<double> (k), -tilt);
            return amps;
        }

        // ─── T-WT-13 leaf B: the SPECTRAL kernels that carry a PHASE law ──────────────────────

        // THE 32-BIT LCG, and it is the ONLY source of pseudo-randomness ADR-041 decision 9
        // permits. Numerical Recipes constants, unsigned wrap-around, value = x / 2^32. Chosen
        // because it ports bit-identically: `std::uint32_t` overflow is DEFINED, so this produces
        // the same sequence here as in the reference implementation and on every run of the same
        // binary. Deliberately not `<random>` — the standard library's ENGINES are specified but
        // its distributions are not, so a bank built through one would depend on a library
        // version, and a bank id that means something cannot.
        [[nodiscard]] std::vector<double> lcgUniforms (std::uint32_t seed, int count)
        {
            std::vector<double> out (static_cast<std::size_t> (std::max (count, 0)));
            std::uint32_t x = seed;
            for (auto& v : out)
            {
                x = 1664525U * x + 1013904223U;
                v = static_cast<double> (x) / 4294967296.0;
            }
            return out;
        }

        [[nodiscard]] std::vector<double> ampsNoiseSpectrum (const kernel::NoiseSpectrum& p,
            double morph,
            std::vector<double>& phasesOut)
        {
            // Fixed-seed pseudo-random amplitudes AND phases under a MOVING TILT — the tilt being
            // the magnitude gesture decision 6 requires of anything with a phase law in it.
            const auto r = lcgUniforms (p.seed, HarmonicFrame::kMaxHarmonics * 2);
            const double tilt = p.tiltFirst + (p.tiltLast - p.tiltFirst) * morph;
            const double dens = p.densityFirst + (p.densityLast - p.densityFirst) * morph;

            std::vector<double> amps (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics));
            phasesOut.assign (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics), 0.0);
            for (int k = 1; k <= HarmonicFrame::kMaxHarmonics; ++k)
            {
                const auto j = static_cast<std::size_t> (k - 1);
                const double gate = r[j] < dens ? 1.0 : 0.0;
                amps[j] = gate * (0.25 + 0.75 * r[j]) * std::pow (static_cast<double> (k), -tilt);
                phasesOut[j] = kTwoPi * r[static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics) + j];
            }
            return amps;
        }

        [[nodiscard]] std::vector<double> ampsScramble (const kernel::Scramble& p,
            double morph,
            std::vector<double>& phasesOut)
        {
            // A deterministic phase scramble DEEPENING across the bank. The scramble alone is the
            // decision-6 case exactly — magnitudes untouched, centroid frozen — so the tilt moves
            // alongside it and that movement is what gives the bank a direction to be ordered by.
            const auto r = lcgUniforms (p.seed, HarmonicFrame::kMaxHarmonics);
            const double depth = p.depthFirst + (p.depthLast - p.depthFirst) * morph;
            const double tilt = p.tilt - p.tiltMove * morph;

            std::vector<double> amps (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics));
            phasesOut.assign (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics), 0.0);
            for (int k = 1; k <= HarmonicFrame::kMaxHarmonics; ++k)
            {
                const auto j = static_cast<std::size_t> (k - 1);
                amps[j] = std::pow (static_cast<double> (k), -tilt);
                phasesOut[j] = kTwoPi * r[j] * depth;
            }
            return amps;
        }

        [[nodiscard]] std::vector<double> ampsPhaseSpread (const kernel::PhaseSpread& p,
            double morph,
            std::vector<double>& phasesOut)
        {
            // The single-cycle stand-in for unison. A single cycle CANNOT DETUNE — detuning is a
            // pitch relation between two oscillators, not a property of one shape — so what a
            // supersaw actually leaves inside one cycle is a progressive phase smear plus an
            // amplitude ripple. Named for what it is rather than for what it imitates.
            const double spread = p.spreadFirst + (p.spreadLast - p.spreadFirst) * morph;
            const double ripple = p.ripple * (0.25 + 0.75 * morph);
            const double slide = 1.0 + p.slide * morph;
            const auto maxH = static_cast<double> (HarmonicFrame::kMaxHarmonics);

            std::vector<double> amps (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics));
            phasesOut.assign (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics), 0.0);
            for (int k = 1; k <= HarmonicFrame::kMaxHarmonics; ++k)
            {
                const auto j = static_cast<std::size_t> (k - 1);
                const double kd = static_cast<double> (k);
                // The ripple COMB slides across the spectrum as the bank advances, and that is the
                // magnitude gesture: without it the spectrum is nearly static and the bank fails
                // the travel floor — a phase law needs one to read as a sweep rather than a
                // shimmer.
                const double rip = 1.0
                                   + ripple
                                         * std::cos (kPi * kd * static_cast<double> (p.voices) * slide / maxH);
                amps[j] = std::max (rip, 0.0) * std::pow (kd, -p.tilt);
                phasesOut[j] = spread * kTwoPi * (kd * kd) / (maxH * 4.0);
            }
            return amps;
        }

        // A chord VOICING: the two interval sets and the two weight sets, selected rather than
        // copied into a recipe (the shared-family-data rule at the top of `WavetableRecipe.h`).
        struct ChordVoicingData
        {
            int count {};
            std::array<double, 4> intervalsFirst {};
            std::array<double, 4> intervalsLast {};
            std::array<double, 4> weightsFirst {};
            std::array<double, 4> weightsLast {};
        };

        [[nodiscard]] ChordVoicingData chordVoicingOf (kernel::ChordVoicing v) noexcept
        {
            switch (v)
            {
                case kernel::ChordVoicing::MajorToOctave:
                    return { 3, { 1.0, 1.25, 1.5 }, { 1.0, 1.5, 2.0 }, { 1.0, 0.0, 0.0 }, { 1.0, 0.85, 0.7 } };
                case kernel::ChordVoicing::MinorToWide:
                    return { 3, { 1.0, 1.2, 1.5 }, { 1.0, 1.5, 1.8 }, { 1.0, 0.0, 0.0 }, { 1.0, 0.85, 0.7 } };
                case kernel::ChordVoicing::Fifths:
                    return { 2, { 1.0, 1.5 }, { 1.0, 2.25 }, { 1.0, 0.0 }, { 1.0, 0.8 } };
                case kernel::ChordVoicing::Sus:
                    return { 3, { 1.0, 1.333, 1.5 }, { 1.0, 1.125, 1.5 }, { 1.0, 0.0, 0.0 }, { 1.0, 0.8, 0.75 } };
                case kernel::ChordVoicing::Seventh:
                    return { 4, { 1.0, 1.25, 1.5, 1.8 }, { 1.0, 1.2, 1.5, 1.777 }, { 1.0, 0.0, 0.0, 0.0 }, { 1.0, 0.8, 0.7, 0.6 } };
                case kernel::ChordVoicing::Octaves:
                    return { 2, { 1.0, 2.0 }, { 1.0, 4.0 }, { 1.0, 0.6 }, { 1.0, 0.6 } };
                case kernel::ChordVoicing::MinorToMajor:
                default:
                    return { 3, { 1.0, 1.5, 2.0 }, { 1.0, 1.2, 1.5 }, { 1.0, 0.0, 0.0 }, { 1.0, 0.9, 0.7 } };
            }
        }

        [[nodiscard]] std::vector<double> ampsChordSeries (const kernel::Chord& p, double morph)
        {
            const auto v = chordVoicingOf (p.voicing);
            const double tilt = p.tilt - p.tiltMove * morph;

            std::vector<double> amps (static_cast<std::size_t> (HarmonicFrame::kMaxHarmonics));
            for (int j = 0; j < v.count; ++j)
            {
                const auto n = static_cast<std::size_t> (j);
                const double ratio = v.intervalsFirst[n] + (v.intervalsLast[n] - v.intervalsFirst[n]) * morph;
                const double weight = v.weightsFirst[n] + (v.weightsLast[n] - v.weightsFirst[n]) * morph;
                for (int i = 1; i <= p.partials; ++i)
                    HarmonicFrame::addBin (amps,
                        ratio * static_cast<double> (i),
                        weight * std::pow (static_cast<double> (i), -tilt));
            }
            return amps;
        }

        // ─── T-WT-13 leaf C: the TIME-domain kernels that are symmetric by construction ───────

        // Subtract the frame mean in place. The laws below that are NOT analytically zero-mean
        // each end with this; `DoublePulse` — which subtracts its own realised duty — deliberately
        // does not, and the difference is stated per law rather than applied blanket — a correction
        // nobody chose is how a DC convention drifts between banks the distinctness tests then
        // compare against each other.
        void removeMean (double* work) noexcept
        {
            double sum = 0.0;
            for (int i = 0; i < kFrameLength; ++i)
                sum += work[i];
            const double mean = sum / static_cast<double> (kFrameLength);
            for (int i = 0; i < kFrameLength; ++i)
                work[i] -= mean;
        }

        void renderDoublePulse (const kernel::DoublePulse& p, double morph, double* work)
        {
            // Two pulses per cycle with a moving width — a comb in disguise. Zero mean is taken by
            // subtracting the REALISED duty rather than by deriving two plateaus the way `addPulse`
            // does, and the reason is that the second pulse can run off the end of the cycle: the
            // high-sample count is then not 2·d·N, and only the count itself is honest.
            const double d = std::clamp (p.widthFirst + (p.widthLast - p.widthFirst) * morph, 0.01, 0.45);

            int high = 0;
            for (int i = 0; i < kFrameLength; ++i)
            {
                const double ph = static_cast<double> (i) / static_cast<double> (kFrameLength);
                const bool on = (ph < d) || (ph >= p.gap && ph < p.gap + d);
                work[i] = on ? 1.0 : 0.0;
                high += on ? 1 : 0;
            }
            const double duty = static_cast<double> (high) / static_cast<double> (kFrameLength);
            for (int i = 0; i < kFrameLength; ++i)
                work[i] -= duty;
        }

        void renderSkew (const kernel::Skew& p, double morph, double* work)
        {
            // Slope ASYMMETRY: a triangle at 0.5, through every intermediate skew, to a saw at the
            // rails. The clamp keeps both slopes finite — at exactly 0 or 1 one segment has zero
            // width and the division is undefined, and a bank's endpoint is the worst place for a
            // NaN because it is the frame the browser draws.
            const double s = std::clamp (p.skewFirst + (p.skewLast - p.skewFirst) * morph, 0.02, 0.98);
            for (int i = 0; i < kFrameLength; ++i)
            {
                const double ph = static_cast<double> (i) / static_cast<double> (kFrameLength);
                const double v = ph < s ? (ph / s) : (1.0 - (ph - s) / (1.0 - s));
                work[i] = 2.0 * v - 1.0;
            }
            removeMean (work);
        }

        void renderSync (const kernel::Sync& p, double morph, double* work)
        {
            // Hard SYNC: a slave saw restarted by the master, ratio swept. At a non-integer ratio
            // the last slave cycle is cut short — that truncation IS the sound, and it is also why
            // this law needs a mean subtraction where a plain saw does not: a partial cycle has a
            // nonzero mean.
            const double ratio = p.ratioFirst + (p.ratioLast - p.ratioFirst) * morph;
            for (int i = 0; i < kFrameLength; ++i)
            {
                const double ph = static_cast<double> (i) / static_cast<double> (kFrameLength);
                work[i] = 2.0 * std::fmod (ph * ratio, 1.0) - 1.0;
            }
            removeMean (work);
        }

        // The triangle FOLD, and it is ODD about zero — which is the whole DC argument for `Fold`:
        // an odd map of a zero-mean source is zero-mean, for any gain and any number of stages, so
        // the stage count is free rather than something the DC pass has to keep up with.
        [[nodiscard]] double triangleFold (double x) noexcept
        {
            double u = std::fmod (x + 1.0, 4.0);
            if (u < 0.0)
                u += 4.0;
            return u < 2.0 ? (u - 1.0) : (3.0 - u);
        }

        // [T-WT-14] `removeMean` IS NEEDED HERE, AND THE OLD COMMENT OVERCLAIMED. "An odd map of a
        // zero-mean source is zero-mean" needs the source ANTISYMMETRIC (x(t') = −x(t) for some
        // pairing), not merely zero-mean — true for `Sine`/`Saw`/`Triangle` (every shipped and CHIP
        // fold bank), false for `Para`/`HalfSine`: both are single-signed HUMPS zero-meaned by a
        // constant subtraction, with no such pairing, so folding them measurably does not cancel
        // (up to 0.26 DC, found by the validator on COMPLEX's `Fold Para`/`Fold Half`). No bank
        // using `Sine`/`Saw`/`Triangle` is moved by this — their fold output is already zero-mean
        // to float rounding, which `removeMean` cannot see past.
        void renderFold (const kernel::Fold& p, double morph, double* work)
        {
            const double g = p.gainFirst + (p.gainLast - p.gainFirst) * morph;
            for (int i = 0; i < kFrameLength; ++i)
            {
                const double t = static_cast<double> (i) / static_cast<double> (kFrameLength);
                double x = shapeOf (p.source, t) * g;
                for (int stage = 0; stage < p.stages; ++stage)
                    x = triangleFold (x);
                work[i] = x;
            }
            removeMean (work);
        }

        void renderSampleHold (const kernel::SampleHold& p, double morph, double* work)
        {
            // Time QUANTISE — the cycle held in S steps, S falling. A STAIRCASE and not a
            // bit-crush: the amplitude axis is untouched and only time is coarsened, which is the
            // distinction from `Quantize` and the reason both exist.
            const double stepsReal = p.stepsFirst + (p.stepsLast - p.stepsFirst) * morph;
            const int steps = std::clamp (static_cast<int> (std::lround (stepsReal)), 2, kFrameLength);
            const int hold = kFrameLength / steps;

            for (int i = 0; i < kFrameLength; ++i)
            {
                const int idx = std::min ((i * steps / kFrameLength) * hold, kFrameLength - 1);
                work[i] = shapeOf (p.source, static_cast<double> (idx) / static_cast<double> (kFrameLength));
            }
            removeMean (work);
        }

        // ─── T-WT-13 leaf D: the TIME-domain kernels that ITERATE ─────────────────────────────
        //
        // ADR-041 DECISION 9 IS THE CONSTRAINT EVERY LAW HERE IS WRITTEN AROUND: two runs of the
        // same binary must produce a bit-identical bank. That rules out a real feedback loop (its
        // output depends on where you started it), a free-running accumulator, and any `static`
        // mutable state. Each law below is a FIXED number of steps from a STATED start, and each
        // one says which.

        void renderFmFeedback (const kernel::FmFeedback& p, double morph, double* work)
        {
            // Self-feedback FM solved by FIXED-POINT ITERATION, not by a loop: y = sin(ph + b·y),
            // run `iterations` times from y = 0. A real feedback path would carry state across
            // samples and across calls, and a table that depends on when you asked for it is not
            // a table.
            const double b = p.indexFirst + (p.indexLast - p.indexFirst) * morph;
            for (int i = 0; i < kFrameLength; ++i)
            {
                const double ph = kTwoPi * static_cast<double> (i) / static_cast<double> (kFrameLength);
                double y = 0.0;
                for (int n = 0; n < p.iterations; ++n)
                    y = std::sin (ph + b * y);
                work[i] = y;
            }
            removeMean (work);
        }

        // One step of a bounded map, CLAMPED into [0, 1] — which is what makes "never divergent"
        // a property of the code rather than a hope about the parameters a recipe carries.
        [[nodiscard]] double chaosStep (kernel::ChaosMap map, double x, double r) noexcept
        {
            double next = x;
            switch (map)
            {
                case kernel::ChaosMap::Tent:
                    next = r * (x < 0.5 ? x : (1.0 - x));
                    break;
                case kernel::ChaosMap::Sine:
                    next = r * std::sin (kPi * x);
                    break;
                case kernel::ChaosMap::Logistic:
                default:
                    next = r * x * (1.0 - x);
                    break;
            }
            return std::clamp (next, 0.0, 1.0);
        }

        // A fixed BURN-IN before the cycle is read, so the orbit has settled onto its attractor at
        // the same point on every run. A count, not a convergence test — a test would terminate at
        // a different iteration on a different build.
        constexpr int kChaosBurnIn = 64;

        void renderChaos (const kernel::Chaos& p, double morph, double* work)
        {
            const double r = p.rateFirst + (p.rateLast - p.rateFirst) * morph;
            double x = p.seedValue;
            for (int n = 0; n < kChaosBurnIn; ++n)
                x = chaosStep (p.map, x, r);
            for (int i = 0; i < kFrameLength; ++i)
            {
                x = chaosStep (p.map, x, r);
                work[i] = 2.0 * x - 1.0;
            }
            removeMean (work);
        }

        void bitwiseState (const kernel::Bitwise& p, int shift, double* out) noexcept
        {
            const int a = std::max (shift, 1);
            for (int i = 0; i < kFrameLength; ++i)
            {
                const int c = (i * p.multiplier) & 0xFFFF;
                int v = 0;
                switch (p.op)
                {
                    case kernel::BitwiseOp::And:
                        v = (c & (c >> a)) & p.mask;
                        break;
                    case kernel::BitwiseOp::Mod:
                        v = (c % std::max (shift, 2)) & p.mask;
                        break;
                    case kernel::BitwiseOp::Mul:
                        v = (c * a) & p.mask;
                        break;
                    case kernel::BitwiseOp::Xor:
                    default:
                        v = (c ^ (c >> a)) & p.mask;
                        break;
                }
                out[i] = 2.0 * static_cast<double> (v) / static_cast<double> (p.mask) - 1.0;
            }
        }

        void renderBitwise (const kernel::Bitwise& p, double morph, double* work)
        {
            // Integer ops on a phase counter — bit rot as a timbre. THE CROSSFADE IS THE POINT:
            // the shift amount is an integer, so quantising it over 32 frames would repeat each
            // state four times and the bank would read as eight frames with three duplicates each,
            // which T-WT-12's duplicate-run row fails and should. Crossfading the two BRACKETING
            // integer states restores a continuous morph without softening either endpoint.
            const double af = p.shiftFirst + (p.shiftLast - p.shiftFirst) * morph;
            const int lo = static_cast<int> (std::floor (af));
            const double t = af - std::floor (af);

            std::vector<double> a (static_cast<std::size_t> (kFrameLength));
            std::vector<double> b (static_cast<std::size_t> (kFrameLength));
            bitwiseState (p, lo, a.data());
            bitwiseState (p, lo + 1, b.data());
            for (int i = 0; i < kFrameLength; ++i)
            {
                const auto j = static_cast<std::size_t> (i);
                work[i] = a[j] + (b[j] - a[j]) * t;
            }
            removeMean (work);
        }

        void renderPhaseDistort (const kernel::PhaseDistort& p, double morph, double* work)
        {
            // CZ-style PHASE DISTORTION: a two-segment phase transfer with a moving breakpoint.
            // The read pointer accelerates to the knee and decelerates after it, so a sine
            // acquires a resonant peak with no filter anywhere in the signal path. The knee
            // position IS the bank.
            const double bp = std::clamp (p.breakFirst + (p.breakLast - p.breakFirst) * morph, 0.02, 0.98);
            for (int i = 0; i < kFrameLength; ++i)
            {
                const double ph = static_cast<double> (i) / static_cast<double> (kFrameLength);
                const double w = ph < bp ? (0.5 * ph / bp) : (0.5 + 0.5 * (ph - bp) / (1.0 - bp));
                const int j = static_cast<int> (w * static_cast<double> (kFrameLength)) % kFrameLength;
                work[i] = shapeOf (p.source, static_cast<double> (j) / static_cast<double> (kFrameLength));
            }
            removeMean (work);
        }

        // Walsh function j at normalised phase — a product of ±1 square factors, one per set bit
        // of j. They are orthogonal and ±1-VALUED, which is why a Walsh stack stays hard-edged
        // where the equivalent sine stack smooths out, and therefore why this sounds digital and
        // `harmonic_count` does not.
        [[nodiscard]] double walshAt (int j, double ph) noexcept
        {
            double v = 1.0;
            int b = 0;
            for (int n = j; n != 0; n >>= 1, ++b)
                if ((n & 1) != 0)
                    v *= (static_cast<int> (ph * static_cast<double> (1 << (b + 1))) % 2 == 0) ? 1.0 : -1.0;
            return v;
        }

        constexpr int kWalshMaxIndex = 64;

        void renderWalsh (const kernel::Walsh& p, double morph, double* work)
        {
            const double nReal = p.countFirst + (p.countLast - p.countFirst) * morph;
            const int n = std::clamp (static_cast<int> (std::lround (nReal)), 1, kWalshMaxIndex);

            for (int i = 0; i < kFrameLength; ++i)
                work[i] = 0.0;
            for (int j = 1; j <= n; ++j)
            {
                const double g = 1.0 / static_cast<double> (j);
                for (int i = 0; i < kFrameLength; ++i)
                    work[i] += g * walshAt (j, static_cast<double> (i) / static_cast<double> (kFrameLength));
            }
            removeMean (work);
        }

        [[nodiscard]] std::array<kernel::Shape, 4> cornerSetOf (kernel::CornerSet set) noexcept
        {
            using S = kernel::Shape;
            switch (set)
            {
                case kernel::CornerSet::ParaHalfsineSawTri:
                    return { S::Para, S::HalfSine, S::Saw, S::Triangle };
                case kernel::CornerSet::SquareSawSquarePara:
                    return { S::Square, S::Saw, S::Square, S::Para };
                case kernel::CornerSet::SineHalfsineTriPara:
                    return { S::Sine, S::HalfSine, S::Triangle, S::Para };
                case kernel::CornerSet::SawSquareTriSine:
                default:
                    return { S::Saw, S::Square, S::Triangle, S::Sine };
            }
        }

        void renderVectorBlend (const kernel::VectorBlend& p, double morph, double* work)
        {
            // Four-corner VECTOR interpolation along a path — the VS gesture rather than its
            // content. `Circle` is the ROUND-TRIP case ADR-041 decision 6's travel-OR-motion
            // disjunction exists for: it ends where it started, so its spectral travel is 0 BY
            // DESIGN and only its waveform motion says whether the bank moves.
            double x = morph;
            double y = morph;
            if (p.path == kernel::VectorPath::Circle)
            {
                x = 0.5 + 0.5 * std::cos (kTwoPi * morph);
                y = 0.5 + 0.5 * std::sin (kTwoPi * morph);
            }
            else if (p.path == kernel::VectorPath::Cross)
            {
                y = 1.0 - morph;
            }

            const auto corners = cornerSetOf (p.corners);
            for (int i = 0; i < kFrameLength; ++i)
            {
                const double t = static_cast<double> (i) / static_cast<double> (kFrameLength);
                const double top = shapeOf (corners[0], t)
                                   + (shapeOf (corners[1], t) - shapeOf (corners[0], t)) * x;
                const double bot = shapeOf (corners[3], t)
                                   + (shapeOf (corners[2], t) - shapeOf (corners[3], t)) * x;
                work[i] = top + (bot - top) * y;
            }
            removeMean (work);
        }

        [[nodiscard]] std::array<int, 4> maskBitsOf (kernel::MaskBits bits) noexcept
        {
            switch (bits)
            {
                case kernel::MaskBits::Odd:
                    return { 1, 3, 5, 7 };
                case kernel::MaskBits::Octaves:
                    return { 2, 4, 8, 16 };
                case kernel::MaskBits::WideThree:
                    return { 1, 3, 6, 11 };
                case kernel::MaskBits::Fibonacci:
                    return { 2, 3, 5, 8 };
                case kernel::MaskBits::OddNine:
                    return { 1, 3, 5, 9 };
                case kernel::MaskBits::Primes:
                    return { 2, 5, 7, 13 };
                case kernel::MaskBits::Sparse:
                    return { 1, 2, 5, 13 };
                case kernel::MaskBits::OneTwoThreeFive:
                default:
                    return { 1, 2, 3, 5 };
            }
        }

        void renderMaskSpace (const kernel::MaskSpace& p, int frame, double* work)
        {
            // The GENERIC 4-bit mask law — a sine carrier plus, per SET bit, one partial taken
            // from the bit list. This is the half of T-WT-12b's third split the build did not
            // have: the shipped Prism / Shatter / Weave are `MaskComplex` and share only the
            // mask-0 anchor with it.
            //
            // THE FUNDAMENTAL IS UNCONDITIONAL (ADR-041 decision 7), for the reason it is there:
            // Gray(0) = 0 is the empty mask, which without a base term is a SILENT frame — and
            // frame 0 is where `synth.osc{1,2}.pos` boots. Mask 0 is therefore the plain wave and
            // each bit ADDS, which also makes the sixteen stops a scale of increasing complexity
            // rather than of volume.
            const int mask = MaskTableGenerator::grayCode (frame);
            const auto bits = maskBitsOf (p.bits);

            for (int i = 0; i < kFrameLength; ++i)
            {
                const double ph = static_cast<double> (i) / static_cast<double> (kFrameLength);
                double v = std::sin (kTwoPi * ph);
                for (int s = 0; s < MaskTableGenerator::kComplexBits; ++s)
                {
                    if (((mask >> s) & 1) == 0)
                        continue;
                    const double k = static_cast<double> (bits[static_cast<std::size_t> (s)]);
                    switch (p.law)
                    {
                        case kernel::MaskSpaceLaw::Shatter:
                            v += (static_cast<int> (ph * 2.0 * k) % 2 == 0 ? 1.0 : -1.0) / k;
                            break;
                        case kernel::MaskSpaceLaw::Weave:
                            v += (4.0 * std::abs (std::fmod (ph * k, 1.0) - 0.5) - 1.0) / k;
                            break;
                        case kernel::MaskSpaceLaw::Prism:
                        default:
                            v += std::sin (kTwoPi * k * ph) / k;
                            break;
                    }
                }
                work[i] = v;
            }
            removeMean (work);
        }

        // ─── the mask-space kernel ────────────────────────────────────────────────────────────

        void writeSine (float* frame) noexcept
        {
            for (int i = 0; i < kFrameLength; ++i)
                frame[i] = static_cast<float> (
                    std::sin (2.0 * kPi * static_cast<double> (i) / static_cast<double> (kFrameLength)));
        }

        // PRISM: octave-band gating of a saw. Bit s enables band s = [2^(s+1), 2^(s+2)); the
        // FUNDAMENTAL IS UNCONDITIONAL (ADR-041 decision 7), so mask 0 is the shared pure-sine
        // anchor and mask 15 the whole 31-harmonic saw. Written through the shared additive kernel
        // rather than as a local sum, which is what makes it band-limited by construction and
        // peak-normalised on the same terms as VOCAL/METALLIC/HARMONIC.
        void renderPrism (int mask, float* frame)
        {
            std::vector<double> amps (static_cast<std::size_t> (MaskTableGenerator::kPrismTopHarmonic), 0.0);
            amps[0] = 1.0; // the fundamental, always
            for (int s = 0; s < MaskTableGenerator::kComplexBits; ++s)
            {
                if (((mask >> s) & 1) == 0)
                    continue;
                for (int k = MaskTableGenerator::prismBandFirst (s); k <= MaskTableGenerator::prismBandLast (s); ++k)
                    amps[static_cast<std::size_t> (k - 1)] = 1.0 / static_cast<double> (k); // the saw's 1/k
            }
            HarmonicFrame::renderAdditive (amps, {}, frame);
        }

        // SHATTER: bit s zero-order-holds segment s at that segment's own hold length. The held
        // value is the window MEAN, so each window's SUM is preserved exactly and the frame's zero
        // mean survives without a subtraction.
        void renderShatter (int mask, float* frame)
        {
            writeSine (frame);
            for (int s = 0; s < MaskTableGenerator::kShatterSegments; ++s)
            {
                if (((mask >> s) & 1) == 0)
                    continue;
                const int begin = MaskTableGenerator::shatterSegmentBegin (s);
                const int end = MaskTableGenerator::shatterSegmentBegin (s + 1);
                const int hold = MaskTableGenerator::shatterHold (s);
                for (int w = begin; w < end; w += hold)
                {
                    double sum = 0.0;
                    for (int i = w; i < w + hold; ++i)
                        sum += static_cast<double> (frame[i]);
                    const auto held = static_cast<float> (sum / static_cast<double> (hold));
                    for (int i = w; i < w + hold; ++i)
                        frame[i] = held;
                }
            }

            // PEAK-NORMALISE (T-WT-17, owner ruling 2026-08-15). The law as first written did not,
            // and the omission was invisible until T-WT-12's validator measured it: a box mean
            // taken across the carrier's crest is necessarily BELOW the crest, so any mask holding
            // the segment that contains i = 512 loses a little peak. The worst case over the
            // sixteen frames is 1.200e-4, and it is predictable rather than merely observed — the
            // window that averages the crest hardest is segment 0's last 8-sample hold, whose mean
            // is 1 − ½·⟨j²⟩·(2π/N)² = 1 − 1.2007e-4.
            //
            // −0.001 dB, i.e. nothing anyone can hear. It is fixed anyway because §2.3 and ADR-006
            // say every frame is peak-normalised and this was the one law that did not, so the
            // alternative was a permanent carve-out in the validator — and T-WT-14 is about to add
            // seven more mask banks that would have had to learn it. A special case is cheapest to
            // remove before it acquires dependants.
            //
            // MASK 0 IS UNMOVED, BY CONSTRUCTION rather than by care: with no bits set no window is
            // held, the frame is the pure sine, its peak is exactly 1.0, and dividing by 1.0 in
            // double returns every sample unchanged. So the mask-0 anchor the three COMPLEX laws
            // share stays bit-exact, which is a shipped fact about the boot patch and not a nicety.
            //
            // Divides rather than multiplying by a reciprocal, and accumulates in double, matching
            // `finishTimeDomain` — one spelling for the whole file.
            //
            // THE DEGENERATE GUARD IS A LABELLED EQUIVALENT — T-CORE-03 category (c), predicted as
            // such before the battery and confirmed by it. Opening it kills nothing, and the reason
            // is structural rather than a weak test: no Shatter frame can be all-zero. The carrier
            // is a full-scale sine, and a zero-order hold of a window's MEAN cannot flatten every
            // window to zero — the mask would have to hold all four segments AND each of a sine's
            // segment means would have to vanish. It stays for the same reason the sibling guard in
            // `finishTimeDomain` does: the alternative on a future law is a frame of NaNs rather
            // than a silent one.
            double peak = 0.0;
            for (int i = 0; i < kFrameLength; ++i)
                peak = std::max (peak, std::abs (static_cast<double> (frame[i])));

            if (peak > 1.0e-12)
                for (int i = 0; i < kFrameLength; ++i)
                    frame[i] = static_cast<float> (static_cast<double> (frame[i]) / peak);
        }

        // WEAVE: bit s swaps each adjacent pair of phase blocks at level s, which is an XOR of the
        // sample index by the block length — hence a bijection, hence the frame holds exactly the
        // carrier's value set. Rendered straight from the permuted index rather than by permuting
        // a buffer, which keeps every sample bit-identical to the carrier's own.
        void renderWeave (int mask, float* frame)
        {
            const int pattern = MaskTableGenerator::weavePattern (mask);
            for (int i = 0; i < kFrameLength; ++i)
                frame[i] = static_cast<float> (
                    std::sin (2.0 * kPi * static_cast<double> (i ^ pattern) / static_cast<double> (kFrameLength)));
        }

        // THE ONE PLACE A FRAME INDEX BECOMES A LAW'S FRAME, and since T-WT-14 that includes the
        // declared ORDER. Slot `frame` of a `Brightness` bank holds generation-frame
        // `frameOrder[frame]`; every other ordering is the identity. Putting the remap here rather
        // than in `render()` means every law accessor gets it too, so a public accessor can never
        // report a different frame than the renderer drew.
        [[nodiscard]] int clampFrame (const WavetableRecipe& recipe, int frame) noexcept
        {
            const int c = std::clamp (frame, 0, recipe.frames - 1);
            if (recipe.ordering != FrameOrdering::Brightness || recipe.frameOrder == nullptr)
                return c;
            return static_cast<int> (recipe.frameOrder[static_cast<std::size_t> (c)]);
        }
    } // namespace

    // ─── the law helpers ──────────────────────────────────────────────────────────────────────

    double WavetableKernels::morphAt (const WavetableRecipe& recipe, int frame) noexcept
    {
        return static_cast<double> (clampFrame (recipe, frame)) / static_cast<double> (recipe.frames - 1);
    }

    double WavetableKernels::combSpacingAt (const WavetableRecipe& recipe, int frame) noexcept
    {
        // Geometric, because a comb is heard in octaves of spacing: a linear sweep from 23.7 to
        // 4.3 would spend half the bank between 14 and 24 harmonics, where the comb barely changes
        // character, and race through the tight end where it changes fastest.
        const auto p = kernel::Comb::from (recipe.params);
        return glide (p.spacingFirst, p.spacingLast, morphAt (recipe, frame));
    }

    int WavetableKernels::harmonicCountAt (const WavetableRecipe& recipe, int frame) noexcept
    {
        const auto p = kernel::HarmonicCount::from (recipe.params);
        return static_cast<int> (p.countFirst) + static_cast<int> (p.countStep) * clampFrame (recipe, frame);
    }

    double WavetableKernels::tiltAt (const WavetableRecipe& recipe, int frame) noexcept
    {
        const double m = morphAt (recipe, frame);
        if (recipe.kernel == WavetableKernel::Modal)
        {
            // Mode i is scaled by tilt^i, so mode 0 never moves and the set thins from the top
            // down — a bell's DECAY. The floor is above zero because a bank that ends on a pure
            // sine has stopped being a bell.
            const auto p = kernel::Modal::from (recipe.params);
            return 1.0 - (1.0 - p.tiltFloor) * m;
        }
        const auto p = kernel::OctaveStack::from (recipe.params);
        return p.tiltFirst + (p.tiltLast - p.tiltFirst) * m;
    }

    double WavetableKernels::quintFadeAt (const WavetableRecipe& recipe, int frame) noexcept
    {
        const auto p = kernel::Drawbars::from (recipe.params);
        return p.quintFadeFirst + (p.quintFadeLast - p.quintFadeFirst) * morphAt (recipe, frame);
    }

    double WavetableKernels::chordBlendAt (const WavetableRecipe& recipe, int frame) noexcept
    {
        return morphAt (recipe, frame);
    }

    int WavetableKernels::dutySixtyFourthsAt (const WavetableRecipe& recipe, int frame) noexcept
    {
        const auto p = kernel::Pulse::from (recipe.params);
        return static_cast<int> (p.dutyTop) - clampFrame (recipe, frame);
    }

    double WavetableKernels::dutyAt (const WavetableRecipe& recipe, int frame) noexcept
    {
        const auto p = kernel::Pulse::from (recipe.params);
        const int f = clampFrame (recipe, frame);

        switch (p.law)
        {
            case kernel::DutyLaw::Staircase:
                return staircaseDuty (p.dutySet, f, recipe.frames);
            case kernel::DutyLaw::SixtyFourths:
                // Duties are exact 1/64ths: an integer numerator per frame, divided once.
                return static_cast<double> (dutySixtyFourthsAt (recipe, frame)) / 64.0;
            case kernel::DutyLaw::Sweep:
            default:
                return p.dutyFirst + (p.dutyLast - p.dutyFirst) * morphAt (recipe, frame);
        }
    }

    double WavetableKernels::partnerLevelAt (const WavetableRecipe& recipe, int frame) noexcept
    {
        const auto p = kernel::Pulse::from (recipe.params);
        return p.partnerLevelFirst + (p.partnerLevelLast - p.partnerLevelFirst) * morphAt (recipe, frame);
    }

    double WavetableKernels::fmIndexAt (const WavetableRecipe& recipe, int frame) noexcept
    {
        const auto p = kernel::Fm2Op::from (recipe.params);
        return p.indexFirst + (p.indexLast - p.indexFirst) * morphAt (recipe, frame);
    }

    // ─── dispatch ─────────────────────────────────────────────────────────────────────────────

    void WavetableKernels::renderMask (kernel::MaskLaw law, int mask, float* out)
    {
        // BATTERY SURVIVOR, LABELLED — category (c), unprovable by construction, and the label was
        // already written one layer down: `MaskTableGenerator.cpp`'s own `wrapMask` carries the
        // same note for the same reason. Every COMPLEX law consumes its mask BIT BY BIT over the
        // 4-bit range, so mask 16 has no bit inside that range either way and a widened wrap is
        // indistinguishable. Deleting BOTH wraps was measured and changes nothing. It stays because
        // this is a public entry point and the domain should be stated where it is entered — not
        // because a test can tell. (Predicted KILLED via the mask path when the battery was
        // written; that prediction was wrong, and it was wrong because the existing note in
        // `MaskTableGenerator.cpp` already said so.)
        const int m = mask & (MaskTableGenerator::kComplexFrames - 1);
        switch (law)
        {
            case kernel::MaskLaw::Shatter:
                renderShatter (m, out);
                return;
            case kernel::MaskLaw::Weave:
                renderWeave (m, out);
                return;
            case kernel::MaskLaw::Prism:
            default:
                renderPrism (m, out);
                return;
        }
    }

    void WavetableKernels::render (const WavetableRecipe& recipe, int frame, float* out)
    {
        // `clampFrame` resolves the declared ordering, so `resolved` is the GENERATION frame this
        // slot holds. Every accessor below takes the ORIGINAL `frame` and resolves it itself —
        // passing `resolved` on would apply the permutation twice.
        const int resolved = clampFrame (recipe, frame);
        const double morph = morphAt (recipe, frame);

        // The SPECTRAL kernels all end the same way — an amplitude law through
        // `HarmonicFrame::renderAdditive`, which is exactly zero-mean for any amplitude law and
        // peak-normalises on its own. The TIME-domain ones own their DC argument and their
        // normalisation, which is why they carry a `Normalise` mode and the spectral ones do not.
        switch (recipe.kernel)
        {
            case WavetableKernel::Formants:
            {
                const auto p = kernel::Formants::from (recipe.params);
                HarmonicFrame::renderAdditive (ampsFormants (p, morph),
                    phasesFor (p.phases, VocalTableGenerator::kMaxHarmonic),
                    out);
                return;
            }
            case WavetableKernel::FormantSweep:
            {
                const auto p = kernel::FormantSweep::from (recipe.params);
                HarmonicFrame::renderAdditive (ampsFormantSweep (p, morph), {}, out);
                return;
            }
            case WavetableKernel::Comb:
            {
                const auto p = kernel::Comb::from (recipe.params);
                HarmonicFrame::renderAdditive (ampsComb (p, combSpacingAt (recipe, frame), morph), {}, out);
                return;
            }
            case WavetableKernel::Modal:
            {
                const auto p = kernel::Modal::from (recipe.params);
                HarmonicFrame::renderAdditive (ampsModal (p, tiltAt (recipe, frame)), {}, out);
                return;
            }
            case WavetableKernel::HarmonicCount:
            {
                const auto p = kernel::HarmonicCount::from (recipe.params);
                HarmonicFrame::renderAdditive (ampsHarmonicCount (p, harmonicCountAt (recipe, frame)), {}, out);
                return;
            }
            case WavetableKernel::OctaveStack:
            {
                const auto p = kernel::OctaveStack::from (recipe.params);
                HarmonicFrame::renderAdditive (ampsTiltSweep (p, tiltAt (recipe, frame)), {}, out);
                return;
            }
            case WavetableKernel::Drawbars:
            {
                const auto p = kernel::Drawbars::from (recipe.params);
                HarmonicFrame::renderAdditive (p.registration == kernel::Registration::Classic
                                                   ? ampsDrawbars (p, quintFadeAt (recipe, frame))
                                                   : ampsDrawbarPair (p, morph),
                    {},
                    out);
                return;
            }
            case WavetableKernel::JustTriad:
            {
                const auto p = kernel::JustTriad::from (recipe.params);
                HarmonicFrame::renderAdditive (ampsChord (p, chordBlendAt (recipe, frame)), {}, out);
                return;
            }
            case WavetableKernel::OddEven:
                HarmonicFrame::renderAdditive (ampsOddEven (kernel::OddEven::from (recipe.params), morph), {}, out);
                return;
            case WavetableKernel::NotchSweep:
                HarmonicFrame::renderAdditive (ampsNotchSweep (kernel::NotchSweep::from (recipe.params), morph), {}, out);
                return;
            case WavetableKernel::Stretch:
                HarmonicFrame::renderAdditive (ampsStretch (kernel::Stretch::from (recipe.params), morph), {}, out);
                return;
            case WavetableKernel::TiltShelf:
                HarmonicFrame::renderAdditive (ampsTiltShelf (kernel::TiltShelf::from (recipe.params), morph), {}, out);
                return;
            case WavetableKernel::Ringmod:
                HarmonicFrame::renderAdditive (ampsRingmod (kernel::Ringmod::from (recipe.params), morph), {}, out);
                return;
            case WavetableKernel::TiltSweep:
                HarmonicFrame::renderAdditive (ampsSeriesTilt (kernel::TiltSweep::from (recipe.params), morph), {}, out);
                return;
            case WavetableKernel::NoiseSpectrum:
            {
                std::vector<double> phases;
                const auto amps = ampsNoiseSpectrum (kernel::NoiseSpectrum::from (recipe.params), morph, phases);
                HarmonicFrame::renderAdditive (amps, phases, out);
                return;
            }
            case WavetableKernel::Scramble:
            {
                std::vector<double> phases;
                const auto amps = ampsScramble (kernel::Scramble::from (recipe.params), morph, phases);
                HarmonicFrame::renderAdditive (amps, phases, out);
                return;
            }
            case WavetableKernel::PhaseSpread:
            {
                std::vector<double> phases;
                const auto amps = ampsPhaseSpread (kernel::PhaseSpread::from (recipe.params), morph, phases);
                HarmonicFrame::renderAdditive (amps, phases, out);
                return;
            }
            case WavetableKernel::Chord:
                HarmonicFrame::renderAdditive (ampsChordSeries (kernel::Chord::from (recipe.params), morph), {}, out);
                return;
            case WavetableKernel::MaskComplex:
            {
                // GRAY-NATIVE: frame f holds mask grayCode(f), so adjacent frames differ by exactly
                // one mask bit and the `mask` param's `Off` default is already the musical
                // traversal (§2.3 / T-PROTO-02 — the 2026-07-29 anti-Binary ruling, kept by
                // construction). Called with the frame INDEX, not the morph, because a mask space
                // is discrete by nature.
                const auto p = kernel::MaskComplex::from (recipe.params);
                renderMask (p.law, MaskTableGenerator::grayCode (resolved), out);
                return;
            }
            default:
                break;
        }

        // The time-domain kernels share one scratch buffer and one finish.
        std::vector<double> work (static_cast<std::size_t> (kFrameLength));

        switch (recipe.kernel)
        {
            case WavetableKernel::Pulse:
            {
                const auto p = kernel::Pulse::from (recipe.params);
                renderPulse (p, recipe, frame, work.data());
                finishTimeDomain (work.data(), out);
                return;
            }
            case WavetableKernel::Quantize:
            {
                const auto p = kernel::Quantize::from (recipe.params);
                renderQuantize (p, morph, work.data());
                finishTimeDomain (work.data(), out);
                return;
            }
            case WavetableKernel::Fm2Op:
            {
                const auto p = kernel::Fm2Op::from (recipe.params);
                renderFm2Op (fmIndexAt (recipe, frame), p.ratio, work.data());
                finishTimeDomain (work.data(), out);
                return;
            }
            case WavetableKernel::DoublePulse:
                renderDoublePulse (kernel::DoublePulse::from (recipe.params), morph, work.data());
                finishTimeDomain (work.data(), out);
                return;
            case WavetableKernel::Skew:
                renderSkew (kernel::Skew::from (recipe.params), morph, work.data());
                finishTimeDomain (work.data(), out);
                return;
            case WavetableKernel::Sync:
                renderSync (kernel::Sync::from (recipe.params), morph, work.data());
                finishTimeDomain (work.data(), out);
                return;
            case WavetableKernel::Fold:
                renderFold (kernel::Fold::from (recipe.params), morph, work.data());
                finishTimeDomain (work.data(), out);
                return;
            case WavetableKernel::SampleHold:
                renderSampleHold (kernel::SampleHold::from (recipe.params), morph, work.data());
                finishTimeDomain (work.data(), out);
                return;
            case WavetableKernel::FmFeedback:
                renderFmFeedback (kernel::FmFeedback::from (recipe.params), morph, work.data());
                finishTimeDomain (work.data(), out);
                return;
            case WavetableKernel::Chaos:
                renderChaos (kernel::Chaos::from (recipe.params), morph, work.data());
                finishTimeDomain (work.data(), out);
                return;
            case WavetableKernel::Bitwise:
                renderBitwise (kernel::Bitwise::from (recipe.params), morph, work.data());
                finishTimeDomain (work.data(), out);
                return;
            case WavetableKernel::PhaseDistort:
                renderPhaseDistort (kernel::PhaseDistort::from (recipe.params), morph, work.data());
                finishTimeDomain (work.data(), out);
                return;
            case WavetableKernel::Walsh:
                renderWalsh (kernel::Walsh::from (recipe.params), morph, work.data());
                finishTimeDomain (work.data(), out);
                return;
            case WavetableKernel::VectorBlend:
                renderVectorBlend (kernel::VectorBlend::from (recipe.params), morph, work.data());
                finishTimeDomain (work.data(), out);
                return;
            case WavetableKernel::MaskSpace:
                // Called with the frame INDEX, not the morph: a mask space is discrete by nature.
                renderMaskSpace (kernel::MaskSpace::from (recipe.params), resolved, work.data());
                finishTimeDomain (work.data(), out);
                return;
            case WavetableKernel::ShapeMorph:
            default:
            {
                const auto p = kernel::ShapeMorph::from (recipe.params);
                renderShapeMorph (p, morph, work.data());
                // The `Basics` frame-0 keystone rests on this being exact: the bare saw's peak is
                // exactly 1.0, so the normalising divide is by 1.0 and changes nothing.
                finishTimeDomain (work.data(), out);
                return;
            }
        }
    }
} // namespace nemesis::dsp
