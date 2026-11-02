#include <nemesis/dsp/DrawEnvelope.h>

#include <cmath>

namespace nemesis::dsp
{
    void DrawEnvelope::prepare (double sampleRate) noexcept
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        const double ramp = sampleRate_ * kDeclickSeconds + 0.5; // round, no libm
        declickSamples_ = ramp >= 1.0 ? static_cast<int> (ramp) : 1;
        lenS_ = 0.0; // no position to carry across a rate change: reset() below
        resolveSamples();
        reset();
    }

    void DrawEnvelope::resolveSamples() noexcept
    {
        const double oldLenS = lenS_;
        const double s = lengthSec_ * sampleRate_;
        lenS_ = s >= 1.0 ? s : 1.0;
        // Keep pos (0–1) where it is: a length change is not a jump.
        if (stage_ == Stage::Done)
            posS_ = lenS_;
        else if (oldLenS > 0.0)
            posS_ = posS_ * (lenS_ / oldLenS);
        loopStartS_ = loopStart_ * lenS_;
        loopEndS_ = loopEnd_ * lenS_;
        refresh();
    }

    void DrawEnvelope::setCurve (const CurveTable* curve) noexcept
    {
        if (curve == curve_)
            return;
        const double before = cur_; // what was about to play — never a re-read of the outgoing table
        curve_ = curve;
        refresh();
        if (stage_ != Stage::Idle)
            jump (before, cur_);
    }

    void DrawEnvelope::setLengthSeconds (double seconds) noexcept
    {
        if (!(seconds > kMinLengthSeconds)) // NaN fails too
            seconds = kMinLengthSeconds;
        if (seconds > kMaxLengthSeconds)
            seconds = kMaxLengthSeconds;
        lengthSec_ = seconds;
        resolveSamples();
    }

    void DrawEnvelope::setLoop (double startPos, double endPos) noexcept
    {
        const double s = startPos > 0.0 ? (startPos < 1.0 ? startPos : 1.0) : 0.0;
        const double e = endPos > 0.0 ? (endPos < 1.0 ? endPos : 1.0) : 0.0;
        if (std::isnan (startPos) || std::isnan (endPos) || !(s < e)) // NaN would clamp to 0: refuse it
        {
            loopValid_ = false;
            return;
        }
        loopStart_ = s;
        loopEnd_ = e;
        loopStartS_ = s * lenS_;
        loopEndS_ = e * lenS_;
        loopValid_ = true;
    }

    void DrawEnvelope::gateOn() noexcept
    {
        const double current = cur_ + offset_;
        const bool declick = isActive() || current != 0.0;
        const double before = cur_;
        gate_ = true;
        stage_ = Stage::Running;
        posS_ = 0.0;
        refresh();
        if (declick)
            jump (before, cur_); // offset becomes current − curve(0)
        else
        {
            offset_ = 0.0;
            rampLeft_ = 0;
        }
    }

    void DrawEnvelope::gateOff() noexcept
    {
        gate_ = false;
        if (stage_ != Stage::Running || !loopValid_ || !(posS_ < loopEndS_))
            return; // one-shot, not running, or already past the loop: play on
        const double before = cur_;
        posS_ = loopEndS_;
        refresh();
        jump (before, cur_);
    }
}
