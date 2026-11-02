#pragma once

#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/ForkConfig.h>
#include <nemesis/dsp/AdsrEnvelope.h>
#include <nemesis/dsp/BypassFade.h>
#include <nemesis/dsp/CathodeVoiceStage.h>
#include <nemesis/dsp/DrawEnvelope.h>
#include <nemesis/dsp/FilterDrive.h>
#include <nemesis/dsp/FilterDriveLanes.h>
#include <nemesis/dsp/LevelLaw.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace nemesis::dsp
{
    // [T-CGUI-61 leaf E6] A `DrawEnvelope` READ THE WAY AN `AdsrEnvelope` IS. The matrix reads every
    // envelope source BEFORE the sample that applies it (`AdsrEnvelope::value()` is the value the next
    // `processSample()` returns — "no lag"), and `DrawEnvelope` has no such peek: it is read-then-
    // advance with no readout of the pending sample. So the first read of a sample TICKS it and caches
    // the result, and `take()` hands that same value to the sample that applies it. `drop()` discards a
    // cached tick on a gate event (the envelope's state just changed under it; one sample of pos is
    // skipped, which no ear or assertion can place). Pure arithmetic; RT-safe.
    class DrawEnvelopeTap
    {
    public:
        DrawEnvelope env;

        [[nodiscard]] float peek() noexcept
        {
            if (!ticked_)
            {
                next_ = env.processSample();
                ticked_ = true;
            }
            return next_;
        }
        float take() noexcept
        {
            const float v = peek();
            ticked_ = false;
            return v;
        }
        void drop() noexcept { ticked_ = false; }

        // [T-CGUI-61 S5R3] `.dtime`, ANY THREAD: only the request (ms) is stored — `setLengthMs`
        // read-modify-writes pos/length/loop and `cur_`, which raced the audio thread's advance/reset
        // (an Idle envelope left holding `cur_` ≠ 0: stuck DC). NaN/negative → 0, which the length
        // clamp maps to the minimum exactly as `setLengthMs` would, so the compare below can settle.
        void requestLengthMs (Real ms) noexcept { lenReqMs_.store (ms > 0.0 ? ms : 0.0, std::memory_order_relaxed); }
        // AUDIO THREAD, in the DRAW edge step: applies a changed request (unchanged: one branch).
        void applyLengthRequest() noexcept
        {
            const Real req = lenReqMs_.load (std::memory_order_relaxed);
            if (req == lenMs_)
                return;
            lenMs_ = req;
            env.setLengthMs (lenMs_);
        }

    private:
        float next_ = 0.0f;
        bool ticked_ = false;
        std::atomic<Real> lenReqMs_ { DrawEnvelope::kDefaultLengthSeconds * 1000.0 }; // REQUESTED — any thread (row 303)
        Real lenMs_ = DrawEnvelope::kDefaultLengthSeconds * 1000.0; // APPLIED — audio thread only
    };

    // CathodeOutputStage — CATHODE's §7 OUTPUT stage: "AMP envelope → drive/level → pan → into the
    // track-FX chain. Per-voice → summed to the track." A pure COMPOSITION of three signed-off
    // primitives into the complete monophonic voice signal path — it owns the tone path AND the AMP
    // envelope, so it is the note-level unit the §6 voice allocator (M1-10c) will instantiate ×N:
    //
    //     CathodeVoiceStage (§0/§2–3, M1-09fk)   osc → grit → filter    (mono, pre-amp)
    //       → · AdsrEnvelope (§4/§7, M1-10a)      AMP DADSR, gate-driven (env, 0..1)
    //       → · velGain                           velocity → amp gain (γ ≈ 1.5)
    //       → FilterDrive (§7 "drive", M1-09fa)   output soft-clip saturate (oversampled)
    //       → · level                             output level (linear)
    //       → equal-power PAN                     mono → stereo (L,R)
    //
    // Signal: (L,R) = pan( level · outputDrive( env · velGain · voice ) ). This is the FIRST stereo /
    // multi-channel path in the engine (everything upstream is mono — except a Stereo filter route,
    // stereo voice S2, which hands this stage an L/R pair: each channel its own drive, the pan per
    // channel; see `processSample`). The per-sample DSP of each
    // primitive is golden-tested; the genuinely-new logic here is the serial wiring plus three small
    // pieces — the velocity→amp gain, the output soft-clip drive (a SECOND FilterDrive, distinct from
    // the §3 in-filter drive that lives inside the voice stage), and the equal-power pan law. The
    // correctness anchor is therefore FAITHFUL COMPOSITION: the stage is bit-identical to a
    // hand-composed chain of the same standalone primitives configured identically (the acceptance
    // keystone) — NOT the golden (a §4.6 drift-guard on our chosen composition).
    //
    // The output "drive" is a genuinely SEPARATE §7 stage (owner decision, 2026-07-14), and it is the
    // analog family, so — like the §3 filter drive — it is OVERSAMPLED to suppress fold-back aliasing
    // (§11); `outputDrive = 0` is a bit-exact bypass at zero OS cost. Because it is oversampled it adds
    // group delay when active and therefore slightly time-smears the env·voice product relative to the
    // un-delayed envelope — inaudible and deterministic (bounce-parity holds; latency ALIGNMENT / PDC
    // is a voice/engine concern, the M1-08b free-running stance). latencySamples() sums every reported
    // through-path delay (the voice stage's own drive/Scream/Acid latency + this output drive) for the
    // caller to align.
    //
    // VELOCITY (§7, a voice concern the pure-0..1 env deliberately leaves out, M1-10a): a per-note
    // static gain velGain = (1 − velSens) + velSens · velocity^γ, γ ≈ 1.5 (the PunchTransient §5
    // convention). velSens = 0 → velGain = 1 (velocity has no effect); at the defaults (velocity 1,
    // velSens 1) → velGain = 1 (transparent). Resolved control-rate (pow in the .cpp).
    //
    // PAN (§7, §9 synth.pan): a constant-power law over the physical channel powers —
    // panL = √(½(1 − pan)), panR = √(½(1 + pan)), pan ∈ [−1,1] — so panL² + panR² = 1 (equal power),
    // centre (pan 0) is −3 dB on both channels, and the hard endpoints are EXACT (the silent channel's
    // gain is exactly 0). Resolved control-rate (sqrt in the .cpp).
    //
    // DEFERRED: voice allocation / rotation / glide / unison / stealing / anti-click / per-voice spread
    // (§6 — M1-10c, which owns N of these); the §4 mod-matrix wire-in (env→dest, LFO/S&H/step); the
    // punch onset-transient fold-in; the downstream track-FX chain (§7, L2+); a dB/perceptual level or
    // pan taper; per-osc/stereo pan spread (one pan per voice — applied per channel to a Stereo-route pair).
    //
    // RT-safety (§11): prepare() prepares the voice, the AMP envelope and the output-drive oversampler
    // (off the audio thread). Setters resolve their coefficients at control rate (the only libm — pow
    // for velGain, sqrt for pan — lives in the .cpp). processSample() is inline and calls only the
    // primitives' inline hot paths (the voice-stage capture/drive lambdas and this drive's shaper
    // lambda inline with no std::function allocation); the μ-law grit path calls RT-safe libm
    // (owner-accepted M1-05 deviation). Deterministic given the voice's drift seed (§11) → bit-identical
    // re-render; reset() reseeds it. No JUCE. Not thread-safe against prepare().
    class CathodeOutputStage
    {
    public:
        // One per-voice stereo output sample (post pan). Summed to the track by the voice/engine layer.
        // [stereo voice S1] The voice stage's own pair type: the tone path hands this stage L/R already.
        using StereoSample = CathodeVoiceStage::StereoSample;

        // Prepares the tone path (osc tables/decimators, filter, converter buffers), the AMP envelope
        // (fs → its stage coefficients) and the output-drive oversampler FIRs. Not RT-safe (allocates).
        void prepare (Wide sampleRate)
        {
            voice_.prepare (sampleRate);
            amp_.prepare (sampleRate);
            draw_.env.prepare (sampleRate); // [T-CGUI-61 leaf E6]
            tailFade_.prepare (sampleRate);
            drawXfLen_ = std::max (1, static_cast<int> (std::lround (sampleRate * kDrawCrossfadeSeconds)));
            resetDraw();
            velGlideLen_ = std::max (1, static_cast<int> (sampleRate * kVelGlideSeconds));
            outDrive_.prepare (fork::Config::driveOversample); // FIREFORK N5; §7 output drive: sample-rate-agnostic; default 0 → bit-exact bypass
            if (driveSlot_ != nullptr)
                driveSlot_->drop(); // [T-CMP-37 L3] prepare cleared the scalar drive
            outDriveR_.prepare (fork::Config::driveOversample); // FIREFORK N5; [stereo voice S2] the right channel's — the same design, so its state can be copied
            rLive_ = false;
            updatePan();
            updateVelGain();
        }

        // Clears the tone-path state (osc phase, converter state + RESEEDED drift, filter integrators),
        // the AMP envelope (→ Idle) and the output-drive state (ADAA memory + OS FIR). RT-safe. Does not
        // change parameters; a subsequent gateOn() re-runs the envelope.
        void reset() noexcept
        {
            voice_.reset();
            amp_.reset();
            resetDraw(); // [T-CGUI-61 leaf E6]
            drawXfLeft_ = 0; // [row 304] nothing sounding to cross from
            outDrive_.reset();
            if (driveSlot_ != nullptr)
                driveSlot_->drop(); // [T-CMP-37 L3] the scalar drive was just zeroed: a lane's copy of the old state is dead
            outDriveR_.reset();
            rLive_ = false;
            mixPoint_ = 0.0f; // [T-CORE-30 leaf D] a reset voice must not hand a stale sample to punch
        }

        // [b39 E-tails] The AMP envelope (and a drawn AMP) to silent Idle, and NOTHING ELSE: the tone path — oscillator
        // phases, filters, converter — keeps free-running (owner ruling: phases survive a preset load). The engine calls
        // it at the START of a cut ([b35 review 3]): the voice is silent from then on, and the engine's own declick (its
        // recent output, mirrored and faded) covers the step, so nothing audible steps.
        void silenceEnvelopes() noexcept
        {
            amp_.reset();
            resetDraw();
            drawXfLeft_ = 0;
            // [b39 review 2] …and the output drive's memory (ADAA + OS FIR), which sits AFTER the AMP: its history is
            // the note at full level. Left in, it plays out for the FIR's span after the AMP reset — a burst of the cut
            // note on the next note's onset. Cleared here with the AMP.
            outDrive_.reset();
            if (driveSlot_ != nullptr)
                driveSlot_->drop(); // [T-CMP-37 L3] …and a lane's copy of it
            outDriveR_.reset();
            rLive_ = false;
            mixPoint_ = 0.0f;
        }

        // ---- Sub-stage access: configure the tone path and the AMP envelope directly ----
        // (Avoids re-delegating the voice stage's ~30 setters and the envelope's a/d/s/r; the output
        // stage owns only the §7-specific params below plus the note-level gate.)
        [[nodiscard]] CathodeVoiceStage& voice() noexcept { return voice_; }
        [[nodiscard]] const CathodeVoiceStage& voice() const noexcept { return voice_; }
        [[nodiscard]] AdsrEnvelope& amp() noexcept { return amp_; }
        [[nodiscard]] const AdsrEnvelope& amp() const noexcept { return amp_; }

        // ---- Note gate (drives the AMP envelope; the whole voice) ----
        // [T-CGUI-61 leaf E6] The drawn envelope is gated ONLY while DRAW is On (S5R): an armed but
        // unticked twin would sit Running forever, and a later flip On would wake every voice ever
        // played. The KEY state is tracked either way, so the On edge (`applyDrawEdge`) re-gates a held
        // note from the drawn start. DRAW Off never touches the drawn path: bit-exact.
        void gateOn() noexcept
        {
            amp_.gateOn();
            drawHeld_ = true;
            if (drawTicking())
                drawGateOn();
        }
        // [T-CGUI-40] The Mono-RT retrigger: the AMP env re-strikes from zero (see AdsrEnvelope::restrike).
        // A drawn envelope has no restrike law of its own: it re-triggers, its de-click carrying the jump.
        void restrike() noexcept
        {
            amp_.restrike();
            drawHeld_ = true;
            if (drawTicking())
                drawGateOn();
        }
        void gateOff() noexcept
        {
            amp_.gateOff();
            drawHeld_ = false;
            if (drawTicking())
            {
                draw_.drop();
                draw_.env.gateOff();
            }
        }
        // Whether the key is down (gateOn/restrike .. gateOff), whichever envelope is selected.
        [[nodiscard]] bool keyHeld() const noexcept { return drawHeld_; }
        // True while the voice is sounding (the AMP envelope is running); false once it frees itself
        // after release — the voice-alloc layer reclaims the voice on this edge.
        //
        // [T-CGUI-61 leaf E6] THE DRAW-MODE RECLAIM RULE. A drawn AMP voice is FREE only when the gate
        // is off AND the drawn envelope has finished its tail AND it is silent. `DrawEnvelope` goes
        // inactive at pos 1 even when the curve ENDS ABOVE 0 (it then holds that value forever), so
        // `isActive()` alone would cut a sounding voice dead — a click. Instead:
        //   * key held: a finished one-shot HOLDS its last value and keeps sounding (active);
        //   * key up, tail finished at 0: free at once — nothing is sounding;
        //   * key up, tail finished ABOVE 0: `tailFade_` (the voice's existing `BypassFade`, the stage-
        //     bypass law — linear, 10 ms, landing exactly on 0) fades the held value out; the voice is
        //     free the sample the fade lands, and the drawn envelope is reset to silent Idle there so
        //     the next note starts from the drawn start exactly.
        [[nodiscard]] bool active() const noexcept
        {
            const bool drawn = drawHeld_ || draw_.env.isActive() || (drawLast_ > 0.0f && !tailFade_.parked());
            if (drawXfLeft_ > 0) // [row 304] both sources sound through a DRAW crossfade
                return amp_.active() || drawn;
            return ampDraw_ ? drawn : amp_.active();
        }

        // ---- [T-CGUI-61 leaf E6] DRAW mode: the AMP envelope from a DRAWN curve ----------------
        // `synth.amp.draw`. On: the stage's env comes from `drawAmp()` (curve, length and loop set by
        // the owner) instead of `amp()`; velocity, drive, level and pan apply exactly as before. Off:
        // the ADSR path, bit-exact. A flip mid-note switches at the next sample (a patch edit).
        // [S5R2] ANY THREAD: the setter only STORES the request (a plain flag store, the other row
        // setters' kind). The edge is applied by `applyDrawEdge()` on the AUDIO thread — a setter
        // that reset/re-gated here raced `gateOff` and could latch the key held (a stuck voice).
        // [row 303] The request is a relaxed atomic: a store from any thread, one load per sample.
        void setAmpDraw (bool on) noexcept
        {
            ampDrawReq_.store (on, std::memory_order_relaxed);
            drawDirty_.store (true, std::memory_order_release); // FIREFORK E9
        }
        // `synth.amp.dtime` — ANY THREAD, request only; `applyDrawEdge()` applies it (S5R3).
        void setAmpDrawLengthMs (Real ms) noexcept
        {
            draw_.requestLengthMs (ms);
            drawDirty_.store (true, std::memory_order_release); // FIREFORK E9
        }
        // AUDIO THREAD, at the top of a sample (`processSample`, and the modulated voice's before it
        // reads `ampEnvValue`). Applies a changed `.dtime` request, then the DRAW flip. The On EDGE
        // starts the drawn envelope clean: from its drawn start if the key is held NOW, silent Idle
        // otherwise — never from state left over from before the flip. Steady state: two predictable
        // not-taken branches, no writes (so DRAW Off stays bit-exact).
        // [row 304] A flip on a SOUNDING voice crossfades the two sources over kDrawCrossfadeSeconds
        // (the drawn envelope's own de-click time); on a silent one it lands at once.
        void applyDrawEdge() noexcept
        {
            if constexpr (fork::kExact) // FIREFORK E9
            {
                if (!drawDirty_.load (std::memory_order_acquire))
                    return;
                drawDirty_.store (false, std::memory_order_relaxed);
            }
            draw_.applyLengthRequest();
            const bool req = ampDrawReq_.load (std::memory_order_relaxed);
            if (req == ampDraw_)
                return;
            const bool sounding = active();
            const int left = drawXfLeft_;
            ampDraw_ = req;
            if (left > 0)
            {
                // [D-M-0173] A flip INSIDE a running crossfade turns it round: the sounding mix is
                // `left / N` of the source that was fading OUT, so the weight carries on from there
                // (new count N - left) instead of restarting from the pure old source — a step. Both
                // sources have been ticking through the fade, so the draw source is still live: no reset.
                drawXfLeft_ = drawXfLen_ - left;
                return;
            }
            if (ampDraw_)
            {
                const bool held = drawHeld_;
                resetDraw();
                if (held)
                    drawGateOn();
            }
            drawXfLeft_ = sounding ? drawXfLen_ : 0;
        }
        // The REQUESTED mode (the patch state); the audio path follows it from its next sample.
        [[nodiscard]] bool ampDraw() const noexcept { return ampDrawReq_.load (std::memory_order_relaxed); }
        [[nodiscard]] DrawEnvelope& drawAmp() noexcept { return draw_.env; }
        [[nodiscard]] const DrawEnvelope& drawAmp() const noexcept { return draw_.env; }
        // Env1 — the AMP gain THIS sample will apply, read before `processSample` (the matrix source).
        // ADSR: `amp().value()`, exactly as before. DRAW: the tap's peek, which `processSample` then
        // consumes, so the source and the applied gain are one value (no lag either way).
        // [row 304] Through a DRAW crossfade, the same blend `processSample` applies.
        [[nodiscard]] float ampEnvValue() noexcept
        {
            if (drawXfLeft_ > 0)
                return drawBlend (amp_.value(), draw_.peek());
            return ampDraw_ ? draw_.peek() : amp_.value();
        }
        // True while a DRAW flip's crossfade runs (for tests).
        [[nodiscard]] bool drawCrossfading() const noexcept { return drawXfLeft_ > 0; }
        // The reclaim fade's gain (1 = not fading), for tests.
        [[nodiscard]] float drawTailGain() const noexcept { return tailFade_.gain(); }

        // ---- The §22.8 LEVEL DOMAIN (T-CORE-09) ------------------------------------------------
        // APP §22.8's Levels row is "−∞…0 dB at v = 100 → +12 dB at 127" (M1-21 locked it as
        // square-law gain below unity, then linear-in-dB above), so the linear gain domain is
        // [0, 10^(12/20)] and NOT [0,1] — v = 100…127 is a make-up-gain region, 28 of 128 values.
        // The ceiling lives HERE because this stage is the destination every level path ends at:
        // CathodeVoiceEngine's level SPREAD and CathodeModulatedVoice's live Amp modulation both
        // clamp to these constants rather than keeping their own, so the three cannot drift apart.
        // The law is shared by seven rows across five engines (`synth.level` · `prism.level` ·
        // `sampler.src.gain` · `sampler.amp.level` · `sampler.sample.gain` · `magma.amp.level` ·
        // `dross.body.level`), and `sampler.amp.level` DEFAULTS to 110 — documented "≈ +1 dB" — so a
        // [0,1] ceiling would have made a spec-stated default silently wrong (owner ruling 2026-07-27).
        // CLIPPING above unity is therefore reachable, and that is the point: it is user-chosen
        // make-up gain, and CATHODE.md's §7 chain has no output saturator to hide it.
        static constexpr float kMaxLevelDb = LevelLaw::kMaxLevelDb; // [D-M-0243] the law's one home
        static constexpr float kMaxLevel = LevelLaw::kMaxLevel;

        // ---- §7 output params (§9 synth.amp velocity, synth.level, synth.pan) ----
        // Note velocity in [0,1]; scaled by velSens into the amp gain. Control-rate (pow in the .cpp).
        void setVelocity (float velocity01) noexcept;
        // [T-CGUI-69 leg 3, owner 2026-09-25] A new velocity on a SOUNDING voice (a mono note-on onto a
        // held stack, legato or Mono-RT): the gain glides there over 2 ms instead of stepping — the
        // restrike dip's own length, so a Mono-RT note speaks at the new gain when its dip lands.
        void glideVelocity (float velocity01) noexcept;
        // Velocity sensitivity in [0,1]: 0 → velocity has no effect (velGain = 1); 1 → velGain =
        // velocity^γ. Control-rate.
        void setVelSens (float sens01) noexcept;
        // Output-stage drive amount in [0,1] (§7 "drive"): 0 = bit-exact bypass; higher = more
        // oversampled soft-clip saturation. Distinct from the §3 in-filter drive (voice().setDrive()).
        void setOutputDrive (float amount) noexcept
        {
            outDrive_.setDrive (amount);
            outDriveR_.setDrive (amount); // [stereo voice S2]
        }
        // Output level, linear gain in [0, kMaxLevel] (§9 synth.level / the §22.8 Levels law: unity
        // at v = 100, up to +12 dB of make-up above it). Pure arithmetic → inline. Control-rate.
        void setLevel (float level) noexcept { level_ = clampLevel (level); }
        // Stereo pan in [−1,1] (§9 synth.pan): −1 hard left, 0 centre, +1 hard right; equal-power.
        // Control-rate (sqrt in the .cpp).
        void setPan (float pan) noexcept;

        // ---- Accessors (metering / tests read the exact scalars the hot path multiplies) ----
        [[nodiscard]] float velGain() const noexcept { return velGain_; }
        [[nodiscard]] float level() const noexcept { return level_; }
        [[nodiscard]] float panGainLeft() const noexcept { return panL_; }
        [[nodiscard]] float panGainRight() const noexcept { return panR_; }
        // Total reported through-path latency for the caller to align (§11): the voice stage's own
        // active-mode latencies (filter drive + the active oversampled model core — Ladder / Scream /
        // Acid / Soviet / Classic / Reed; mode-exclusive, so at most one model core is nonzero) plus
        // this output drive's. The LADDER term is new in T-CMP-23 and is 0 unless the drive is in-loop
        // and above zero, so an undriven Ladder patch reports exactly what it did before the growl.
        [[nodiscard]] int latencySamples() const noexcept
        {
            // [T-CGUI-60] …through `filterLatencySamples`, which is exactly that F1 sum while F2 is Off
            // and adds F2 per the route (Series sums, Parallel takes the longer) while it is On.
            return voice_.filterLatencySamples() + outDrive_.latencySamples();
        }

        // The MIX-POINT sample the last processSample() produced: `env · velGain · voice`, i.e. the
        // voice's own mono signal after the AMP envelope and before drive/level/pan.
        //
        // [T-CORE-30 leaf D] Read-back for the same reason every other accessor on this stage is one
        // (velGain / level / panGain*): it is the exact scalar the hot path used, so a test pins the
        // composition rather than inferring it. Its live consumer is the punch transient's
        // `ringmod_zing` flavour (punch_transient.md §4.5), the ONE flavour that derives its output
        // from the voice — it reads the PREVIOUS sample here, because punch and voice are summed in
        // the same sample and something has to be first. One sample at 48 kHz is 8.6° of phase at
        // the 1.2 kHz ring carrier: inaudible, deterministic, and it keeps the whole path
        // feed-forward instead of threading a callback through two stages.
        [[nodiscard]] float mixPointSample() const noexcept { return mixPoint_; }

        // Produces one per-voice STEREO sample: (L,R) = pan( level · outputDrive( env · velGain · voice
        // + auxPostAmp ) ). Hot path: the primitives' inline hot paths + a handful of multiplies. When
        // the AMP env is Idle and nothing is injected it returns exact (0,0) (env = 0 → 0 into the
        // odd-symmetric drive → 0 out).
        //
        // [T-CORE-30 leaf D] The two injection points are the punch transient's two §2 summing
        // options. A caller that passes neither is bit-exact against the pre-card stage — and the
        // mechanism for that is `x + 0.0f == x`, NOT the guard below, which the battery is what
        // established.
        //
        // BATTERY SURVIVOR, CLASSIFIED (c), and the classification corrects what this comment
        // originally claimed. Removing `if (auxPostAmp != 0.0f)` kills nothing: adding a literal
        // 0.0f is the identity for every float a voice can produce EXCEPT −0.0f, where the sum is
        // +0.0f — a different bit pattern that no assertion in this project can see, because float
        // `==` compares the two zeros EQUAL and a 24-bit PCM encode maps both to 0. So the guard is
        // not the reason the OFF path is bit-exact; it protects a sign of zero, and only against a
        // future byte-level comparator.
        //
        // KEPT rather than deleted, which is the T-CORE-02 rule NOT applying: there the guard was
        // provably harmless by CONSTRUCTION, here it is harmless because nothing in the current test
        // corpus lands a −0.0f at an injection point — an accident of the corpus, not a property.
        // The same reasoning `setLfoTrig`'s labelled bound in `CathodeModulatedVoice` records, and
        // the reason the §5 texture layer's own `textureLive_` guard states the identical rule (that
        // one IS load-bearing for a second reason: it also refuses to advance the noise RNG).
        // `auxPreFilter` is forwarded into the tone path (through the filter);
        // `auxPostAmp` joins here, downstream of the filter AND of the AMP envelope — punch carries
        // its own decay envelope by design, so gating it with the voice's would be a second envelope
        // on one burst. It DOES take drive/level/pan, because those are what "voice out" means: a
        // punch that ignored `synth.level` would keep blaring after the voice was turned down.
        StereoSample processSample (float auxPreFilter = 0.0f, float auxPostAmp = 0.0f) noexcept
        {
            processSampleBegin();
            processSamplePre (auxPreFilter);
            processSampleMid (voice_.f1CoreScalar());
            return processSamplePost (voice_.f2CoreScalar(), auxPostAmp);
        }

        // [T-CMP-37 L2] `processSample` in two halves around F1's core (CathodeVoiceStage::processStereoPre): PRE is the DRAW
        // edge and the tone path up to the core's input; POST takes the core's sample and runs the rest — the amp envelope,
        // output drive, level and pan. `processSample` is pre → the scalar core → post.
        // [T-CMP-37 L3] BEGIN is the DRAW edge and the oscillator core's prologue and PD generators; PRE the rest of the tone path
        // up to F1's core input (CathodeVoiceStage::processStereoBegin).
        void processSampleBegin() noexcept
        {
            applyDrawEdge(); // [S5R2] a requested DRAW flip lands here, on the audio thread
            voice_.processStereoBegin();
        }
        void processSamplePre (float auxPreFilter = 0.0f) noexcept { voice_.processStereoPre (auxPreFilter); }

        // [T-CMP-37 L3] …and in three: MID takes F1's core sample and runs the filter stage up to F2's core input
        // (CathodeVoiceStage::processStereoMid); POST takes F2's core sample and runs the rest.
        void processSampleMid (float f1Core) noexcept { voice_.processStereoMid (f1Core); }

        // [T-CMP-37 L3] POST IN TWO HALVES around the output drive, so the engine can run it for four voices at once
        // (FilterDriveLanes). AMP is everything up to the drive's input — the tone path's rest, the AMP envelope, velocity, the
        // post-amp injection; a STEREO voice finishes here entirely (its two drives stay scalar) and TAIL then returns that
        // result. TAIL takes the mono drive's output and applies level and pan. `processSamplePost` is amp → the scalar drive
        // → tail, so there is one code path and the lane-wise drive replaces only the middle call.
        StereoSample processSamplePost (float f2Core, float auxPostAmp = 0.0f) noexcept
        {
            processSampleAmp (f2Core, auxPostAmp);
            return processSampleTail (driveScalar());
        }

        void processSampleAmp (float f2Core, float auxPostAmp = 0.0f) noexcept
        {
            // [stereo voice S1/S2] The tone path's L/R pair. While the voice is mono (L == R) the chain below
            // runs once, on the left value, and pans it — the stage as it was, bit for bit. While its right
            // channel runs (`stereoActive`, the Stereo filter route) the right takes its own output drive,
            // which starts from the left's state: until this sample both drives heard one signal.
            const StereoSample pair = voice_.processStereoPost (f2Core);
            const float voice = pair.left;
            const bool stereo = voice_.stereoActive();
            if (stereo)
                pullDriveFromLane(); // [T-CMP-37 L3] the right drive copies, and both run, from the scalar's CURRENT state
            if (stereo && !rLive_)
                outDriveR_.copyStateFrom (outDrive_); // no allocation: into the storage `prepare` sized
            rLive_ = stereo;
            float env;
            if (drawXfLeft_ > 0) // [row 304] both sources tick; the sounding blend moves old → new
            {
                const float a = tickAmp (!ampDraw_);
                env = drawBlend (a, takeDrawEnv());
                --drawXfLeft_;
            }
            else if (ampDraw_)
            {
                (void) tickAmp (false); // the ADSR stays current in DRAW mode, so a flip Off lands on it
                env = takeDrawEnv(); // [T-CGUI-61 leaf E6]
            }
            else
                env = tickAmp (true);
            if (velGlideLeft_ > 0) // [T-CGUI-69 leg 3]
                velGain_ = --velGlideLeft_ == 0 ? velGainTarget_ : velGain_ + velGlideStep_;
            float amped = voice * env * velGain_; // AMP envelope · velocity
            if (stereo)
            {
                drv_.mono = false;
                drv_.result = stereoTail (amped, pair.right * env * velGain_, auxPostAmp);
                return;
            }
            mixPoint_ = amped;
            if (auxPostAmp != 0.0f)
                amped += auxPostAmp; // §2 post_filter / parallel sum
            drv_.mono = true;
            drv_.in = amped;
        }

        StereoSample processSampleTail (float driven) noexcept
        {
            if (!drv_.mono)
                return drv_.result;
            const float leveled = driven * level_; // output level
            return { leveled * panL_, leveled * panR_ }; // equal-power pan → stereo
        }
        // The scalar drive's sample for the voice AMP just ran (§7 output drive, bypass at 0); 0 for a stereo voice, which TAIL ignores.
        [[nodiscard]] float driveScalar() noexcept { return drv_.mono ? outDrive_.processSample (drv_.in) : 0.0f; }
        [[nodiscard]] float driveInput() const noexcept { return drv_.in; }
        // A lane-wise drive can stand in for the scalar one this sample: a mono voice whose drive is engaged, at the lanes' design.
        [[nodiscard]] bool driveLaneReady (const lanes::DriveLanes4& batch) const noexcept
        {
            return drv_.mono && outDrive_.engaged() && batch.matches (outDrive_);
        }
        [[nodiscard]] FilterDrive& outputDrive() noexcept { return outDrive_; }
        void setDriveLaneSlot (lanes::DriveLaneSlot* slot) noexcept { driveSlot_ = slot; }
        // The scalar drive holds the true state again (a no-op while it is not lane-resident).
        void pullDriveFromLane() noexcept
        {
            if (driveSlot_ != nullptr)
                driveSlot_->pull (outDrive_);
        }

    private:
        // [stereo voice S2] The tail for a split pair: each channel its own drive, both the same level, and
        // the pan law per channel (centre: each at √½ — a Stereo voice with F1 == F2 is the mono voice).
        // The mix point the punch's ring-mod reads is the pair's mid.
        StereoSample stereoTail (float ampedL, float ampedR, float auxPostAmp) noexcept
        {
            mixPoint_ = 0.5f * (ampedL + ampedR);
            if (auxPostAmp != 0.0f)
            {
                ampedL += auxPostAmp;
                ampedR += auxPostAmp;
            }
            const float leveledL = outDrive_.processSample (ampedL) * level_;
            const float leveledR = outDriveR_.processSample (ampedR) * level_;
            return { leveledL * panL_, leveledR * panR_ };
        }

        static constexpr float kVelGamma = 1.5f; // velocity exponent (PunchTransient §5 convention)
        static constexpr Wide kVelGlideSeconds = 0.002; // [T-CGUI-69 leg 3] = the restrike dip (same truncation)

        // clamp01 stays for the two params that ARE 0..1 — setVelocity / setVelSens, in the .cpp.
        static float clamp01 (float v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
        // The level clamp's own ceiling (§22.8), deliberately NOT the shared clamp01: level is the only
        // param on this stage with a make-up region, so the ceiling is named where it applies (T-CORE-09).
        static float clampLevel (float v) noexcept { return v < 0.0f ? 0.0f : (v > kMaxLevel ? kMaxLevel : v); }

        // [row 304] The DRAW crossfade: sample k of N (1-based) weighs the NEW source k/N, so the first
        // sample has already left the old one and the last lands exactly on the new one.
        static constexpr Real kDrawCrossfadeSeconds = DrawEnvelope::kDeclickSeconds;
        [[nodiscard]] float drawBlend (float adsr, float drawn) const noexcept
        {
            const float w = static_cast<float> (drawXfLen_ - drawXfLeft_ + 1) / static_cast<float> (drawXfLen_);
            return ampDraw_ ? adsr + w * (drawn - adsr) : drawn + w * (adsr - drawn);
        }
        [[nodiscard]] bool drawTicking() const noexcept { return ampDraw_ || drawXfLeft_ > 0; }

        // One ADSR sample. `snap`: the envelope is the one SOUNDING (not ticked in the background).
        float tickAmp (bool snap) noexcept
        {
            const bool dipping = amp_.dipping();
            const float a = amp_.processSample();
            // [T-CGUI-69 leg 2] The restrike's dip has landed at 0: the dry leg snaps to this note's
            // latency now, while the note is silent, rather than gliding into it audibly. A note-off
            // mid-dip is NOT a landing: `gateOff` moves Dip→Release outside this function, so `dipping`
            // is already false here (S5R; pinned by the leg 2 case, and a level guard was an equivalent mutant).
            if (snap && dipping && !amp_.dipping())
                voice_.snapGritDryDelay();
            return a;
        }

        // [T-CGUI-61 leaf E6] The drawn gate: re-trigger, and cancel any reclaim fade. From a PARKED
        // (fully faded, silent) fade the gain snaps back to 1 — the drawn envelope was reset to Idle on
        // the landing, so its drawn start is honoured exactly; mid-fade it fades back up from where it
        // is, continuous with the de-clicked re-trigger underneath it.
        void drawGateOn() noexcept
        {
            drawHeld_ = true;
            draw_.drop();
            draw_.env.gateOn();
            const bool wasParked = tailFade_.parked();
            tailFade_.set (false);
            if (wasParked)
                tailFade_.settle();
        }

        void resetDraw() noexcept
        {
            draw_.env.reset();
            draw_.drop();
            drawHeld_ = false;
            drawLast_ = 0.0f;
            tailFade_.set (false);
            tailFade_.settle();
        }

        // One sample of the drawn AMP gain, with the reclaim fade (see `active()`). The fade starts on
        // the first sample the envelope is key-up, finished and still above 0; it lands on EXACTLY 0,
        // and on that sample the envelope resets to silent Idle so nothing is left holding.
        float takeDrawEnv() noexcept
        {
            const float v = draw_.take();
            drawLast_ = v;
            if (!drawHeld_ && v > 0.0f && !draw_.env.isActive() && !tailFade_.bypassed())
                tailFade_.set (true);
            if (tailFade_.inCircuit())
                return v;
            const float g = tailFade_.gain();
            if (tailFade_.tick())
            {
                draw_.env.reset();
                draw_.drop();
                drawLast_ = 0.0f;
            }
            return v * g;
        }

        void updatePan() noexcept; // pan_ → panL_/panR_ (sqrt, control-rate)
        void updateVelGain() noexcept; // velocity_/velSens_ → velGain_ (pow, control-rate)

        CathodeVoiceStage voice_;
        AdsrEnvelope amp_;
        FilterDrive outDrive_;
        lanes::DriveLaneSlot* driveSlot_ = nullptr; // [T-CMP-37 L3] outDrive_'s seat in the engine's lane batch (null: scalar only)
        // What AMP leaves for the drive and TAIL: the drive's input (mono), or a stereo voice's finished result.
        struct DriveStash
        {
            StereoSample result {};
            float in = 0.0f;
            bool mono = true;
        };
        DriveStash drv_;
        FilterDrive outDriveR_; // [stereo voice S2] the right channel's output drive, run while `voice_.stereoActive()`
        bool rLive_ = false; // [stereo voice S2] `outDriveR_` ran last sample (a rising edge copies the left's state)

        // [T-CGUI-61 leaf E6] DRAW mode's state — see `active()` for the reclaim rule it implements.
        DrawEnvelopeTap draw_;
        BypassFade tailFade_; // in circuit (gain 1) except while a finished drawn tail fades out
        bool ampDraw_ = false; // APPLIED — audio thread only (`applyDrawEdge`)
        std::atomic<bool> drawDirty_ { true }; // FIREFORK E9: a draw request may be pending
        std::atomic<bool> ampDrawReq_ { false }; // REQUESTED — `setAmpDraw`, any thread (row 303)
        int drawXfLeft_ = 0; // [row 304] crossfade samples left
        int drawXfLen_ = 240;
        bool drawHeld_ = false; // the key is down (gateOn .. gateOff), whichever envelope is selected — `keyHeld()`
        float drawLast_ = 0.0f; // the drawn value last applied (0 once a fade has landed)

        // §7 output params (control-rate).
        float velocity_ = 1.0f;
        float velSens_ = 1.0f;
        float level_ = 1.0f;
        float pan_ = 0.0f;

        // Resolved control-rate coefficients. Defaults are the pan-centre / unity-velocity values so a
        // freshly-constructed stage (pre-prepare) is already sane; prepare() recomputes them.
        float velGain_ = 1.0f;
        float velGainTarget_ = 1.0f; // [T-CGUI-69 leg 3] where a glide lands, exactly
        float velGlideStep_ = 0.0f;
        int velGlideLeft_ = 0;
        int velGlideLen_ = 96;
        float panL_ = 0.70710678f; // √½
        float panR_ = 0.70710678f;

        // [T-CORE-30 leaf D] The last mix-point sample (see mixPointSample()). Written every
        // processSample, read by the voice layer's ringmod punch flavour.
        float mixPoint_ = 0.0f;
    };
}
