#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/CathodeVoiceEngine.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace nemesis::dsp
{
    Wide CathodeVoiceEngine::midiNoteToHz (int midiNote) noexcept
    {
        // Equal temperament, A4 (MIDI 69) = 440 Hz.
        return 440.0 * std::exp2 ((static_cast<Real> (midiNote) - 69.0) / 12.0);
    }

    Wide CathodeVoiceEngine::unisonDetuneMultiplier (int index, int count, float amount01) noexcept
    {
        // count ≤ 1 OR amount 0 → EXACTLY 1.0 (the U=1 / detune-off bit-identity anchor).
        if (count <= 1 || amount01 <= 0.0f)
            return 1.0;
        // Symmetric linear-cents spread: member i sits at frac ∈ [−0.5, +0.5] of the total spread.
        const Real frac = static_cast<Real> (index) / static_cast<Real> (count - 1) - 0.5;
        const Wide cents = static_cast<Real> (amount01) * kUnisonDetuneCents * frac;
        return std::exp2 (cents / 1200.0);
    }

    void CathodeVoiceEngine::resolveUnison() noexcept
    {
        // [owner 2026-10-11] Poly no longer caps the stack at voiceCount_ (OI-S4): VOICES counts notes and the stack
        // is each note's copies (`polySpan`). The pool is the only cap, in both modes.
        int u = unison_ < 1 ? 1 : unison_;
        if (u > kMaxVoices)
            u = kMaxVoices;
        resolvedStack_ = u;
        for (int k = 0; k < u; ++k)
        {
            unisonMult_[static_cast<std::size_t> (k)] = unisonDetuneMultiplier (k, u, unisonDetune_);
            unisonPan_[static_cast<std::size_t> (k)] = unisonPan (k, u);
        }
    }

    void CathodeVoiceEngine::setUnison (int count) noexcept
    {
        int n = count;
        if (n < 1)
            n = 1;
        if (n > kMaxUnison)
            n = kMaxUnison;
        if (n == unison_)
            return;
        if (latchPass_)
        {
            latchUnison (n); // [T-CGUI-76] a routed unison at note-on: soft
            return;
        }
        unison_ = n;
        reqUnison_.store (n, std::memory_order_relaxed); // [T-CGUI-79 A4] a later request keeps it
        publishPoolShape(); // [T-CGUI-79 S5R-1]
        resolveUnison();
        reset(); // hard stop on stack-size change — no stuck notes / orphaned stacks
    }

    void CathodeVoiceEngine::latchUnison (int n) noexcept
    {
        const int oldStack = resolvedStack_;
        unison_ = n;
        publishPoolShape(); // [T-CGUI-79 S5R-1]
        resolveUnison();
        if (!isMonophonic())
        {
            // Poly: stacks are group-tagged, so every sounding stroke keeps its own size; a larger span grows into
            // reset-clean voices, and a smaller one trims as its voices fall idle (`reclaimIdlePoly`).
            if (polySpan() > renderSpan_)
                renderSpan_ = polySpan();
            return;
        }
        // Mono: one stack for the one note. Members past the new size release (their tail rings on,
        // still rendered); members the new size adds join at the next trigger. The held members take
        // the new stack's detune now.
        for (int i = resolvedStack_; i < oldStack; ++i)
            voices_[static_cast<std::size_t> (i)].gateOff();
        if (oldStack > renderSpan_)
            renderSpan_ = oldStack;
        if (resolvedStack_ > renderSpan_)
            renderSpan_ = resolvedStack_;
        applyMonoPitch();
    }

    void CathodeVoiceEngine::setUnisonDetune (float amount01) noexcept
    {
        float a = amount01;
        if (a < 0.0f)
            a = 0.0f;
        if (a > 1.0f)
            a = 1.0f;
        unisonDetune_ = a;
        resolveUnison(); // applies to notes/glides started after this call (held voices keep pitch)
    }

    void CathodeVoiceEngine::modulateUnisonDetune (float amount01) noexcept
    {
        setUnisonDetune (amount01);
        if (isMonophonic())
        {
            // A glide re-applies every sample from the new table; a settled stack takes it now.
            if (!gliding_ && currentHz_ > 0.0)
                applyMonoPitch();
            return;
        }
        for (int i = 0; i < renderSpan_; ++i)
        {
            const auto u = static_cast<std::size_t> (i);
            // A stack of one never detunes; a voice mid steal-fade (Out) still plays its OLD note.
            if (voiceNote_[u] == -1 || voiceStackSize_[u] < 2 || !voices_[u].active() || fadePhase_[u] == FadePhase::Out)
                continue;
            voices_[u].setNoteHz (voiceKeyHz_[u] * unisonDetuneMultiplier (voiceStackIndex_[u], voiceStackSize_[u], unisonDetune_)
                                  * voiceSpreadMult_[u] * voiceOrganicPitch_[u]);
        }
    }

    // ---- Rotation allocator (M1-18e, §6) ----------------------------------------------------------

    void CathodeVoiceEngine::setVoiceAlloc (VoiceAlloc alloc) noexcept
    {
        if (alloc != alloc_)
        {
            alloc_ = alloc;
            reqAlloc_.store (alloc, std::memory_order_relaxed); // [T-CGUI-79 A4] a later request keeps it
            publishPoolShape(); // [T-CGUI-79 S5R-1]
            reset(); // hard stop on a policy change — no stuck notes (the setVoiceMode idiom)
        }
    }

    void CathodeVoiceEngine::setRotateCount (int count) noexcept
    {
        int n = count;
        if (n < 1)
            n = 1;
        if (n > kMaxVoices)
            n = kMaxVoices;
        rotateCount_ = n; // effective window = min(rotateCount_, voiceCount_) notes (× U for Rotate), resolved per pick
    }

    void CathodeVoiceEngine::setRotateSeed (std::uint32_t seed) noexcept
    {
        rotateSeed_ = seed;
        rotState_ = seed;
    }

    void CathodeVoiceEngine::setUnisonGrace (float seconds) noexcept
    {
        graceSec_ = seconds < 0.0f ? 0.0f : seconds;
        graceSamples_ = static_cast<std::uint64_t> (static_cast<Real> (graceSec_) * sampleRate_);
    }

    void CathodeVoiceEngine::setStealFade (float seconds) noexcept
    {
        stealFadeSec_ = seconds < 0.0f ? 0.0f : seconds;
        if (stealFadeSec_ > 0.0f)
        {
            const auto n = static_cast<int> (std::lround (static_cast<Real> (stealFadeSec_) * sampleRate_));
            stealFadeSamples_ = n < 1 ? 1 : n;
            stealFadeStep_ = 1.0f / static_cast<float> (stealFadeSamples_);
            return;
        }
        // Turning the fade OFF flushes any in-flight fade: a pending swap is applied (the note must
        // still land), gains restore to unity, phases clear.
        stealFadeSamples_ = 0;
        stealFadeStep_ = 0.0f;
        for (std::size_t u = 0; u < voices_.size(); ++u)
        {
            if (fadePhase_[u] == FadePhase::Out)
                applyPendingAssign (u);
            fadePhase_[u] = FadePhase::None;
            fadeGain_[u] = 1.0f;
        }
    }

    int CathodeVoiceEngine::pickVoice() noexcept
    {
        switch (alloc_)
        {
            case VoiceAlloc::RotateForward:
                return pickVoiceRotate (+1, false);
            case VoiceAlloc::RotateBackward:
                return pickVoiceRotate (-1, false);
            case VoiceAlloc::RotateRandom:
                return pickVoiceRotate (+1, true);
            case VoiceAlloc::Unison: // strokes route through noteOnUnisonAlloc, never here
            case VoiceAlloc::Poly:
                break;
        }
        return pickVoicePoly();
    }

    int CathodeVoiceEngine::pickVoiceRotate (int dir, bool random) noexcept
    {
        // [owner 2026-10-11] The window counts NOTES, so it spans their stacks: min(rotCount, voices) · U voices.
        const int notes = rotateCount_ < voiceCount_ ? rotateCount_ : voiceCount_;
        const int w = notes * resolvedStack_ < kMaxVoices ? notes * resolvedStack_ : kMaxVoices;
        int start;
        if (random)
        {
            // Seeded LCG draw (the BBD/spread idiom) → a random start position in the window.
            rotState_ = rotState_ * 1664525u + 1013904223u;
            start = static_cast<int> ((rotState_ >> 8) % static_cast<std::uint32_t> (w));
        }
        else
        {
            start = rotNext_ % w; // the window may have shrunk since the cursor was set
        }

        // Class-ordered skip-busy search from the start position, in rotation order (owner decision,
        // 2026-07-15): first Idle; else first releasing (not held); else steal AT the start position.
        const int step = random ? 1 : dir;
        int chosen = start;
        bool found = false;
        for (int i = 0; i < w && !found; ++i)
        {
            const int idx = ((start + step * i) % w + w) % w;
            if (voiceNote_[static_cast<std::size_t> (idx)] == -1)
            {
                chosen = idx;
                found = true;
            }
        }
        for (int i = 0; i < w && !found; ++i)
        {
            const int idx = ((start + step * i) % w + w) % w;
            if (!voiceHeld_[static_cast<std::size_t> (idx)])
            {
                chosen = idx;
                found = true;
            }
        }
        if (!random)
            rotNext_ = ((chosen + dir) % w + w) % w; // anchor the walk to the actual assignment
        return chosen;
    }

    int CathodeVoiceEngine::pickStrokeRotate (std::array<int, kMaxVoices>& take) noexcept
    {
        // [D-M-0574 review] One U > 1 Rotate stroke. Picking member by member let member 0 take the one free voice and
        // member 1 steal ONE copy of another note (RotateRandom could even re-pick member 0's voice). The window and
        // the start are `pickVoiceRotate`'s; the free voices come in rotation order, then whole notes are stolen in
        // its class order — the first releasing voice from the start, else the first held one (its steal-at-start) —
        // every member of the victim's group, the surplus of a group larger than U released (`pickStrokePoly`'s law).
        const bool random = alloc_ == VoiceAlloc::RotateRandom;
        const int dir = alloc_ == VoiceAlloc::RotateBackward ? -1 : +1;
        const int notes = rotateCount_ < voiceCount_ ? rotateCount_ : voiceCount_;
        const int w = notes * resolvedStack_ < kMaxVoices ? notes * resolvedStack_ : kMaxVoices;
        int start;
        if (random)
        {
            rotState_ = rotState_ * 1664525u + 1013904223u;
            start = static_cast<int> ((rotState_ >> 8) % static_cast<std::uint32_t> (w));
        }
        else
            start = rotNext_ % w;
        const int step = random ? 1 : dir;
        const auto at = [start, step, w] (int i) noexcept { return ((start + step * i) % w + w) % w; };
        const int need = resolvedStack_;
        std::array<bool, kMaxVoices> taken {};
        int n = 0;
        for (int i = 0; i < w && n < need; ++i)
        {
            const int idx = at (i);
            if (voiceNote_[static_cast<std::size_t> (idx)] == -1)
            {
                take[static_cast<std::size_t> (n++)] = idx;
                taken[static_cast<std::size_t> (idx)] = true;
            }
        }
        const int scan = renderSpan_ > w ? renderSpan_ : w; // a member may ring past a latched limit
        while (n < need)
        {
            int victim = -1;
            for (int i = 0; i < w && victim < 0; ++i)
            {
                const auto u = static_cast<std::size_t> (at (i));
                if (!taken[u] && voiceNote_[u] != -1 && !voiceHeld_[u])
                    victim = at (i);
            }
            for (int i = 0; i < w && victim < 0; ++i)
            {
                const auto u = static_cast<std::size_t> (at (i));
                if (!taken[u] && voiceNote_[u] != -1)
                    victim = at (i);
            }
            if (victim < 0)
                break;
            const std::uint64_t group = voiceGroup_[static_cast<std::size_t> (victim)];
            for (int i = 0; i < scan; ++i)
            {
                const auto u = static_cast<std::size_t> (i);
                if (taken[u] || voiceNote_[u] == -1 || voiceGroup_[u] != group)
                    continue;
                taken[u] = true;
                if (n < need)
                    take[static_cast<std::size_t> (n++)] = i;
                else if (voiceHeld_[u])
                {
                    voiceHeld_[u] = false;
                    voiceReleaseAt_[u] = sampleClock_;
                    voices_[u].gateOff();
                }
            }
        }
        if (!random && n > 0)
            rotNext_ = ((take[static_cast<std::size_t> (n - 1)] + dir) % w + w) % w; // anchor the walk to the last member
        return n;
    }

    void CathodeVoiceEngine::noteOnUnisonAlloc (int note, float velocity01, std::uint32_t noteOrdinal) noexcept
    {
        reclaimIdlePoly();
        const int window = rotateCount_ < voiceCount_ ? rotateCount_ : voiceCount_;

        // A voice is PROTECTED while its release is younger than the grace window (owner decision,
        // 2026-07-15: protect ringing tails) — a new stroke leaves it alone while alternatives exist.
        auto isProtected = [this] (std::size_t u) noexcept {
            return graceSamples_ > 0 && voiceNote_[u] != -1 && !voiceHeld_[u]
                   && (sampleClock_ - voiceReleaseAt_[u]) < graceSamples_;
        };

        // Collect up to `window` voices in class order (index order within a class): Idle →
        // releasing-past-grace → held (steal). All-protected → fallback-steal the protected ones
        // (a note must always sound).
        std::array<int, kMaxVoices> take {};
        std::array<bool, kMaxVoices> taken {};
        int n = 0;
        auto scan = [&] (auto&& wanted) noexcept {
            for (int i = 0; i < voiceCount_ && n < window; ++i)
            {
                const auto u = static_cast<std::size_t> (i);
                if (!taken[u] && wanted (u))
                {
                    take[static_cast<std::size_t> (n++)] = i;
                    taken[u] = true;
                }
            }
        };
        scan ([this] (std::size_t u) noexcept { return voiceNote_[u] == -1; });
        scan ([&] (std::size_t u) noexcept { return !voiceHeld_[u] && !isProtected (u); });
        scan ([this] (std::size_t u) noexcept { return voiceHeld_[u]; });
        if (n == 0)
            scan ([] (std::size_t) noexcept { return true; }); // everything protected → steal

        // The stroke's n voices form one group with the M1-10d stack laws resolved PER STROKE
        // (control-rate exp2 at noteOn); spread composes exactly as in assignVoice.
        const std::uint64_t group = groupCounter_++;
        for (int k = 0; k < n; ++k)
            assignVoice (take[static_cast<std::size_t> (k)], note, velocity01, group, unisonDetuneMultiplier (k, n, unisonDetune_), unisonPan (k, n), n, k, noteOrdinal);
    }

    // ---- Per-voice variation / Spread (M1-18d, §6) ------------------------------------------------

    Real CathodeVoiceEngine::spreadTuneMultiplier (float offset, float amount01) noexcept
    {
        // offset 0 OR spread off → EXACTLY 1.0 (x·1.0 == x, the U=1 bit-identity idiom).
        if (offset == 0.0f || amount01 <= 0.0f)
            return 1.0;
        const Wide cents = static_cast<Real> (offset) * static_cast<Real> (amount01) * kSpreadTuneCents;
        return std::exp2 (cents / 1200.0);
    }

    float CathodeVoiceEngine::spreadCutoffHz (float baseHz, float offset, float amount01) noexcept
    {
        if (offset == 0.0f || amount01 <= 0.0f)
            return baseHz; // exact base (no ·1.0 rounding trip)
        const Real oct = static_cast<Real> (offset) * static_cast<Real> (amount01) * kSpreadCutoffOctaves;
        return static_cast<float> (static_cast<Real> (baseHz) * std::exp2 (oct));
    }

    float CathodeVoiceEngine::spreadRateHz (float baseHz, float offset, float amount01) noexcept
    {
        if (offset == 0.0f || amount01 <= 0.0f)
            return baseHz;
        const Real oct = static_cast<Real> (offset) * static_cast<Real> (amount01) * kSpreadRateOctaves;
        return static_cast<float> (static_cast<Real> (baseHz) * std::exp2 (oct));
    }

    float CathodeVoiceEngine::spreadLfoRateHz (float baseHz, float offset, float amount01) noexcept
    {
        if (offset == 0.0f || amount01 <= 0.0f)
            return baseHz; // exact base (no ·1.0 rounding trip)
        const Real oct = static_cast<Real> (offset) * static_cast<Real> (amount01) * kLfoRateSpreadOctaves;
        return static_cast<float> (static_cast<Real> (baseHz) * std::exp2 (oct));
    }

    float CathodeVoiceEngine::spreadResoQ (float baseQ, float offset, float amount01) noexcept
    {
        if (offset == 0.0f || amount01 <= 0.0f)
            return baseQ; // exact base (no ·1.0 rounding trip)
        const Real oct = static_cast<Real> (offset) * static_cast<Real> (amount01) * kSpreadResoOctaves;
        return static_cast<float> (static_cast<Real> (baseQ) * std::exp2 (oct));
    }

    float CathodeVoiceEngine::spreadAdsrMs (float baseMs, float offset, float amount01) noexcept
    {
        if (offset == 0.0f || amount01 <= 0.0f)
            return baseMs; // exact base; and a 0 ms base stays exactly 0 (instant stays instant)
        const Real oct = static_cast<Real> (offset) * static_cast<Real> (amount01) * kSpreadAdsrOctaves;
        return static_cast<float> (static_cast<Real> (baseMs) * std::exp2 (oct));
    }

    void CathodeVoiceEngine::resolveSpread() noexcept
    {
        // One LCG stream (the BBD/NoiseSource idiom), consumed in a FIXED (voice, target) order so a
        // seed always yields the same table; top 24 bits → uniform [−1, 1). This stream fills ONLY
        // the original M1-18d columns (kSpreadPrimaryStreamTargets) — appended targets draw from
        // their own stream below, so this table stays byte-identical (the M1-18f/g extension rule).
        std::uint32_t s = spreadSeed_;
        for (auto& perVoice : spreadOff_)
        {
            for (int t = 0; t < kSpreadPrimaryStreamTargets; ++t)
            {
                s = s * 1664525u + 1013904223u;
                perVoice[static_cast<std::size_t> (t)] =
                    static_cast<float> (static_cast<Real> (s >> 8) / 8388608.0 - 1.0);
            }
        }
        // [b39 B, owner ruling 2026-10-14] THE FIRST VOICE SITS CENTRED. Poly hands a single sounding note voice 0
        // every time, so its seeded pan offset was a constant lean, not variation. Zeroed AFTER the stream is drawn,
        // so every other cell keeps its seeded value; an explicit `perVoice` cell still wins (`appliedOff`).
        spreadOff_[0][static_cast<std::size_t> (SpreadTarget::Pan)] = 0.0f;
        for (int v = 0; v < kMaxVoices; ++v)
            spreadTuneMult_[static_cast<std::size_t> (v)] =
                spreadTuneMultiplier (appliedOff (v, SpreadTarget::Tune), appliedAmt (v, SpreadTarget::Tune));

        // The M1-18f LFO phase-offset table: a SECOND, XOR-decorrelated stream off the same §6 seed
        // (fixed (voice, slot) order; top 24 bits → uniform [0, 1)), so the spreadOff_ values above
        // are byte-identical to M1-18d and one seed still drives all per-voice variation.
        std::uint32_t p = spreadSeed_ ^ 0xA5A5A5A5u;
        for (auto& perVoice : lfoPhaseOff_)
        {
            for (auto& off : perVoice)
            {
                p = p * 1664525u + 1013904223u;
                off = static_cast<float> (static_cast<Real> (p >> 8) / 16777216.0);
            }
        }

        // The M1-18g LFO rate-offset table: a THIRD XOR-decorrelated stream off the same §6 seed
        // (fixed (voice, slot) order; top 24 bits → uniform [−1, 1), bipolar like spreadOff_), so
        // both prior tables stay byte-identical and one seed still drives all per-voice variation.
        std::uint32_t r = spreadSeed_ ^ 0x3C6EF372u;
        for (auto& perVoice : lfoRateOff_)
        {
            for (auto& off : perVoice)
            {
                r = r * 1664525u + 1013904223u;
                off = static_cast<float> (static_cast<Real> (r >> 8) / 8388608.0 - 1.0);
            }
        }

        // The M1-18i Reso/AdsrTimes columns: a FOURTH XOR-decorrelated stream off the same §6 seed
        // (fixed (voice, target) order; bipolar [−1, 1) like spreadOff_), so all three prior tables
        // stay byte-identical and one seed still drives all per-voice variation.
        std::uint32_t w = spreadSeed_ ^ 0x7F4A7C15u;
        for (auto& perVoice : spreadOff_)
        {
            for (int t = kSpreadPrimaryStreamTargets; t < kSpreadTargetCount; ++t)
            {
                w = w * 1664525u + 1013904223u;
                perVoice[static_cast<std::size_t> (t)] =
                    static_cast<float> (static_cast<Real> (w >> 8) / 8388608.0 - 1.0);
            }
        }
    }

    void CathodeVoiceEngine::reapplyPatch() noexcept
    {
        for (int v = 0; v < kMaxVoices; ++v)
        {
            const auto u = static_cast<std::size_t> (v);
            CathodeModulatedVoice& mv = voices_[u];
            if (hasPatchCutoff_)
                mv.setBaseCutoff (
                    spreadCutoffHz (patchCutoffHz_, appliedOff (v, SpreadTarget::Cutoff), appliedAmt (v, SpreadTarget::Cutoff)),
                    spreadResoQ (patchQ_, appliedOff (v, SpreadTarget::Reso), appliedAmt (v, SpreadTarget::Reso)));
            if (hasPatchLevel_)
                mv.setBaseLevel (spreadLevel (patchLevel_, appliedOff (v, SpreadTarget::Level), appliedAmt (v, SpreadTarget::Level)));
            if (hasPatchPan_)
                mv.setBasePan (spreadPan (patchPan_, panOffFor (v), appliedAmt (v, SpreadTarget::Pan))); // [b39 review 7]
            if (hasPatchRate_)
                mv.setBaseConverterRate (
                    spreadRateHz (patchRateHz_, appliedOff (v, SpreadTarget::ConvRate), appliedAmt (v, SpreadTarget::ConvRate)));
            if (hasPatchLoopErr_)
                mv.setBaseLoopError (
                    spreadLoopError (patchLoopErr_, appliedOff (v, SpreadTarget::ConvLoopErr), appliedAmt (v, SpreadTarget::ConvLoopErr)));
            if (hasPatchDrift_)
                mv.setBaseDrift (spreadDrift (patchDrift_, appliedOff (v, SpreadTarget::ConvDrift), appliedAmt (v, SpreadTarget::ConvDrift)));
            if (hasPatchAmpEnv_)
            {
                // ONE AdsrTimes offset scales a/d/r uniformly (M1-18i owner decision); sustain is a
                // LEVEL, not a time — forwards exact.
                const float off = appliedOff (v, SpreadTarget::AdsrTimes);
                const float amt = appliedAmt (v, SpreadTarget::AdsrTimes);
                AdsrEnvelope& env = mv.amp();
                env.setAttackMs (spreadAdsrMs (patchAmpAttackMs_, off, amt));
                env.setDecayMs (spreadAdsrMs (patchAmpDecayMs_, off, amt));
                env.setSustain (patchAmpSustain_);
                env.setReleaseMs (spreadAdsrMs (patchAmpReleaseMs_, off, amt));
            }
            // The M1-18g LFO-rate fan-out (its own amount, not spread_): only patch-set slots write.
            // [T-CGUI-46 leaf 1] A slot whose rate is a bound destination takes the spread rate as the
            // voice's generic BASE (written through while unrouted, modulated around while routed).
            const auto& rateOff = lfoRateOff_[u];
            Lfo* const lfos[] = { &mv.lfo1(), &mv.lfo2(), &mv.lfo3(), &mv.sampleHold() };
            for (std::size_t s = 0; s < static_cast<std::size_t> (kLfoFamilyCount); ++s)
            {
                if (!hasPatchLfoRate_[s])
                    continue;
                const float hz = spreadLfoRateHz (patchLfoRateHz_[s], rateOff[s], lfoRateSpread_);
                const auto id = lfoRateDest_[s];
                if (id >= 0 && mv.genericDestBound (id))
                    mv.setGenericBase (id, hz);
                else
                    lfos[s]->setRateHz (hz);
            }
        }
    }

    void CathodeVoiceEngine::setSpread (float amount01) noexcept
    {
        float a = amount01;
        if (a < 0.0f)
            a = 0.0f;
        if (a > 1.0f)
            a = 1.0f;
        spread_ = a;
        resolveSpread();
        reapplyPatch(); // applies immediately to the patch-forwarded targets; tune → next notes
    }

    void CathodeVoiceEngine::setSpreadSeed (std::uint32_t seed) noexcept
    {
        spreadSeed_ = seed;
        resolveSpread();
        reapplyPatch();
        requestPhaseReseat(); // one §6 seed drives all variation — re-apply the (new) phases [T-CGUI-81: audio side]
    }

    // ---- Source-side spread (M1-18f, §6) ----------------------------------------------------------

    void CathodeVoiceEngine::setLfoPhaseSpread (float amount01) noexcept
    {
        float a = amount01;
        if (a < 0.0f)
            a = 0.0f;
        if (a > 1.0f)
            a = 1.0f;
        // [T-CGUI-81, row 315] A REQUEST: `applyLfoPhases` zeroes `phaseSlewLeft_`, which the audio side's
        // glide decrements, so it runs there (`latchEngineRequests`). The amount, then SawOn, then pending.
        reqPhaseSpread_.store (a, std::memory_order_relaxed);
        if (a > 0.0f)
            phaseSpreadSawOn_.store (true, std::memory_order_relaxed);
        phaseSpreadPending_.store (true, std::memory_order_release);
        engineReqSerial_.fetch_add (1, std::memory_order_release);
    }

    void CathodeVoiceEngine::setLfoRateSpread (float amount01) noexcept
    {
        float a = amount01;
        if (a < 0.0f)
            a = 0.0f;
        if (a > 1.0f)
            a = 1.0f;
        lfoRateSpread_ = a;
        // Re-forward every patch-set rate (the amount-0 remap early-returns the exact base, so
        // amount → 0 restores; never-patch-set slots are never written — no transition gate needed).
        reapplyPatch();
    }

    void CathodeVoiceEngine::setPatchLfoRate (int lfoIndex, float rateHz) noexcept
    {
        if (lfoIndex < 0 || lfoIndex >= kLfoFamilyCount)
            return;
        const auto s = static_cast<std::size_t> (lfoIndex);
        patchLfoRateHz_[s] = rateHz;
        hasPatchLfoRate_[s] = true;
        reapplyPatch();
    }

    void CathodeVoiceEngine::setLfoRateDest (int lfoIndex, CathodeModMatrix::DestId id) noexcept
    {
        if (lfoIndex < 0 || lfoIndex >= kLfoFamilyCount)
            return;
        lfoRateDest_[static_cast<std::size_t> (lfoIndex)] = id;
        reapplyPatch(); // the voices learn their base for the new id now, not at the next rate write
    }

    // ---- [T-CGUI-46 leaf 2] Engine-wide modulation destinations -----------------------------------

    void CathodeVoiceEngine::bindEngineDest (CathodeModMatrix::DestId id,
        EngineWrite write,
        float lo,
        float hi,
        CathodeModulatedVoice::GenericLaw law,
        float span,
        EngineRate rate) noexcept
    {
        if (!engineIdOk (id) || write == nullptr)
            return;
        const auto u = static_cast<std::size_t> (id);
        if (engineDest_[u].write == nullptr)
        {
            if (engineBoundCount_ >= kMaxEngineDests)
                return;
            engineBound_[static_cast<std::size_t> (engineBoundCount_++)] = id;
        }
        engineDest_[u] = { write, lo, hi, law, span, rate };
        engineApplied_[u] = std::numeric_limits<float>::quiet_NaN();
        engineDirty_ = true; // the next pass re-reads the live set (an epoch guess could collide)
    }

    // [T-CGUI-81] A REQUEST, from any thread: the value, then its kind (release), then the serial. The
    // audio side takes it up in `latchEngineRequests`; the last request per dest wins.
    void CathodeVoiceEngine::setEngineBase (CathodeModMatrix::DestId id, float value, bool writeThrough) noexcept
    {
        if (!engineIdOk (id))
            return;
        const auto u = static_cast<std::size_t> (id);
        reqBase_[u].store (value, std::memory_order_relaxed);
        reqBaseKind_[u].store (writeThrough ? kBaseWriteThrough : kBaseStoreOnly, std::memory_order_release);
        engineReqSerial_.fetch_add (1, std::memory_order_release);
    }

    // AUDIO THREAD. What `setEngineBase` used to do on the calling thread, now on the thread that owns
    // `engineBase_`/`engineApplied_`/`engineLive_`.
    void CathodeVoiceEngine::latchEngineRequests() noexcept
    {
        // [T-CGUI-81, row 315] The phase-spread knob and the seed re-seat FIRST — the knob's row stores its
        // engine base after it. Transition-gated (the M1-17g rule): apply while on, or once on the on→off
        // edge (restores phase 0 — spreadLfoPhase returns exactly 0 at amount 0). Never enabled → never touched.
        const bool seedReseat = phaseReseatPending_.exchange (false, std::memory_order_acq_rel);
        bool reseat = false;
        if (phaseSpreadPending_.exchange (false, std::memory_order_acq_rel))
        {
            const bool sawOn = phaseSpreadSawOn_.exchange (false, std::memory_order_acq_rel);
            const bool wasOn = lfoPhaseSpread_ > 0.0f;
            lfoPhaseSpread_ = reqPhaseSpread_.load (std::memory_order_relaxed);
            reseat = lfoPhaseSpread_ > 0.0f || wasOn || sawOn;
        }
        if (reseat || (seedReseat && lfoPhaseSpread_ > 0.0f))
            applyLfoPhases();
        for (auto u = static_cast<std::size_t> (CathodeModMatrix::kDestCount); u < reqBaseKind_.size(); ++u)
        {
            const std::uint8_t kind = reqBaseKind_[u].exchange (0, std::memory_order_acq_rel);
            if (kind == 0)
                continue;
            const float value = reqBase_[u].load (std::memory_order_relaxed);
            engineBase_[u] = value;
            if (engineDest_[u].write == nullptr)
                continue;
            if (kind == kBaseStoreOnly)
            {
                // The caller set the target by ANOTHER path, so what was last written is no longer what the
                // target holds: the next live pass must write whatever it computes.
                engineApplied_[u] = std::numeric_limits<float>::quiet_NaN();
                continue;
            }
            if (!engineLive_[u])
            {
                engineApplied_[u] = value;
                engineDest_[u].write (*this, value);
            }
            // Routed: store only — the live pass offsets from the new base, and its unchanged-value skip
            // lets a per-block re-push of the SAME base cost nothing.
        }
        // [T-CGUI-81, row 304] The routed LFO-rate ceilings: the one field, on the thread that reads it.
        for (std::size_t s = 0; s < reqLfoTopPending_.size(); ++s)
        {
            if (!reqLfoTopPending_[s].exchange (false, std::memory_order_acq_rel))
                continue;
            const auto id = lfoRateDest_[s];
            if (id >= CathodeModMatrix::kDestCount && id < CathodeModMatrix::kDestCapacity
                && genericDest_[static_cast<std::size_t> (id)].write != nullptr)
                genericDest_[static_cast<std::size_t> (id)].hi = reqLfoTop_[s].load (std::memory_order_relaxed);
        }
    }

    int CathodeVoiceEngine::engineDriverVoice() const noexcept
    {
        const int range = isMonophonic() ? resolvedStack_ : renderSpan_; // [T-CGUI-76] poly: past a latched limit too
        if (lastTriggered_ >= 0 && lastTriggered_ < range && voices_[static_cast<std::size_t> (lastTriggered_)].active())
            return lastTriggered_;
        int best = -1;
        std::uint64_t bestAge = 0;
        for (int i = 0; i < range; ++i)
        {
            const auto u = static_cast<std::size_t> (i);
            if (voices_[u].active() && (best < 0 || voiceAge_[u] > bestAge))
            {
                best = i;
                bestAge = voiceAge_[u];
            }
        }
        return best;
    }

    void CathodeVoiceEngine::applyEngineDests (bool atNoteOn) noexcept
    {
        // Re-read the live set when voice 0's routing moved; restore a dest whose last route went.
        if (const auto epoch = voices_[0].liveEpoch(); engineDirty_ || epoch != engineEpoch_)
        {
            engineEpoch_ = epoch;
            engineDirty_ = false;
            engineLiveCount_ = 0;
            engineControlPhase_ = 0; // phase 0 writes, so a new route lands on its first sample
            for (int b = 0; b < engineBoundCount_; ++b)
            {
                const auto id = engineBound_[static_cast<std::size_t> (b)];
                const auto u = static_cast<std::size_t> (id);
                const bool live = voices_[0].destLive (id);
                if (live)
                    engineLiveList_[static_cast<std::size_t> (engineLiveCount_++)] = id;
                else if (engineLive_[u])
                {
                    // [T-CGUI-76 S5R] The restore is SOFT too: a voice count never changes under a
                    // sounding note, so an un-route binds the next allocation and cuts nothing.
                    engineApplied_[u] = engineBase_[u];
                    latchPass_ = true;
                    engineDest_[u].write (*this, engineBase_[u]);
                    if (cutLeft_ == 0) // [b39 E-tails] mid-cut, `processSample` lands it once the fade is done
                        applyPoolRequest (true); // [T-CGUI-79 S5R-1] a pool writer REQUESTS; land it soft, here
                    else if (poolRequestPending())
                        poolSoftOwed_ = true; // [b39 review 3] …and SOFT there too: a hard landing rewinds every phase
                    latchPass_ = false;
                }
                engineLive_[u] = live;
            }
        }
        if (engineLiveCount_ == 0)
            return;
        const bool controlTick = (engineControlPhase_ == 0);
        if (!atNoteOn)
            engineControlPhase_ = (engineControlPhase_ + 1) % CathodeModulatedVoice::kGenericControlDecimation;

        // Every value is computed BEFORE any write: a NoteOn writer can `reset()` the pool, and the
        // rows after it must still read the driver voice's modulation, not a cleared one.
        const int driver = engineDriverVoice();
        // [T-CGUI-76 S5R2] A chord's 2nd..Nth note-on reads the voice the previous one triggered on this
        // same sample, before it has rendered: evaluate it NOW, or it latches from its previous note.
        // [T-CGUI-79 A4] …and a driver mid steal-fade still SOUNDS its old note while the newest one waits
        // behind the fade: that pending note's velocity and key are what the latch follows.
        if (atNoteOn && driver >= 0)
        {
            const auto d = static_cast<std::size_t> (driver);
            if (fadePhase_[d] == FadePhase::Out)
                voices_[d].evaluateModulationFor (pending_[d].velocity01, pending_[d].noteHz);
            else
                voices_[d].evaluateModulationNow();
        }
        std::array<float, kMaxEngineDests> values {};
        std::array<bool, kMaxEngineDests> due {};
        for (int i = 0; i < engineLiveCount_; ++i)
        {
            const auto k = static_cast<std::size_t> (i);
            const auto id = engineLiveList_[k];
            const auto& g = engineDest_[static_cast<std::size_t> (id)];
            due[k] = atNoteOn ? (g.rate == EngineRate::NoteOn)
                              : (g.rate == EngineRate::Audio || (g.rate == EngineRate::Control && controlTick));
            if (!due[k])
                continue;
            const float base = engineBase_[static_cast<std::size_t> (id)];
            const float mod = driver >= 0 ? voices_[static_cast<std::size_t> (driver)].matrix().modulation (id) : 0.0f;
            float v = base + mod * (g.hi - g.lo); // GenericLaw::Linear
            if (g.law == CathodeModulatedVoice::GenericLaw::Octaves)
                v = static_cast<float> (static_cast<Real> (base)
                                        * std::exp2 (static_cast<Real> (mod) * static_cast<Real> (g.span)));
            else if (g.law == CathodeModulatedVoice::GenericLaw::Proportional)
                v = base * (1.0f + mod * g.span);
            values[k] = v < g.lo ? g.lo : (v > g.hi ? g.hi : v);
        }
        // [T-CGUI-76] At note-on the pool writers (voices/unison) take their SOFT law: the latch must
        // not cut the notes still ringing. The restore above is soft as well; only the knob is hard.
        latchPass_ = atNoteOn;
        for (int i = 0; i < engineLiveCount_; ++i)
        {
            const auto k = static_cast<std::size_t> (i);
            if (!due[k])
                continue;
            const auto u = static_cast<std::size_t> (engineLiveList_[k]);
            // NOLINTNEXTLINE(clang-diagnostic-float-equal) — "is this the value I last WROTE", exactly
            if (values[k] == engineApplied_[u])
                continue;
            engineApplied_[u] = values[k];
            engineDest_[u].write (*this, values[k]);
        }
        if (atNoteOn)
            applyPoolRequest (true); // [T-CGUI-79 S5R-1] the pool writers REQUESTED: land them soft, before allocation
        latchPass_ = false;
    }

    void CathodeVoiceEngine::shiftLfoPhaseSpread (float amount01) noexcept
    {
        float a = amount01;
        if (a < 0.0f)
            a = 0.0f;
        if (a > 1.0f)
            a = 1.0f;
        const float was = lfoPhaseSpread_;
        lfoPhaseSpread_ = a;
        // [T-CGUI-79 A4, row 338] The running phase GLIDES by the change across the next control tick
        // (`stepPhaseSlew`, 1/kTick of it a sample) instead of jumping by it at the tick, and a period
        // boundary crossed on the way is counted (`Lfo::shiftPhase`). A glide still owed lands whole first.
        if (phaseSlewLeft_ > 0)
            stepPhaseSlew (phaseSlewLeft_);
        constexpr int kTick = CathodeModulatedVoice::kGenericControlDecimation;
        for (int v = 0; v < kMaxVoices; ++v)
        {
            const auto u = static_cast<std::size_t> (v);
            CathodeModulatedVoice& mv = voices_[u];
            Lfo* const lfos[] = { &mv.lfo1(), &mv.lfo2(), &mv.lfo3(), &mv.sampleHold() };
            for (std::size_t i = 0; i < 4; ++i)
            {
                const float from = spreadLfoPhase (lfoPhaseOff_[u][i], was);
                const float to = spreadLfoPhase (lfoPhaseOff_[u][i], a);
                const Real running = static_cast<Real> (lfos[i]->phase());
                lfos[i]->setPhase (to); // the retrigger START phase
                lfos[i]->lockPhase (running); // …the running one keeps its place and glides
                phaseSlewStep_[u][i] = static_cast<Real> (to - from) / static_cast<Real> (kTick);
            }
        }
        phaseSlewLeft_ = kTick;
    }

    void CathodeVoiceEngine::stepPhaseSlew (int samples) noexcept
    {
        const Real n = static_cast<Real> (samples);
        for (int v = 0; v < kMaxVoices; ++v)
        {
            const auto u = static_cast<std::size_t> (v);
            CathodeModulatedVoice& mv = voices_[u];
            Lfo* const lfos[] = { &mv.lfo1(), &mv.lfo2(), &mv.lfo3(), &mv.sampleHold() };
            for (std::size_t i = 0; i < 4; ++i)
                lfos[i]->shiftPhase (phaseSlewStep_[u][i] * n);
        }
        phaseSlewLeft_ -= samples;
    }

    void CathodeVoiceEngine::setSourceSeed (std::uint32_t baseSeed) noexcept
    {
        sourceSeed_ = baseSeed;
        hasSourceSeed_ = true;
        applySourceSeeds();
        requestPhaseReseat(); // Lfo::setSeed zeroes the phase — seeds always precede phases [T-CGUI-81: audio side]
    }

    void CathodeVoiceEngine::applyLfoPhases() noexcept
    {
        phaseSlewLeft_ = 0; // [T-CGUI-79 A4] the knob RE-SEATS every phase: a glide still owed is void
        for (int v = 0; v < kMaxVoices; ++v)
        {
            const auto u = static_cast<std::size_t> (v);
            CathodeModulatedVoice& mv = voices_[u];
            mv.lfo1().setPhase (spreadLfoPhase (lfoPhaseOff_[u][0], lfoPhaseSpread_));
            mv.lfo2().setPhase (spreadLfoPhase (lfoPhaseOff_[u][1], lfoPhaseSpread_));
            mv.lfo3().setPhase (spreadLfoPhase (lfoPhaseOff_[u][2], lfoPhaseSpread_));
            mv.sampleHold().setPhase (spreadLfoPhase (lfoPhaseOff_[u][3], lfoPhaseSpread_));
        }
    }

    void CathodeVoiceEngine::lockModulators (const std::array<Real, kLfoFamilyCount + 1>& cycles, unsigned mask, Real tolerance) noexcept
    {
        if (mask == 0u)
            return;
        // [T-CGUI-83] The lock runs BEFORE the first sample's latch: take pending requests up first, in
        // `processSample`'s order, or a spread's re-seat / a pool reshape's `reset()` overwrites the lock and the
        // block runs off the bar. Audio thread, as the latch is.
        applyCutRequest(); // [b39 E-tails]
        applyEngineRequests(); // before the pool: a base's write-through may request a reshape
        if (cutLeft_ == 0 && cutInFlight_) // [b39 E-tails] mid-cut the reshape waits for the fade (`processSample` lands it)
            landCut(); // [b39 review 2]
        else if (cutLeft_ == 0)
            applyPoolRequest();
        // Signed distance a − b on a circle of length `period`, in (−period/2, period/2].
        const auto wrapped = [] (Real a, Real b, Real period) noexcept {
            Real d = a - b;
            d -= period * static_cast<Real> (static_cast<long long> (d / period));
            if (d > 0.5 * period)
                d -= period;
            else if (d <= -0.5 * period)
                d += period;
            return d;
        };
        // [S5R r3] RATE SPREAD WINS on the LFO family: it gives each voice its own rate on purpose,
        // so a lock would pull every voice back each block (a block-rate zipper). Sync stays rate-only
        // there, as before T-CGUI-59. Step has no rate spread. [T-CGUI-76 S5R] The spread's BASE decides,
        // and a ROUTED spread counts as on — the modulated amount crossing 0 must not toggle the lock.
        const bool spreadBound = engineDestBound (lfoRateSpreadDest_);
        const bool rateSpreadOn = spreadBound ? (engineBase_[static_cast<std::size_t> (lfoRateSpreadDest_)] > 0.0f
                                                    || voices_[0].destLive (lfoRateSpreadDest_))
                                              : lfoRateSpread_ > 0.0f;
        for (int v = 0; v < kMaxVoices; ++v)
        {
            const auto u = static_cast<std::size_t> (v);
            CathodeModulatedVoice& mv = voices_[u];
            Lfo* const lfos[] = { &mv.lfo1(), &mv.lfo2(), &mv.lfo3(), &mv.sampleHold() };
            for (std::size_t i = 0; i < 4; ++i)
            {
                // [T-CGUI-76 S5R] …and so does a ROUTED RATE, for the same reason, per voice.
                if ((mask & (1u << i)) == 0u || rateSpreadOn || mv.destLive (lfoRateDest_[i]))
                    continue;
                const Real target = cycles[i] + static_cast<Real> (spreadLfoPhase (lfoPhaseOff_[u][i], lfoPhaseSpread_));
                if (std::abs (wrapped (static_cast<Real> (lfos[i]->phase()), target, 1.0)) > tolerance)
                    lfos[i]->lockPhase (target);
            }
            if ((mask & (1u << 4u)) != 0u)
            {
                StepModulator& st = mv.step();
                const Real count = static_cast<Real> (st.stepCount());
                const Real now = static_cast<Real> (st.currentStep()) + st.stepPhase();
                if (std::abs (wrapped (now, cycles[4], count)) > tolerance)
                    st.lockPosition (cycles[4]);
            }
        }
    }

    void CathodeVoiceEngine::applySourceSeeds() noexcept
    {
        for (int v = 0; v < kMaxVoices; ++v)
        {
            CathodeModulatedVoice& mv = voices_[static_cast<std::size_t> (v)];
            mv.lfo1().setSeed (sourceSeedFor (sourceSeed_, v, SourceStream::Lfo1));
            mv.lfo2().setSeed (sourceSeedFor (sourceSeed_, v, SourceStream::Lfo2));
            mv.lfo3().setSeed (sourceSeedFor (sourceSeed_, v, SourceStream::Lfo3));
            mv.sampleHold().setSeed (sourceSeedFor (sourceSeed_, v, SourceStream::SampleHold));
            mv.voice().setNoiseSeed (sourceSeedFor (sourceSeed_, v, SourceStream::Noise));
            mv.voice().setDriftSeed (sourceSeedFor (sourceSeed_, v, SourceStream::Drift));
        }
    }

    void CathodeVoiceEngine::setSpreadTarget (SpreadTarget target, bool enabled) noexcept
    {
        if (enabled)
            spreadTargetMask_ = static_cast<std::uint16_t> (spreadTargetMask_ | maskBit (target));
        else
            spreadTargetMask_ = static_cast<std::uint16_t> (spreadTargetMask_ & ~maskBit (target));
        resolveSpread(); // the tune-mult table depends on the Tune enable
        reapplyPatch();
    }

    // ---- perVoice[] explicit offsets (M1-18j, §6) --------------------------------------------------

    void CathodeVoiceEngine::setPerVoiceOffset (int voice, SpreadTarget target, float offset) noexcept
    {
        if (voice < 0 || voice >= kMaxVoices)
            return;
        const auto u = static_cast<std::size_t> (voice);
        perVoiceOff_[u][static_cast<std::size_t> (target)] = clampUnitF (offset);
        perVoiceMask_[u] = static_cast<std::uint16_t> (perVoiceMask_[u] | maskBit (target));
        resolveSpread(); // the tune-mult table consults explicit Tune cells
        reapplyPatch(); // re-forward SET bases through the new cell; tune → next assignments
    }

    void CathodeVoiceEngine::clearPerVoiceOffset (int voice, SpreadTarget target) noexcept
    {
        if (voice < 0 || voice >= kMaxVoices)
            return;
        const auto u = static_cast<std::size_t> (voice);
        perVoiceMask_[u] = static_cast<std::uint16_t> (perVoiceMask_[u] & ~maskBit (target));
        perVoiceOff_[u][static_cast<std::size_t> (target)] = 0.0f;
        resolveSpread();
        reapplyPatch(); // the cell's targets fall back to seeded·spread (restores the auto path)
    }

    void CathodeVoiceEngine::clearAllPerVoiceOffsets() noexcept
    {
        perVoiceMask_.fill (0);
        for (auto& row : perVoiceOff_)
            row.fill (0.0f);
        resolveSpread();
        reapplyPatch();
    }

    void CathodeVoiceEngine::setPatchCutoff (float cutoffHz, float q) noexcept
    {
        patchCutoffHz_ = cutoffHz;
        patchQ_ = q;
        hasPatchCutoff_ = true;
        for (int v = 0; v < kMaxVoices; ++v)
            voices_[static_cast<std::size_t> (v)].setBaseCutoff (
                spreadCutoffHz (cutoffHz, appliedOff (v, SpreadTarget::Cutoff), appliedAmt (v, SpreadTarget::Cutoff)),
                spreadResoQ (q, appliedOff (v, SpreadTarget::Reso), appliedAmt (v, SpreadTarget::Reso)));
    }

    void CathodeVoiceEngine::setPatchAmpEnv (float attackMs, float decayMs, float sustain01, float releaseMs) noexcept
    {
        patchAmpAttackMs_ = attackMs;
        patchAmpDecayMs_ = decayMs;
        patchAmpSustain_ = sustain01;
        patchAmpReleaseMs_ = releaseMs;
        hasPatchAmpEnv_ = true;
        for (int v = 0; v < kMaxVoices; ++v)
        {
            const float off = appliedOff (v, SpreadTarget::AdsrTimes);
            const float amt = appliedAmt (v, SpreadTarget::AdsrTimes);
            AdsrEnvelope& env = voices_[static_cast<std::size_t> (v)].amp();
            env.setAttackMs (spreadAdsrMs (attackMs, off, amt));
            env.setDecayMs (spreadAdsrMs (decayMs, off, amt));
            env.setSustain (sustain01);
            env.setReleaseMs (spreadAdsrMs (releaseMs, off, amt));
        }
    }

    void CathodeVoiceEngine::setPatchLevel (float level) noexcept
    {
        patchLevel_ = level;
        hasPatchLevel_ = true;
        for (int v = 0; v < kMaxVoices; ++v)
            voices_[static_cast<std::size_t> (v)].setBaseLevel (
                spreadLevel (level, appliedOff (v, SpreadTarget::Level), appliedAmt (v, SpreadTarget::Level)));
    }

    void CathodeVoiceEngine::setPatchPan (float pan) noexcept
    {
        patchPan_ = pan;
        hasPatchPan_ = true;
        for (int v = 0; v < kMaxVoices; ++v)
            voices_[static_cast<std::size_t> (v)].setBasePan (
                spreadPan (pan, panOffFor (v), appliedAmt (v, SpreadTarget::Pan))); // [b39 review 7]
    }

    void CathodeVoiceEngine::setPatchConverterRate (float rateHz) noexcept
    {
        patchRateHz_ = rateHz;
        hasPatchRate_ = true;
        for (int v = 0; v < kMaxVoices; ++v)
            voices_[static_cast<std::size_t> (v)].setBaseConverterRate (
                spreadRateHz (rateHz, appliedOff (v, SpreadTarget::ConvRate), appliedAmt (v, SpreadTarget::ConvRate)));
    }

    void CathodeVoiceEngine::setPatchLoopError (float amount) noexcept
    {
        patchLoopErr_ = amount;
        hasPatchLoopErr_ = true;
        for (int v = 0; v < kMaxVoices; ++v)
            voices_[static_cast<std::size_t> (v)].setBaseLoopError (
                spreadLoopError (amount, appliedOff (v, SpreadTarget::ConvLoopErr), appliedAmt (v, SpreadTarget::ConvLoopErr)));
    }

    void CathodeVoiceEngine::setPatchDrift (float amount) noexcept
    {
        patchDrift_ = amount;
        hasPatchDrift_ = true;
        for (int v = 0; v < kMaxVoices; ++v)
            voices_[static_cast<std::size_t> (v)].setBaseDrift (
                spreadDrift (amount, appliedOff (v, SpreadTarget::ConvDrift), appliedAmt (v, SpreadTarget::ConvDrift)));
    }

    void CathodeVoiceEngine::prepare (Wide sampleRate)
    {
        sampleRate_ = sampleRate;
        stackGlideLen_ = std::max (1, static_cast<int> (sampleRate * 0.002)); // [T-CGUI-69 leg 3] the dip's 2 ms
        cutSamples_ = std::clamp (static_cast<int> (std::lround (sampleRate * kCutFadeMs * 0.001)), 1, kMaxCutSamples); // [b39 E-tails]
        cutStep_ = 1.0f / static_cast<float> (cutSamples_);
        chordWindowSamples_ = static_cast<std::uint64_t> (std::max (1L, std::lround (sampleRate * kChordWindowMs * 0.001))); // [b35 review 8]
        panGlideSamples_ = std::max (1, static_cast<int> (std::lround (sampleRate * kPanGlideMs * 0.001)));
        stackGlideLeft_ = {};
        // [T-CMP-37 L2] Seat every voice in its lane group BEFORE its stage is prepared (prepare drops a seat's residency).
        // Allocates (the groups' FIRs): off the audio thread, like the voices' own prepare.
        // FIREFORK: fork::Config::lanes = false (a target without SIMD) never prepares the lane groups and keeps every site off.
        f1Lanes_.on = f2Lanes_.on = driveLanes_.on = fork::Config::lanes;
        for (PdSite& site : pdLanes_)
            site.on = fork::Config::lanes;
        for (std::size_t g = 0; fork::Config::lanes && g < f1Lanes_.groups.size(); ++g)
        {
            f1Lanes_.groups[g].prepare (sampleRate, ClassicFilter::kDefaultOversample);
            f2Lanes_.groups[g].prepare (sampleRate, ClassicFilter::kDefaultOversample);
            driveLanes_.groups[g].prepare (FilterDrive::kDefaultOversample);
            for (PdSite& site : pdLanes_)
                site.groups[g].prepare (PhaseDistortionOscillator::kDefaultOversample);
        }
        for (std::size_t u = 0; u < voices_.size(); ++u)
        {
            f1Lanes_.slots[u] = { &f1Lanes_.groups[u / 4], static_cast<int> (u % 4), false };
            f2Lanes_.slots[u] = { &f2Lanes_.groups[u / 4], static_cast<int> (u % 4), false };
            voices_[u].voice().setClassicLaneSlot (&f1Lanes_.slots[u]);
            voices_[u].voice().setClassicLaneSlot2 (&f2Lanes_.slots[u]);
            driveLanes_.slots[u] = { &driveLanes_.groups[u / 4], static_cast<int> (u % 4), false };
            voices_[u].outputStage().setDriveLaneSlot (&driveLanes_.slots[u]);
            for (std::size_t slot = 0; slot < pdLanes_.size(); ++slot)
            {
                pdLanes_[slot].slots[u] = { &pdLanes_[slot].groups[u / 4], static_cast<int> (u % 4), false };
                voices_[u].voice().setOscPdLaneSlot (static_cast<int> (slot), &pdLanes_[slot].slots[u]);
            }
        }
        for (auto& v : voices_)
        {
            v.prepare (sampleRate);
            // [T-CMP-20 leaf F] Point every voice at the ONE generic-destination table before any
            // bind arrives. Doing it here rather than only in `bindGenericDest` keeps the semantics
            // bit-identical to the per-voice table this replaced: an unbound destination used to be
            // a null `write` in an owned table, and is now a null `write` in a shared one — never a
            // null TABLE, which is the state a voice would otherwise sit in until the first bind.
            v.setGenericDestTable (&genericDest_);
        }
        // [T-CGUI-44] Each (voice, slot) its own Macro stream. Set HERE, once, not in
        // `applySourceSeeds`: `setSeed` rewinds the stream, and that runs on a drifter.seed change.
        for (int v = 0; v < kMaxVoices; ++v)
            for (int slot = 0; slot < CathodeVoiceStage::kNumSlots; ++slot) // [T-CGUI-57] + SUB's slot
                voices_[static_cast<std::size_t> (v)].outputStage().voice().setMacroSeed (slot, macroSeedFor (v, slot));
        // [T-MOD-01 leaf B] The drifter's rate law is stated in Hz, so the bank has to learn the
        // sample rate here; `DrifterBank::prepare` also resets, and `reset()` below re-does that.
        drifters_.prepare (sampleRate);
        setGlideTime (glideSec_); // resolve glideDecay_ at the (now-known) sample rate
        setUnisonGrace (graceSec_); // resolve graceSamples_ at the (now-known) sample rate
        setStealFade (stealFadeSec_); // resolve the steal-fade window at the (now-known) sample rate
        resolveUnison(); // fill the detune/pan tables (default U=1 → mult 1.0, no pan)
        resolveSpread(); // fill the offset/tune-mult tables (default spread 0 → mults exactly 1.0)
        reapplyPatch(); // re-forward SET patch bases (voice prepare re-primes e.g. the converter rate)
        if (hasSourceSeed_)
            applySourceSeeds(); // re-fan the per-voice seeds (robust to a pre-prepare setSourceSeed)
        reset(); // ends by re-applying the spread phases (after the voices' reset zeroes them)
    }

    void CathodeVoiceEngine::reset() noexcept
    {
        for (std::size_t i = 0; i < voices_.size(); ++i)
        {
            voices_[i].reset();
            voiceNote_[i] = -1;
            voiceHeld_[i] = false;
            voiceAge_[i] = 0;
            voiceGroup_[i] = 0;
            voiceReleaseAt_[i] = 0;
            fadePhase_[i] = FadePhase::None; // drop any in-flight steal-fade (pending included) — and a cut
            fadeGain_[i] = 1.0f;
            silentRun_[i] = 0; // [D-M-0574] a reset voice ticks again until it has held silence for the skip run
            f1Lanes_.slots[i].drop(); // [T-CMP-37 L2] the voice's reset zeroed its scalar core: a lane's copy of the state is dead
            f2Lanes_.slots[i].drop(); // [L3] …and F2's
            driveLanes_.slots[i].drop(); // [L3] …and the output drive's
            pdLanes_[0].slots[i].drop(); // [L3] …and the PD decimators'
            pdLanes_[1].slots[i].drop();
        }
        // [T-MOD-01 leaf B] THE RESET POINT, and it is a decision rather than housekeeping. The walk is
        // a pure function of (seed, samples since reset) — there is no playhead to read (DEFERRED §B
        // HOST-PLAYHEAD) — so WHERE reset() is called is what "bounce-parity" means for this source.
        // Anchoring it to the ENGINE's own reset puts the drifter on exactly the same footing as every
        // other seeded stream here (`rotState_` below, the voices' LFO phases and source RNGs): a
        // render started from a prepared/reset engine is bit-identical however it is chunked, and
        // re-rendering the same project offline reproduces it. It deliberately does NOT reset on
        // note-on — a drifter that restarted per note would be an envelope.
        drifters_.reset();
        // [b39 E-tails] every voice is silent now: nothing is owed a fade. A panic/reset() during the cut's declick ends
        // it here too — the output steps to 0 on this sample, by contract (a hard stop is a hard stop).
        cutLeft_ = 0;
        outRing_.fill ({}); // [b35 review 8] a cut after this declicks from the silence since, never from before it
        outRingPos_ = 0;
        poolSoftOwed_ = false; // [b39 review 3]
        cutInFlight_ = false; // [b39 review 2] …and a held-back note does not outlive a reset (no stuck notes)
        deferredCount_ = 0;
        cutDoneSerial_ = cutSeenSerial_; // [b39 review 5] every voice is silent: whatever was cut has landed
        ageCounter_ = 0;
        groupCounter_ = 0;
        renderSpan_ = isMonophonic() ? resolvedStack_ : polySpan(); // [T-CGUI-76] every voice past it is reset-clean
        lastTriggered_ = -1; // [T-CGUI-15]
        rotNext_ = 0;
        rotState_ = rotateSeed_; // reseed the RotateRandom stream (§6 deterministic re-render)
        sampleClock_ = 0;
        loneVoice_ = -1; // [b35 review 5]
        for (std::size_t u = 0; u < panGlideLeft_.size(); ++u)
            endPanGlide (u); // [b35 review 8/9] a glide in flight lands on its target
        monoCount_ = 0;
        lastMonoNote_ = -1; // [T-CGUI-79 G6]
        currentHz_ = 0.0;
        targetHz_ = 0.0;
        gliding_ = false;
        phaseSlewLeft_ = 0; // [T-CGUI-79 A4] the phases a glide was moving are gone
        // The voices' reset zeroed every LFO phase (and re-seeded each source to its stored seed) —
        // re-apply the spread phases so a render/reset/re-render is bit-identical. Transition-gated:
        // a never-enabled engine still never touches a phase.
        if (lfoPhaseSpread_ > 0.0f)
            applyLfoPhases();
        // [T-CMP-21 leaf C, branch S5R finding 1] …and the SAME argument for the two channel
        // controls, which the voices' reset also zeroed. Without this, a player holding the wheel up
        // lost it on any VOICES change or patch recall — both reach here — and got it back only by
        // moving the wheel again. UNGATED, unlike the phases above: re-broadcasting 0 costs 32 stores
        // and is bit-exact (`exp2(0) == 1.0` exactly), so there is no state to be careful about, and
        // a `> 0.0f` guard would miss a bend held DOWN.
        for (auto& v : voices_)
        {
            v.setModWheel (modWheel_);
            v.setChannelPressure (channelPressure_); // [T-CGUI-95]
            v.setPitchBend (bend_);
        }
    }

    void CathodeVoiceEngine::setVoiceMode (VoiceMode mode) noexcept
    {
        if (mode != mode_)
        {
            mode_ = mode;
            reqMode_.store (mode, std::memory_order_relaxed); // [T-CGUI-79 A4] a later request keeps it
            publishPoolShape(); // [T-CGUI-79 S5R-1]
            resolveUnison(); // [T-CGUI-76 S5R] re-resolve the stack for the new mode
            reset(); // hard stop on mode change — no stuck notes
        }
    }

    void CathodeVoiceEngine::setVoiceCount (int count) noexcept
    {
        int n = count;
        if (n < 1)
            n = 1;
        if (n > kMaxVoices)
            n = kMaxVoices;
        if (n == voiceCount_)
            return;
        if (latchPass_)
        {
            latchVoiceCount (n); // [T-CGUI-76] a routed voice count at note-on: soft
            return;
        }
        voiceCount_ = n;
        reqCount_.store (n, std::memory_order_relaxed); // [T-CGUI-79 A4] a later request keeps it
        publishPoolShape(); // [T-CGUI-79 S5R-1]
        resolveUnison(); // [T-CGUI-76 S5R] re-resolve the stack tables
        reset(); // clear any orphaned rings outside the new range
    }

    // [T-CGUI-79 A4] The request half — see the header. Each stores its field and bumps the serial
    // only when the requested value moved, so a whole-table re-apply of an unchanged shape costs one
    // exchange per row and the audio thread sees nothing new.
    // [T-CGUI-79 S5R-1] ATOMICS ONLY, from either thread. These used to branch on `latchPass_` — a plain
    // bool the audio thread toggles — so a message-thread request landing mid-pass took the SOFT latch on
    // the message thread, writing the pool shape there and never recording the request. The latch pass
    // now applies what its writers requested itself (`applyPoolRequest (true)`), on the audio thread.
    void CathodeVoiceEngine::requestVoiceCount (int count) noexcept
    {
        const int n = std::clamp (count, 1, kMaxVoices);
        if (reqCount_.exchange (n, std::memory_order_relaxed) != n)
            bumpPoolRequest();
    }

    void CathodeVoiceEngine::requestUnison (int count) noexcept
    {
        const int n = std::clamp (count, 1, kMaxUnison);
        if (reqUnison_.exchange (n, std::memory_order_relaxed) != n)
            bumpPoolRequest();
    }

    void CathodeVoiceEngine::requestVoiceMode (VoiceMode mode) noexcept
    {
        if (reqMode_.exchange (mode, std::memory_order_relaxed) != mode)
            bumpPoolRequest();
    }

    void CathodeVoiceEngine::requestVoiceAlloc (VoiceAlloc alloc) noexcept
    {
        if (reqAlloc_.exchange (alloc, std::memory_order_relaxed) != alloc)
            bumpPoolRequest();
    }

    void CathodeVoiceEngine::latchVoiceCount (int n) noexcept
    {
        voiceCount_ = n;
        publishPoolShape(); // [T-CGUI-79 S5R-1]
        resolveUnison();
        if (!isMonophonic() && polySpan() > renderSpan_)
            renderSpan_ = polySpan(); // grown into reset-clean voices; a LOWER limit leaves the span alone
    }

    void CathodeVoiceEngine::setGlideTime (float seconds) noexcept
    {
        glideSec_ = seconds < 0.0f ? 0.0f : seconds;
        const Real n = static_cast<Real> (glideSec_) * sampleRate_;
        // −60 dB of the frequency gap at glideTime (the ModalResonator / AdsrEnvelope T60 convention);
        // n < 1 sample → instant (glideDecay_ 0 → the gap collapses in one step / the noteOn jumps).
        glideDecay_ = (n < 1.0) ? 0.0 : std::pow (10.0, -3.0 / n);
    }

    void CathodeVoiceEngine::cutSounding() noexcept
    {
        bool any = false;
        for (int i = 0; i < renderSpan_; ++i)
        {
            const auto u = static_cast<std::size_t> (i);
            if (fadePhase_[u] == FadePhase::None && !voices_[u].active())
                continue;
            // [b35 review 3] Ended NOW, not faded through the voice: the load applies the incoming patch to every voice
            // right after its request, so a voice still rendering would re-voice the outgoing note mid-fade (a new filter
            // model or oscillator mode). The declick is the engine's last kCutFadeMs of output, mirrored and ramped to 0
            // (`processSample`, [b35 review 8]). A pending steal swap is dropped with it: its note was cut too.
            endCut (u);
            voiceHeld_[u] = false; // a releasing voice to the allocator, and its key-up finds nothing
            any = true;
        }
        monoCount_ = 0; // the mono key stack goes with its notes: the next note is a fresh one from silence
        if (any)
        {
            int r = outRingPos_; // [b35 review 8] newest first: the tail starts where the output left off
            for (int n = 0; n < cutSamples_; ++n)
            {
                r = r == 0 ? cutSamples_ - 1 : r - 1;
                cutTail_[static_cast<std::size_t> (n)] = outRing_[static_cast<std::size_t> (r)];
            }
            cutLeft_ = cutSamples_;
            cutInFlight_ = true;
        }
    }

    // [b39 review 2] The cut's declick has run its course. A reshape the cut held back lands on silence, and the note
    // events it held back replay IN ORDER — every cut voice went Idle when the cut started ([b35 review 3]), so each new
    // note starts from silence at full gain: nothing it could have stolen was still sounding.
    void CathodeVoiceEngine::landCut() noexcept
    {
        cutLeft_ = 0;
        cutInFlight_ = false;
        cutDoneSerial_ = cutSeenSerial_; // [b39 review 5]
        const int held = deferredCount_; // BEFORE the reshape: a hard one `reset()`s, which empties the queue
        applyPoolRequest();
        for (int i = 0; i < held; ++i)
        {
            const DeferredNote& d = deferred_[static_cast<std::size_t> (i)];
            if (d.on)
                noteOnNow (d.note, d.velocity01, d.noteOrdinal);
            else
                noteOffNow (d.note);
        }
        deferredCount_ = 0;
    }

    void CathodeVoiceEngine::deferNote (bool on, int note, float velocity01, std::uint32_t noteOrdinal) noexcept
    {
        if (deferredCount_ < kMaxDeferredNotes)
        {
            deferred_[static_cast<std::size_t> (deferredCount_)] = { note, velocity01, noteOrdinal, on };
            ++deferredCount_;
            return;
        }
        // Full (more than kMaxDeferredNotes events inside 2 ms — a stated limit): a note-on is dropped, and a key-up
        // cancels its own queued note-on, so the queue can never leave a note stuck.
        if (on)
            return;
        for (int i = deferredCount_ - 1; i >= 0; --i)
            if (deferred_[static_cast<std::size_t> (i)].on && deferred_[static_cast<std::size_t> (i)].note == note)
            {
                for (auto k = static_cast<std::size_t> (i); k + 1 < static_cast<std::size_t> (deferredCount_); ++k)
                    deferred_[k] = deferred_[k + 1];
                --deferredCount_;
                return;
            }
    }

    void CathodeVoiceEngine::noteOn (int midiNote, float velocity01, std::uint32_t noteOrdinal) noexcept
    {
        applyCutRequest(); // [b39 E-tails] a load before this note silences what sounded before it
        // [b39 review 2] Inside the cut the note WAITS for it (≤ 2 ms): taking a cut voice at unity stepped its fade back
        // up and attacked from the old AMP level, and landing the cut early (for a pending reshape) stepped it to 0.
        if (cutLeft_ > 0)
        {
            deferNote (true, midiNote, velocity01, noteOrdinal);
            return;
        }
        if (cutInFlight_)
            landCut();
        noteOnNow (midiNote, velocity01, noteOrdinal);
    }

    void CathodeVoiceEngine::noteOnNow (int midiNote, float velocity01, std::uint32_t noteOrdinal) noexcept
    {
        applyEngineRequests(); // [T-CGUI-81] before the pool request: a base's write-through may request one
        applyPoolRequest(); // [T-CGUI-79 A4] a reshape requested before this note shapes its allocation
        // [T-CGUI-46 leaf 2] The LATCH point: a NoteOn-rate engine dest (voices/rotCount/unison) is
        // written HERE, before allocation, from the newest voice sounding before this note.
        if (engineBoundCount_ > 0)
            applyEngineDests (true);
        if (isMonophonic())
            noteOnMono (midiNote, velocity01, noteOrdinal);
        else
            noteOnPoly (midiNote, velocity01, noteOrdinal);
    }

    void CathodeVoiceEngine::noteOff (int midiNote) noexcept
    {
        applyCutRequest(); // [b39 E-tails] a cut note's key-up then ends nothing
        if (cutLeft_ > 0 && deferredCount_ > 0)
        {
            deferNote (false, midiNote, 0.0f, 0); // [b39 review 2] its note-on may be waiting: keep the order
            return;
        }
        if (cutLeft_ == 0 && cutInFlight_)
            landCut();
        noteOffNow (midiNote);
    }

    void CathodeVoiceEngine::noteOffNow (int midiNote) noexcept
    {
        applyEngineRequests(); // [T-CGUI-81]
        if (cutLeft_ == 0) // [b39 E-tails] mid-cut the reshape waits for the fade (`processSample` lands it)
            applyPoolRequest(); // [T-CGUI-79 A4] a reshape requested before this key-up has already cut it
        if (isMonophonic())
            noteOffMono (midiNote);
        else
            noteOffPoly (midiNote);
    }
} // namespace nemesis::dsp
