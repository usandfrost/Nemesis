#include <nemesis/dsp/MaskTableGenerator.h>

#include <nemesis/dsp/HarmonicFrame.h>
#include <nemesis/dsp/WavetableKernels.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace nemesis::dsp
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        // In-place iterative radix-2 Cooley-Tukey FFT on split re/im arrays of length n (a
        // power of two). Build-time only (spectralCentroid), so twiddles use libm directly —
        // the same private-FFT pattern as Wavetable::build / Convolution.
        void fft (double* re, double* im, int n) noexcept
        {
            for (int i = 1, j = 0; i < n; ++i)
            {
                int bit = n >> 1;
                for (; (j & bit) != 0; bit >>= 1)
                    j ^= bit;
                j ^= bit;
                if (i < j)
                {
                    const double tr = re[i];
                    re[i] = re[j];
                    re[j] = tr;
                    const double ti = im[i];
                    im[i] = im[j];
                    im[j] = ti;
                }
            }

            for (int len = 2; len <= n; len <<= 1)
            {
                const int half = len >> 1;
                const double theta = -2.0 * kPi / static_cast<double> (len);
                for (int i = 0; i < n; i += len)
                {
                    for (int k = 0; k < half; ++k)
                    {
                        const double ang = theta * static_cast<double> (k);
                        const double wRe = std::cos (ang);
                        const double wIm = std::sin (ang);
                        const int a = i + k;
                        const int b = i + k + half;
                        const double vRe = re[b] * wRe - im[b] * wIm;
                        const double vIm = re[b] * wIm + im[b] * wRe;
                        re[b] = re[a] - vRe;
                        im[b] = im[a] - vIm;
                        re[a] += vRe;
                        im[a] += vIm;
                    }
                }
            }
        }

        // The mask wraps in the LAW'S OWN space (T-WT-03 leaf G2): SegmentOps is 9-bit, the three
        // COMPLEX laws are 4-bit because §2.3 ties a law's mask width to its frame count.
        //
        // BATTERY SURVIVOR, LABELLED — category (c), unprovable by construction (T-CORE-03's
        // three-kinds rule). Mutating `maskSpaceFor (law)` back to the shared `kMaskSpace` kills
        // nothing, and the reason is that every law below consumes its mask BIT BY BIT over its own
        // `bitsFor` range: mask 16 under a 4-bit law has no bit inside that range either way, so the
        // two wraps are indistinguishable — the observable contract ("renderMask (law, m) renders m
        // modulo the law's space") is delivered by those loops whether this line is narrow or wide.
        // What the wrap IS load-bearing for is the DOMAIN: a mask at or past 512 folds back rather
        // than reading bits nobody defined, and the SegmentOps case in the tests pins that end.
        //
        // KEPT NARROW RATHER THAN WIDENED, and the reason is not defence-in-depth theatre: the width
        // is the law's own (§2.3 ties it to the frame count), so a law added later inherits the right
        // domain from `bitsFor` instead of a 9 that happens to be SegmentOps'. It becomes observable
        // the moment a law reads the mask value ARITHMETICALLY rather than bit by bit — which is
        // exactly what T-PROTO-02's `mask` param does when it maps a param value into a law's space,
        // so the pin belongs there, with its consumer, rather than as an accessor with no user today
        // (the same call leaf G1 made when it deleted `permuteFrame`).
        int wrapMask (MaskTableGenerator::Law law, int mask) noexcept
        {
            return mask & (MaskTableGenerator::maskSpaceFor (law) - 1);
        }

        // One pure sine cycle — the carrier all four laws start from, and the mask-0 anchor is this
        // exact formula (the SegmentOps golden pins it, and Shatter/Weave inherit it bit-for-bit).
        void writeSine (float* frame) noexcept
        {
            for (int i = 0; i < MaskTableGenerator::kFrameLength; ++i)
                frame[i] = static_cast<float> (
                    std::sin (2.0 * kPi * static_cast<double> (i)
                              / static_cast<double> (MaskTableGenerator::kFrameLength)));
        }

        // SegmentOps, T-MASK-03 leaf B / ADR-037: the AUTHORED nine-entry op table over
        // {Flip, Zero, Mirror, Repeat, Scale}, walked through the SHARED `segmentTap` law so this
        // build-time view and `dsp::BitmaskOscillator`'s render-time view cannot drift apart. The
        // 2026-07-16 `[s % 3]` law it replaces reasoned about nothing and lacked the reference
        // instrument's `repeated` and `scaled`; nothing shipped it (no catalogue entry), so
        // re-authoring it moved no sound — the golden was re-minted (owner ruling 2026-08-04).
        void renderSegmentOps (int mask, float* frame)
        {
            // The carrier is read from a scratch copy because the ops are READS at displaced
            // positions (Mirror reflects, Repeat resamples): writing into `frame` while reading it
            // would let an already-transformed sample feed the next op. The old in-place law could
            // do without this only because every one of its ops was position-preserving or a whole
            // -segment reverse.
            std::array<float, static_cast<std::size_t> (MaskTableGenerator::kFrameLength)> carrier {};
            writeSine (carrier.data());

            for (int i = 0; i < MaskTableGenerator::kFrameLength; ++i)
            {
                const auto tap = MaskTableGenerator::segmentTap (mask, static_cast<double> (i));
                frame[i] = tap.gain * carrier[static_cast<std::size_t> (static_cast<int> (tap.index))];
            }

            // Zero mean (the class note's DC argument). Mask 0's correction is exactly 0.0f, so the
            // pure-sine anchor stays bit-exact.
            const float dc = MaskTableGenerator::dcOffsetFor (mask);
            for (int i = 0; i < MaskTableGenerator::kFrameLength; ++i)
                frame[i] -= dc;
        }

        // THE THREE COMPLEX LAWS — Prism, Shatter and Weave — MOVED TO THE KERNEL UNIT at T-WT-11
        // (`WavetableKernels.cpp`, the `mask_space` kernel), because they are the three that ARE
        // factory banks and so are the three that own recipes. Their laws are unchanged and their
        // notes travelled with them.
        //
        // `SegmentOps` deliberately did NOT move. It is ADR-037's render-time bitmask view — a
        // 9-bit space against the COMPLEX laws' 4-bit one, reached from `BitmaskOscillator` rather
        // than from the catalogue — so it is not a factory bank, has no recipe, and keeps its own
        // path below alongside `dcOffsetFor`, which the audio thread calls.

        // The COMPLEX laws' recipes. `frames == 2^bits` is arithmetic and not a preference
        // (ADR-041 decision 7): every mask must be reachable as a pure state, and a 4-bit Gray
        // sequence really is one-bit-adjacent.
        constexpr std::array<WavetableRecipe, 3> kComplexRecipes {
            WavetableRecipe { "factory.complex.prism",
                "Prism",
                WavetableCategory::Complex,
                MaskTableGenerator::kComplexFrames,
                WavetableKernel::MaskComplex,
                kernel::MaskComplex::make (kernel::MaskLaw::Prism),
                FrameOrdering::Gray },
            WavetableRecipe { "factory.complex.shatter",
                "Shatter",
                WavetableCategory::Complex,
                MaskTableGenerator::kComplexFrames,
                WavetableKernel::MaskComplex,
                kernel::MaskComplex::make (kernel::MaskLaw::Shatter),
                FrameOrdering::Gray },
            WavetableRecipe { "factory.complex.weave",
                "Weave",
                WavetableCategory::Complex,
                MaskTableGenerator::kComplexFrames,
                WavetableKernel::MaskComplex,
                kernel::MaskComplex::make (kernel::MaskLaw::Weave),
                FrameOrdering::Gray }
        };

        // `Law` is 0-based at SegmentOps, so a COMPLEX law's recipe index is its enumerator minus
        // one. Written as a switch rather than as that arithmetic: the offset would be a silent
        // wrong answer the day a law is inserted, and a switch is a compile error instead.
        [[nodiscard]] kernel::MaskLaw maskLawOf (const WavetableRecipe& recipe) noexcept
        {
            return kernel::MaskComplex::from (recipe.params).law;
        }
    }

    const WavetableRecipe* MaskTableGenerator::recipeFor (Law law) noexcept
    {
        switch (law)
        {
            case Law::Prism:
                return &kComplexRecipes[0];
            case Law::Shatter:
                return &kComplexRecipes[1];
            case Law::Weave:
                return &kComplexRecipes[2];
            case Law::SegmentOps:
            default:
                return nullptr; // not a factory bank — see the note above
        }
    }

    float MaskTableGenerator::dcOffsetFor (int mask) noexcept
    {
        // Built once, off the audio thread, on first use — 512 floats. The render-time oscillator
        // reads this per sample (the mask is a modulation destination), so it cannot be a
        // per-change measurement.
        // STATIC STORAGE, not a heap buffer — this function is reached from the render-time
        // oscillator's `setMask`, i.e. from the audio thread, so a first call that allocated would
        // make the `noexcept` a lie and leave the RT-safety claim resting on a warm-up nobody
        // guarantees. 2 KB of table plus a transient carrier at static-init time.
        static const std::array<float, static_cast<std::size_t> (kMaskSpace)> table = [] {
            std::array<float, static_cast<std::size_t> (kMaskSpace)> t {};
            std::array<float, static_cast<std::size_t> (kFrameLength)> carrier {};
            writeSine (carrier.data());

            // Mask 0's sum, accumulated in exactly the order the masked loop below uses. The
            // subtraction is what makes the mask-0 correction exactly 0.0f rather than ~1e-16.
            double sum0 = 0.0;
            for (int i = 0; i < kFrameLength; ++i)
                sum0 += static_cast<double> (carrier[static_cast<std::size_t> (i)]);

            for (int m = 0; m < kMaskSpace; ++m)
            {
                double sum = 0.0;
                for (int i = 0; i < kFrameLength; ++i)
                {
                    const auto tap = segmentTap (m, static_cast<double> (i));
                    sum += static_cast<double> (
                        tap.gain * carrier[static_cast<std::size_t> (static_cast<int> (tap.index))]);
                }
                t[static_cast<std::size_t> (m)] =
                    static_cast<float> ((sum - sum0) / static_cast<double> (kFrameLength));
            }
            return t;
        }();

        return table[static_cast<std::size_t> (mask & (kMaskSpace - 1))];
    }

    void MaskTableGenerator::renderMask (Law law, int mask, float* frame)
    {
        const int m = wrapMask (law, mask);
        if (const auto* recipe = recipeFor (law))
        {
            WavetableKernels::renderMask (maskLawOf (*recipe), m, frame);
            return;
        }
        renderSegmentOps (m, frame);
    }

    void MaskTableGenerator::renderFrame (Law law, int frame, float* out)
    {
        // GRAY-NATIVE: frame f holds mask grayCode(f), so adjacent frames differ by exactly one
        // mask bit and the `mask` param's `Off` default is already the musical traversal
        // (§2.3 / T-PROTO-02 — the 2026-07-29 anti-Binary ruling, kept by construction).
        //
        // THE CLAMP IS LOAD-BEARING; ITS TOP EDGE IS AN EQUIVALENT MUTANT — labelled, T-CORE-03
        // category (c). Deleting the clamp is KILLED (an index off the end reads an arbitrary middle
        // frame: grayCode(99) folds to mask 2), so the guard is not decorative. But `frames` in place
        // of `frames − 1` cannot be caught in the frame domain at all, because the reflected Gray
        // code satisfies `G(2^n) ≡ G(2^n − 1)  (mod 2^n)`: grayCode(16) is 24, the mask wrap folds it
        // to 8, and grayCode(15) is 8 — the two spellings render the same frame for every index above
        // the bank. Written `frames − 1` because that is the index the contract names (the LAST
        // frame), not because a test can tell.
        // The three COMPLEX laws go through the recipe seam; `SegmentOps` keeps the direct path,
        // because it has no recipe (it is not a factory bank — see the note above `kComplexRecipes`).
        if (const auto* recipe = recipeFor (law))
        {
            WavetableKernels::render (*recipe, frame, out);
            return;
        }

        const int frames = maskSpaceFor (law);
        const int f = frame < 0 ? 0 : (frame >= frames ? frames - 1 : frame);
        renderMask (law, grayCode (f), out);
    }

    std::vector<int> MaskTableGenerator::layoutMasks (Law law, Ordering ordering, int maskFirst, int maskCount)
    {
        // Clamp the slice into the LAW's mask space (4-bit for the COMPLEX laws, 9-bit for
        // SegmentOps) — so `generate (law, Gray, 0, kComplexFrames)` is the whole of a COMPLEX
        // bank and asking for more cannot hand back masks the law has no frames for.
        const int space = maskSpaceFor (law);
        int first = maskFirst < 0 ? 0 : (maskFirst >= space ? space - 1 : maskFirst);
        int count = maskCount < 1 ? 1 : maskCount;
        if (first + count > space)
            count = space - first;

        std::vector<int> layout (static_cast<std::size_t> (count));

        // Gray needs a power-of-two count and an aligned first so `first | grayCode(i)` is a
        // bijection onto the slice with the one-bit-step property; otherwise fall back to Binary.
        const bool grayValid = (count & (count - 1)) == 0 && (first & (count - 1)) == 0;

        if (ordering == Ordering::Gray && grayValid)
        {
            for (int i = 0; i < count; ++i)
                layout[static_cast<std::size_t> (i)] = first | grayCode (i);
            return layout;
        }

        for (int i = 0; i < count; ++i)
            layout[static_cast<std::size_t> (i)] = first + i;

        if (ordering == Ordering::Centroid)
        {
            std::vector<double> centroids (static_cast<std::size_t> (count));
            std::vector<float> frame (static_cast<std::size_t> (kFrameLength));
            for (int i = 0; i < count; ++i)
            {
                renderMask (law, first + i, frame.data());
                centroids[static_cast<std::size_t> (i)] = spectralCentroid (frame.data(), kFrameLength);
            }
            std::sort (layout.begin(), layout.end(), [&] (int a, int b) {
                const double ca = centroids[static_cast<std::size_t> (a - first)];
                const double cb = centroids[static_cast<std::size_t> (b - first)];
                if (ca != cb)
                    return ca < cb;
                return a < b; // deterministic tie-break on the mask value
            });
        }

        return layout;
    }

    std::vector<float> MaskTableGenerator::generate (Law law, Ordering ordering, int maskFirst, int maskCount)
    {
        const auto layout = layoutMasks (law, ordering, maskFirst, maskCount);
        std::vector<float> frames (layout.size() * static_cast<std::size_t> (kFrameLength));
        for (std::size_t f = 0; f < layout.size(); ++f)
            renderMask (law, layout[f], frames.data() + f * static_cast<std::size_t> (kFrameLength));
        return frames;
    }

    double MaskTableGenerator::spectralCentroid (const float* frame, int length)
    {
        std::vector<double> re (static_cast<std::size_t> (length));
        std::vector<double> im (static_cast<std::size_t> (length), 0.0);
        for (int i = 0; i < length; ++i)
            re[static_cast<std::size_t> (i)] = static_cast<double> (frame[i]);

        fft (re.data(), im.data(), length);

        double weighted = 0.0;
        double total = 0.0;
        for (int k = 1; k <= length / 2; ++k)
        {
            const double mag = std::sqrt (re[static_cast<std::size_t> (k)] * re[static_cast<std::size_t> (k)]
                                          + im[static_cast<std::size_t> (k)] * im[static_cast<std::size_t> (k)]);
            weighted += static_cast<double> (k) * mag;
            total += mag;
        }
        return total > 0.0 ? weighted / total : 0.0;
    }
}
