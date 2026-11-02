#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ForkConfig.h>
#include <nemesis/dsp/ScanOrder.h>
#include <nemesis/dsp/ScanOrderTableSet.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nemesis::dsp
{
    // Band-limited wavetable primitive shared by PRISM and AETHER (OI-AE-10, LOCKED
    // 2026-07-10): single-cycle frames of `frameLength` samples (2048 by the family
    // convention) morphed by a 0..1 position, made alias-free by a per-octave mip pyramid.
    //
    // A `Wavetable` owns the frame content and, in build() (off the audio thread), derives
    // for every frame a set of band-limited mip levels: FFT the single-cycle frame, zero all
    // bins above the mip's top harmonic, inverse-FFT back to the time domain. Mip level j
    // keeps harmonics 1..(frameLength/2 >> j) — each level halves the band (one per octave),
    // down to the fundamental. This is the Vital/Serum trade: store mips (memory) so playback
    // never has to oversample the table (CPU). The build FFT is a self-contained radix-2
    // routine private to the .cpp, so the L1 dsp layer stays JUCE-free (as with Convolution).
    //
    // A `WavetableOscillator` reads a Wavetable alias-free at any pitch. It picks the mip whose
    // band-limit is safely below Nyquist for the current increment and cross-fades between the
    // two bracketing mips (smoothness across pitch) AND the two bracketing frames (position
    // morph). Mip/position selection uses libm (log2/floor) but is CONTROL-RATE — resolved in
    // the setters (like Svf's per-cutoff coefficient math), not per sample. The per-sample hot
    // path processSample() is inline, pure arithmetic: four linear table reads and three
    // linear blends, no libm, no allocation. Coeffs/state in double, I/O float. NOT thread-safe
    // against Wavetable::build().
    //
    // Anti-aliasing guard: mip selection is biased to the band-limited (safe) side by
    // kMipGuard octaves so BOTH bracketing mips satisfy Nyquist — the cross-fade of two
    // alias-free spectra is itself alias-free (a linear blend creates no new partials). The
    // cost is ~kMipGuard octaves of top harmonics rolled off early; a tighter guard and
    // higher-order intra-frame interpolation are later refinements. Reference: the Vital/Surge
    // "wavetable mipmap" approach; band-limiting by FFT bin-zeroing is textbook (Zölzer, DAFX).
    class Wavetable
    {
    public:
        // Builds the mip pyramid from `numFrames` single-cycle frames laid out contiguously
        // (frame f occupies samples [f*frameLength, (f+1)*frameLength)). `frameLength` must be
        // a power of two >= 2 (2048 by the family convention). Not RT-safe: runs FFTs and
        // allocates (call off the audio thread). `samples` is read only during this call.
        void build (const float* samples, int numFrames, int frameLength, bool decimate = false); // FIREFORK W1: decimate

        // FIREFORK W1: the length (samples) mip `mip` is stored at (frameLength() unless built decimated), the samples one frame
        // takes in all its mips, and the raw pyramid ([frame][mip concatenated][sample]) for the host tool that writes it to a file
        [[nodiscard]] int mipLength (int mip) const noexcept { return mipLen_[static_cast<std::size_t> (mip)]; }
        [[nodiscard]] std::size_t frameSamples() const noexcept { return frameSize_; }
        [[nodiscard]] bool decimated() const noexcept { return decimated_; }
        [[nodiscard]] const Real* raw() const noexcept { return store(); }
        [[nodiscard]] std::size_t rawSize() const noexcept { return static_cast<std::size_t> (numFrames_) * frameSize_; }
        // Samples a table's pyramid will take (frames x all mips) for this geometry, without building it (the device sizes its arena with it)
        static std::size_t pyramidSamples (int numFrames, int frameLength, bool decimated);
        // Gives the table memory that is not its own (the device's arena: its heap never frees): adoptPyramid then fills it instead of allocating
        void setStorage (Real* mem, std::size_t capacity) noexcept { ext_ = mem; extCap_ = capacity; }
        // Sets the geometry and fills the pyramid from `count` 16-bit samples x scale (no FFT): the device's loader. False (and an empty
        // table) if `count` is not what that geometry holds. Not RT-safe (allocates).
        bool adoptPyramid (int numFrames, int frameLength, bool decimated, const std::int16_t* q, std::size_t count, float scale);

        [[nodiscard]] int numFrames() const noexcept { return numFrames_; }
        [[nodiscard]] int frameLength() const noexcept { return frameLength_; }
        [[nodiscard]] int numMips() const noexcept { return numMips_; }

        // Top harmonic retained by mip level `j` (frameLength/2 >> j). Introspection / tests.
        [[nodiscard]] int mipTopHarmonic (int j) const noexcept { return (frameLength_ / 2) >> j; }

        // Read pointer to one band-limited frame (frameLength() doubles). `frame` in
        // [0,numFrames), `mip` in [0,numMips): mip 0 is full-band. Used by the oscillator hot
        // path and available for tests / GUI frame display.
        [[nodiscard]] const Real* frame (int frameIdx, int mip) const noexcept
        {
            return store() + frameStride (frameIdx, mip); // FIREFORK W1
        }

        // ── [T-PROTO-02 leaf 4] THE SCAN-ORDER HALF, and it lives HERE for one reason ────────
        //
        // `scanOrderSlot` takes TWO inputs — the requested ordering and the STORED one — plus the
        // measured tables built off this bank's own frames. The requested half is a param and
        // belongs to the oscillator; the stored half and the tables are properties OF THIS TABLE,
        // and pairing them anywhere else is the mismatch `detail::lookupOr`'s length guard exists
        // to catch. Owner-ruled 2026-08-28.
        //
        // What makes it the right home rather than merely a tidy one is the PING-PONG:
        // `CathodeMachine::buildBank` builds into `b.table[b.spare()]` and `bindBank` then swaps a
        // single pointer. With the ordering and the tables riding on the Wavetable, that one
        // pointer swap carries all three atomically — a bank switch cannot land a new table beside
        // the previous bank's ordering, because there is no separate thing to forget to update.
        //
        // NOT folded into `build()`: `SubOscillator` and `PulseOscillator` build one- and
        // three-frame tables that have no recipe and no ordering, and widening the shared
        // signature for them would be a parameter they must pass and cannot answer. A table that
        // never gets this call keeps `Morph` + no tables, which the law reads as "unmeasured →
        // identity" — the same path an unbound oscillator takes, and not an error.
        //
        // `samples` is the SOURCE float buffer, passed again rather than kept: `build()` stores the
        // mip pyramid as `double` and does not retain the float frames, and the centroid
        // measurement needs the un-band-limited originals. Call it immediately after `build()`,
        // from the one site that still holds them. Not RT-safe — it allocates and, when a centroid
        // table is owed, runs an FFT per frame.
        void setScanOrdering (FrameOrdering stored, const std::uint8_t* frameOrder, const float* samples)
        {
            scanOrder_.build (stored, frameOrder, samples, numFrames_, frameLength_);
        }

        [[nodiscard]] FrameOrdering scanOrdering() const noexcept { return scanOrder_.ordering(); }
        [[nodiscard]] ScanOrderTables scanTables() const noexcept { return scanOrder_.view(); }

    private:
        [[nodiscard]] std::size_t frameStride (int frame, int mip) const noexcept
        {
            return static_cast<std::size_t> (frame) * frameSize_ + mipOff_[static_cast<std::size_t> (mip)]; // FIREFORK W1
        }
        void setGeometry (int numFrames, int frameLength, bool decimate);
        [[nodiscard]] Real* store() const noexcept { return ext_ != nullptr ? ext_ : const_cast<Real*> (data_.data()); }

        int frameLength_ = 0;
        int numFrames_ = 0;
        int numMips_ = 0;
        std::size_t frameSize_ = 0; // FIREFORK W1: samples of one frame over all its mips
        bool decimated_ = false;
        std::array<int, 24> mipLen_ {};
        std::array<std::size_t, 24> mipOff_ {};
        Real* ext_ = nullptr; // external storage (setStorage), else data_
        std::size_t extCap_ = 0;
        std::vector<Real> data_; // flat [frame][mip][sample]: each mip at mipLen_[mip] samples
        ScanOrderTableSet scanOrder_; // [T-PROTO-02 leaf 4] the stored ordering + its measured tables
    };

    class WavetableOscillator
    {
    public:
        // Guard, in octaves, biasing mip selection toward the band-limited side so both
        // bracketing mips are alias-free. 1.0 = provably clean (see class note).
        static constexpr Real kMipGuard = 1.0;

        // Binds a Wavetable (non-owning; must outlive the oscillator and not be rebuilt while
        // in use) and re-resolves selection. Clears phase.
        void setWavetable (const Wavetable& wt) noexcept
        {
            wt_ = &wt;
            length_ = wt.frameLength();
            numMips_ = wt.numMips();
            numFrames_ = wt.numFrames();
            reset();
            resolveMip();
            resolvePosition();
        }

        // Sets the playback sample rate (Hz). Control-rate; re-resolves mip selection.
        void setSampleRate (Wide sampleRate) noexcept
        {
            sampleRate_ = sampleRate > 0.0 ? sampleRate : 1.0;
            invSampleRate_ = 1.0 / sampleRate_; // FIREFORK N3
            resolveMip();
        }

        // Sets the oscillator frequency (Hz). Control-rate; re-resolves mip selection.
        void setFrequency (Wide hz) noexcept
        {
            freqHz_ = hz;
            inc_ = fork::kApprox ? freqHz_ * invSampleRate_ : freqHz_ / sampleRate_; // FIREFORK N3
            resolveMip();
        }

        // Sets the morph position in [0,1] across the table's frames. Control-rate.
        void setPosition (float pos) noexcept
        {
            position_ = pos < 0.0f ? 0.0f : (pos > 1.0f ? 1.0f : pos);
            resolvePosition();
        }

        // [T-PROTO-02 leaf 4] The REQUESTED half of the scan-order law — `synth.osc{1,2}.scanOrder`
        // for CATHODE. The STORED half and the measured tables ride on the bound `Wavetable`.
        // Control-rate; re-resolves position because that is the only thing it changes.
        //
        // `Off` (the default, and every table's state until something sets otherwise) is the exact
        // identity, so an oscillator nobody calls this on behaves bit-for-bit as it did before this
        // card — which is what makes the param's own `Off` default a true neutral rather than a
        // nominal one.
        void setScanOrder (ScanOrder order) noexcept
        {
            scanOrder_ = order;
            resolvePosition();
        }

        [[nodiscard]] ScanOrder scanOrder() const noexcept { return scanOrder_; }

        // Zeroes the phase (does not touch frequency/position/wavetable binding).
        void reset() noexcept { phase_ = 0.0; }

        // [T-CMP-04 leaf 1] Sets the phase outright — `reset()` generalised from "to zero" to "to
        // an arbitrary point". Hard sync needs it because a restart lands BETWEEN samples: by the
        // time the next sample is produced the slave has already run a fraction of a step past
        // its restart, and zeroing would put it a fraction of a cycle behind where it belongs,
        // which is audible as a pitch error at high ratios rather than as a click.
        //
        // Wrapped rather than refused, unlike `sampleAt`'s offset: that precondition exists so a
        // wrong OFFSET fails loudly instead of serving a different duty, while a phase is
        // periodic by definition and a caller handing over 1.25 means 0.25 — there is no wrong
        // answer to hide. Not `noexcept`-free arithmetic by accident: `fmod` would put libm on a
        // hot path, so this is the same conditional subtract `processSample()` uses.
        void setPhase (Real p) noexcept
        {
            if (p >= 1.0 || p < 0.0)
                p -= std::floor (p);
            phase_ = p;
        }

        [[nodiscard]] Wide phase() const noexcept { return phase_; }
        // [T-CORE-14 leaf D] The APPLIED morph position, i.e. after this class's own [0,1] clamp
        // (`synth.osc*.pos` composed with the §2.9 `macro.oscPos` rider — a sum that can legitimately
        // leave the range, and the clamp is named ONCE, here at the destination). Read back rather
        // than inferred from a render: the rider's BipolarPercent conversion differs from a
        // hand-rolled `/64` by 0.016 at the top of the range, which moves the render without any
        // assertion being able to say WHICH value arrived.
        [[nodiscard]] float position() const noexcept { return position_; }
        [[nodiscard]] int currentMip() const noexcept { return mipJ0_; }
        // [T-CMP-02] The APPLIED frequency, the twin of `position()` above and read for the same
        // reason: `SubOscillator` owns a fixed −1-octave law, and a test that recomputed
        // `noteHz · 0.5` itself would pass whether or not the oscillator was ever told.
        [[nodiscard]] Wide frequencyHz() const noexcept { return freqHz_; }

        // [T-WT-02 leaf C] The table this oscillator is ACTUALLY reading, or nullptr before
        // any bind. Introspection / tests, and the reason it is the oscillator that answers:
        // a bank selection is a two-step edge (build a table, then bind it to every voice's
        // slot) and the only claim worth asserting is that the VOICE holds the table the
        // catalogue built — which the L2 side's own bookkeeping copy cannot say, since a
        // missed bind leaves that copy correct and the sound wrong (T-CORE-14's rule that a
        // read-back reads the instrument, never a cache one layer up).
        [[nodiscard]] const Wavetable* boundTable() const noexcept { return wt_; }

        // Produces one output sample and advances the phase. Hot path: four linear table
        // reads (two mips x two frames) blended across mip and position, no libm/allocation.
        float processSample() noexcept
        {
            const float out = readAt (phase_);

            phase_ += inc_;
            if (phase_ >= 1.0) // FIREFORK B10: inc_ >= 1 (a pitch at or above the sample rate) left the phase growing without bound
                phase_ -= (phase_ < 2.0 ? 1.0 : std::floor (phase_));

            return out;
        }

        // [T-CMP-15 leaf B] A NON-ADVANCING read `offsetPhase` AHEAD of the current phase, through the
        // SAME resolved mip/frame state `processSample()` reads. It exists so a pulse can be the
        // DIFFERENCE of two reads of one band-limited saw at a continuous offset: that is what makes a
        // duty continuous (the offset is a double — nothing is quantised into frames, which is the whole
        // gap to the 7 shipped duty-sweep banks) while the anti-aliasing stays THIS pyramid's, already
        // golden-tested. T-CMP-15 R1, which is T-CMP-02 R1 held to rather than re-litigated.
        //
        // WHY THE ANTI-ALIASING SURVIVES THE SUBTRACTION, stated here because it is the reason this
        // method is legitimate and not a shortcut: a difference of two band-limited signals is a LINEAR
        // operation, so it creates no partial the operands did not carry — the same argument the class
        // note already makes for the mip cross-fade ("a linear blend creates no new partials"), applied
        // one level up. The comb `1 − e^{−jωd}` reshapes the existing harmonics; it cannot add one.
        //
        // IT IS AN ADDITIONAL ENTRY POINT, NOT A CHANGE TO THE HOT PATH. The read arithmetic moved into
        // `readAt` and `processSample()` calls it — the same operations, on the same doubles, in the
        // same order — so the committed goldens hold bit for bit. That was the constraint this design
        // had to meet: a card that needs a second read has no business moving a golden.
        //
        // `offsetPhase` MUST be in [0,1). The wrap is one conditional subtract, the same one
        // `processSample()` uses and legal for the same reason (`phase_ + offsetPhase < 2`), which is
        // what keeps libm off the path. An out-of-range offset is the CALLER's bug and is deliberately
        // NOT clamped: clamping would silently serve a different duty instead of failing, and
        // `PulseOscillator` already owns the 5–95 % rails where that decision belongs.
        [[nodiscard]] float sampleAt (Wide offsetPhase) const noexcept
        {
            Real ph = phase_ + offsetPhase;
            if (ph >= 1.0)
                ph -= 1.0;
            return readAt (ph);
        }

    private:
        // [T-CMP-15 leaf B] The read, at an arbitrary phase in [0,1), with no side effect — extracted
        // from `processSample()` so `sampleAt()` cannot become a second copy of it. Four linear table
        // reads (two mips x two frames) blended across mip and position; no libm, no allocation.
        [[nodiscard]] float readAt (Real ph) const noexcept
        {
            const Tap t0 = tapFor (mipJ0_, ph); // FIREFORK W1: each mip is read at its own length
            const Tap t1 = (mipJ1_ == mipJ0_) ? t0 : tapFor (mipJ1_, ph);
            const Real sf0 = blendMip (posF0_, t0, t1);
            const Real sf1 = (posF1_ == posF0_) ? sf0 : blendMip (posF1_, t0, t1);
            return static_cast<float> (sf0 + (sf1 - sf0) * posFrac_);
        }

        struct Tap
        {
            int i0 = 0;
            int i1 = 0;
            Real frac = 0.0;
        };
        [[nodiscard]] Tap tapFor (int mip, Real ph) const noexcept
        {
            const int len = wt_->mipLength (mip);
            const Real p = ph * static_cast<Real> (len);
            Tap t;
            t.i0 = static_cast<int> (p); // 0..len-1 (ph in [0,1))
            if (t.i0 >= len) // FIREFORK W1: a phase that rounds up to 1.0 (float) must not read past the level
                t.i0 = len - 1;
            t.i1 = t.i0 + 1;
            if (t.i1 >= len)
                t.i1 = 0;
            t.frac = p - static_cast<Real> (t.i0);
            return t;
        }

        // Linear-interpolated read of frame `f` at both bracketing mips, blended by mipFrac_.
        [[nodiscard]] Real blendMip (int f, const Tap& t0, const Tap& t1) const noexcept
        {
            const Real* a = wt_->frame (f, mipJ0_);
            const Real v0 = a[static_cast<std::size_t> (t0.i0)]
                              + (a[static_cast<std::size_t> (t0.i1)] - a[static_cast<std::size_t> (t0.i0)]) * t0.frac;
            if (mipJ1_ == mipJ0_)
                return v0;
            const Real* b = wt_->frame (f, mipJ1_);
            const Real v1 = b[static_cast<std::size_t> (t1.i0)]
                              + (b[static_cast<std::size_t> (t1.i1)] - b[static_cast<std::size_t> (t1.i0)]) * t1.frac;
            return v0 + (v1 - v0) * mipFrac_;
        }

        // Control-rate: pick the two bracketing mip levels + cross-fade fraction for inc_.
        // Continuous level m where a mip's top harmonic equals Nyquist is m = log2(length*inc)
        // (top harmonic length/2 >> m == 0.5/inc). Bias by +kMipGuard octaves so floor(m') is
        // already band-limited below Nyquist, then cross-fade floor(m')..floor(m')+1.
        void resolveMip() noexcept
        {
            if (wt_ == nullptr || numMips_ <= 0)
            {
                mipJ0_ = mipJ1_ = 0;
                mipFrac_ = 0.0;
                return;
            }
            Real m = 0.0;
            const Real x = static_cast<Real> (length_) * inc_;
            if (x > 0.0)
                m = std::log2 (x) + kMipGuard;
            if (m < 0.0)
                m = 0.0;
            const Real maxIdx = static_cast<Real> (numMips_ - 1);
            if (m > maxIdx)
                m = maxIdx;
            mipJ0_ = static_cast<int> (std::floor (m));
            mipFrac_ = m - static_cast<Real> (mipJ0_);
            mipJ1_ = mipJ0_ + 1 < numMips_ ? mipJ0_ + 1 : mipJ0_;
        }

        // Control-rate: pick the two bracketing frames + morph fraction for position_.
        //
        // [T-PROTO-02 leaf 4] THE SCAN ORDER PERMUTES WHICH FRAMES BRACKET THE POSITION, AND
        // NOTHING ELSE. Owner-ruled 2026-08-28, and the card had proposed the opposite — quantise
        // to the `2^b` grid, permute, then set `posFrac_ = 0` so each stop lands on exactly one
        // real frame.
        //
        // The ruling that settled it is `family_overview.md` §2.3's own claim for the split: the
        // param "applies to *any* table, so `Basics` can be scanned in Gray order, which the old
        // model could not express at all". A discrete scan makes that sentence false in practice —
        // `Basics` is a MORPH bank, and turning its 32-frame morph into 32 hard steps is not
        // "scanning it in Gray order", it is replacing what the bank is for. Interpolation and
        // ordering are orthogonal, which is the word §2.3 uses, so this permutes the two
        // bracketing SLOTS and blends between them exactly as before.
        //
        // WHAT THAT COSTS, stated rather than left for someone to discover: on a mask-space bank a
        // fractional position is now a BLEND of two masks rather than one exact mask. The
        // 2026-08-03 `fp == q` ruling protected the property that every stop CAN land on a real
        // frame, and it still does — an integer scan index still resolves to exactly one slot —
        // but nothing forces a position to be one. R9's `MASK POS` relabel is therefore a statement
        // about WHICH axis you are on, not a promise that the axis is discrete.
        //
        // Both endpoints go through the law, not just `posF0_`. Permuting one and stepping the
        // other would blend slot `g(i)` into slot `i+1`, which is not a permutation of anything and
        // is wrong in a way that still sounds like a wavetable.
        void resolvePosition() noexcept
        {
            if (wt_ == nullptr || numFrames_ <= 0)
            {
                posF0_ = posF1_ = 0;
                posFrac_ = 0.0;
                return;
            }
            // `Off` short-circuits the two table reads AND the view() call on the hot-ish control
            // path. Not an optimisation for its own sake: it is also the statement that the
            // default path is untouched code, so nothing about this card can change how a patch
            // built before it sounds. `resolveScanFrames` itself never reads the tables under `Off`.
            //
            // [leaf 6] A LABELLED BATTERY SURVIVOR, CLASSIFIED (c) — EQUIVALENT MUTANT, and this one
            // was PREDICTED as a survivor before the battery ran rather than explained afterwards.
            // Removing it routes `Off` through `scanOrderSlot`, which returns the identity for `Off`
            // on every stored ordering, so the rendered result is bit-identical. It is a COST guard,
            // not a correctness one, and the honest label is that no test can kill it — not that a
            // test is missing.
            const bool off = scanOrder_ == ScanOrder::Off;
            const ScanFrames r = resolveScanFrames (static_cast<Real> (position_),
                numFrames_,
                scanOrder_,
                off ? FrameOrdering::Morph : wt_->scanOrdering(),
                off ? ScanOrderTables {} : wt_->scanTables());
            posF0_ = r.f0;
            posF1_ = r.f1;
            posFrac_ = r.frac;
        }

        const Wavetable* wt_ = nullptr;
        int length_ = 0;
        int numMips_ = 0;
        int numFrames_ = 0;

        Wide sampleRate_ = 48000.0;
        Wide invSampleRate_ = 1.0 / 48000.0; // FIREFORK N3: 1 / sampleRate_
        Wide freqHz_ = 0.0;
        Wide inc_ = 0.0; // cycles per sample
        Wide phase_ = 0.0; // [0,1)
        float position_ = 0.0f;
        ScanOrder scanOrder_ = ScanOrder::Off; // [T-PROTO-02 leaf 4] the REQUESTED half of the law

        int mipJ0_ = 0; // lower (more harmonics) bracketing mip
        int mipJ1_ = 0; // upper (more band-limited) bracketing mip
        Real mipFrac_ = 0.0; // cross-fade mipJ0_ -> mipJ1_
        int posF0_ = 0; // lower bracketing frame
        int posF1_ = 0; // upper bracketing frame
        Real posFrac_ = 0.0; // morph posF0_ -> posF1_
    };
}
