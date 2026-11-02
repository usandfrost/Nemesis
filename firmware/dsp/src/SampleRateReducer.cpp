#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/FastMath.h>
#include <nemesis/dsp/SampleRateReducer.h>

#include <cmath>

namespace nemesis::dsp
{
    namespace
    {
        Real clamp (Real v, Real lo, Real hi) noexcept
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }
    }

    void SampleRateReducer::prepare (Wide hostSampleRate) noexcept
    {
        fs_ = hostSampleRate > 0.0 ? hostSampleRate : 44100.0;
        recompute();
        reset();
    }

    void SampleRateReducer::setDeviceRateHz (float rateHz) noexcept
    {
        deviceRate_ = static_cast<Real> (rateHz);
        if (deviceRate_ < kMinRateHz)
            deviceRate_ = kMinRateHz;
        recompute();
    }

    void SampleRateReducer::setKeyTrack (float amount) noexcept
    {
        track_ = clamp (static_cast<Real> (amount), 0.0, 1.0);
        recompute();
    }

    void SampleRateReducer::setNoteHz (float noteHz) noexcept
    {
        noteHz_ = static_cast<Real> (noteHz);
        if (noteHz_ < kMinRateHz)
            noteHz_ = kMinRateHz;
        recompute();
    }

    void SampleRateReducer::reset() noexcept
    {
        phase_ = 0.0;
        heldCur_ = 0.0;
        heldPrev_ = 0.0;
        first_ = true;
    }

    // Control-rate: fs_eff = deviceRate · (noteHz/refHz)^track (full varispeed at track=1 — an
    // octave up doubles the effective rate), and inc = fs_eff/fs captures per host sample. The
    // exp2/log2 live here, off the audio thread, so processSample stays pure arithmetic.
    void SampleRateReducer::recompute() noexcept
    {
        Real pitchMult = 1.0;
        if (track_ > 0.0)
            pitchMult = fm::exp2 (track_ * fm::log2 (noteHz_ / kReferenceHz));

        fsEff_ = deviceRate_ * pitchMult;
        if (fsEff_ < kMinRateHz)
            fsEff_ = kMinRateHz;

        inc_ = fs_ > 0.0 ? fsEff_ / fs_ : 1.0;
        if (inc_ < kMinInc)
            inc_ = kMinInc; // never stall in an eternal hold
    }
}
