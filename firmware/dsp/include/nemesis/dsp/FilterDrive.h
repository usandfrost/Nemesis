#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/Oversampler.h>
#include <nemesis/dsp/Waveshaper.h>

#include <algorithm>
#include <array>

namespace nemesis::dsp
{
    // FilterDrive — CATHODE's §3 filter DRIVE stage: "built-in drive (reuse ../dsp/drive.md saturate,
    // oversampled)". A standalone, self-contained saturator built by COMPOSITION of two already-
    // signed-off primitives — the ADAA cubic soft-clip nonlinearity (M1-02 `AdaaSoftClip`, drive.md
    // §0.3 / §6 SATURATE curve) run at OS rate inside the integer oversampler (M1-02b `Oversampler`,
    // drive.md §0.2). One `drive` amount in [0,1] is mapped to a PRE-GAIN that pushes the signal into
    // the fixed soft-clip knee (|x| < 1): more drive -> more of the signal clips -> more (odd)
    // harmonics and stronger program-dependent compression ("glue"). Because the curve saturates,
    // the output stays bounded (~+-2/3) however hard it is driven — a saturator, not a gain.
    //
    // OVERSAMPLED ON PURPOSE (drive.md §0.2, CATHODE §11): a waveshaper makes harmonics above Nyquist
    // that fold back as inharmonic alias fizz, so the analog family is oversampled to suppress it —
    // UNLIKE the broken converter (whose aliasing IS the effect and is deliberately kept at base rate).
    // The ADAA curve additionally anti-aliases within each OS step; together they drop the alias floor
    // well below a naive base-rate clip (the M1-02/M1-02b keystone).
    //
    // `drive = 0` is a REAL bit-exact BYPASS (early return) — the "dial to clean" anchor, at zero OS
    // cost. NOTE the bypass reports 0 latency while an active drive reports the oversampler group delay:
    // a live-modulated drive therefore changes latency. Latency alignment / PDC across a modulated
    // drive is a voice/engine concern (matching the M1-08b free-running-oscillator stance), NOT this
    // primitive's; it just reports `latencySamples()` for the caller to align.
    //
    // The genuinely-new surface over the two composed primitives is only the drive->pre-gain mapping,
    // the bypass, and the wiring — waveshaping is sample-domain (fs-agnostic), so one design / one
    // golden (like M1-02b), no per-sample-rate variants.
    //
    // DEFERRED (stable §9 names): the pre/post-FILTER placement wiring into CathodeVoiceStage (§3
    // "pre or post"; a later fold-in card); the drive.md §6.3 CHARACTER flavours (tape/tube/xfmr),
    // ASYMMETRY, an explicit COMPRESSION knob, TONE-TILT, MIX and output LEVEL/makeup; the FOLD
    // wavefolder (§13.2); the other filter extras (comb/formant/24 dB/model-zoo/keytrack/envAmt).
    //
    // RT-safety: prepare() designs the oversampler FIRs and allocates (off the audio thread, like
    // prepareToPlay). setDrive() resolves the pre-gain at control rate (pure arithmetic, no libm) so
    // the header needs no .cpp. processSample() is inline: the shaper lambda inlines through the
    // Oversampler template (no std::function, zero allocation) and the ADAA hot path is pure
    // polynomial arithmetic. Deterministic (no RNG) -> bit-identical re-render. No JUCE. Not
    // thread-safe against prepare().
    class FilterDrive
    {
    public:
        // Oversampling factor for the saturator. drive.md §0.2/§6: Saturate/Overdrive ~ 2-4x; 4 gives
        // comfortable alias-floor margin (matches the M1-08 PD choice). 1 == no oversampling (naive).
        static constexpr int kDefaultOversample = 4;

        // Designs the up/down FIR cascade for `oversampleFactor` and clears state. Allocates; call off
        // the audio thread. The default drive (0) leaves the stage a bit-exact bypass until dialed.
        void prepare (int oversampleFactor = kDefaultOversample)
        {
            os_.prepare (oversampleFactor);
            reset();
        }

