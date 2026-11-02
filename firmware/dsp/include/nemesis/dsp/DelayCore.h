#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/Waveshaper.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nemesis::dsp
{
    // [T-CMP-08 leaf A] THE SHARED DELAY PRIMITIVE — `specs/dsp/delay.md` §0, mono, and
    // nothing above it. §0 exists precisely because all six delay TYPES are these
    // primitives plus one signature stage, so this class owns the parts every type needs
    // and owns no type's identity:
    //
    //   * §0.2  a power-of-2 ring read at a FRACTIONAL delay, with the interpolator
    //           SELECTABLE — Lagrange-3 (flat top end, the Digital type's "pristine
    //           repeats") or linear (cheap, mildly lowpass; what Analog/Tape want, since
    //           they are dark anyway). The spec calls this "the single most important
    //           design axis"; a primitive that hardcoded one of them would have decided
    //           three unbuilt types' character.
    //   * §0.3  BOTH time-change behaviours, also selectable — equal-power CROSSFADE
    //           between two read pointers (no pitch artifact: the repeats just relocate)
    //           or VARISPEED (a one-pole-smoothed time parameter, so the moving read
    //           pointer Doppler-glides the buffered audio). §7 states the axis outright:
    //           "getting this one axis right is 80 % of why each type feels correct."
    //   * §0.4  the feedback path in the spec's own order — DC blocker (R ≈ 0.995) →
    //           optional gentle HP → tone LP → soft-clip → × fb → sum into write.
    //
    // NOT here, on purpose: stereo, tempo sync (§0.1), LFO modulation of the delay length
    // (§0.5), ducking (§0.6), wet/dry mix, and every per-type signature stage (BBD
    // companding, tape wow/flutter + heads, ping-pong cross-feed). This is the mono core a
    // stereo wrapper instantiates twice and a 100 %-wet consumer (the T-FX-04 phaser's JMJ
    // stereo mode) calls directly.
    //
    // TOPOLOGY, and why the OUTPUT is the raw read. The §0.4 filters sit in the FEEDBACK
    // path, not in series with the output tap, so at `fb == 0` the wet output is the
    // delayed input and nothing else — a real bit-exact identity, not a "nearly clean"
    // one. The whole feedback chain is then skipped by an early return, which is what
    // makes that identity true rather than merely approximate.
    //
    // RT-safety: `prepare` allocates the ring once. Every transcendental (the one-pole
    // coefficients, the glide constant) is resolved in `resolveControls`, called once per
    // `kControlChunk` samples from the sample loop — the `ChorusEffect` idiom, so a leaf
    // that wraps this class wraps a shape it already knows. The per-sample path is pure
    // arithmetic plus, DURING A CROSSFADE ONLY, two `sqrt`s (a single arm64 `fsqrt`, the
    // sanctioned category — see `BbdDelayLine`'s compressor). No allocation, no locks, no
    // JUCE. Deterministic: same input + same param snapshot → bit-identical output.
    // Computed in double internally, emitted as float. Not thread-safe against `prepare`.
    class DelayCore
    {
    public:
        // §0.2's two interpolators. The choice is a TYPE's decision, not this class's.
        enum class Interp : std::uint8_t {
            Lagrange3 = 0, // 4-point cubic — flat top end, Digital/Ping-pong/Multi-tap
            Linear = 1 // 2-point — cheap and mildly dark, Analog/Tape
        };

        // §0.3's two time-change behaviours. Digital defaults to Crossfade (§1.2 "that's
        // the digital expectation"); Tape/Analog will select Varispeed (§3.1 calls it
        // "mandatory, not optional").
        enum class TimeMode : std::uint8_t {
            Crossfade = 0,
            Varispeed = 1
        };

        // §1.3's Digital feedback ceiling. Tape's §3.3 goes to 1.1 (runaway self-osc) and
        // Analog's §2.4 to 1.0; those are THEIR leaves' ceilings, so this clamp is stated
        // as the Digital one rather than as a universal law.
        static constexpr Real kMaxFeedback = 0.95;

        // The Lagrange-3 stencil reads i-1 … i+2, i.e. up to two samples NEWER than the
        // integer part of the read position, so the shortest honest delay is 2 samples.
        static constexpr Real kMinDelaySamples = 2.0;

        // §7 buffer sizing: `max_time + mod_depth + interp_margin`, rounded to a power of
        // two for masked indexing. The mod-depth reserve is sized for §1.3's 0–10 ms
        // chorus modulation even though leaf A builds no LFO — a later leaf adding one
        // must not have to re-`prepare` a line that is already running.
        static constexpr Real kModDepthMarginMs = 10.0;
        static constexpr int kInterpMarginSamples = 4;

        // How often the control-rate parameters are re-resolved. 16 samples is ~3 kHz at
        // 48 kHz — far above any parameter gesture, and far inside the 10–30 ms crossfade.
        static constexpr int kControlChunk = 16;

        // §0.4's DC-blocker pole.
        static constexpr Real kDcBlockerR = 0.995;

        // §7 "Denormals": a long feedback tail trickles toward the denormal range and
        // stalls the FPU. The injected term ALTERNATES sign so it is a Nyquist-rate
        // signal rather than a DC offset — a DC injection is exactly what the DC blocker
        // one line below would integrate away, leaving the guard doing nothing.
        static constexpr Real kDenormalGuard = 1.0e-20;

        // Allocates the ring for delays up to `maxDelayMs` (plus the §7 margins) and
        // clears state. Not RT-safe — call like prepareToPlay.
        void prepare (Real sampleRate, Real maxDelayMs);

        // Silences the line, clears every filter state and SNAPS the read pointer to the
        // requested delay (no start-up fade or glide), then re-resolves the coefficients.
        void reset() noexcept;
        // [S5R B31 F3] `reset` less zeroing the line memory: for a caller that KNOWS the line is still empty (nothing has
        // run since a reset) and only owes the snap to settings pushed since. Alloc-free, no memset.
        void snap() noexcept;

        // --- control-rate setters -------------------------------------------------
        // These STORE and clamp; the transcendental math they imply happens in
        // `resolveControls` at the next chunk boundary, so a caller may push all of them
        // in one go without paying an `exp` per setter.

        // Target delay in (fractional) samples, clamped to [kMinDelaySamples, max].
        // What happens NEXT depends on `setTimeMode`: a crossfade to a second read
        // pointer, or a glide of the single one.
        void setDelaySamples (Real samples) noexcept;
        void setDelayMs (Real ms) noexcept { setDelaySamples (ms * sampleRate_ * 0.001); }

        // Loop gain, clamped to [0, kMaxFeedback]. Exactly 0 takes the early return that
        // makes the output bit-exactly the delayed input.
        void setFeedback (Real fb) noexcept
        {
            fbGain_ = fb < 0.0 ? 0.0 : (fb > kMaxFeedback ? kMaxFeedback : fb);
            fbActive_ = fbGain_ > 0.0;
        }

        // §1.3 "Tone LP 1–20 kHz, default 20 k, off = pristine" — the loop lowpass that
        // makes each repeat progressively darker. `hz <= 0` is OFF (bypassed, not a
        // near-Nyquist pole), which is what "pristine" has to mean if it is to be exact.
        void setToneLpHz (Real hz) noexcept
        {
            toneLpHz_ = hz;
            coeffsDirty_ = true;
        }

        // §1.2's OPTIONAL gentle HP (§1.3: 20–500 Hz, default 30) so lows don't build up
        // over many repeats. `hz <= 0` is OFF. It is optional in the spec and therefore
        // bypassable here — which is also what leaves the DC blocker as the sole DC guard
        // in the one configuration where that claim can be measured.
        void setToneHpHz (Real hz) noexcept
        {
            toneHpHz_ = hz;
            coeffsDirty_ = true;
        }

        void setInterp (Interp mode) noexcept { interp_ = mode; }
        void setTimeMode (TimeMode mode) noexcept { timeMode_ = mode; }

        // §0.3 #1's fade length — the spec's ~10–30 ms. Clamped to [1 sample, the
        // ring's mod-depth reserve].
        void setCrossfadeMs (Real ms) noexcept
        {
            crossfadeMs_ = ms;
            coeffsDirty_ = true;
        }

        // §0.3 #2 / §7 "for varispeed types the time-smoothing time-constant IS the glide
        // speed — expose it". One-pole time constant in ms.
        void setGlideMs (Real ms) noexcept
        {
            glideMs_ = ms;
            coeffsDirty_ = true;
        }

        // The two §0.4 GUARDS, individually defeatable. Both default ON and both are
        // product settings as much as test hooks: a later type may want a clean digital
        // loop with no saturation. Their reason for existing as switches is that each
        // guard's claim is only measurable against a render with that guard removed —
        // "the loop stayed bounded" is not evidence about the DC blocker if the soft-clip
        // was also holding it down.
        void setDcBlockerEnabled (bool on) noexcept { dcBlockerOn_ = on; }
        void setSoftClipEnabled (bool on) noexcept { softClipOn_ = on; }

        // --- audio ----------------------------------------------------------------

        // One sample in, the WET (100 % delayed) sample out. Inline, alloc-free.
        float processSample (float x) noexcept
        {
            if (chunkCountdown_ <= 0)
            {
                resolveControls();
                chunkCountdown_ = kControlChunk;
            }
            --chunkCountdown_;

            // Write first, so a 2-sample delay can still read the sample just written.
            const Real fbIn = fbActive_ ? fbGain_ * fbState_ : 0.0;
            buf_[static_cast<std::size_t> (writePos_)] = static_cast<Real> (x) + fbIn;

            Real y;
            if (timeMode_ == TimeMode::Varispeed)
            {
                // §0.3 #2: the pointer CHASES the target, and the chase itself is the
                // Doppler pitch shift. No second pointer, no fade.
                current_ += glideCoef_ * (requested_ - current_);
                y = readAt (current_);
            }
            else if (fading_)
            {
                // §0.3 #1, equal power. sqrt(t) / sqrt(1-t) is exactly equal-power
                // (gA² + gB² == 1) and needs no table and no libm — one `fsqrt` each,
                // and only while a fade is in flight.
                const Real t = static_cast<Real> (fadePos_) / static_cast<Real> (fadeLen_);
                const Real gB = std::sqrt (t);
                const Real gA = std::sqrt (1.0 - t);
                y = gA * readAt (fadeFrom_) + gB * readAt (fadeTo_);
                if (++fadePos_ >= fadeLen_)
                {
                    current_ = fadeTo_;
                    fading_ = false;
                }
            }
            else
            {
                y = readAt (current_);
            }

            writePos_ = (writePos_ + 1) & mask_;

            // §0.4, in the spec's order. Skipped WHOLE at fb == 0 — that early return is
            // what makes the bypass bit-exact rather than "clean to within a filter".
            if (fbActive_)
            {
                Real v = y + denorm_;
                denorm_ = -denorm_;

                if (dcBlockerOn_)
                {
                    const Real o = v - dcPrev_ + kDcBlockerR * dcState_;
                    dcPrev_ = v;
                    dcState_ = o;
                    v = o;
                }
                if (hpActive_)
                {
                    hpState_ += hpCoef_ * (v - hpState_);
                    v -= hpState_;
                }
                if (lpActive_)
                {
                    lpState_ += lpCoef_ * (v - lpState_);
                    v = lpState_;
                }
                if (softClipOn_)
                    v = static_cast<Real> (clip_.processSample (static_cast<float> (v)));

                fbState_ = v;
            }

            return static_cast<float> (y);
        }

        // `in` and `out` may alias (in-place is the 100 %-wet consumer's case).
        void processBlock (const float* in, float* out, int numSamples) noexcept
        {
            if (in == nullptr || out == nullptr || numSamples <= 0 || size_ <= 0)
                return;
            for (int n = 0; n < numSamples; ++n)
                out[n] = processSample (in[n]);
        }

        // --- the MULTI-TAP pair (§5.1/§5.2) ----------------------------------------
        //
        // "Taps are reads, not separate buffers — all taps share one delay line, so it's
        // cheap to have 4–16 taps" (§5.2). These two are that sentence: `write` puts a
        // sample on the line and advances, and `readTapSamples` reads it back at ANY
        // delay, as many times per sample as the caller likes.
        //
        // A multi-tap consumer uses this pair INSTEAD of `processSample`, not alongside
        // it, because everything `processSample` adds above the ring is single-tap by
        // construction: one §0.3 read pointer (§5 needs K, each with its own time-change
        // state) and one §0.4 loop (§5.2 closes the loop from ONE designated tap, above
        // the line). K `DelayCore`s would be K buffers, which is exactly what §5.2
        // declines. `MultiTapCore` is the consumer.
        void write (float x) noexcept
        {
            buf_[static_cast<std::size_t> (writePos_)] = static_cast<Real> (x);
            writePos_ = (writePos_ + 1) & mask_;
        }

        // The line `delaySamples` before the NEWEST written sample — the same frame
        // `setDelaySamples` states its argument in, which is why the +1 is here rather
        // than at four call sites: `write` has already advanced past the sample delay 0
        // names. Clamped to what the ring can serve, and floored at `kMinDelaySamples`
        // for the same reason `processSample`'s own read is — the cubic stencil reaches
        // two samples NEWER than the read point, and past the newest is unwritten.
        [[nodiscard]] Real readTapSamples (Real delaySamples) const noexcept
        {
            const Real d = delaySamples < kMinDelaySamples
                                 ? kMinDelaySamples
                                 : (delaySamples > maxDelay_ ? maxDelay_ : delaySamples);
            return readAt (d + 1.0);
        }

        // --- read-backs (tests, latency reporting, a surface's display) -------------

        // The delay the line is ACTUALLY reading at. During a crossfade this is still the
        // OUTGOING pointer — the line is reading two places at once and neither is "the"
        // delay, which `isCrossfading` is how a caller finds out.
        [[nodiscard]] Real currentDelaySamples() const noexcept { return current_; }
        [[nodiscard]] Real requestedDelaySamples() const noexcept { return requested_; }
        [[nodiscard]] Real maxDelaySamples() const noexcept { return maxDelay_; }
        [[nodiscard]] bool isCrossfading() const noexcept { return fading_; }
        [[nodiscard]] Real feedback() const noexcept { return fbGain_; }
        [[nodiscard]] Interp interp() const noexcept { return interp_; }
        [[nodiscard]] TimeMode timeMode() const noexcept { return timeMode_; }

        // Largest magnitude across the feedback loop's own state — the instrument the §7
        // denormal claim is measured with. A loop left to decay in silence walks every
        // one of these toward zero; the guard is what stops them arriving.
        [[nodiscard]] Real loopStateMagnitude() const noexcept
        {
            auto mag = [] (Real v) noexcept { return v < 0.0 ? -v : v; };
            Real m = mag (fbState_);
            m = mag (dcState_) > m ? mag (dcState_) : m;
            m = mag (hpState_) > m ? mag (hpState_) : m;
            m = mag (lpState_) > m ? mag (lpState_) : m;
            return m;
        }

    private:
        // Reads the ring at a fractional delay, with the selected §0.2 interpolator.
        // Called BEFORE `writePos_` advances, so the just-written sample is in range.
        [[nodiscard]] Real readAt (Real delaySamples) const noexcept
        {
            const Real rp = static_cast<Real> (writePos_) + static_cast<Real> (size_) - delaySamples;
            const int i = static_cast<int> (rp); // rp > 0 by construction → trunc == floor
            const Real d = rp - static_cast<Real> (i);

            if (interp_ == Interp::Linear)
            {
                const Real a = buf_[static_cast<std::size_t> (i & mask_)];
                const Real b = buf_[static_cast<std::size_t> ((i + 1) & mask_)];
                return a + d * (b - a);
            }

            // 4-point (cubic) Lagrange over i-1, i, i+1, i+2. At d == 0 the weights are
            // exactly (0, 1, 0, 0), so an integer delay is a BIT-EXACT tap — that is what
            // lets the fb == 0 identity be an equality rather than a tolerance.
            const Real xm1 = buf_[static_cast<std::size_t> ((i - 1) & mask_)];
            const Real x0 = buf_[static_cast<std::size_t> (i & mask_)];
            const Real x1 = buf_[static_cast<std::size_t> ((i + 1) & mask_)];
            const Real x2 = buf_[static_cast<std::size_t> ((i + 2) & mask_)];

            const Real dm1 = d - 1.0;
            const Real dm2 = d - 2.0;
            const Real dp1 = d + 1.0;
            const Real cm1 = -d * dm1 * dm2 * (1.0 / 6.0);
            const Real c0 = dp1 * dm1 * dm2 * 0.5;
            const Real c1 = -dp1 * d * dm2 * 0.5;
            const Real c2 = dp1 * d * dm1 * (1.0 / 6.0);

            return (cm1 * xm1 + c0 * x0) + (c1 * x1 + c2 * x2);
        }

        // Resolves everything transcendental, plus the §0.3 hand-over from a requested
        // delay to whatever the current time mode does about it. Called once per
        // kControlChunk from the sample loop. Lives in the .cpp — it is the only place
        // this class calls libm.
        void resolveControls() noexcept;

        Real sampleRate_ = 48000.0;
        int size_ = 0; // ring length, a power of two
        int mask_ = 0; // size_ - 1
        Real maxDelay_ = 0.0; // the largest delay the ring can serve
        int writePos_ = 0;

        Real requested_ = kMinDelaySamples; // what the caller asked for
        Real current_ = kMinDelaySamples; // what the (single / outgoing) pointer reads

        Interp interp_ = Interp::Lagrange3; // §1.1 — the Digital default
        TimeMode timeMode_ = TimeMode::Crossfade; // §1.2 — the Digital default

        // Crossfade state (§0.3 #1). `requested_` doubles as the PENDING target: a time
        // change arriving mid-fade is picked up when the fade lands, so a fast knob
        // sweep neither drops targets nor cuts a fade off part-way.
        bool fading_ = false;
        Real fadeFrom_ = kMinDelaySamples;
        Real fadeTo_ = kMinDelaySamples;
        int fadePos_ = 0;
        int fadeLen_ = 1;

        // Requested control values (clamped + resolved in `resolveControls`).
        Real toneLpHz_ = 20000.0; // §1.3 default
        Real toneHpHz_ = 30.0; // §1.3 default
        Real crossfadeMs_ = 20.0; // mid-range of §0.3's 10–30 ms
        Real glideMs_ = 30.0; // matches BbdDelayLine's varispeed constant
        bool coeffsDirty_ = true;

        // Resolved coefficients.
        Real lpCoef_ = 1.0;
        Real hpCoef_ = 0.0;
        Real glideCoef_ = 1.0;
        bool lpActive_ = true;
        bool hpActive_ = true;

        Real fbGain_ = 0.0;
        bool fbActive_ = false;
        bool dcBlockerOn_ = true;
        bool softClipOn_ = true;

        // Feedback-path state.
        Real fbState_ = 0.0;
        Real dcState_ = 0.0, dcPrev_ = 0.0;
        Real hpState_ = 0.0;
        Real lpState_ = 0.0;
        Real denorm_ = kDenormalGuard;
        AdaaSoftClip clip_;

        std::vector<Real> buf_;

        int chunkCountdown_ = 0;
    };
}
