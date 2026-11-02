#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ForkConfig.h>
#include <nemesis/dsp/FirKernel.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace nemesis::dsp
{
#if NEMESIS_FORK_EXACT
    // FIREFORK E14/N1: one pool for every oversampler's coefficients (and, with NEMESIS_FORK_APPROX, the compact kernels'
    // coefficients and tap offsets). Defined in Oversampler.cpp; NEMESIS_FORK_FAST_DATA places it (the firmware: DTCM).
    struct ForkTapPool
    {
        static constexpr std::size_t kSize = 1024;
        static Real pool[kSize];
        static std::uint16_t idx[kSize];
        static std::size_t used;
    };
#endif
    // Integer-factor oversampled wrapper for a per-sample nonlinearity — the analog-family
    // drive path `OS↑ → waveshaper → OS↓` (drive.md §0.2). A memoryless (or ADAA) shaper
    // makes harmonics above Nyquist that fold back as inharmonic aliasing; running it at
    // OS·fs and band-limiting on the way in and out pushes that fold-back above the audible
    // band. Pairs with the ADAA primitive (Waveshaper.h): ADAA carries the alias floor at
    // low OS, oversampling carries it for the hard/near-square curves. Factor 1 is a true
    // bypass (the W1a "drop the OS factor" switch) — the shaper runs once, unfiltered.
    //
    // Sample-rate agnostic: the FIR cutoff is base-Nyquist relative to the OS rate, so one
    // design (and one golden) serves every host rate — waveshaping is a sample-domain op.
    //
    // Design: prototype low-pass = windowed sinc (4-term Blackman-Harris), length
    // Ntaps = 2·M·L + 1 with M taps per phase either side of centre, cutoff = 1/(2L)
    // normalised. Upsampling is polyphase (L phases, coeffs scaled by L for unity passband);
    // downsampling is the same prototype as a decimating FIR, evaluated only at the retained
    // phase. Both have integer group delay M·L OS-samples, so the cascade delays by exactly
    // 2·M base-rate samples. Coeffs held in double, I/O float.
    //
    // PASSBAND: M = 8 (17 taps per polyphase phase) with the cutoff AT base Nyquist means the audible
    // top end is already in the transition band. Through the identity-shaper cascade at fs = 48 kHz,
    // approximately -0.1 dB at 16 kHz, -0.5 dB at 18 kHz, -1.9 dB at 20 kHz, the same at every factor
    // (relative-frequency design; measured with a scratch sine sweep, not asserted by a test). Up and
    // down share the prototype, so a single pass is about half those figures (roughly -1 dB at 20 kHz).
    //
    // References: Crochiere & Rabiner, "Multirate Digital Signal Processing"; Vaidyanathan,
    // "Multirate Systems and Filter Banks" (polyphase decomposition, linear-phase FIR).
    //
    // RT-safe: prepare() designs coefficients and allocates; the hot path (processSample)
    // is inline, pure arithmetic, no libm, no allocation. NOT thread-safe against prepare().
    class Oversampler
    {
    public:
        // Designs the filters for an integer oversampling `factor` (>= 1; values < 1 are
        // clamped to 1 = bypass), allocates the delay lines, and clears state. Not RT-safe
        // and may allocate (hence not noexcept); call off the audio thread (like prepareToPlay).
        // No sample rate needed.
        void prepare (int factor);

        // Clears the delay lines (silences the wrapper) without touching coefficients.
        void reset() noexcept;

        // [D-M-0188 S5R 4] COPYING BETWEEN TWO INSTANCES OF ONE DESIGN COPIES STATE ONLY. The coefficients are a
        // pure function of the factor (`prepare` takes nothing else), so two instances prepared at the same factor
        // hold equal FIRs already: an assignment between them copies the delay lines and positions and leaves
        // the coefficients where they are. That is what a voice's right chain does when it starts as a copy of
        // the left — every filter bank's oversamplers, in one audio sample. Any other pair copies whole.
        Oversampler() = default;
        ~Oversampler() = default;
        Oversampler (const Oversampler&) = default;
        Oversampler (Oversampler&&) noexcept = default;
        Oversampler& operator= (Oversampler&&) noexcept = default;
        Oversampler& operator= (const Oversampler& other)
        {
            if (this == &other)
                return *this;
            if (sameDesign (other))
            {
                copyStateFrom (other);
                return *this;
            }
            factor_ = other.factor_;
            phaseTaps_ = other.phaseTaps_;
            downTaps_ = other.downTaps_;
            ohistLen_ = other.ohistLen_;
            latency_ = other.latency_;
            up_ = other.up_;
            down_ = other.down_;
            xhist_ = other.xhist_;
            ohist_ = other.ohist_;
            xpos_ = other.xpos_;
            opos_ = other.opos_;
            return *this;
        }
        // Prepared at one factor, so one set of coefficients and of buffer sizes.
        [[nodiscard]] bool sameDesign (const Oversampler& other) const noexcept
        {
            return other.factor_ == factor_ && other.up_.size() == up_.size() && other.down_.size() == down_.size()
                   && other.xhist_.size() == xhist_.size() && other.ohist_.size() == ohist_.size();
        }

        // [stereo voice S2] Takes `other`'s delay-line STATE (never its coefficients), so a second
        // instance continues from the history the first one heard. Both must be prepared with the same
        // factor; a mismatched pair is left untouched. RT-safe: copies into the existing storage.
        void copyStateFrom (const Oversampler& other) noexcept
        {
            if (other.factor_ != factor_ || other.xhist_.size() != xhist_.size() || other.ohist_.size() != ohist_.size())
                return;
            std::copy (other.xhist_.begin(), other.xhist_.end(), xhist_.begin());
            std::copy (other.ohist_.begin(), other.ohist_.end(), ohist_.begin());
            xpos_ = other.xpos_;
            opos_ = other.opos_;
        }

        // Current oversampling factor (1 == bypass).
        [[nodiscard]] int factor() const noexcept { return factor_; }

        // [T-CMP-37 L2] THE DELAY LINES AS VALUES, for the lane-wise (across-voices) filter to take a voice's history in and
        // give it back. Each window is the history in LOGICAL order, newest first: `inputWindowLen()` input samples and
        // `osWindowLen()` OS-rate samples, which is every slot the FIRs can still read (phaseTaps_ and ohistLen_). The
        // ring POSITIONS are not state a reader can see (the mirror makes a read window contiguous wherever it starts),
        // so `setWindows` is free to put the newest at slot 0 — the next sample then continues exactly as if the ring
        // had never been copied. 0 at factor 1 (no history).
        [[nodiscard]] int inputWindowLen() const noexcept { return phaseTaps_; }
        [[nodiscard]] int osWindowLen() const noexcept { return factor_ == 1 ? 0 : ohistLen_; }
        template <class T> // FIREFORK: lanes hand over double windows even when the scalar is float
        void copyWindowsTo (T* x, T* o) const noexcept
        {
            if (factor_ == 1)
                return;
            const auto xl = static_cast<std::size_t> (phaseTaps_);
            for (std::size_t i = 0; i < xl; ++i)
                x[i] = xhist_[(static_cast<std::size_t> (xpos_) + xl - i) % xl];
            const auto ol = static_cast<std::size_t> (ohistLen_);
            for (std::size_t i = 0; i < ol; ++i)
                o[i] = ohist_[(static_cast<std::size_t> (opos_) + ol - i) % ol];
        }
        template <class T>
        void setWindows (const T* x, const T* o) noexcept
        {
            if (factor_ == 1)
                return;
            xpos_ = 0;
            opos_ = 0;
            const auto xl = static_cast<std::size_t> (phaseTaps_);
            for (std::size_t i = 0; i < xl; ++i)
            {
                const std::size_t slot = (xl - i) % xl;
                xhist_[slot] = x[i];
                xhist_[slot + xl] = x[i];
            }
            const auto ol = static_cast<std::size_t> (ohistLen_);
            for (std::size_t i = 0; i < ol; ++i)
            {
                const std::size_t slot = (ol - i) % ol;
                ohist_[slot] = o[i];
                ohist_[slot + ol] = o[i];
            }
        }

        // Base-rate latency introduced by the up/down FIR cascade (0 when bypassed) — the figure
        // for a signal PASSED IN as `x`, which is every shaper consumer (the filter drive cores).
        [[nodiscard]] int latencySamples() const noexcept { return latency_; }

        // Base-rate group delay for a signal GENERATED inside the shaper rather than passed in —
        // the PhaseDistortion / Bitmask oscillator seam, where `x` is a dummy 0 and the waveform is
        // synthesised at each OS phase. THAT SIGNAL NEVER TOUCHES THE INTERPOLATING FIR (a zero
        // input upsamples to zeros), so it sees only the decimator's M·L OS-samples — HALF the
        // cascade figure above — plus the (factor−1) OS-samples the phase-0 retention holds it back
        // by. Hence M + (factor−1)/factor, which is FRACTIONAL, which is why this returns a double
        // and why `latencySamples()` is the wrong number to report for a generator.
        //
        // [T-MASK-03 leaf C] Measured, not derived after the fact: the phase lag of a pure sine
        // through this seam is 8.5 / 8.75 / 8.875 base samples at factor 2 / 4 / 8, frequency-
        // independent (linear phase), against the 16 the cascade figure claims at every factor.
        [[nodiscard]] Real generatedLatencySamples() const noexcept
        {
            if (factor_ == 1)
                return 0.0;
            const Real m = 0.5 * static_cast<Real> (phaseTaps_ - 1); // phaseTaps_ = 2M + 1
            return m + static_cast<Real> (factor_ - 1) / static_cast<Real> (factor_);
        }

        // Processes one base-rate sample: upsample by `factor`, apply `shape` to every
        // oversampled sample (so a stateful shaper advances at OS·fs), then downsample back
        // to one base-rate output. `shape` is any callable `float(float)` — kept a template
        // so it inlines with no std::function allocation. Hot path: inline, no libm/alloc.
        template <typename Shaper>
        float processSample (float x, Shaper&& shape) noexcept
        {
            if (factor_ == 1)
                return shape (x);

            // Push the new input as the newest sample of the circular input history (both mirror halves).
            xpos_ = xpos_ + 1;
            if (xpos_ >= phaseTaps_)
                xpos_ = 0;
            const auto xd = static_cast<Real> (x);
            xhist_[static_cast<std::size_t> (xpos_)] = xd;
            xhist_[static_cast<std::size_t> (xpos_) + static_cast<std::size_t> (phaseTaps_)] = xd;
            // [T-CMP-34] x[n-j] == newest[-j] for j < phaseTaps_: the mirror makes the window contiguous.
            const Real* newest = xhist_.data() + xpos_ + phaseTaps_;

            // [T-CMP-34] The phase sums read only the input history, never the shaper, so up to kPhaseChunk of them
            // are summed BEFORE the shaper runs on them in phase order: independent chains the core overlaps. Each
            // sum keeps its own j order and expression, so the result is bit-identical to one phase at a time.
            for (int p0 = 0; p0 < factor_; p0 += kPhaseChunk)
            {
                const int pEnd = std::min (factor_, p0 + kPhaseChunk);
                Real os[kPhaseChunk];
                for (int p = p0; p < pEnd; ++p)
                {
                    // Upsample: phase-p interpolated sample  os = sum_j up[p][j] * x[n-j], rounded as fir::dot pins it.
                    const Real* row = up_.data() + static_cast<std::size_t> (p) * static_cast<std::size_t> (phaseTaps_);
                    os[p - p0] = (fork::kApprox && fork::kExact) ? upRow (p, newest) : fir::dot (row, newest, phaseTaps_); // FIREFORK N1
                }
                // Nonlinearity at the OS rate, then push into the OS-rate history.
                for (int p = p0; p < pEnd; ++p)
                    pushOs (static_cast<Real> (shape (static_cast<float> (os[p - p0]))));
            }
            return decimate();
        }

        // [D-M-0356] The DECIMATING HALF alone, for a consumer that GENERATES its OS-rate stream rather
        // than shaping an upsampled input: `next()` (any callable `float()`) supplies this step's `factor`
        // OS-rate samples, oldest first, and they are band-limited back to one base-rate output. Bit for
        // bit what `processSample` returns when its shaper ignores its argument — minus the interpolating
        // FIR whose output that shaper would discard. Factor 1 returns `next()` once. The input history
        // is not advanced, so an instance should be driven through ONE of the two entries, not both.
        template <typename Source>
        float decimateSample (Source&& next) noexcept
        {
            if (factor_ == 1)
                return next();
            for (int p = 0; p < factor_; ++p)
                pushOs (static_cast<Real> (static_cast<float> (next())));
            return decimate();
        }

        // FIREFORK E2: the INTERPOLATING half alone, for a pair whose decimated output nobody reads: the phase sums go to
        // `shape` (which records them) in the order processSample uses, the OS history is not advanced and nothing is
        // decimated. Each phase sum is the same expression as in processSample, so what `shape` sees is bit-identical.
        template <typename Shaper>
        void upsampleOnly (float x, Shaper&& shape) noexcept
        {
            if (factor_ == 1)
            {
                shape (x);
                return;
            }
            pushInput (x);
            const Real* newest = xhist_.data() + xpos_ + phaseTaps_;
            for (int p = 0; p < factor_; ++p)
                shape (static_cast<float> (upRow (p, newest)));
        }
        // FIREFORK E6: processSample without the final decimation (its output would be discarded); the OS history advances.
        template <typename Shaper>
        void processNoDecimate (float x, Shaper&& shape) noexcept
        {
            if (factor_ == 1)
            {
                (void) shape (x);
                return;
            }
            pushInput (x);
            const Real* newest = xhist_.data() + xpos_ + phaseTaps_;
            for (int p = 0; p < factor_; ++p)
                pushOs (static_cast<Real> (shape (static_cast<float> (upRow (p, newest)))));
        }
        // FIREFORK E6: decimateSample without the decimation: the generated OS samples only enter the history.
        template <typename Source>
        void pushOsOnly (Source&& next) noexcept
        {
            if (factor_ == 1)
            {
                (void) next();
                return;
            }
            for (int p = 0; p < factor_; ++p)
                pushOs (static_cast<Real> (static_cast<float> (next())));
        }
        // FIREFORK E15: a GENERATED OS stream (the shaper ignores its argument): decimateSample with NEMESIS_FORK_EXACT,
        // otherwise the upstream processSample (0, ...) form, unchanged.
        template <typename Source>
        float generateSample (Source&& next) noexcept
        {
            if constexpr (fork::kExact)
                return decimateSample (next);
            else
                return processSample (0.0f, [&next] (float) noexcept { return next(); });
        }
        // FIREFORK E2: only advance the input history (so a later processSample/upsampleOnly sees the right window).
        void pushInput (float x) noexcept
        {
            if (factor_ == 1)
                return;
            xpos_ = xpos_ + 1;
            if (xpos_ >= phaseTaps_)
                xpos_ = 0;
            const auto xd = static_cast<Real> (x);
            xhist_[static_cast<std::size_t> (xpos_)] = xd;
            xhist_[static_cast<std::size_t> (xpos_) + static_cast<std::size_t> (phaseTaps_)] = xd;
        }

    private:
        static constexpr int kPhaseChunk = 8; // phase sums held at once (processSample); any factor works

        void pushOs (Real w) noexcept
        {
            opos_ = opos_ + 1;
            if (opos_ >= ohistLen_)
                opos_ = 0;
            ohist_[static_cast<std::size_t> (opos_)] = w;
            ohist_[static_cast<std::size_t> (opos_) + static_cast<std::size_t> (ohistLen_)] = w;
        }

        // Downsample: decimating FIR over the OS history, referenced at the phase-0
        // OS sample of this step (delay factor-1 back from the newest). Retaining the
        // grid-aligned phase makes the cascade group delay the integer 2*M base samples;
        // referencing the newest phase would leave a half-sample offset for even factors.
        [[nodiscard]] float decimate() const noexcept
        {
            // [T-CMP-34] ONE serial chain, j order kept: splitting it would round differently. The mirror only removes
            // the wrap: ref[-j] for j < downTaps_ stays inside the 2·ohistLen_ buffer (ohistLen_ = downTaps_ + phase0).
            const int phase0 = factor_ - 1;
            const Real* ref = ohist_.data() + opos_ + ohistLen_ - phase0;
#if NEMESIS_FORK_EXACT && NEMESIS_FORK_APPROX
            { // FIREFORK N1: the compact, folded decimator
                const Real* blk = down_.data() + downTaps_;
                const std::uint16_t* off = ForkTapPool::idx + (blk - ForkTapPool::pool) + 2;
                const Real* pc = blk + 2;
                const int n = static_cast<int> (blk[1]);
                const int last = downTaps_ - 1;
                Real a0 = blk[0] * ref[-(downTaps_ / 2)], a1 = 0.0, a2 = 0.0, a3 = 0.0;
                int i = 0;
                for (; i + 3 < n; i += 4)
                {
                    a0 += pc[i] * (ref[-off[i]] + ref[-(last - off[i])]);
                    a1 += pc[i + 1] * (ref[-off[i + 1]] + ref[-(last - off[i + 1])]);
                    a2 += pc[i + 2] * (ref[-off[i + 2]] + ref[-(last - off[i + 2])]);
                    a3 += pc[i + 3] * (ref[-off[i + 3]] + ref[-(last - off[i + 3])]);
                }
                for (; i < n; ++i)
                    a0 += pc[i] * (ref[-off[i]] + ref[-(last - off[i])]);
                return static_cast<float> ((a0 + a1) + (a2 + a3));
            }
#else
            return static_cast<float> (fir::dot (down_.data(), ref, downTaps_));
#endif
        }

        // One upsampling phase. FIREFORK N1 (approx builds): phase 0 is the centre tap alone, the others drop their exact-0
        // last tap and sum in four accumulators. Otherwise the pinned fir::dot.
        [[nodiscard]] Real upRow (int p, const Real* newest) const noexcept
        {
            const Real* row = up_.data() + static_cast<std::size_t> (p) * static_cast<std::size_t> (phaseTaps_);
            if constexpr (fork::kApprox && fork::kExact)
            {
                const int m = (phaseTaps_ - 1) / 2;
                if (p == 0)
                    return row[m] * newest[-m];
                const int n = phaseTaps_ - 1;
                Real a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
                int j = 0;
                for (; j + 3 < n; j += 4)
                {
                    a0 += row[j] * newest[-j];
                    a1 += row[j + 1] * newest[-(j + 1)];
                    a2 += row[j + 2] * newest[-(j + 2)];
                    a3 += row[j + 3] * newest[-(j + 3)];
                }
                for (; j < n; ++j)
                    a0 += row[j] * newest[-j];
                return (a0 + a1) + (a2 + a3);
            }
            return fir::dot (row, newest, phaseTaps_);
        }

        int factor_ = 1;
        int phaseTaps_ = 0; // taps per upsampling polyphase phase (2M + 1)
        int downTaps_ = 0; // prototype length Ntaps = 2ML + 1 (decimating FIR)
        int ohistLen_ = 0; // OS ring length = downTaps_ + factor_ - 1 (phase-0 retention)
        int latency_ = 0; // base-rate group delay of the cascade
        // [S5R B32] NOT a value: the 4 bytes the compiler would otherwise pad here before the vectors. With no implicit
        // padding anywhere, the sizeof check below grows with ANY added member — a bool or an int included.
        [[maybe_unused]] int layoutPad_ = 0;

#if NEMESIS_FORK_EXACT
    public:
        // FIREFORK E14: a view of a design held in the shared pool (Oversampler.cpp), with the vector calls this class uses.
        struct SharedTaps
        {
            const Real* p = nullptr;
            std::size_t n = 0;
            [[nodiscard]] const Real* data() const noexcept { return p; }
            [[nodiscard]] std::size_t size() const noexcept { return n; }
            void clear() noexcept
            {
                p = nullptr;
                n = 0;
            }
        };
        using TapStore = SharedTaps;
    private:
#else
    public:
        using TapStore = std::vector<Real>;
    private:
#endif
        TapStore up_; // flat [factor_ * phaseTaps_] polyphase up coefficients
        TapStore down_; // [downTaps_] decimation FIR coefficients

        // [T-CMP-34] Both histories are MIRRORED: slot i and i + len always hold the same sample, so a read window
        // ending at the newest slot is contiguous and the hot loops carry no wrap branch.
        std::vector<Real> xhist_; // circular input history (2 x phaseTaps_, mirrored)
        std::vector<Real> ohist_; // circular OS-rate history (2 x ohistLen_, mirrored)
        int xpos_ = 0; // newest slot in xhist_
        int opos_ = 0; // newest slot in ohist_
    };
    // [S5R 4] The hand-written copy assignment above names every member, and `copyStateFrom` every STATE member: a member
    // added here must join the first and, if it is state, the second. Exact (no padding: see `layoutPad_`), so it fires.
    static_assert (sizeof (Oversampler) == 8 * sizeof (int) + 2 * sizeof (Oversampler::TapStore) + 2 * sizeof (std::vector<Real>),
        "Oversampler gained a member: add it to operator= (const Oversampler&), and to copyStateFrom if it is state");
}