        // Clears the ADAA one-sample memory and the oversampler FIR state. RT-safe.
        void reset() noexcept
        {
            adaa_.reset();
            os_.reset();
        }

        // [stereo voice S2] Takes `other`'s signal STATE — the ADAA memory and the oversampler history —
        // not its drive setting. For a second drive that must continue from what the first one heard
        // (both prepared alike). RT-safe, no allocation.
        void copyStateFrom (const FilterDrive& other) noexcept
        {
            adaa_ = other.adaa_;
            os_.copyStateFrom (other.os_);
        }

        // Drive amount in [0,1] (`synth.filt.drive`): 0 = bit-exact bypass; higher = more pre-gain into
        // the soft-clip knee -> more saturation. Control-rate, pure arithmetic.
        void setDrive (float amount) noexcept
        {
            const float clamped = std::clamp (amount, 0.0f, 1.0f);
            bypass_ = (clamped <= 0.0f);
            preGain_ = preGainFor (clamped);
        }

        // The pre-gain a given drive maps to: unity at 0, linear to (1 + kDriveRange) at 1, clamped.
        // Exposed so tests can build the exact naive reference the oversampled path is measured against.
        [[nodiscard]] static float preGainFor (float amount) noexcept
        {
            return 1.0f + kDriveRange * std::clamp (amount, 0.0f, 1.0f);
        }

        // Oversampling factor (1 == no oversampling).
        [[nodiscard]] int factor() const noexcept { return os_.factor(); }

        // [T-CMP-37 L3] WHAT THE LANE-WISE DRIVE READS FROM A LIVE SCALAR ONE (FilterDriveLanes.h): the pre-gain as `setDrive` left
        // it, the oversampler (delay lines in and out as values) and the two ADAA doubles. The lanes are bit-identical to this
        // class, so a voice's drive may move between the two at any sample and continue exactly. STALE while lane-resident.
        [[nodiscard]] float preGain() const noexcept { return preGain_; }
        [[nodiscard]] Oversampler& oversampler() noexcept { return os_; }
        [[nodiscard]] const Oversampler& oversampler() const noexcept { return os_; }
        [[nodiscard]] std::array<Real, 2> adaaState() const noexcept { return adaa_.state(); }
        void setAdaaState (const std::array<Real, 2>& s) noexcept { adaa_.setState (s); }

        // Base-rate latency: the oversampler group delay when active, 0 when bypassed (drive == 0).
        [[nodiscard]] int latencySamples() const noexcept { return bypass_ ? 0 : os_.latencySamples(); }
        // Whether the drive is in circuit (drive > 0) — its output is then bounded (~+-2/3) whatever goes in.
        [[nodiscard]] bool engaged() const noexcept { return !bypass_; }

        // Processes one base-rate sample. drive == 0 -> exact passthrough (zero OS cost); otherwise
        // upsample -> pre-gain -> ADAA soft clip (per OS sample, so the ADAA memory runs at OS rate)
        // -> downsample. The lambda inlines through the Oversampler template (no allocation).
        float processSample (float x) noexcept
        {
            if (bypass_)
                return x;

            return os_.processSample (
                x, [this] (float u) noexcept { return adaa_.processSample (u * preGain_); });
        }

    private:
        // Pre-gain span: drive 1 pushes a unit-amplitude signal ~ (1 + kDriveRange)x into the |x| < 1
        // knee -> firm clipping. A perceptual (dB) curve is a later refinement; linear keeps the
        // "harmonics grow monotonically with drive" keystone clean and the header libm-free.
        static constexpr float kDriveRange = 6.0f;

        Oversampler os_;
        AdaaSoftClip adaa_;
        float preGain_ = 1.0f;
        bool bypass_ = true; // default drive 0 -> bit-exact bypass
    };
}
