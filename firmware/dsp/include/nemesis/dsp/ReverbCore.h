#pragma once

#include <nemesis/dsp/Real.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nemesis::dsp
{
    // [T-CMP-13 leaf A] THE SHARED REVERB PRIMITIVE — `specs/dsp/reverb.md` §0, and
    // nothing above it. §0 exists because Room, Hall and Cathedral are all "a Jot FDN of
    // some SIZE plus a signature stage", so this class owns every part the three of them
    // share and owns no type's identity:
    //
    //   * §0.1  fractional delay lines, read with LINEAR interpolation — the spec's own
    //           recommendation for slow-LFO late-reverb modulation (Hall/Cathedral). The
    //           Lagrange-3/Thiran reads it also lists are for Plate tank modulation and
    //           Spring dispersion, which are DIFFERENT topologies (§3/§4), not this core.
    //   * §0.3  a cascade of Schroeder allpasses (0–6) as the input diffuser. Exposed as
    //           its own `Allpass` type, because "the diffuser has flat magnitude" is a
    //           claim about THAT object and measuring it through the FDN would be
    //           measuring the FDN too.
    //   * §0.4  the Jot FDN itself, at M ∈ {4, 8, 16} lines with a UNITARY mixing matrix —
    //           Householder (Room, §1.3) or Hadamard (Hall §2.3, Cathedral §6.2). Unitary
    //           mixing is what decouples decay from echo pattern: A is lossless, so the
    //           decay is set ENTIRELY by the per-line attenuators `g_i` and the per-line
    //           one-pole damping `h_i`. Plus Jot's tonal-correction shelf.
    //   * §0.5  pre-delay, a per-line DC blocker, and the input tilt (LP + HP).
    //   * §0.6  one slow LFO per line at UNCORRELATED phases, modulating the read length.
    //
    // NOT here, on purpose: Room's early-reflection tap set (§1.2), Hall's stereo width,
    // wet/dry mix, the Plate (Dattorro) and Spring (dispersion) topologies, Shimmer's
    // pitch-shifted loop, Cathedral's modal bank (§6.3), and every registry/`FxEffect`
    // wiring concern. Those are later leaves. This is the 100 %-wet mono-in/stereo-out
    // engine those leaves configure.
    //
    // SIZE IS A PREPARE-TIME DECISION, not a runtime one: `prepare` allocates M lines and
    // the matrix follows M. A type does not change its line count while running.
    //
    // RT-safety: `prepare` allocates every ring once. Everything transcendental (decay
    // gains, one-pole coefficients, the LFO rotation, the prime snap) is resolved in
    // `resolveControls`, called from the sample loop at most once per `kControlChunk`
    // samples and only when a setter dirtied it — the `DelayCore` idiom. The per-sample
    // path is pure arithmetic: no allocation, no locks, no libm, no JUCE. Deterministic:
    // same input + same param snapshot -> bit-identical output. Computed in double
    // internally, emitted as float. Not thread-safe against `prepare`.
    class ReverbCore
    {
    public:
        // §0.4's two unitary matrices. Householder `A = I - (2/M)·u·uᵀ` is one running sum
        // per sample whatever M is; Hadamard is an M·log2(M) butterfly and mixes more
        // diffusely. Both are exactly orthogonal, which is the property the whole Jot
        // decoupling argument rests on.
        enum class Mixing : std::uint8_t {
            Householder = 0, // §1.3 — Room's 4×4
            Hadamard = 1 // §2.3 / §6.2 — Hall's 8×8, Cathedral's 16×16
        };

        struct Stereo
        {
            float l = 0.0f;
            float r = 0.0f;
        };

        // §0.4 sizes actually used by the three FDN types. `prepare` snaps to the nearest
        // of these rather than accepting an arbitrary M: Hadamard needs a power of two,
        // and a size no type asks for is untested surface.
        static constexpr int kMaxLines = 16;
        static constexpr int kMaxDiffusers = 6; // §6.1 — Cathedral's 6-stage diffuser

        // A linear read needs its upper neighbour to be an ALREADY-WRITTEN slot, and the
        // write happens after the read, so the shortest honest line is 2 samples.
        static constexpr Real kMinLineDelaySamples = 2.0;

        // §0.6 depth is "a few samples" (2–6 for Hall, 1–3 for Cathedral). The ring
        // reserves this much on top of the longest delay so a later leaf can turn
        // modulation up without re-`prepare`ing a running line — §7 "delay buffer sizing".
        static constexpr Real kMaxModDepthSamples = 32.0;

        static constexpr Real kMaxDiffuserMs = 60.0;
        static constexpr Real kMaxDiffusion = 0.9; // allpass `g`; §0.3 says 0.5–0.7

        // §0.5's DC-blocker pole.
        static constexpr Real kDcBlockerR = 0.995;

        // §7 "Denormals": Cathedral-length loops trickle toward the denormal range and
        // stall the FPU. The injection ALTERNATES sign, so it is a Nyquist-rate signal
        // rather than a DC offset — a DC injection is exactly what the per-line DC
        // blockers would integrate away, leaving the guard doing nothing.
        static constexpr Real kDenormalGuard = 1.0e-20;

        // How often control-rate parameters are re-resolved: 16 samples is ~3 kHz at
        // 48 kHz, far above any parameter gesture.
        static constexpr int kControlChunk = 16;

        // ------------------------------------------------------------------
        // §0.3 — the Schroeder allpass, one diffuser stage.
        //
        //   v[n] = x[n] - g·v[n-D]
        //   y[n] = g·v[n] + v[n-D]
        //
        // H(z) = (g + z^-D) / (1 + g·z^-D), whose magnitude is EXACTLY 1 at every
        // frequency for real |g| < 1 — it adds echo density without colouring the
        // spectrum, which is the entire reason §0.3 uses it rather than a comb.
        //
        // [T-CMP-13 leaf E] The sign above was corrected: this is what the two lines
        // ACTUALLY realise (§0.3's own "equivalently y[n] = −g·x[n] + (1−g²)·v[n−D]" has
        // the sign the other way round, and the recursion is what the spec states first
        // and what leaf A implemented). It matters because the two differ in PHASE, not in
        // magnitude — no diffuser test could see it, and `SpringReverbCore` reads this
        // object's group delay, whose direction is set by the sign of `g`: the pole sits
        // at −g, so a POSITIVE g delays highs most and a NEGATIVE one delays lows most.
        class Allpass
        {
        public:
            void prepare (int maxDelaySamples);
            void reset() noexcept;

            void setDelaySamples (int d) noexcept
            {
                const int hi = size_ > 1 ? size_ - 1 : 1;
                delay_ = d < 1 ? 1 : (d > hi ? hi : d);
            }

            [[nodiscard]] int delaySamples() const noexcept { return delay_; }

            // The INTERNAL state `v[n-d]`, as of the most recent `process` — §3.3's
            // Dattorro output taps read INSIDE the tank allpasses ("−AP_h2[1913]"), not
            // at their outputs, so a plate needs this and an FDN diffuser never does.
            // Read-only: it cannot change what the cascade renders.
            [[nodiscard]] Real tap (int d) const noexcept
            {
                return size_ > 0 ? buf_[static_cast<std::size_t> ((pos_ - 1 - d) & mask_)] : 0.0;
            }

            Real process (Real x, Real g) noexcept
            {
                if (size_ <= 0)
                    return x;

                const Real vD = buf_[static_cast<std::size_t> ((pos_ + size_ - delay_) & mask_)];
                const Real v = x - g * vD;
                buf_[static_cast<std::size_t> (pos_)] = v;
                pos_ = (pos_ + 1) & mask_;
                return g * v + vD;
            }

        private:
            std::vector<Real> buf_;
            int size_ = 0;
            int mask_ = 0;
            int pos_ = 0;
            int delay_ = 1;
        };

        // ------------------------------------------------------------------
        // Analytic laws, exposed as statics so a test pins the implementation against the
        // spec's own formula rather than against a golden.

        // §0.2's RT60 law, `g = 10^(-3·D/RT60)` with D in SECONDS. `rt60 <= 0` is the §7
        // FREEZE case: loop gain exactly 1.0, i.e. the lossless prototype.
        [[nodiscard]] static Real decayGainFor (Real delaySamples, Real rt60Seconds, Real fs) noexcept;

        // §0.4's unitary mix, in place over `numLines` values. Static because energy
        // conservation is a property of the MATRIX, testable with no reverb around it.
        static void mixInPlace (Real* v, int numLines, Mixing m) noexcept
        {
            if (m == Mixing::Householder)
            {
                // A = I - (2/M)·u·uᵀ with u = [1…1]ᵀ: one running sum, one MAC per line.
                Real sum = 0.0;
                for (int i = 0; i < numLines; ++i)
                    sum += v[i];

                const Real k = 2.0 * sum / static_cast<Real> (numLines);
                for (int i = 0; i < numLines; ++i)
                    v[i] -= k;
                return;
            }

            // Unnormalised Hadamard butterfly, then the 1/sqrt(M) that makes it unitary.
            // Spelled as literals (exact for 4 and 16) so the hot path stays libm-free.
            for (int len = 1; len < numLines; len <<= 1)
                for (int i = 0; i < numLines; i += (len << 1))
                    for (int j = 0; j < len; ++j)
                    {
                        const Real a = v[i + j];
                        const Real b = v[i + j + len];
                        v[i + j] = a + b;
                        v[i + j + len] = a - b;
                    }

            const Real s = numLines == 4    ? 0.5
                             : numLines == 16 ? 0.25
                                              : 0.35355339059327376220042218105242; // 1/sqrt(8)
            for (int i = 0; i < numLines; ++i)
                v[i] *= s;
        }

        // ------------------------------------------------------------------

        // Allocates every ring and clears state. `numLines` snaps to the nearest of
        // {4, 8, 16}. Not RT-safe — call like prepareToPlay.
        void prepare (Real sampleRate,
            int numLines,
            Mixing mixing,
            Real maxLineDelayMs = 200.0,
            Real maxPreDelayMs = 150.0);

        // Silences every line, clears every filter state, and re-seeds the §0.6 LFO
        // phases (so a reset render repeats bit-for-bit).
        void reset() noexcept;

        // --- control-rate setters -------------------------------------------------
        // These STORE; the transcendental math they imply happens in `resolveControls`
        // at the next chunk boundary, so a caller may push all of them in one go.

        // §0.4 delay lengths. The stored value is SNAPPED to a prime sample count, and
        // to a prime no other line is using — that is the spec's "mutually prime …
        // never harmonically related" rule, enforced here rather than trusted to each
        // type's table. Read `lineDelaySamples` for what the line actually runs at.
        void setLineDelaySamples (int line, Real samples) noexcept;
        void setLineDelayMs (int line, Real ms) noexcept
        {
            setLineDelaySamples (line, ms * sampleRate_ * 0.001);
        }

        // §0.2's RT60, in seconds, measured at the UNDAMPED (low) band — §2.3 sets the
        // per-line gain from the low-frequency RT60 and lets damping shorten the top.
        // `<= 0` freezes the network (g == 1, the lossless prototype).
        void setRt60Seconds (Real seconds) noexcept
        {
            rt60_ = seconds;
            dirty_ = true;
        }

        // §0.4 per-line damping `h_i`: a one-pole LP with unity DC gain, so it shortens
        // the HF RT60 and leaves the LF one alone. `<= 0` (or >= Nyquist) is OFF, and OFF
        // means BYPASSED — the filter is not run at all, which is what makes "damping off"
        // an exact statement rather than "a pole up near Nyquist".
        void setDampingHz (Real hz) noexcept
        {
            dampingHz_ = hz;
            dirty_ = true;
        }

        // §0.3 diffuser: how many allpasses are in circuit (0…kMaxDiffusers), their
        // delays, and the shared `g`. 0 stages is a bit-exact bypass.
        void setDiffuserCount (int n) noexcept
        {
            diffuserCount_ = n < 0 ? 0 : (n > kMaxDiffusers ? kMaxDiffusers : n);
        }
        void setDiffuserDelayMs (int idx, Real ms) noexcept;
        void setDiffusion (Real g) noexcept
        {
            diffusion_ = g < 0.0 ? 0.0 : (g > kMaxDiffusion ? kMaxDiffusion : g);
        }

        // §0.5 pre-delay. 0 ms is a bit-exact bypass (the read lands on the sample just
        // written), which is what lets a type dial it out completely.
        void setPreDelayMs (Real ms) noexcept
        {
            preDelayMs_ = ms;
            dirty_ = true;
        }

        // §0.5 input tilt — an LP (~8–12 kHz) and an HP (~20–100 Hz) that keep the
        // network from accumulating rumble and brittle top. `<= 0` is OFF for either.
        void setInputLpHz (Real hz) noexcept
        {
            inLpHz_ = hz;
            dirty_ = true;
        }
        void setInputHpHz (Real hz) noexcept
        {
            inHpHz_ = hz;
            dirty_ = true;
        }

        // §0.6 modulation. Depth 0 (or rate 0) is a bit-exact bypass — the LFOs do not
        // even advance — so an unmodulated network is exactly the fixed-delay one.
        void setModRateHz (Real hz) noexcept
        {
            modRateHz_ = hz;
            dirty_ = true;
        }
        void setModDepthSamples (Real samples) noexcept
        {
            modDepth_ = samples;
            dirty_ = true;
        }

        // The two §0.4/§0.5 GUARDS, individually defeatable — for the same reason
        // `DelayCore`'s are: each guard's claim is only measurable against a render with
        // that one guard removed. Both default ON.
        void setDcBlockEnabled (bool on) noexcept { dcBlockOn_ = on; }
        void setTonalCorrectionEnabled (bool on) noexcept
        {
            tonalCorrectionOn_ = on;
            dirty_ = true;
        }

        // --- audio ----------------------------------------------------------------

        // One sample in, the 100 %-WET stereo pair out.
        Stereo processSample (float x) noexcept
        {
            if (--chunkCountdown_ < 0)
            {
                chunkCountdown_ = kControlChunk;
                if (dirty_)
                    resolveControls();
                if (modActive_) // FIREFORK B11: hold the LFO amplitude at 1 (float rounding of cos grows it)
                    for (int i = 0; i < numLines_; ++i)
                    {
                        const auto k = static_cast<std::size_t> (i);
                        const Real g = 1.5 - 0.5 * (lfoU_[k] * lfoU_[k] + lfoV_[k] * lfoV_[k]);
                        lfoU_[k] *= g;
                        lfoV_[k] *= g;
                    }
            }

            if (numLines_ <= 0)
                return {};

            Real in = static_cast<Real> (x);

            // §0.5 input tilt, HP then LP.
            if (inHpActive_)
            {
                inHpState_ += inHpCoef_ * (in - inHpState_);
                in -= inHpState_;
            }
            if (inLpActive_)
            {
                inLpState_ += inLpCoef_ * (in - inLpState_);
                in = inLpState_;
            }

            in = preDelay (in);

            // §0.3 diffuser cascade.
            for (int k = 0; k < diffuserCount_; ++k)
                in = diffusers_[static_cast<std::size_t> (k)].process (in, diffusion_);

            // §0.4, in Jot's own order: read the lines, tap the output from them, then
            // damp -> DC-block -> attenuate -> MIX -> write back with the new input.
            std::array<Real, kMaxLines> v {};
            Real l = 0.0;
            Real r = 0.0;

            for (int i = 0; i < numLines_; ++i)
            {
                Real d = lineDelay_[static_cast<std::size_t> (i)];
                if (modActive_)
                {
                    // Coupled-form (rotation) LFO: two MACs, no libm, and the per-line
                    // start phases are spread so the lines are UNCORRELATED (§0.6).
                    const Real u = lfoU_[static_cast<std::size_t> (i)];
                    const Real w = lfoV_[static_cast<std::size_t> (i)];
                    lfoU_[static_cast<std::size_t> (i)] = lfoCos_ * u - lfoSin_ * w;
                    lfoV_[static_cast<std::size_t> (i)] = lfoSin_ * u + lfoCos_ * w;
                    d += modDepth_ * w;
                    if (d < kMinLineDelaySamples)
                        d = kMinLineDelaySamples;
                    else if (d > maxLineDelay_)
                        d = maxLineDelay_;
                }

                const Real s = readLine (i, d);

                // §7 stereo decorrelation: L and R are driven by DIFFERENT line subsets
                // (even / odd), not by panning one mono tail.
                if ((i & 1) == 0)
                    l += s;
                else
                    r += s;

                Real f = s;
                if (dampActive_)
                {
                    dampState_[static_cast<std::size_t> (i)] += dampCoef_ * (f - dampState_[static_cast<std::size_t> (i)]);
                    f = dampState_[static_cast<std::size_t> (i)];
                }
                if (dcBlockOn_)
                {
                    const Real o = f - dcPrev_[static_cast<std::size_t> (i)]
                                     + kDcBlockerR * dcState_[static_cast<std::size_t> (i)];
                    dcPrev_[static_cast<std::size_t> (i)] = f;
                    dcState_[static_cast<std::size_t> (i)] = o;
                    f = o;
                }
                v[static_cast<std::size_t> (i)] = f * decayGain_[static_cast<std::size_t> (i)];
            }

            mixInPlace (v.data(), numLines_, mixing_);

            const Real inject = in * inputGain_ + denorm_;
            denorm_ = -denorm_;

            for (int i = 0; i < numLines_; ++i)
                lineBuf_[static_cast<std::size_t> (i) * static_cast<std::size_t> (lineStride_)
                         + static_cast<std::size_t> (writePos_)] = inject + v[static_cast<std::size_t> (i)];

            writePos_ = (writePos_ + 1) & lineMask_;

            l *= tapGain_;
            r *= tapGain_;

            // §0.4 Jot tonal correction: a first-order high shelf that gives back the
            // average top end the damping filters took, so dry -> wet stays neutral in
            // timbre while the DECAY stays frequency-dependent. Unity at DC by
            // construction, and exactly the identity when damping is off (b == 0).
            if (tcActive_)
            {
                const Real ol = tcGain_ * (l - tcB_ * tcPrevL_);
                const Real orr = tcGain_ * (r - tcB_ * tcPrevR_);
                tcPrevL_ = l;
                tcPrevR_ = r;
                l = ol;
                r = orr;
            }

            return { static_cast<float> (l), static_cast<float> (r) };
        }

        void processBlock (const float* in, float* outL, float* outR, int numSamples) noexcept
        {
            if (in == nullptr || outL == nullptr || outR == nullptr || numSamples <= 0)
                return;

            for (int n = 0; n < numSamples; ++n)
            {
                const Stereo s = processSample (in[n]);
                outL[n] = s.l;
                outR[n] = s.r;
            }
        }

        // --- read-backs (tests, a surface's display) ------------------------------

        [[nodiscard]] int numLines() const noexcept { return numLines_; }
        [[nodiscard]] Mixing mixing() const noexcept { return mixing_; }

        // The RESOLVED (prime-snapped) length of a line, and the per-line attenuator the
        // §0.2 law produced for it.
        [[nodiscard]] Real lineDelaySamples (int i) const noexcept
        {
            return (i >= 0 && i < numLines_) ? lineDelay_[static_cast<std::size_t> (i)] : 0.0;
        }
        [[nodiscard]] Real lineDecayGain (int i) const noexcept
        {
            return (i >= 0 && i < numLines_) ? decayGain_[static_cast<std::size_t> (i)] : 0.0;
        }
        [[nodiscard]] int diffuserDelaySamples (int i) const noexcept
        {
            return (i >= 0 && i < kMaxDiffusers) ? diffusers_[static_cast<std::size_t> (i)].delaySamples() : 0;
        }

        // Total energy STORED in the delay lines — i.e. summed over exactly the D_i most
        // recent samples of each line, which is what the network is holding. This is the
        // instrument the §0.4 "unitary A is lossless" claim is measured with: each sample
        // the network drops one value per line and writes A·(g·h(s)), so with g == 1 and
        // h == identity the two have equal norm and this number is CONSTANT. Meaningful
        // only with modulation off (a modulated read has no integer window).
        [[nodiscard]] Real networkEnergy() const noexcept
        {
            Real e = 0.0;
            for (int i = 0; i < numLines_; ++i)
            {
                const std::size_t base = static_cast<std::size_t> (i) * static_cast<std::size_t> (lineStride_);
                // The stored length is a prime SAMPLE COUNT, i.e. an exact integral
                // double — no rounding needed, and none wanted.
                const int d = static_cast<int> (lineDelay_[static_cast<std::size_t> (i)]);
                for (int k = 1; k <= d; ++k)
                {
                    const Real s = lineBuf_[base + static_cast<std::size_t> ((writePos_ - k) & lineMask_)];
                    e += s * s;
                }
            }
            return e;
        }

        // Largest magnitude anywhere in the stored network state — the instrument the §7
        // denormal claim is measured with. A tail left to decay in silence walks this
        // toward zero; the guard is what stops it arriving.
        [[nodiscard]] Real lineStateMagnitude() const noexcept
        {
            Real m = 0.0;
            for (int i = 0; i < numLines_; ++i)
            {
                const std::size_t base = static_cast<std::size_t> (i) * static_cast<std::size_t> (lineStride_);
                // The stored length is a prime SAMPLE COUNT, i.e. an exact integral
                // double — no rounding needed, and none wanted.
                const int d = static_cast<int> (lineDelay_[static_cast<std::size_t> (i)]);
                for (int k = 1; k <= d; ++k)
                {
                    const Real s = lineBuf_[base + static_cast<std::size_t> ((writePos_ - k) & lineMask_)];
                    const Real a = s < 0.0 ? -s : s;
                    if (a > m)
                        m = a;
                }
            }
            return m;
        }

    private:
        // §0.1 fractional read, linear. Called BEFORE `writePos_` advances, so the oldest
        // in-range slot is `writePos_` itself and a delay of >= 2 keeps the upper
        // neighbour on an already-written sample.
        [[nodiscard]] Real readLine (int i, Real delaySamples) const noexcept
        {
            const Real rp = static_cast<Real> (writePos_) + static_cast<Real> (lineSize_) - delaySamples;
            const int idx = static_cast<int> (rp);
            const Real f = rp - static_cast<Real> (idx);
            const std::size_t base = static_cast<std::size_t> (i) * static_cast<std::size_t> (lineStride_);
            const Real a = lineBuf_[base + static_cast<std::size_t> (idx & lineMask_)];
            const Real b = lineBuf_[base + static_cast<std::size_t> ((idx + 1) & lineMask_)];
            return a + f * (b - a);
        }

        // §0.5 pre-delay: write first, then read, so a delay of exactly 0 returns the
        // sample just written — bit-exact, not "almost".
        Real preDelay (Real x) noexcept
        {
            if (preSize_ <= 0)
                return x;

            preBuf_[static_cast<std::size_t> (prePos_)] = x;
            const Real rp = static_cast<Real> (prePos_) + static_cast<Real> (preSize_) - preDelaySamples_;
            const int idx = static_cast<int> (rp);
            const Real f = rp - static_cast<Real> (idx);
            const Real a = preBuf_[static_cast<std::size_t> (idx & preMask_)];
            const Real b = preBuf_[static_cast<std::size_t> ((idx + 1) & preMask_)];
            prePos_ = (prePos_ + 1) & preMask_;
            return a + f * (b - a);
        }

        // The only place this class calls libm. Lives in the .cpp.
        void resolveControls() noexcept;

        Real sampleRate_ = 48000.0;
        int numLines_ = 0;
        Mixing mixing_ = Mixing::Hadamard;

        int lineSize_ = 0; // per-line ring length, a power of two
        int lineStride_ = 0; // FIREFORK B12: the distance between the lines in lineBuf_
        int lineMask_ = 0;
        int writePos_ = 0;
        Real maxLineDelay_ = 0.0;

        std::vector<Real> lineBuf_; // numLines_ × lineSize_, contiguous
        std::array<Real, kMaxLines> requestedDelay_ {};
        std::array<Real, kMaxLines> lineDelay_ {};
        std::array<Real, kMaxLines> decayGain_ {};
        std::array<Real, kMaxLines> dampState_ {};
        std::array<Real, kMaxLines> dcState_ {};
        std::array<Real, kMaxLines> dcPrev_ {};
        std::array<Real, kMaxLines> lfoU_ {};
        std::array<Real, kMaxLines> lfoV_ {};

        std::array<Allpass, kMaxDiffusers> diffusers_ {};
        std::array<Real, kMaxDiffusers> diffuserMs_ {};
        int diffuserCount_ = 0;
        Real diffusion_ = 0.7; // §2.2's g

        std::vector<Real> preBuf_;
        int preSize_ = 0;
        int preMask_ = 0;
        int prePos_ = 0;
        Real preDelaySamples_ = 0.0;
        Real preDelayMs_ = 0.0;
        Real maxPreDelaySamples_ = 0.0;

        // Requested control values.
        Real rt60_ = 2.0;
        Real dampingHz_ = 0.0;
        Real inLpHz_ = 0.0;
        Real inHpHz_ = 0.0;
        Real modRateHz_ = 0.0;
        Real modDepth_ = 0.0;
        bool dcBlockOn_ = true;
        bool tonalCorrectionOn_ = true;
        bool dirty_ = true;

        // Resolved coefficients.
        Real dampCoef_ = 0.0;
        bool dampActive_ = false;
        Real inLpCoef_ = 0.0;
        bool inLpActive_ = false;
        Real inHpCoef_ = 0.0;
        bool inHpActive_ = false;
        Real lfoCos_ = 1.0;
        Real lfoSin_ = 0.0;
        bool modActive_ = false;
        Real tcB_ = 0.0;
        Real tcGain_ = 1.0;
        bool tcActive_ = false;
        Real inputGain_ = 1.0; // b_i = 1/sqrt(M): an impulse injects unit ENERGY
        Real tapGain_ = 1.0; // c_i = sqrt(2/M) over half the lines: level-neutral

        // Output-side state.
        Real inLpState_ = 0.0;
        Real inHpState_ = 0.0;
        Real tcPrevL_ = 0.0;
        Real tcPrevR_ = 0.0;
        Real denorm_ = kDenormalGuard;

        int chunkCountdown_ = 0;
    };
}
