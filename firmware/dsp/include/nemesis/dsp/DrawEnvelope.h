#pragma once

#include <nemesis/dsp/CurveTable.h>

#include <cstdint>

namespace nemesis::dsp
{
    // [T-CGUI-61 leaf E5] DrawEnvelope — a DRAWN, optionally LOOPING envelope: the alternative to
    // AdsrEnvelope that AMP, ENV 2 and ENV 3 select in DRAW mode. A pure 0–1 generator over a baked
    // CurveTable read at `pos` 0 → 1, the whole shape taking `length` seconds.
    //
    //   * gateOn  — plays from pos 0 at 1/length per second.
    //   * held, loop valid — when pos CROSSES loopEnd it wraps to loopStart (the overshoot carried,
    //     so the period is exactly (end − start)·length) and cycles while the gate is held. A loop
    //     set while pos is already past loopEnd does not snap back: only a crossing wraps.
    //   * gateOff with a loop — pos before loopEnd jumps TO loopEnd and plays the tail (loopEnd → 1)
    //     as the release; pos at or past loopEnd just keeps playing.
    //   * no loop — ONE-SHOT: plays 0 → 1 whatever the gate does; gateOff changes nothing.
    //   * pos 1 outside a held loop — holds the curve's last value, and once any de-click has
    //     settled isActive() is false. INACTIVE MEANS every later processSample() returns the same
    //     value (curve(1); 0 if the curve ends at 0) until gateOn()/setCurve()/reset(). A curve that
    //     ends above 0 therefore goes inactive while still outputting it — the owner decides reclaim.
    //
    // DE-CLICK: every discontinuous jump sets an offset that ramps LINEARLY to exactly 0 over
    // kDeclickSeconds; output = clamp01(curve(pos) + offset). The offset starts the ramp at what the
    // envelope was EMITTING — the clamped output — so a curve + offset sum carried above full scale (or
    // below 0) by an earlier ramp is never a phantom starting level (row 303). The jumps:
    //   loop wrap — the SEAM step curve(loopEnd) − curve(loopStart), so equal-valued loop ends add
    //               nothing and the loop continues bit-exact (no offset, no ramp). Only when the loop
    //               period is at least the ramp: a shorter loop wraps raw (see advance());
    //   gateOff   — curve(pos) − curve(loopEnd);
    //   gateOn    — from the current output to curve(0), unless the envelope is inactive AND silent
    //               (then the drawn start is honoured exactly, a non-zero start is a hard attack);
    //   setCurve  — old curve(pos) − new curve(pos). The old value is the CACHED one the envelope was
    //               about to play, so the swap never reads the outgoing table (S5R: a publisher may
    //               already be rewriting it). A table is read only while it is the current one.
    // A new jump while a ramp runs folds the remaining offset in and restarts the ramp.
    //
    // RT-safety: setters and prepare() are control-rate (no libm anywhere; divisions only);
    // processSample/gateOn/gateOff/setCurve are pure arithmetic — no allocation, no locks, no libm.
    // Deterministic. Position is counted in SAMPLES (+1.0 per sample, exact in double), so the
    // sample-n position is exactly n / (length·fs). Computed in double, emitted as float. No JUCE.
    class DrawEnvelope
    {
    public:
        // The de-click ramp time: a jump's offset reaches exactly 0 after round(fs·5 ms) samples
        // (240 at 48 kHz), so a jump of size d steps the output by at most |d|/declickSamples() per
        // sample on top of the curve's own slope.
        static constexpr double kDeclickSeconds = 0.005;
        static constexpr double kMinLengthSeconds = 0.001;
        static constexpr double kMaxLengthSeconds = 600.0;
        static constexpr double kDefaultLengthSeconds = 1.0;

        // Sets the sample rate, re-resolves length/loop/de-click in samples, returns to silent Idle.
        void prepare (double sampleRate) noexcept;

        // Silent Idle: output 0, inactive, gate released, no offset. Keeps curve/length/loop.
        void reset() noexcept
        {
            stage_ = Stage::Idle;
            posS_ = 0.0;
            cur_ = 0.0;
            gate_ = false;
            offset_ = 0.0;
            offsetStep_ = 0.0;
            rampLeft_ = 0;
        }

        // Non-owning; nullptr reads 0 everywhere. While sounding, the swap is de-clicked.
        void setCurve (const CurveTable* curve) noexcept;

        // The time the whole shape (pos 0 → 1) takes, clamped to [kMin, kMax]LengthSeconds (NaN →
        // kMin). A change mid-play keeps pos (0–1) where it is: no jump.
        void setLengthSeconds (double seconds) noexcept;
        void setLengthMs (double ms) noexcept { setLengthSeconds (ms / 1000.0); }

