#pragma once

#include <nemesis/dsp/Real.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace nemesis::dsp
{
    // Drifter — one member of the shared-spine DRIFTER (T-MOD-01; promoted from AETHER's private
    // `p.drifter`, AETHER §0.1 "candidates for §0 promotion" / §4). A pure per-sample control
    // GENERATOR, like Lfo: processSample() takes no audio and returns a bipolar value in
    // [-depth, +depth]. It is NOT an LFO — nothing loops. It is a smooth, BOUNDED random walk that
    // keeps wandering and does not repeat within any musical horizon.
    //
    // THE LAW, and why it is this one. The card names "one-pole-filtered noise / Ornstein–Uhlenbeck";
    // in discrete time those are the same object, and the choice that matters is how the noise enters:
    //
    //   target[n] = v0 + (v1 - v0)·phase   // seeded value noise: random targets in [-1, 1) linearly
    //                                      // interpolated across one segment (the ConverterDrift idiom)
    //   walk[n]   = walk[n-1] + a·(target[n] - walk[n-1])   // one-pole smoother, a ∈ [0, 1]
    //   out[n]    = depth · walk[n]
    //
    // Both lines are CONVEX COMBINATIONS of values already inside [-1, 1], so |walk| ≤ 1 is a
    // STRUCTURAL property — no clamp, no statistics, no "typically". A naive Euler-integrated walk
    // (x += σ·ξ) has variance growing without bound and would need one; the test carries that as an
    // explicit control. Feeding the smoother with VALUE noise rather than white noise is the other
    // deliberate choice: a one-pole over white noise has a steady-state amplitude of √((1-a)/(1+a)),
    // i.e. it goes silent as the rate falls, which would ship a modulation source that does nothing
    // at the slow settings the feature exists for. Value noise is full-scale at every rate.
    //
    // Rate is the walk's characteristic rate in Hz: it sets both the segment length (fs/rate) and the
    // smoother's corner (≈ rate). Rate 0 holds the current value (as Lfo). Both derive from the same
    // per-sample increment, so the whole primitive is division/multiply/compare — no libm even at
    // control rate, hence header-only.
    //
    // DETERMINISM / BOUNCE-PARITY (DSP-03), stated because a drifter that quietly breaks it is the
    // kind of defect only a render shows. The state advances ONCE PER PROCESSED SAMPLE from a seeded
    // LCG and is returned to a defined state by reset() (which prepare() calls). It reads no clock and
    // no host playhead — none exists to read (see process/DEFERRED.md, HOST-PLAYHEAD) — so the
    // trajectory is a pure function of (seed, samples since reset) and is therefore identical however
    // the render is CHUNKED into buffers, which is the way a bounce actually differs from a live take.
    // What that buys and what it does not: repeated offline renders from the same reset point are
    // bit-identical (the property DSP-03 is about), while a take captured mid-session and a bounce of
    // it start from different points in the walk, exactly as the shipped free-running Lfo does.
    // Anchoring the walk to a TRANSPORT POSITION would close that too and is deferred with the
    // host-playhead read it requires; the hook is already here — it is reset().
    //
    // RT-safety (§11): no allocation, no libm, no locks, no branches beyond a wrap loop. Computed in
    // double, emitted as float. No JUCE. Not thread-safe against prepare().
    //
    // NOT `ConverterDrift` (a lo-fi converter-artefact PROCESSOR, CATHODE §2) and not the matrix's
    // `Source::ConvDriftReadout` (that processor's readout, live since T-CGUI-24 — named `Drift`
    // until T-MOD-01 leaf B renamed it to end exactly this collision). This is a general modulation
    // GENERATOR; leaf B wires it as `Source::Drifter1..3` off the engine-global bank.
    class Drifter
    {
    public:
        // Caches the sample rate, resolves the increment + smoothing coefficient, and resets.
        // Allocation-free (there is nothing to allocate), but call it like prepareToPlay.
        void prepare (Wide sampleRate) noexcept
        {
            fs_ = sampleRate > 0.0 ? sampleRate : 48000.0;
            updateCoeffs();
            reset();
        }

        // Returns the walk to its defined start state and RESEEDS. This is the bounce-parity anchor:
        // everything after it is a pure function of the processed-sample count. RT-safe.
        void reset() noexcept
        {
            state_ = baseSeed_;
            phase_ = 0.0;
            v0_ = lcg (state_);
            v1_ = lcg (state_);
            walk_ = v0_; // start ON the first target — no audible ramp-in from 0 after a reset
        }

        // The walk's characteristic rate in Hz. Clamped ≥ 0 (0 = hold). Control-rate.
        void setRateHz (float hz) noexcept
        {
            rateHz_ = hz > 0.0f ? hz : 0.0f;
            updateCoeffs();
        }

        [[nodiscard]] float rateHz() const noexcept { return rateHz_; }

        // Output depth in [0, 1] — a PURE output multiply that does not perturb the walk (so a
        // depth setting can never steer the bank's cross-mod web). Control-rate.
        void setDepth (float d) noexcept
        {
            depth_ = d < 0.0f ? 0.0 : (d > 1.0f ? 1.0 : static_cast<Real> (d));
        }

        [[nodiscard]] float depth() const noexcept { return static_cast<float> (depth_); }

        // Base RNG seed. Reseeds and resets, so a render from here is reproducible.
        void setSeed (std::uint32_t seed) noexcept
        {
            baseSeed_ = seed;
            reset();
        }

        [[nodiscard]] std::uint32_t seed() const noexcept { return baseSeed_; }

        // The pre-depth walk value in [-1, 1] — this sample's, not the next one's. DrifterBank uses
        // it as the cross-mod signal; a test uses it to show depth left the trajectory alone.
        [[nodiscard]] Real walk() const noexcept { return walk_; }

        // This sample's output, without advancing.
        [[nodiscard]] float value() const noexcept { return static_cast<float> (depth_ * walk_); }

        // Returns THIS sample's value, then advances one sample (read-then-advance, as Lfo /
        // AdsrEnvelope). The two factors are DrifterBank's cross-mod; both are exactly 1.0 for a
        // free-standing drifter, and ×1.0 is exact in IEEE-754, so an unwebbed member is bit-
        // identical to a standalone one. rateFactor scales the increment; depthFactor scales the
        // output only — NEITHER touches the state recursion's gain, which is why no cross-mod
        // topology built on this can diverge.
        float processSample (Wide rateFactor = 1.0, Real depthFactor = 1.0) noexcept
        {
            const Real out = depth_ * depthFactor * walk_;

            phase_ += inc_ * rateFactor;
            // A while-loop wrap stays correct even at inc ≥ 1 (one draw per segment crossed).
            while (phase_ >= 1.0)
            {
                phase_ -= 1.0;
                v0_ = v1_;
                v1_ = lcg (state_);
            }

            const Real target = v0_ + (v1_ - v0_) * phase_; // convex → |target| ≤ 1
            walk_ += a_ * (target - walk_); // convex (a ∈ [0,1]) → |walk| ≤ 1

            return static_cast<float> (out);
        }

    private:
        void updateCoeffs() noexcept
        {
            inc_ = static_cast<Real> (rateHz_) / fs_;
            // One-pole corner ≈ the walk rate. 2π·inc is the small-angle form of 1 − exp(−2π·fc/fs);
            // the min() both keeps the coefficient inside [0, 1] (the convexity the bound rests on)
            // and keeps this primitive libm-free.
            const Real a = kSmoothing * inc_;
            a_ = a > 1.0 ? 1.0 : a;
        }

        // Deterministic LCG in [-1, 1) — the same generator as Lfo / ConverterDrift / BbdDelayLine.
        static Real lcg (std::uint32_t& s) noexcept
        {
            s = s * 1664525u + 1013904223u;
            return static_cast<Real> (static_cast<std::int32_t> (s)) * (1.0 / 2147483648.0);
        }

        static constexpr Real kSmoothing = 6.283185307179586; // 2π
        static constexpr std::uint32_t kDefaultSeed = 0x1357'9BDFu;

        // Control-rate configuration + derived coefficients.
        Wide fs_ = 48000.0;
        float rateHz_ = 1.0f;
        Real depth_ = 1.0;
        std::uint32_t baseSeed_ = kDefaultSeed;
        Wide inc_ = 1.0 / 48000.0;
        Real a_ = kSmoothing / 48000.0;

        // Per-sample state.
        Wide phase_ = 0.0; // position within the current value-noise segment, [0, 1)
        Real v0_ = 0.0, v1_ = 0.0; // the segment's start/end random targets
        Real walk_ = 0.0; // the smoothed walk — the output, pre-depth
        std::uint32_t state_ = kDefaultSeed;
    };

    // DrifterBank — the three-member RING (AETHER §4's "sparse cross-mod web where drifters modulate
    // each other's rate and depth", at the smallest topology that is still a web): member i nudges
    // member (i+1) % 3, and nothing else. Not a dense 3×3 matrix — sparse to stay cheap and provably
    // stable, per the spec's own instruction.
    //
    // ENGINE-GLOBAL, one instance per instrument, NOT one per voice: the matrix header describes its
    // random source as "the synth's own random/drift" (singular), and the wiring precedent is
    // ModWheel / KeyTrack — computed once and fed identically into every active voice's matrix. Tick
    // it ONCE PER SAMPLE (the state must advance on the sample grid for the rate law and bounce-
    // parity to mean anything) and broadcast value(0..2).
    //
    // Why the ring cannot run away, structurally rather than by measurement: the cross-mod signals
    // are the predecessors' RAW walk values, and they enter only (a) the phase increment and (b) an
    // output scale in [1-crossDepth, 1]. Neither is inside the state recursion, whose bound is the
    // convex-combination argument in Drifter above and holds whatever the factors do. Using the RAW
    // walk rather than the depth-scaled output also keeps every member's trajectory independent of
    // every member's depth — so muting a member does not silently re-shape the other two.
    class DrifterBank
    {
    public:
        static constexpr std::size_t kNumDrifters = 3;

        // The rate-factor range is [1 - crossRate, 1 + crossRate]; capping crossRate below 1 keeps
        // the factor strictly positive, so a member's walk can be slowed but never frozen or run
        // backwards.
        static constexpr float kMaxCrossRate = 0.95f;

        // Each member draws from its own XOR-decorrelated stream off the one bank seed (the M1-18f
        // rule). Exposed so a test can build the reference drifters the ring must equal when the
        // cross-mod is off.
        static constexpr std::uint32_t streamSeed (std::uint32_t seed, std::size_t index) noexcept
        {
            constexpr std::array<std::uint32_t, kNumDrifters> kSalt { 0x0000'0000u, 0x9E37'79B9u, 0x85EB'CA6Bu };
            return seed ^ kSalt[index % kNumDrifters];
        }

        void prepare (Wide sampleRate) noexcept
        {
            for (auto& d : d_)
                d.prepare (sampleRate);
            refreshOutputs();
        }

        void reset() noexcept
        {
            for (auto& d : d_)
                d.reset();
            refreshOutputs();
        }

        // Reseeds every member from one bank seed. Resets (setSeed does), so a render from here is
        // reproducible.
        void setSeed (std::uint32_t seed) noexcept
        {
            baseSeed_ = seed;
            for (std::size_t i = 0; i < kNumDrifters; ++i)
                d_[i].setSeed (streamSeed (seed, i));
            refreshOutputs();
        }

        [[nodiscard]] std::uint32_t seed() const noexcept { return baseSeed_; }

        // Per-member rate/depth. An out-of-range index is REFUSED (the L2 wiring leaf reaches these
        // from a registry row, so the index is a trust boundary).
        void setRateHz (std::size_t i, float hz) noexcept
        {
            if (i < kNumDrifters)
                d_[i].setRateHz (hz);
        }

        void setDepth (std::size_t i, float depth) noexcept
        {
            if (i < kNumDrifters)
                d_[i].setDepth (depth);
        }

        [[nodiscard]] float rateHzOf (std::size_t i) const noexcept
        {
            return i < kNumDrifters ? d_[i].rateHz() : 0.0f;
        }

        [[nodiscard]] float depthOf (std::size_t i) const noexcept
        {
            return i < kNumDrifters ? d_[i].depth() : 0.0f;
        }

        // Cross-mod amounts, shared by the whole ring. 0 = no web (bit-exactly three independent
        // drifters — the neutral anchor).
        void setCrossRate (float amount) noexcept
        {
            crossRate_ = amount < 0.0f ? 0.0f : (amount > kMaxCrossRate ? kMaxCrossRate : amount);
        }

        void setCrossDepth (float amount) noexcept
        {
            crossDepth_ = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
        }

        [[nodiscard]] float crossRate() const noexcept { return crossRate_; }
        [[nodiscard]] float crossDepth() const noexcept { return crossDepth_; }

        // Advances the whole ring one sample. The predecessors' walk values are read BEFORE any
        // member advances, so the three updates are simultaneous and the ring has no algebraic loop
        // and no index-order bias.
        void processSample() noexcept
        {
            const std::array<Real, kNumDrifters> m { d_[0].walk(), d_[1].walk(), d_[2].walk() };

            for (std::size_t i = 0; i < kNumDrifters; ++i)
            {
                const Real in = m[(i + kNumDrifters - 1) % kNumDrifters]; // member i-1 drives i
                out_[i] = d_[i].processSample (rateFactor (in), depthFactor (in));
            }
        }

        // The value this member contributed on the last processSample() — what the engine broadcasts
        // into each voice's mod matrix. Out-of-range reads 0.
        [[nodiscard]] float value (std::size_t i) const noexcept
        {
            return i < kNumDrifters ? out_[i] : 0.0f;
        }

    private:
        // Both factors are EXACTLY 1.0 at their zero amount (0.0f · anything is 0, and 1 ± 0 is 1),
        // which is what makes "no cross-mod" bit-exact rather than merely close.
        [[nodiscard]] Wide rateFactor (Real m) const noexcept
        {
            return 1.0 + static_cast<Real> (crossRate_) * m;
        }

        // A DUCK, not a boost: the factor spans [1 - crossDepth, 1], so a member's output can never
        // exceed the depth its own knob dials and the |out| ≤ depth bound survives the web.
        [[nodiscard]] Real depthFactor (Real m) const noexcept
        {
            return 1.0 - static_cast<Real> (crossDepth_) * (1.0 - m) * 0.5;
        }

        void refreshOutputs() noexcept
        {
            for (std::size_t i = 0; i < kNumDrifters; ++i)
                out_[i] = d_[i].value();
        }

        std::array<Drifter, kNumDrifters> d_ {};
        std::array<float, kNumDrifters> out_ {};
        std::uint32_t baseSeed_ = 0x1357'9BDFu;
        float crossRate_ = 0.0f;
        float crossDepth_ = 0.0f;
    };
}