        // Loop markers in pos units, clamped to [0, 1]. Valid only if start < end (NaN is invalid);
        // an invalid pair CLEARS the loop (one-shot). Neither call moves pos.
        void setLoop (double startPos, double endPos) noexcept;
        void clearLoop() noexcept { loopValid_ = false; }

        void gateOn() noexcept;
        void gateOff() noexcept;

        float processSample() noexcept
        {
            const double out = cur_ + offset_;
            if (rampLeft_ > 0)
            {
                --rampLeft_;
                offset_ = rampLeft_ == 0 ? 0.0 : offset_ - offsetStep_; // lands on EXACT 0
            }
            if (stage_ == Stage::Running)
            {
                advance();
                refresh();
            }
            return static_cast<float> (out > 0.0 ? (out < 1.0 ? out : 1.0) : 0.0);
        }

        [[nodiscard]] bool isActive() const noexcept { return stage_ == Stage::Running || rampLeft_ > 0; }

        // Readouts for tests and a playhead: pos (0–1) of the NEXT sample (0 when Idle), the
        // de-click offset that sample will carry, the ramp length, and the loop state.
        [[nodiscard]] double position() const noexcept { return stage_ == Stage::Idle ? 0.0 : posS_ / lenS_; }
        [[nodiscard]] double declickOffset() const noexcept { return offset_; }
        [[nodiscard]] int declickSamples() const noexcept { return declickSamples_; }
        [[nodiscard]] bool hasLoop() const noexcept { return loopValid_; }
        [[nodiscard]] double lengthSeconds() const noexcept { return lengthSec_; }

    private:
        enum class Stage : std::uint8_t {
            Idle, // never triggered since reset: silent
            Running, // pos advancing
            Done // pos held at 1
        };

        [[nodiscard]] double valueAtS (double posS) const noexcept
        {
            return curve_ != nullptr ? curve_->valueAtPos (posS / lenS_) : 0.0;
        }

        // The curve steps from `before` to `after`: the offset restarts its ramp from the OUTPUT the
        // envelope was emitting, clamp01(before + offset), down to the new curve. A zero jump changes
        // nothing, so an equal-valued seam leaves a running ramp (or none) exactly as it was.
        void jump (double before, double after) noexcept
        {
            if (before == after)
                return;
            const double out = before + offset_;
            offset_ = (out > 0.0 ? (out < 1.0 ? out : 1.0) : 0.0) - after;
            offsetStep_ = offset_ / static_cast<double> (declickSamples_);
            rampLeft_ = declickSamples_;
        }

        void advance() noexcept
        {
            const double prev = posS_;
            posS_ += 1.0;
            if (gate_ && loopValid_ && prev < loopEndS_ && posS_ >= loopEndS_)
            {
                posS_ -= loopEndS_ - loopStartS_;
                if (posS_ >= loopEndS_) // a loop shorter than one sample: sit on loopStart
                    posS_ = loopStartS_;
                // A period shorter than the ramp gets NO seam de-click (S5R): each wrap would add a
                // step before the last had ramped out, stacking ≈ declickSamples·d/P and railing the
                // output. Such a loop is an audio-rate wave; its seam is part of the drawn shape.
                if (loopEndS_ - loopStartS_ >= static_cast<double> (declickSamples_))
                    jump (valueAtS (loopEndS_), valueAtS (loopStartS_));
                return;
            }
            if (posS_ >= lenS_)
            {
                posS_ = lenS_;
                stage_ = Stage::Done;
            }
        }

        void resolveSamples() noexcept;

        // Re-reads `cur_` from the CURRENT curve — after anything that moves pos, the length or the curve.
        void refresh() noexcept { cur_ = stage_ == Stage::Idle ? 0.0 : valueAtS (posS_); }

        const CurveTable* curve_ = nullptr;
        double sampleRate_ = 48000.0;
        double lengthSec_ = kDefaultLengthSeconds;
        double lenS_ = kDefaultLengthSeconds * 48000.0;
        double loopStart_ = 0.0;
        double loopEnd_ = 1.0;
        double loopStartS_ = 0.0;
        double loopEndS_ = 0.0;
        bool loopValid_ = false;
        Stage stage_ = Stage::Idle;
        double posS_ = 0.0;
        double cur_ = 0.0; // curve(pos) of the NEXT sample (0 when Idle) — the only read of a table's value
        bool gate_ = false;
        double offset_ = 0.0;
        double offsetStep_ = 0.0;
        int rampLeft_ = 0;
        int declickSamples_ = 240;
    };
}
