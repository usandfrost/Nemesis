"""Hand patches applied by sync_dsp.py after the mechanical double -> Real rewrite. Each anchor is ASSERTED.

Anchors are written against the text AFTER the rewrite (so `double` appears as `Real` / `Wide`).
Every patch must leave the default build (Real = double, no switches) bit-identical to upstream.
"""
import glob
import os
import re

INC = "include/nemesis/dsp"


def apply(P):
    # ---- 1. lazy Plaits allocation (default OFF = upstream behaviour) -----------------------------------------------
    P.edit(f"{INC}/MacroOscillator.h", "MacroOscillator::isPrepared", [(
        "        void reset() noexcept;\n",
        "        void reset() noexcept;\n\n"
        "        // FIREFORK: true once prepare() has allocated the Plaits storage. An unprepared macro renders silence.\n"
        "        [[nodiscard]] bool isPrepared() const noexcept { return impl_ != nullptr; }\n")])
    P.edit(f"{INC}/CathodeOscCore.h", "CathodeOscCore lazy macro", [
        ("        void setMode (int slot, Mode mode) noexcept\n        {\n            slots_[idx (slot)].mode = mode;\n",
         "        void setMode (int slot, Mode mode) noexcept\n        {\n"
         "            // FIREFORK: with lazy macro on, the ~27 KB Plaits block is allocated the first time a slot is switched to\n"
         "            // Macro, not in prepare(). That allocates: call this from the main loop, never the audio interrupt.\n"
         "            if (mode == Mode::Macro && lazyMacro_ && !slots_[idx (slot)].macro.isPrepared())\n"
         "                slots_[idx (slot)].macro.prepare (sampleRate_);\n"
         "            slots_[idx (slot)].mode = mode;\n"),
        ("        void setMode (int slot, Mode mode) noexcept\n",
         "        // FIREFORK: process-wide switch, set BEFORE prepare(). Off (default) = every slot prepares its Plaits block up\n"
         "        // front (upstream). On = defer it until a slot uses Macro; saves 54.8 KB per voice for patches that never do.\n"
         "        static void setLazyMacro (bool on) noexcept { lazyMacro_ = on; }\n"
         "        [[nodiscard]] static bool lazyMacro() noexcept { return lazyMacro_; }\n\n"
         "        void setMode (int slot, Mode mode) noexcept\n"),
        ("        Wide sampleRate_ = 48000.0;\n        Wide noteHz_ = 0.0;\n",
         "        Wide sampleRate_ = 48000.0;\n        Wide noteHz_ = 0.0;\n        static inline bool lazyMacro_ = false; // FIREFORK\n")])
    P.edit("src/CathodeOscCore.cpp", "CathodeOscCore::prepare macro", [(
        "            s.macro.prepare (sampleRate_); // allocates (pimpl + upstream carve) — off the audio thread\n",
        "            if (!lazyMacro_) // FIREFORK: lazy mode allocates this in setMode(Macro) instead\n"
        "                s.macro.prepare (sampleRate_); // allocates (pimpl + upstream carve) — off the audio thread\n")])

    # ---- 2. clampT(Real, 1.0, nyquistGuard): lo/hi must not take part in template deduction ----------------------------
    for f in sorted(glob.glob(os.path.join(P.root, "src", "*.cpp"))):
        s = open(f).read()
        old = "T clampT (T v, T lo, T hi) noexcept"
        if old in s:
            s = s.replace(old, "T clampT (T v, typename std::common_type<T>::type lo, typename std::common_type<T>::type hi) noexcept")
            if "<type_traits>" not in s:
                m = re.search(r"#include\s+[<\"][^\n]+\n", s)
                s = s[:m.start()] + "#include <type_traits>\n" + s[m.start():]
            open(f, "w").write(s)
    P.log.append("  clampT non-deduced bounds          src/*.cpp")

    # ---- 3. companding: hoist ln(1+mu) (bit-identical) and route the transcendentals through fm:: -------------------
    P.edit(f"{INC}/BrokenConverter.h", "BrokenConverter hoist + fm::", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/FastMath.h>\n"),
        ("levelsHalf_ = std::exp2 (b - 1.0);",
         "levelsHalf_ = fm::exp2 (b - 1.0); // FIREFORK: exact at integer b; runs per sample when bit depth is modulated"),
        ("            companding_ = mu_ > kMuMin;\n        }",
         "            companding_ = mu_ > kMuMin;\n"
         "            // FIREFORK: ln(1+mu) is constant between control changes; the per-sample path used to recompute it twice\n"
         "            // per call. Same value, so the output is bit-identical.\n"
         "            log1pMu_ = companding_ ? std::log1p (mu_) : static_cast<Real> (0.0);\n        }"),
        ("                const Real c = muLawEncode (v, mu_);", "                const Real c = encodeHoisted (v);"),
        ("                v = muLawDecode (cq, mu_);", "                v = decodeHoisted (cq);"),
        ("    private:\n        static constexpr Real kMuMax",
         "    private:\n"
         "        // FIREFORK: the per-sample forms of muLawEncode / muLawDecode: ln(1+mu) from log1pMu_, transcendental via fm::\n"
         "        // (std:: unless NEMESIS_DSP_FAST_MATH). Operation order matches the static versions, which stay as the tested reference.\n"
         "        Real encodeHoisted (Real x) const noexcept\n        {\n"
         "            const Real s = (x < 0.0) ? -1.0 : 1.0;\n"
         "            return s * fm::log1p (mu_ * std::abs (x)) / log1pMu_;\n        }\n"
         "        Real decodeHoisted (Real c) const noexcept\n        {\n"
         "            const Real s = (c < 0.0) ? -1.0 : 1.0;\n"
         "            return s * fm::expm1 (std::abs (c) * log1pMu_) / mu_;\n        }\n\n"
         "        static constexpr Real kMuMax"),
        ("        bool companding_ = false;\n", "        Real log1pMu_ = 0.0; // ln(1 + mu_), valid while companding_\n        bool companding_ = false;\n")])

    # ---- 4. per-sample transcendentals through fm:: ------------------------------------------------------------------
    P.edit("src/Svf.cpp", "Svf prewarp tan", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/FastMath.h>\n"),
        ("g_ = std::tan (kPi * fc / sampleRate_);", "g_ = fm::tan (kPi * fc / sampleRate_);")])
    P.edit("src/SampleRateReducer.cpp", "SampleRateReducer exp2/log2", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/FastMath.h>\n"),
        ("pitchMult = std::exp2 (track_ * std::log2 (noteHz_ / kReferenceHz));",
         "pitchMult = fm::exp2 (track_ * fm::log2 (noteHz_ / kReferenceHz));")])
    for rel in (f"{INC}/CathodeModulatedVoice.h", f"{INC}/CathodeVoiceEngine.h"):
        p = os.path.join(P.root, rel)
        s = open(p).read()
        lines, n = [], 0
        for ln in s.split("\n"):
            code, sep, c = ln.partition("//")
            new = code.replace("std::exp2 (", "fm::exp2 (")
            n += new != code
            lines.append(new + sep + c)
        s = "\n".join(lines)
        if n:
            m = re.search(r"#include <nemesis/dsp/Real.h>\n", s)
            if m and "FastMath.h" not in s:
                s = s[:m.end()] + "#include <nemesis/dsp/FastMath.h>\n" + s[m.end():]
            open(p, "w").write(s)
        P.log.append(f"  std::exp2 -> fm::exp2 ({n} sites)        {rel}")

    # ---- 5. FIR kernel: a float overload for the scalar Oversampler (lanes keep the pinned double kernel) ---------------
    P.edit(f"{INC}/FirKernel.h", "FirKernel float overload", [(
        "        return acc;\n    }\n}",
        "        return acc;\n    }\n\n"
        "    // FIREFORK: single-precision twin for Real = float. Every tap fuses (one VFMA.F32 each on the M7); bit-exactness with\n"
        "    // a double reference is not a goal in float builds. The double overload above stays the pinned reference.\n"
        "    [[nodiscard]] inline float dot (const float* c, const float* newest, int taps) noexcept\n"
        "    {\n"
        "        float acc = 0.0f;\n"
        "        for (int j = 0; j < taps; ++j)\n"
        "            acc = std::fma (c[j], newest[-j], acc);\n"
        "        return acc;\n"
        "    }\n}")])

    # ---- 6. float builds: the lane code keeps double state, the scalar classes hold Real ---------------------------------
    # (the firmware never RUNS the lanes -- no NEON on the M7 -- but they must compile, and a float host build may run them)
    P.edit(f"{INC}/Oversampler.h", "Oversampler windows as templates", [
        ("        void copyWindowsTo (Real* x, Real* o) const noexcept",
         "        template <class T> // FIREFORK: lanes hand over double windows even when the scalar is float\n"
         "        void copyWindowsTo (T* x, T* o) const noexcept"),
        ("        void setWindows (const Real* x, const Real* o) noexcept",
         "        template <class T>\n        void setWindows (const T* x, const T* o) noexcept")])
    P.edit(f"{INC}/FilterDriveLanes.h", "FilterDriveLanes adaa cast", [
        ("d.setAdaaState ({ x1_[i], f1x1_[i] });",
         "d.setAdaaState ({ static_cast<Real> (x1_[i]), static_cast<Real> (f1x1_[i]) });")])
    P.edit(f"{INC}/ClassicFilterLanes.h", "ClassicFilterLanes setState cast", [
        ("            f.setState (state (lane));",
         "            {\n                const auto st = state (lane); // FIREFORK: lane state is double, the scalar's is Real\n"
         "                std::array<Real, 8> r {};\n"
         "                for (std::size_t k = 0; k < 8; ++k)\n                    r[k] = static_cast<Real> (st[k]);\n"
         "                f.setState (r);\n            }")])
    P.edit(f"{INC}/CathodeOscCore.h", "OscCore unison arrays are Wide", [
        ("static void layoutMultipliers (float detune, int n, std::array<Real, kMaxOscUnison>& out) noexcept",
         "static void layoutMultipliers (float detune, int n, std::array<Wide, kMaxOscUnison>& out) noexcept")])
    P.edit(f"{INC}/CathodeVoiceEngine.h", "Engine onesArray is Wide", [
        ("static constexpr std::array<Real, kMaxVoices> onesArray() noexcept\n        {\n            std::array<Real, kMaxVoices> a {};",
         "static constexpr std::array<Wide, kMaxVoices> onesArray() noexcept\n        {\n            std::array<Wide, kMaxVoices> a {};")])
    P.edit(f"{INC}/CathodeOscCore.h", "OscCore stack phase refs are Wide", [
        ("Real& ph = phase[k];", "Wide& ph = phase[k];")], once=False)
    P.edit(f"{INC}/CathodeOscCore.h", "OscCore layout array is Wide", [
        ("            std::array<Real, kMaxOscUnison> l {};\n            layoutMultipliers (st.detune, st.voices, l);",
         "            std::array<Wide, kMaxOscUnison> l {};\n            layoutMultipliers (st.detune, st.voices, l);")])
    # -fsingle-precision-constant makes `1.0` a float, so a max/min against a Wide (sampleRate) needs both sides Wide.
    P.edit(f"{INC}/CathodeVoiceStage.h", "VoiceStage prepare max(Wide)", [
        ("std::max (1.0, 0.010 * sampleRate)", "std::max (static_cast<Wide> (1.0), 0.010 * sampleRate)"),
        ("std::max (1.0, kWidthGlideSeconds * sampleRate)", "std::max (static_cast<Wide> (1.0), kWidthGlideSeconds * sampleRate)")])
    # The firmware builds with -fno-exceptions. startRightChain's try/catch only guards a copy that cannot throw.
    P.edit(f"{INC}/CathodeVoiceStage.h", "VoiceStage try/catch only with exceptions", [
        ("        bool startRightChain() noexcept\n        {\n            try\n            {\n",
         "        bool startRightChain() noexcept\n        {\n#if __cpp_exceptions // FIREFORK: the firmware has exceptions off; the copy cannot throw there\n            try\n#endif\n            {\n"),
        ("            } catch (...) // NOLINT(bugprone-empty-catch) — the alternative is std::terminate on the audio thread\n            {\n                return false;\n            }\n",
         "            }\n#if __cpp_exceptions\n            catch (...) // NOLINT(bugprone-empty-catch) — the alternative is std::terminate on the audio thread\n            {\n                return false;\n            }\n#endif\n")])

    # ---- 7. every filter model's prewarp tan() goes through fm:: (cutoff moves per sample when it is modulated) ---------
    n_tan = 0
    for f in sorted(glob.glob(os.path.join(P.root, "src", "*.cpp"))):
        s = open(f).read()
        if "nemesis/dsp/Real.h" not in s or "std::tan (" not in s:
            continue
        lines = []
        for ln in s.split("\n"):
            code, sep, c = ln.partition("//")
            lines.append(code.replace("std::tan (", "fm::tan (") + sep + c)
        s = "\n".join(lines)
        if "FastMath.h" not in s:
            s = s.replace("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/FastMath.h>\n", 1)
        open(f, "w").write(s)
        n_tan += 1
    P.log.append(f"  std::tan -> fm::tan                {n_tan} filter sources")


    # ---- 8. CPU savings that remove no feature (all OFF by default; see ForkConfig.h) -----------------------------------
    CFG = "#include <nemesis/dsp/ForkConfig.h>\n"
    # 8a. idle mod matrix: no active slot means evaluate() is a no-op, so feeding and evaluating it is wasted work
    P.edit(f"{INC}/CathodeModMatrix.h", "ModMatrix::idle", [(
        "        // [T-CGUI-08] The MTX bypass fade's gain on the user slots, set per sample by the voice.",
        "        // FIREFORK: true when no slot is active. Then evaluate() changes nothing (rebuildTouched zeroed whatever left the\n"
        "        // route list), so a caller may skip feeding and evaluating it.\n"
        "        [[nodiscard]] bool idle() const noexcept { return touchedCount_ == 0; }\n\n"
        "        // [T-CGUI-08] The MTX bypass fade's gain on the user slots, set per sample by the voice.")])
    P.edit(f"{INC}/CathodeModulatedVoice.h", "ModulatedVoice idle matrix", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n" + CFG),
        ("            feedSources ({ e1, e2, e3, l1, l2, l3, st, sh, dd1, dd2, dd3, envGain, lfoGain, driftGain, mtxGain });\n\n            // 3. Sum every destination's modulation.\n            matrix_.evaluate();\n",
         "            // FIREFORK: with no active slot the matrix has nothing to sum (see CathodeModMatrix::idle). The sources above and\n"
         "            // the fades keep ticking, so nothing downstream can tell.\n"
         "            if (!(fork::Config::idleMatrixSkip && matrix_.idle()))\n            {\n"
         "                feedSources ({ e1, e2, e3, l1, l2, l3, st, sh, dd1, dd2, dd3, envGain, lfoGain, driftGain, mtxGain });\n\n"
         "                // 3. Sum every destination's modulation.\n                matrix_.evaluate();\n            }\n")])

    # 8b. a silent, uncoupled oscillator slot is not computed
    P.edit(f"{INC}/CathodeOscCore.h", "OscCore skip silent slots", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n" + CFG),
        ("        StereoOut finishBody() noexcept\n        {\n            Real s1 = static_cast<Real> (slotSample (1));",
         "        // FIREFORK: a mixing slot may be skipped only when its level is 0 and nothing couples it to the other slot: no FM or ring\n"
         "        // leg live OR still fading, no hard sync, no priming, and it is not showing a unison stack. Then its contribution is\n"
         "        // exactly 0 anyway. (xmodFadeStep_ > 0 only means fades are ENABLED; it is the default, so it is not a condition.)\n"
         "        [[nodiscard]] bool slotUncoupledAndSilent (int slot) const noexcept\n        {\n"
         "            return fork::Config::skipSilentSlots && slots_[slot].level <= 0.0f && !stackDips_[static_cast<std::size_t> (slot)].shown\n"
         "                   && !fmLive_ && !ringLive_ && syncAmount_ <= 0.0f && !fmFade_.live && !ringFade_.live && fmFade_.prime == 0\n"
         "                   && ringFade_.prime == 0 && !xmodFresh_;\n        }\n"
         "        Real slotOrSilence (int slot) noexcept\n        {\n"
         "            if (slotUncoupledAndSilent (slot))\n            {\n                NEMESIS_FORK_COUNT (slotSkips);\n                return static_cast<Real> (0.0);\n            }\n"
         "            return static_cast<Real> (slotSample (slot));\n        }\n\n"
         "        StereoOut finishBody() noexcept\n        {\n            Real s1 = slotOrSilence (1);"),
        ("                         : static_cast<Real> (slotSample (0));\n                if (ringLive_)",
         "                         : slotOrSilence (0);\n                if (ringLive_)"),
        ("                        : static_cast<Real> (slotSample (0));\n            }\n            else\n            {\n                if (fmWant && !fmFade_.live)",
         "                        : slotOrSilence (0);\n            }\n            else\n            {\n                if (fmWant && !fmFade_.live)"),
        ("                else\n                    s0 = static_cast<Real> (slotSample (0));\n            }\n",
         "                else\n                    s0 = slotOrSilence (0);\n            }\n"),
        ("            st.staged = false;\n            st.done = false;\n            const Slot& s = slots_[slot];\n",
         "            st.staged = false;\n            st.done = false;\n            if (slot < kNumMixSlots && slotUncoupledAndSilent (slot)) // FIREFORK: no PD generation for a silent, uncoupled slot\n                return;\n            const Slot& s = slots_[slot];\n")])

    # 8c. oversampling factors come from the config (upstream defaults 4 and 4)
    P.edit(f"{INC}/CathodeVoiceStage.h", "VoiceStage oversample defaults", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n" + CFG),
        ("            int pdOversample = PhaseDistortionOscillator::kDefaultOversample,\n            int screamOversample = ScreamFilter::kDefaultOversample,\n            int acidOversample = AcidFilter::kDefaultOversample,\n            int sovietOversample = SovietFilter::kDefaultOversample,\n            int classicOversample = ClassicFilter::kDefaultOversample,\n            int reedOversample = ReedFilter::kDefaultOversample)",
         "            int pdOversample = fork::Config::pdOversample, // FIREFORK: upstream 4\n            int screamOversample = fork::Config::filterOversample,\n            int acidOversample = fork::Config::filterOversample,\n            int sovietOversample = fork::Config::filterOversample,\n            int classicOversample = fork::Config::filterOversample,\n            int reedOversample = fork::Config::filterOversample)")])

    # ---- 9. bug fixes (docs/DSP_REVIEW.md section 2) ----------------------------------------------------------------------
    # B1: the PD / Bitmask phase accumulators are Wide, but wrap01 took a Real, so in float builds the double phase was rounded to
    # float on every oversampled step (-3 cents at 55 Hz). Taking Wide is the same expression in double builds: bit-identical.
    for f in ("PhaseDistortionOscillator.h", "BitmaskOscillator.h"):
        P.edit(f"{INC}/{f}", f"{f} wrap01 keeps Wide", [(
            "static Wide wrap01 (Real p) noexcept { return p - std::floor (p); }",
            "static Wide wrap01 (Wide p) noexcept { return p - std::floor (p); } // FIREFORK: was (Real p): float builds lost the phase")])

    # B9 (found by the firmware's end-to-end stress under ASan): setNoteHz can shorten the loop period below the phase already
    # reached, and processSample subtracts the period once per sample, so phase_/period_ stays above 1 for many samples and the
    # read offset runs past the 6-sample ring (a heap overflow). Wrapping the phase into the new period changes nothing where
    # upstream is defined (phase_ < period_ already); where it was not, the read stays inside the ring.
    P.edit(f"{INC}/LoopError.h", "B9 LoopError phase wraps into a shorter period", [
        ("#include <cstddef>\n", "#include <cmath>\n#include <cstddef>\n"), (
        "            period_ = fs_ / n; // >= 2 by the clamp\n",
        "            period_ = fs_ / n; // >= 2 by the clamp\n"
        "            if (phase_ >= period_) // FIREFORK B9: a shorter period: keep the phase inside it (the read stays in the ring)\n"
        "                phase_ = std::fmod (phase_, period_);\n")])

    # B10 (found by the firmware's end-to-end stress under ASan, MIDI note 127 with +36 semitones of OSC OCT/SEMI): a frequency at or above
    # the sample rate makes inc_ >= 1, and processSample subtracts 1 once, so the phase keeps growing and readAt indexes ever further past
    # the table (heap overflow, then a hard fault on the board). Taking floor(phase_) instead of 1.0 is the same subtraction while
    # phase_ is in [1,2) (exactly 1.0), which is every case upstream handles; above that the phase now stays in [0,1).
    P.edit(f"{INC}/WavetableOscillator.h", "B10 WavetableOscillator wraps a phase above 2", [(
        "            phase_ += inc_;\n            if (phase_ >= 1.0)\n                phase_ -= 1.0;\n",
        "            phase_ += inc_;\n            if (phase_ >= 1.0) // FIREFORK B10: inc_ >= 1 (a pitch at or above the sample rate) left the phase growing without bound\n"
        "                phase_ -= (phase_ < 2.0 ? 1.0 : std::floor (phase_));\n")])

    # F1 (FX section): the amp as a mono insert. With mono_ set the right channel's preamp, oversampler, power amp and cabinet are skipped and
    # the output copies the left: half the cost, and the caller feeds the sum of both channels. Off by default (the upstream path).
    P.edit(f"{INC}/GampEffect.h", "F1 GampEffect mono option", [(
        "        bool prepared_ = false;\n",
        "        bool prepared_ = false;\n        bool mono_ = false; // FIREFORK F1\n"),
        ("        void setMixPercent (float percent) noexcept;\n",
         "        void setMixPercent (float percent) noexcept;\n"
         "        // FIREFORK F1: process only the left channel and copy it to the right (the caller feeds the mono sum): half the cost\n"
         "        void setMono (bool on) noexcept { mono_ = on; }\n")])
    P.edit("src/GampEffect.cpp", "F1 GampEffect mono process", [
        ("            float wr = cr.os[mi].processSample (xr * gInput_.cur,",
         "            float wr = mono_ ? 0.0f : cr.os[mi].processSample (xr * gInput_.cur,"),
        ("            wr = cr.dcBlock.hp (wr);", "            if (!mono_)\n                wr = cr.dcBlock.hp (wr);"),
        ("            wr += gPresence_.cur * cr.presenceSvf.processSample (wr).hp;",
         "            if (!mono_)\n                wr += gPresence_.cur * cr.presenceSvf.processSample (wr).hp;"),
        ("            wr = cabinet (cr, wr);", "            wr = mono_ ? wl : cabinet (cr, wr);")])

    # B11 (found by the FX review): ReverbCore's coupled-form LFO rotates (u, w) by cos/sin of 2*pi*rate/fs. In float cos(2*pi*0.6/44100) is
    # exactly 1.0, so each step multiplies the amplitude by sqrt(1 + sin^2): x1.8 after an hour of reverb time, x100 after eight, and the
    # modulation depth grows with it until the tail is wrecked. One first-order renormalisation per 16-sample control chunk holds the
    # amplitude at 1 (a no-op in double up to rounding).
    P.edit(f"{INC}/ReverbCore.h", "B11 ReverbCore LFO renormalised", [(
        "                chunkCountdown_ = kControlChunk;\n                if (dirty_)\n                    resolveControls();\n",
        "                chunkCountdown_ = kControlChunk;\n                if (dirty_)\n                    resolveControls();\n"
        "                if (modActive_) // FIREFORK B11: hold the LFO amplitude at 1 (float rounding of cos grows it)\n"
        "                    for (int i = 0; i < numLines_; ++i)\n                    {\n"
        "                        const auto k = static_cast<std::size_t> (i);\n"
        "                        const Real g = 1.5 - 0.5 * (lfoU_[k] * lfoU_[k] + lfoV_[k] * lfoV_[k]);\n"
        "                        lfoU_[k] *= g;\n                        lfoV_[k] *= g;\n                    }\n")])

    # B12 (found on the board): the reverb's eight lines are written at the same index every sample. At a power-of-two distance (16 KB at half rate) those writes fall in one
    # set of the M7's 4-way D-cache and evict each other on every sample: 7 points of the audio budget. Spacing the lines by a further 261 values (1,044 B, an eighth of a way)
    # puts each in a set of its own. Only the layout of the buffer changes, not one sample of the output, so it is not behind a switch.
    P.edit(f"{INC}/ReverbCore.h", "B12 ReverbCore line stride", [
        ("static_cast<std::size_t> (i) * static_cast<std::size_t> (lineSize_)", "static_cast<std::size_t> (i) * static_cast<std::size_t> (lineStride_)")], once=False)
    P.edit(f"{INC}/ReverbCore.h", "B12 ReverbCore line stride member", [(
        "        int lineSize_ = 0; // per-line ring length, a power of two\n",
        "        int lineSize_ = 0; // per-line ring length, a power of two\n"
        "        int lineStride_ = 0; // FIREFORK B12: the distance between the lines in lineBuf_\n")])
    P.edit("src/ReverbCore.cpp", "B12 ReverbCore line buffer", [(
        "        lineBuf_.assign (static_cast<std::size_t> (numLines_) * static_cast<std::size_t> (lineSize_), 0.0);\n",
        "        lineStride_ = lineSize_ + 261; // FIREFORK B12: each line's write in a D-cache set of its own\n"
        "        lineBuf_.assign (static_cast<std::size_t> (numLines_) * static_cast<std::size_t> (lineStride_), 0.0);\n")])

    # W1 (wavetables on the device): the mip pyramid stores every band-limited level at full length (11 x 2048 floats per frame: 2.9 MB for a
    # 32-frame table). A level that keeps harmonics 1..top needs only 2 x top samples, 4 x top for clean linear interpolation: with
    # `decimate` the pyramid stores level j at max(L >> (j - 2), 128) samples (levels 0-2 at L): 8,576 samples a frame instead of 22,528, so a
    # 32-frame table is 1.10 MB float (error against the full pyramid over the 56 banks: median 0.8%, worst 3.8%). The oscillator reads each level at its own length. Without `decimate` every length is L and the arithmetic
    # is the original's (bit for bit). `adoptPyramid` fills a table from the 16-bit file the host tool writes (tools/ wt_pyramid), so the board runs no FFT.
    P.edit(f"{INC}/WavetableOscillator.h", "W1 Wavetable per-mip lengths + adoptPyramid", [
        ("#include <cmath>\n#include <cstddef>\n#include <cstdint>\n#include <vector>\n", "#include <array>\n#include <cmath>\n#include <cstddef>\n#include <cstdint>\n#include <vector>\n"),
        ("            return data_.data() + frameStride (frameIdx, mip);\n", "            return store() + frameStride (frameIdx, mip); // FIREFORK W1\n"),
        ("        void build (const float* samples, int numFrames, int frameLength);\n",
         "        void build (const float* samples, int numFrames, int frameLength, bool decimate = false); // FIREFORK W1: decimate\n\n"
         "        // FIREFORK W1: the length (samples) mip `mip` is stored at (frameLength() unless built decimated), the samples one frame\n"
         "        // takes in all its mips, and the raw pyramid ([frame][mip concatenated][sample]) for the host tool that writes it to a file\n"
         "        [[nodiscard]] int mipLength (int mip) const noexcept { return mipLen_[static_cast<std::size_t> (mip)]; }\n"
         "        [[nodiscard]] std::size_t frameSamples() const noexcept { return frameSize_; }\n"
         "        [[nodiscard]] bool decimated() const noexcept { return decimated_; }\n"
         "        [[nodiscard]] const Real* raw() const noexcept { return store(); }\n"
         "        [[nodiscard]] std::size_t rawSize() const noexcept { return static_cast<std::size_t> (numFrames_) * frameSize_; }\n"
         "        // Samples a table's pyramid will take (frames x all mips) for this geometry, without building it (the device sizes its arena with it)\n"
         "        static std::size_t pyramidSamples (int numFrames, int frameLength, bool decimated);\n"
         "        // Gives the table memory that is not its own (the device's arena: its heap never frees): adoptPyramid then fills it instead of allocating\n"
         "        void setStorage (Real* mem, std::size_t capacity) noexcept { ext_ = mem; extCap_ = capacity; }\n"
         "        // Sets the geometry and fills the pyramid from `count` 16-bit samples x scale (no FFT): the device's loader. False (and an empty\n"
         "        // table) if `count` is not what that geometry holds. Not RT-safe (allocates).\n"
         "        bool adoptPyramid (int numFrames, int frameLength, bool decimated, const std::int16_t* q, std::size_t count, float scale);\n"),
        ("""        [[nodiscard]] std::size_t frameStride (int frame, int mip) const noexcept
        {
            return (static_cast<std::size_t> (frame) * static_cast<std::size_t> (numMips_)
                       + static_cast<std::size_t> (mip))
                   * static_cast<std::size_t> (frameLength_);
        }
""", """        [[nodiscard]] std::size_t frameStride (int frame, int mip) const noexcept
        {
            return static_cast<std::size_t> (frame) * frameSize_ + mipOff_[static_cast<std::size_t> (mip)]; // FIREFORK W1
        }
        void setGeometry (int numFrames, int frameLength, bool decimate);
        [[nodiscard]] Real* store() const noexcept { return ext_ != nullptr ? ext_ : const_cast<Real*> (data_.data()); }
"""),
        ("        int numMips_ = 0;\n        std::vector<Real> data_; // flat [frame][mip][sample], size numFrames_*numMips_*frameLength_\n",
         "        int numMips_ = 0;\n        std::size_t frameSize_ = 0; // FIREFORK W1: samples of one frame over all its mips\n        bool decimated_ = false;\n"
         "        std::array<int, 24> mipLen_ {};\n        std::array<std::size_t, 24> mipOff_ {};\n        Real* ext_ = nullptr; // external storage (setStorage), else data_\n        std::size_t extCap_ = 0;\n"
         "        std::vector<Real> data_; // flat [frame][mip][sample]: each mip at mipLen_[mip] samples\n"),
        ("""            const Real p = ph * static_cast<Real> (length_);
            auto i0 = static_cast<int> (p); // 0..length_-1 (ph in [0,1))
            int i1 = i0 + 1;
            if (i1 >= length_)
                i1 = 0;
            const Real frac = p - static_cast<Real> (i0);

            const Real sf0 = blendMip (posF0_, i0, i1, frac);
            const Real sf1 = (posF1_ == posF0_) ? sf0 : blendMip (posF1_, i0, i1, frac);
            return static_cast<float> (sf0 + (sf1 - sf0) * posFrac_);
        }
""", """            const Tap t0 = tapFor (mipJ0_, ph); // FIREFORK W1: each mip is read at its own length
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
"""),
        ("""        [[nodiscard]] Real blendMip (int f, int i0, int i1, Real frac) const noexcept
        {
            const Real* a = wt_->frame (f, mipJ0_);
            const Real v0 = a[static_cast<std::size_t> (i0)]
                              + (a[static_cast<std::size_t> (i1)] - a[static_cast<std::size_t> (i0)]) * frac;
            if (mipJ1_ == mipJ0_)
                return v0;
            const Real* b = wt_->frame (f, mipJ1_);
            const Real v1 = b[static_cast<std::size_t> (i0)]
                              + (b[static_cast<std::size_t> (i1)] - b[static_cast<std::size_t> (i0)]) * frac;
            return v0 + (v1 - v0) * mipFrac_;
        }""", """        [[nodiscard]] Real blendMip (int f, const Tap& t0, const Tap& t1) const noexcept
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
        }"""),
    ])
    P.edit("src/WavetableOscillator.cpp", "W1 Wavetable build decimated + adoptPyramid", [
        ("""    void Wavetable::build (const float* samples, int numFrames, int frameLength)
    {
        frameLength_ = frameLength < 2 ? 2 : frameLength;
        numFrames_ = numFrames < 1 ? 1 : numFrames;

        // Mip count: top harmonic frameLength/2 >> j, down to the fundamental (>= 1).
        numMips_ = 1;
        for (int t = frameLength_ / 2; t > 1; t >>= 1)
            ++numMips_;

        const auto L = static_cast<std::size_t> (frameLength_);
        data_.assign (static_cast<std::size_t> (numFrames_) * static_cast<std::size_t> (numMips_) * L, 0.0);
""", """    // FIREFORK W1: the geometry of a pyramid: mip count (top harmonic frameLength/2 >> j, down to the fundamental), and where each mip lives.
    void Wavetable::setGeometry (int numFrames, int frameLength, bool decimate)
    {
        frameLength_ = frameLength < 2 ? 2 : frameLength;
        numFrames_ = numFrames < 1 ? 1 : numFrames;
        numMips_ = 1;
        for (int t = frameLength_ / 2; t > 1; t >>= 1)
            ++numMips_;
        if (numMips_ > static_cast<int> (mipLen_.size()))
            numMips_ = static_cast<int> (mipLen_.size());
        decimated_ = decimate;
        constexpr int kDecShift = 2; // a decimated mip j is stored at L >> (j - 2) samples: 2^2 x its Nyquist length (clean linear reads)
        constexpr int kDecMin = 128; // and never fewer than 128: the top mips hold one or two harmonics, and linear interpolation of a 64-sample level leaves spurs at -47 dB
        std::size_t off = 0;
        for (int j = 0; j < numMips_; ++j)
        {
            int len = frameLength_;
            if (decimate && j > 0)
            {
                len = frameLength_ >> (j > kDecShift ? j - kDecShift : 0); // 2^kDecShift x oversampled: linear interpolation stays clean
                if (len < kDecMin)
                    len = kDecMin < frameLength_ ? kDecMin : frameLength_;
            }
            mipLen_[static_cast<std::size_t> (j)] = len;
            mipOff_[static_cast<std::size_t> (j)] = off;
            off += static_cast<std::size_t> (len);
        }
        frameSize_ = off;
    }

    std::size_t Wavetable::pyramidSamples (int numFrames, int frameLength, bool decimated)
    {
        Wavetable t;
        t.setGeometry (numFrames, frameLength, decimated);
        return static_cast<std::size_t> (t.numFrames_) * t.frameSize_;
    }

    bool Wavetable::adoptPyramid (int numFrames, int frameLength, bool decimated, const std::int16_t* q, std::size_t count, float scale)
    {
        setGeometry (numFrames, frameLength, decimated);
        const std::size_t want = static_cast<std::size_t> (numFrames_) * frameSize_;
        if (count != want || (ext_ != nullptr && want > extCap_))
        {
            numFrames_ = 0;
            data_.clear();
            return false;
        }
        Real* dst = ext_;
        if (dst == nullptr)
        {
            data_.assign (want, 0.0);
            dst = data_.data();
        }
        // `q` may be the second half of `dst` itself (the device reads the file there and converts forward in place: every write lands
        // on bytes already read), so both pointers are may_alias: the compiler must not move a load past a store
        using Q16 = std::int16_t __attribute__ ((may_alias));
        using RA = Real __attribute__ ((may_alias));
        const Q16* qq = q;
        RA* dd = dst;
        for (std::size_t i = 0; i < want; ++i)
            dd[i] = static_cast<Real> (qq[i]) * static_cast<Real> (scale);
        return true;
    }

    void Wavetable::build (const float* samples, int numFrames, int frameLength, bool decimate)
    {
        ext_ = nullptr; // a built table lives in data_
        setGeometry (numFrames, frameLength, decimate);

        const auto L = static_cast<std::size_t> (frameLength_);
        data_.assign (static_cast<std::size_t> (numFrames_) * frameSize_, 0.0);
"""),
        ("""                Real* dst = data_.data() + frameStride (f, mip);
                for (std::size_t i = 0; i < L; ++i)
                    dst[i] = re[i];""", """                Real* dst = data_.data() + frameStride (f, mip);
                const auto len = static_cast<std::size_t> (mipLen_[static_cast<std::size_t> (mip)]);
                const std::size_t step = L / len; // 1 unless decimated: the level holds nothing above 2 x len / 4, so every step-th sample is exact
                for (std::size_t i = 0; i < len; ++i)
                    dst[i] = re[i * step];"""),
    ])

    # B5: startRightChain copied whole filter banks into the twin in one sample, including the Comb ring buffer (fs/20 floats) and
    # every core's oversampler history. Under NEMESIS_FORK_EXACT only what can matter is copied: see the patch in section 10.

    # ==== 10. EXACT optimizations (NEMESIS_FORK_EXACT; docs/DSP_REVIEW.md section 3). Bit-identical to the build without them. =====
    X = "fork::kExact"
    # E1: the NEON lane phases walk every voice's flags each sample even with every lane site off (the firmware's case).
    P.edit(f"{INC}/CathodeVoiceEngine.h", "E1 lane phases return when their site is off", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/ForkConfig.h>\n"),
        ("            PdSite& site = pdLanes_[static_cast<std::size_t> (Slot)];\n",
         "            PdSite& site = pdLanes_[static_cast<std::size_t> (Slot)];\n"
         f"            if constexpr ({X}) // FIREFORK E1: a site that is off has no resident voice (pulled when it was switched off)\n"
         "                if (!site.on)\n                    return;\n"),
        ("        void runDriveLanes (int span) noexcept\n        {\n",
         "        void runDriveLanes (int span) noexcept\n        {\n"
         f"            if constexpr ({X}) // FIREFORK E1\n                if (!driveLanes_.on)\n                    return;\n"),
        ("        void runClassicLanes (ClassicSite& site, int span) noexcept\n        {\n",
         "        void runClassicLanes (ClassicSite& site, int span) noexcept\n        {\n"
         f"            if constexpr ({X}) // FIREFORK E1\n                if (!site.on)\n                    return;\n"),
        ("        void markPdReady (std::size_t u) noexcept\n        {\n",
         "        void markPdReady (std::size_t u) noexcept\n        {\n"
         f"            if constexpr ({X}) // FIREFORK E1: `ready` is only read by runPdLanes, which returns at once for a site that is off\n"
         "                if (!pdLanes_[0].on && !pdLanes_[1].on)\n                    return;\n")])
    # E1b: fork::Config::lanes = false skips preparing the lane groups (their FIRs) and starts with every site off.
    P.edit("src/CathodeVoiceEngine.cpp", "E1b no lane groups", [
        ("        for (std::size_t g = 0; g < f1Lanes_.groups.size(); ++g)\n        {\n            f1Lanes_.groups[g].prepare",
         "        // FIREFORK: fork::Config::lanes = false (a target without SIMD) never prepares the lane groups and keeps every site off.\n"
         "        f1Lanes_.on = f2Lanes_.on = driveLanes_.on = fork::Config::lanes;\n"
         "        for (PdSite& site : pdLanes_)\n            site.on = fork::Config::lanes;\n"
         "        for (std::size_t g = 0; fork::Config::lanes && g < f1Lanes_.groups.size(); ++g)\n        {\n            f1Lanes_.groups[g].prepare")])
    # E2: cross-mod legs that decimate into (void). The A halves of the ring and FM pairs only record their OS phases, and FM's B
    # half upsamples a constant 0 whose result its shaper ignores. Their OS histories are read by nothing else (only reset/prepare).
    P.edit(f"{INC}/Oversampler.h", "E2 upsampleOnly / pushInput", [(
        "    private:\n        static constexpr int kPhaseChunk = 8;",
        "        // FIREFORK E2: the INTERPOLATING half alone, for a pair whose decimated output nobody reads: the phase sums go to\n"
        "        // `shape` (which records them) in the order processSample uses, the OS history is not advanced and nothing is\n"
        "        // decimated. Each phase sum is the same expression as in processSample, so what `shape` sees is bit-identical.\n"
        "        template <typename Shaper>\n"
        "        void upsampleOnly (float x, Shaper&& shape) noexcept\n        {\n"
        "            if (factor_ == 1)\n            {\n                shape (x);\n                return;\n            }\n"
        "            pushInput (x);\n"
        "            const Real* newest = xhist_.data() + xpos_ + phaseTaps_;\n"
        "            for (int p = 0; p < factor_; ++p)\n"
        "                shape (static_cast<float> (fir::dot (up_.data() + static_cast<std::size_t> (p) * static_cast<std::size_t> (phaseTaps_), newest, phaseTaps_)));\n"
        "        }\n"
        "        // FIREFORK E2: only advance the input history (so a later processSample/upsampleOnly sees the right window).\n"
        "        void pushInput (float x) noexcept\n        {\n"
        "            if (factor_ == 1)\n                return;\n"
        "            xpos_ = xpos_ + 1;\n            if (xpos_ >= phaseTaps_)\n                xpos_ = 0;\n"
        "            const auto xd = static_cast<Real> (x);\n"
        "            xhist_[static_cast<std::size_t> (xpos_)] = xd;\n"
        "            xhist_[static_cast<std::size_t> (xpos_) + static_cast<std::size_t> (phaseTaps_)] = xd;\n        }\n\n"
        "    private:\n        static constexpr int kPhaseChunk = 8;")])
    P.edit(f"{INC}/CathodeOscCore.h", "E2 cross-mod legs skip discarded work", [
        ("            int p = 0;\n            (void) xmodA_.processSample (a, [this, &p] (float os) noexcept {\n"
         "                aPhase_[static_cast<std::size_t> (p)] = os;\n                ++p;\n                return os;\n            });\n",
         "            int p = 0;\n"
         "            const auto recordA = [this, &p] (float os) noexcept {\n"
         "                aPhase_[static_cast<std::size_t> (p)] = os;\n                ++p;\n                return os;\n            };\n"
         f"            if constexpr ({X}) // FIREFORK E2: A's decimation was discarded; only its OS phases are used\n"
         "                xmodA_.upsampleOnly (a, recordA);\n            else\n"
         "                (void) xmodA_.processSample (a, recordA);\n"),
        ("                int ps = 0;\n                (void) sideOs_->ringA.processSample (*side, [&sideUp, &ps] (float os) noexcept {\n"
         "                    sideUp[static_cast<std::size_t> (ps++)] = os;\n                    return os;\n                });\n",
         "                int ps = 0;\n"
         "                const auto recordSide = [&sideUp, &ps] (float os) noexcept {\n"
         "                    sideUp[static_cast<std::size_t> (ps++)] = os;\n                    return os;\n                };\n"
         f"                if constexpr ({X}) // FIREFORK E2\n                    sideOs_->ringA.upsampleOnly (*side, recordSide);\n                else\n"
         "                    (void) sideOs_->ringA.processSample (*side, recordSide);\n"),
        ("            int p = 0;\n            (void) fmA_.processSample (modBase, [this, &p] (float os) noexcept {\n"
         "                mPhase_[static_cast<std::size_t> (p)] = os;\n                ++p;\n                return os;\n            });\n\n"
         "            Slot& s = slots_[0];\n",
         "            int p = 0;\n"
         "            const auto recordM = [this, &p] (float os) noexcept {\n"
         "                mPhase_[static_cast<std::size_t> (p)] = os;\n                ++p;\n                return os;\n            };\n"
         "            Slot& s = slots_[0];\n"
         f"            if constexpr ({X}) // FIREFORK E2: the decimation was discarded, and only the wavetable carrier reads mPhase_\n"
         "            {\n"
         "                if (s.mode == Mode::PhaseDistortion || s.mode == Mode::Bitmask || s.mode == Mode::Macro || s.mode == Mode::Pulse)\n"
         "                    fmA_.pushInput (modBase);\n                else\n                    fmA_.upsampleOnly (modBase, recordM);\n            }\n"
         "            else\n                (void) fmA_.processSample (modBase, recordM);\n"),
        ("            int q = 0;\n            const float leg = fmB_.processSample (0.0f, [this, &s, &q, incOs, d] (float) noexcept {\n",
         "            int q = 0;\n"
         "            const auto readCarrier = [this, &s, &q, incOs, d]() noexcept {\n"),
        ("                return s.wt.sampleAt (wrapOffsetTurns (off));\n            });\n",
         "                return s.wt.sampleAt (wrapOffsetTurns (off));\n            };\n"
         f"            // FIREFORK E2: B's input is a constant 0 its shaper ignores; decimateSample is documented bit-identical without it\n"
         f"            const float leg = {X} ? fmB_.decimateSample (readCarrier)\n"
         "                                  : fmB_.processSample (0.0f, [&readCarrier] (float) noexcept { return readCarrier(); });\n")])
    # E6: the driven Ladder decimates both taps (24 and 12 dB) every sample; the bank reads one. The unused tap's OS history is still
    # pushed, so switching between Ladder24 and Ladder12 mid-note finds it exactly as before.
    P.edit(f"{INC}/Oversampler.h", "E6 processNoDecimate / pushOsOnly", [(
        "        // FIREFORK E2: only advance the input history",
        "        // FIREFORK E6: processSample without the final decimation (its output would be discarded); the OS history advances.\n"
        "        template <typename Shaper>\n"
        "        void processNoDecimate (float x, Shaper&& shape) noexcept\n        {\n"
        "            if (factor_ == 1)\n            {\n                (void) shape (x);\n                return;\n            }\n"
        "            pushInput (x);\n"
        "            const Real* newest = xhist_.data() + xpos_ + phaseTaps_;\n"
        "            for (int p = 0; p < factor_; ++p)\n"
        "                pushOs (static_cast<Real> (shape (static_cast<float> (fir::dot (up_.data() + static_cast<std::size_t> (p) * static_cast<std::size_t> (phaseTaps_), newest, phaseTaps_)))));\n"
        "        }\n"
        "        // FIREFORK E6: decimateSample without the decimation: the generated OS samples only enter the history.\n"
        "        template <typename Source>\n"
        "        void pushOsOnly (Source&& next) noexcept\n        {\n"
        "            if (factor_ == 1)\n            {\n                (void) next();\n                return;\n            }\n"
        "            for (int p = 0; p < factor_; ++p)\n                pushOs (static_cast<Real> (static_cast<float> (next())));\n        }\n"
        "        // FIREFORK E2: only advance the input history")])
    P.edit(f"{INC}/LadderFilter.h", "E6 decimate only the used tap", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/ForkConfig.h>\n"),
        ("        // Resonance feedback k of the current coefficients (0 .. ~4). Exposed for tests/inspection.\n",
         "        // FIREFORK E6: which driven tap the caller reads (CathodeFilterBank sets it with the mode). With NEMESIS_FORK_EXACT the\n"
         "        // other tap's decimation is skipped; its value in Outputs is then 0 and must not be read.\n"
         "        void setUsedTap (bool lp24) noexcept { use24_ = lp24 ? 1 : 0; use12_ = lp24 ? 0 : 1; }\n"
         "        void setBothTapsUsed() noexcept { use24_ = use12_ = 1; }\n\n"
         "        // Resonance feedback k of the current coefficients (0 .. ~4). Exposed for tests/inspection.\n"),
        ("            fifoFill_ = 0;\n            const float a = os24_.processSample (x, [this] (float u) noexcept {\n"
         "                const Outputs o = coreStep (static_cast<Real> (u));\n"
         "                fifo_[static_cast<std::size_t> (fifoFill_)] = o.lp12;\n                ++fifoFill_;\n                return o.lp24;\n            });\n\n"
         "            fifoRead_ = 0;\n            const float b = os12_.decimateSample ([this]() noexcept {\n"
         "                const float v = fifo_[static_cast<std::size_t> (fifoRead_)];\n                ++fifoRead_;\n                return v;\n            });\n\n"
         "            return { a, b };\n",
         "            fifoFill_ = 0;\n"
         "            const auto core = [this] (float u) noexcept {\n"
         "                const Outputs o = coreStep (static_cast<Real> (u));\n"
         "                fifo_[static_cast<std::size_t> (fifoFill_)] = o.lp12;\n                ++fifoFill_;\n                return o.lp24;\n            };\n"
         "            fifoRead_ = 0;\n"
         "            const auto stashed = [this]() noexcept {\n"
         "                const float v = fifo_[static_cast<std::size_t> (fifoRead_)];\n                ++fifoRead_;\n                return v;\n            };\n"
         "            if constexpr (fork::kExact) // FIREFORK E6\n            {\n"
         "                float a = 0.0f, b = 0.0f;\n"
         "                if (use24_)\n                    a = os24_.processSample (x, core);\n                else\n                    os24_.processNoDecimate (x, core);\n"
         "                if (use12_)\n                    b = os12_.decimateSample (stashed);\n                else\n                    os12_.pushOsOnly (stashed);\n"
         "                return { a, b };\n            }\n"
         "            const float a = os24_.processSample (x, core);\n"
         "            const float b = os12_.decimateSample (stashed);\n"
         "            return { a, b };\n"),
        ("        int fifoFill_ = 0, fifoRead_ = 0;\n",
         "        int fifoFill_ = 0, fifoRead_ = 0;\n        unsigned char use24_ = 1, use12_ = 1; // FIREFORK E6\n")])
    P.edit(f"{INC}/CathodeFilterBank.h", "E6 bank tells the ladder its tap", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/ForkConfig.h>\n"), (
        "        void setFilterMode (FilterMode mode) noexcept\n        {\n            ++modeWrites_;\n",
        "        void setFilterMode (FilterMode mode) noexcept\n        {\n            ++modeWrites_;\n"
        "            if constexpr (fork::kExact) // FIREFORK E6: only Ladder24 / Ladder12 read the ladder, one tap each\n"
        "                ladder_.setUsedTap (mode != FilterMode::Ladder12);\n")])
    # E3: cutoff modulated per sample (any filter envelope). (a) The bank's TptSvf only sounds in LowPass/BandPass/HighPass/Notch,
    # yet F1's setCutoff recomputed it (tan + 4 div) in every mode: keep the request, apply it when an SVF mode is (or becomes) active.
    P.edit(f"{INC}/CathodeFilterBank.h", "E3a SVF coefficients only in SVF modes", [
        ("        void setCutoff (float cutoffHz, float q) noexcept { filter_.setCutoff (cutoffHz, q); }\n",
         "        void setCutoff (float cutoffHz, float q) noexcept\n        {\n"
         "            if constexpr (fork::kExact) // FIREFORK E3a: TptSvf::setCutoff is a pure function of (hz, q); defer it while out of circuit\n"
         "            {\n                svfHz_ = cutoffHz;\n                svfQ_ = q;\n"
         "                svfPending_ = !isSvfMode (filterMode_);\n                if (svfPending_)\n                    return;\n            }\n"
         "            filter_.setCutoff (cutoffHz, q);\n        }\n"),
        ("            if constexpr (fork::kExact) // FIREFORK E6: only Ladder24 / Ladder12 read the ladder, one tap each\n"
         "                ladder_.setUsedTap (mode != FilterMode::Ladder12);\n",
         "            if constexpr (fork::kExact) // FIREFORK E6: only Ladder24 / Ladder12 read the ladder, one tap each\n"
         "                ladder_.setUsedTap (mode != FilterMode::Ladder12);\n"
         "            if constexpr (fork::kExact) // FIREFORK E3a: entering an SVF mode applies the deferred cutoff\n"
         "                if (svfPending_ && isSvfMode (mode))\n                {\n"
         "                    filter_.setCutoff (svfHz_, svfQ_);\n                    svfPending_ = false;\n                }\n"),
        ("        [[nodiscard]] bool isReedMode() const noexcept\n",
         "        // FIREFORK E3a\n"
         "        static constexpr bool isSvfMode (FilterMode m) noexcept\n        {\n"
         "            return m == FilterMode::LowPass || m == FilterMode::BandPass || m == FilterMode::HighPass || m == FilterMode::Notch;\n        }\n"
         "        float svfHz_ = 0.0f, svfQ_ = 0.0f;\n        bool svfPending_ = false;\n\n"
         "        [[nodiscard]] bool isReedMode() const noexcept\n")])
    # (b) Classic: k, the stability bound (sqrt + div) and the compensation gains depend on the resonance only.
    P.edit(f"{INC}/ClassicFilter.h", "E3b Classic resonance cache members", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/ForkConfig.h>\n"),
        ("        Real gComp_ = 1.0; // 1 / (1 + k*G^4)",
         "        Real cachedRes_ = -1.0, cachedGStable_ = 0.0; // FIREFORK E3b: the resonance-only terms, keyed on the clamped resonance\n"
         "        Real gComp_ = 1.0; // 1 / (1 + k*G^4)")])
    P.edit("src/ClassicFilter.cpp", "E3b Classic resonance cache", [(
        "        const Real gTan = fm::tan (kPi * fc / fsOs);\n"
        "        k_ = resonance::ceiling::feedbackK (resonance::ceiling::kClassicMaxK, res);\n"
        "        const Real gStable = 1.0 / std::sqrt (1.0 + kLagStabilityK * k_); // see kLagStabilityK\n"
        "        g_ = std::min (gTan / (1.0 + gTan), gStable); // TPT one-pole coefficient G\n",
        "        const Real gTan = fm::tan (kPi * fc / fsOs);\n"
        "        if constexpr (fork::kExact) // FIREFORK E3b: same expressions, evaluated only when the resonance moves\n"
        "        {\n            if (res != cachedRes_)\n            {\n                cachedRes_ = res;\n"
        "                k_ = resonance::ceiling::feedbackK (resonance::ceiling::kClassicMaxK, res);\n"
        "                cachedGStable_ = 1.0 / std::sqrt (1.0 + kLagStabilityK * k_);\n"
        "                compGain_ = 1.0 + k_;\n                invComp_ = 1.0 / compGain_;\n            }\n"
        "            g_ = std::min (gTan / (1.0 + gTan), cachedGStable_);\n"
        "            oneMinusG_ = 1.0 - g_;\n            g2_ = g_ * g_;\n            g3_ = g2_ * g_;\n            g4_ = g3_ * g_;\n"
        "            gComp_ = 1.0 / (1.0 + k_ * g4_);\n            ++coefGen_;\n            return;\n        }\n"
        "        k_ = resonance::ceiling::feedbackK (resonance::ceiling::kClassicMaxK, res);\n"
        "        const Real gStable = 1.0 / std::sqrt (1.0 + kLagStabilityK * k_); // see kLagStabilityK\n"
        "        g_ = std::min (gTan / (1.0 + gTan), gStable); // TPT one-pole coefficient G\n")])
    # (c) Scream: the stability limit on g (sqrt + 2 div) depends on r only.
    P.edit(f"{INC}/ScreamFilter.h", "E3c Scream limit cache member", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/ForkConfig.h>\n"),
        ("        Real r_ = 0.0; // resonance, r = K - 1 in [0, ~2)\n",
         "        Real r_ = 0.0; // resonance, r = K - 1 in [0, ~2)\n"
         "        Real cachedR_ = -1.0, cachedGLimit_ = 0.0; // FIREFORK E3c\n")])
    P.edit("src/ScreamFilter.cpp", "E3c Scream limit cache", [(
        "        const Real g = r > 1.0 ? std::min (gTan, std::sqrt (1.0 + 1.0 / (kLagStabilityR * (r - 1.0))) - 1.0) : gTan;\n",
        "        Real g = gTan;\n"
        "        if constexpr (fork::kExact) // FIREFORK E3c: the same limit, recomputed only when r moves\n"
        "        {\n            if (r > 1.0)\n            {\n"
        "                if (r != cachedR_)\n                {\n                    cachedR_ = r;\n"
        "                    cachedGLimit_ = std::sqrt (1.0 + 1.0 / (kLagStabilityR * (r - 1.0))) - 1.0;\n                }\n"
        "                g = std::min (gTan, cachedGLimit_);\n            }\n        }\n"
        "        else\n            g = r > 1.0 ? std::min (gTan, std::sqrt (1.0 + 1.0 / (kLagStabilityR * (r - 1.0))) - 1.0) : gTan;\n")])
    # (d) the voice maps Q -> resonance01 on every push; cache it on Q.
    P.edit(f"{INC}/CathodeModulatedVoice.h", "E3d resonance01ForQ cache", [
        ("            outputStage_.voice().setCutoff (cutoffHz, q);\n"
         "            outputStage_.voice().setModelCutoffReso (cutoffHz, resonance::resonance01ForQ (q));\n",
         "            outputStage_.voice().setCutoff (cutoffHz, q);\n"
         "            float reso01;\n"
         "            if constexpr (fork::kExact) // FIREFORK E3d: two divisions per push, the same value while Q stands still\n"
         "            {\n                if (!(q == pushedQ_))\n                {\n"
         "                    pushedQ_ = q;\n                    pushedReso01_ = resonance::resonance01ForQ (q);\n                }\n"
         "                reso01 = pushedReso01_;\n            }\n"
         "            else\n                reso01 = resonance::resonance01ForQ (q);\n"
         "            outputStage_.voice().setModelCutoffReso (cutoffHz, reso01);\n"),
        ("        float organicCutoffMult_ = 1.0f;\n",
         "        float organicCutoffMult_ = 1.0f;\n"
         "        float pushedQ_ = -1.0f, pushedReso01_ = 0.0f; // FIREFORK E3d (Q is always > 0, so -1 never matches)\n")])
    # E5: a modulated vowel recomputed the formant bank (6 band-passes, tan + divisions each) every sample in every mode. The bank's
    # coefficients are a pure function of (last vowel, last Q), so the vowel can wait until Formant is (or becomes) the mode.
    P.edit(f"{INC}/CathodeFilterBank.h", "E5 vowel only in Formant mode", [
        ("        void setVowel (float position) noexcept { formant_.setVowel (position); }\n",
         "        void setVowel (float position) noexcept\n        {\n"
         "            if constexpr (fork::kExact) // FIREFORK E5\n"
         "                if (filterMode_ != FilterMode::Formant)\n                {\n"
         "                    vowelPending_ = true;\n                    pendingVowel_ = position;\n                    return;\n                }\n"
         "            formant_.setVowel (position);\n            vowelPending_ = false;\n        }\n"),
        ("            if constexpr (fork::kExact) // FIREFORK E3a: entering an SVF mode applies the deferred cutoff\n",
         "            if constexpr (fork::kExact) // FIREFORK E5: entering Formant applies the deferred vowel\n"
         "                if (vowelPending_ && mode == FilterMode::Formant)\n                {\n"
         "                    formant_.setVowel (pendingVowel_);\n                    vowelPending_ = false;\n                }\n"
         "            if constexpr (fork::kExact) // FIREFORK E3a: entering an SVF mode applies the deferred cutoff\n"),
        ("        float svfHz_ = 0.0f, svfQ_ = 0.0f;\n",
         "        float svfHz_ = 0.0f, svfQ_ = 0.0f;\n        float pendingVowel_ = 0.0f; // FIREFORK E5\n        bool vowelPending_ = false;\n")])
    # E4: with the stereo right chain running, every cutoff push ran each core's coefficient maths twice (left bank, then its twin,
    # with identical inputs and identical mode). The twin now copies the left bank's freshly computed coefficients instead. Comb and
    # Formant keep calling the setter on both (their setters touch more than coefficients).
    COPIES = {
        "Svf.h": ("        void setCutoff (float cutoffHz, float q) noexcept;\n", "TptSvf", ["g_", "k_", "a1_", "a2_", "a3_"], ""),
        "AcidFilter.h": ("        void setCutoff (float cutoffHz, float resonance) noexcept;\n", "AcidFilter", ["g_", "oneMinusG_", "g2_", "g3_", "k_", "gComp_"], ""),
        "SovietFilter.h": ("        void setCutoff (float cutoffHz, float resonance) noexcept;\n", "SovietFilter", ["k_", "a1_", "a2_", "a3_"], ""),
        "ReedFilter.h": ("        void setCutoff (float cutoffHz, float resonance) noexcept;\n", "ReedFilter", ["k_", "a1_", "a2_", "a3_"], ""),
        "StateFilter.h": ("        void setCutoff (float cutoffHz, float resonance) noexcept;\n", "StateFilter", ["k_", "a1_", "a2_", "a3_"], ""),
        "ScreamFilter.h": ("        void setCutoff (float cutoffHz, float resonance) noexcept;\n", "ScreamFilter",
                           ["g_", "r_", "gr_", "twoGR_", "oneMinusGR_", "gOnePlus2r_", "onePlus2g_", "invDet_", "damp_", "cachedR_", "cachedGLimit_"], ""),
        "ClassicFilter.h": ("        void setCutoff (float cutoffHz, float resonance) noexcept;\n", "ClassicFilter",
                            ["g_", "oneMinusG_", "g2_", "g3_", "g4_", "k_", "compGain_", "invComp_", "gComp_", "cachedRes_", "cachedGStable_"], " ++coefGen_;"),
        "LadderFilter.h": ("        void setCutoff (float cutoffHz, float resonance) noexcept;\n", "LadderFilter", ["cutoffHz_", "reso_", "k_", "lin_", "drv_"], ""),
    }
    for f, (anchor, cls, members, extra) in COPIES.items():
        body = " ".join(f"{m} = o.{m};" for m in members)
        P.edit(f"{INC}/{f}", f"E4 {cls}::copyCoefficientsFrom", [(anchor, anchor +
            f"        // FIREFORK E4: what setCutoff would compute, taken from a twin that just ran it with the same inputs.\n"
            f"        void copyCoefficientsFrom (const {cls}& o) noexcept {{ {body}{extra} }}\n")])
    P.edit(f"{INC}/CathodeFilterBank.h", "E4 bank copyCoefficientsFrom", [(
        "        // FIREFORK E3a\n        static constexpr bool isSvfMode",
        "    public:\n"
        "        // FIREFORK E4: everything the cutoff setters (setCutoff, setModelCutoffReso, setCutoffQ) leave in this bank, copied from a\n"
        "        // bank in the same mode that just received the same call. Not for Comb or Formant (see coefficientsCopyable).\n"
        "        [[nodiscard]] bool coefficientsCopyable() const noexcept { return filterMode_ != FilterMode::Comb && filterMode_ != FilterMode::Formant; }\n"
        "        void copyCoefficientsFrom (const CathodeFilterBank& o) noexcept\n        {\n"
        "            filter_.copyCoefficientsFrom (o.filter_);\n"
        "            svfHz_ = o.svfHz_;\n            svfQ_ = o.svfQ_;\n            svfPending_ = o.svfPending_;\n"
        "            appliedModelCutoffHz_ = o.appliedModelCutoffHz_;\n            appliedModelReso01_ = o.appliedModelReso01_;\n"
        "            selfOscBlend_ = o.selfOscBlend_;\n"
        "            switch (filterMode_)\n            {\n"
        "                case FilterMode::Ladder24: case FilterMode::Ladder12: ladder_.copyCoefficientsFrom (o.ladder_); break;\n"
        "                case FilterMode::ScreamLp: case FilterMode::ScreamHp: scream_.copyCoefficientsFrom (o.scream_); break;\n"
        "                case FilterMode::Acid: acid_.copyCoefficientsFrom (o.acid_); break;\n"
        "                case FilterMode::SovietLp: case FilterMode::SovietBp: case FilterMode::SovietHp: soviet_.copyCoefficientsFrom (o.soviet_); break;\n"
        "                case FilterMode::ClassicLp24: case FilterMode::ClassicLp12: case FilterMode::ClassicBp: case FilterMode::ClassicHp:\n"
        "                    classic_.copyCoefficientsFrom (o.classic_); break;\n"
        "                case FilterMode::ReedLp: case FilterMode::ReedBp: case FilterMode::ReedHp: case FilterMode::ReedMorph: reed_.copyCoefficientsFrom (o.reed_); break;\n"
        "                case FilterMode::StateLp: case FilterMode::StateBp: case FilterMode::StateHp: case FilterMode::StateNotch: case FilterMode::StateAp:\n"
        "                case FilterMode::StateMorph: state_.copyCoefficientsFrom (o.state_); break;\n"
        "                default: break;\n            }\n        }\n\n    private:\n"
        "        // FIREFORK E3a\n        static constexpr bool isSvfMode")])
    P.edit(f"{INC}/CathodeVoiceStage.h", "E4 twin copies coefficients", [
        ("        void setCutoff (float cutoffHz, float q) noexcept\n        {\n"
         "            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setCutoff (cutoffHz, q); });\n        }\n",
         "        void setCutoff (float cutoffHz, float q) noexcept\n        {\n"
         "            editFilter1Coefficients ([&] (CathodeFilterBank& b) noexcept { b.setCutoff (cutoffHz, q); });\n        }\n"),
        ("            editFilter1 ([&] (CathodeFilterBank& b) noexcept { b.setModelCutoffReso (cutoffHz, reso01); });\n",
         "            editFilter1Coefficients ([&] (CathodeFilterBank& b) noexcept { b.setModelCutoffReso (cutoffHz, reso01); });\n"),
        ("            editFilter2 ([&] (CathodeFilterBank& b) noexcept { b.setCutoffQ (cutoffHz, q); });\n",
         "            editFilter2Coefficients ([&] (CathodeFilterBank& b) noexcept { b.setCutoffQ (cutoffHz, q); });\n"),
        ("    private:\n        struct RightChain;\n",
         "        // FIREFORK E4: editFilter* for the cutoff setters: the right twin copies the coefficients the left bank just computed.\n"
         "        template <typename Fn>\n        void editFilter1Coefficients (Fn&& fn) noexcept\n        {\n"
         "            fn (f1_);\n            if (rightRun_)\n            {\n"
         "                if (fork::kExact && f1_.coefficientsCopyable())\n                    right_->f1.copyCoefficientsFrom (f1_);\n"
         "                else\n                    fn (right_->f1);\n            }\n        }\n"
         "        template <typename Fn>\n        void editFilter2Coefficients (Fn&& fn) noexcept\n        {\n"
         "            fn (f2_);\n            if (rightRun_)\n            {\n"
         "                if (fork::kExact && f2_.coefficientsCopyable())\n                    right_->f2.copyCoefficientsFrom (f2_);\n"
         "                else\n                    fn (right_->f2);\n            }\n        }\n\n"
         "    private:\n        struct RightChain;\n")])
    # E15 (found by the upstream review): the Bitmask oscillator generates its OS stream but still drove the interpolating FIR with
    # a constant 0 whose upsampled value its shaper ignores (six call sites). decimateSample is documented bit-identical (D-M-0356),
    # as the PD oscillator already uses it.
    P.edit(f"{INC}/Oversampler.h", "E15 generateSample", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/ForkConfig.h>\n"),
        ("        // FIREFORK E2: only advance the input history",
         "        // FIREFORK E15: a GENERATED OS stream (the shaper ignores its argument): decimateSample with NEMESIS_FORK_EXACT,\n"
         "        // otherwise the upstream processSample (0, ...) form, unchanged.\n"
         "        template <typename Source>\n        float generateSample (Source&& next) noexcept\n        {\n"
         "            if constexpr (fork::kExact)\n                return decimateSample (next);\n"
         "            else\n                return processSample (0.0f, [&next] (float) noexcept { return next(); });\n        }\n"
         "        // FIREFORK E2: only advance the input history")])
    p = os.path.join(P.root, INC, "BitmaskOscillator.h")
    s = open(p).read()
    s, n = re.subn(r"\.processSample \(0\.0f, (\[[^\]]*\]) \(float\) noexcept \{", r".generateSample (/* FIREFORK E15 */ \1 () noexcept {", s)
    assert n == 6, f"E15: expected 6 Bitmask generator sites, found {n}"
    open(p, "w").write(s)
    P.log.append("  E15 Bitmask generateSample (6 sites)  include/nemesis/dsp/BitmaskOscillator.h")
    # E7: vibrato retunes every oscillator every sample. Exact parts: the PD saw knee depends on (amount, shape, eps(inc)) and only
    # rarely moves, so its two divisions are skipped when the knee is unchanged (the slopes are a function of the knee alone); the
    # increments' division by the oversampling factor is a multiply by an exact power-of-two reciprocal.
    P.edit(f"{INC}/PhaseDistortionOscillator.h", "E7 PD knee early-out + factor multiply", [
        ("            if (d > 1.0 - eps)\n                d = 1.0 - eps;\n            knee_ = d;\n",
         "            if (d > 1.0 - eps)\n                d = 1.0 - eps;\n"
         "            if constexpr (fork::kExact) // FIREFORK E7: seg0/seg1 slopes are 0.5/d and 0.5/(1-d): unchanged with d\n"
         "                if (d == knee_)\n                    return;\n"
         "            knee_ = d;\n"),
        ("            incOs_ = factor_ > 0 ? inc_ / static_cast<Real> (factor_) : inc_;\n            resolveSawKnee();\n",
         "            if constexpr (fork::kExact) // FIREFORK E7: exact for the power-of-two factors\n"
         "                incOs_ = factor_ > 0 ? fork::divByFactor (inc_, factor_) : inc_;\n            else\n"
         "                incOs_ = factor_ > 0 ? inc_ / static_cast<Real> (factor_) : inc_;\n            resolveSawKnee();\n")])
    P.edit(f"{INC}/BitmaskOscillator.h", "E7 BM factor multiply", [(
        "            incOs_ = f > 0 ? inc_ / static_cast<Real> (f) : inc_;\n",
        "            if constexpr (fork::kExact) // FIREFORK E7\n                incOs_ = f > 0 ? fork::divByFactor (inc_, f) : inc_;\n"
        "            else\n                incOs_ = f > 0 ? inc_ / static_cast<Real> (f) : inc_;\n")])
    # E13: the WT/Pulse unison stack divides freq by the rate every sample; cache it on (freq, rate).
    P.edit(f"{INC}/CathodeOscCore.h", "E13 stack increment cache", [
        ("            Wide freqHz = 0.0; // noteHz · mult\n",
         "            Wide freqHz = 0.0; // noteHz · mult\n"
         "            Wide incKeyHz = -1.0, incKeySr = -1.0, incCached = 0.0; // FIREFORK E13: freqHz / sampleRate_, cached\n"),
        ("                    if (wt ? !s.wtBound : !s.pulse.live())\n                        return 0.0f;\n"
         "                    const Wide inc = sampleRate_ > 0.0 ? s.freqHz / sampleRate_ : 0.0;\n",
         "                    if (wt ? !s.wtBound : !s.pulse.live())\n                        return 0.0f;\n"
         "                    const Wide inc = stackInc (s);\n"),
        ("                    if (wt ? !s.wtBound : !s.pulse.live())\n                        break;\n"
         "                    const Wide inc = sampleRate_ > 0.0 ? s.freqHz / sampleRate_ : 0.0;\n",
         "                    if (wt ? !s.wtBound : !s.pulse.live())\n                        break;\n"
         "                    const Wide inc = stackInc (s);\n"),
        ("        void updateFrequency (int slot) noexcept\n",
         "        // FIREFORK E13: the same expression as before, evaluated only when the frequency or the rate moved.\n"
         "        Wide stackInc (Slot& s) noexcept\n        {\n"
         "            if constexpr (!fork::kExact)\n                return sampleRate_ > 0.0 ? s.freqHz / sampleRate_ : 0.0;\n"
         "            if (s.freqHz != s.incKeyHz || sampleRate_ != s.incKeySr)\n            {\n"
         "                s.incKeyHz = s.freqHz;\n                s.incKeySr = sampleRate_;\n"
         "                s.incCached = sampleRate_ > 0.0 ? s.freqHz / sampleRate_ : 0.0;\n            }\n"
         "            return s.incCached;\n        }\n\n"
         "        void updateFrequency (int slot) noexcept\n")])
    # E8: the matrix walked all 24 slots per sample (real patches always have hard-wired slots active, so the idle skip rarely
    # fires), and every LFO computed its waveform value even when no active slot reads it. The matrix now keeps its active slots
    # in slot order (so every destination sum adds the same terms in the same order) and a mask of the sources they read; an LFO
    # no slot reads only advances (the value is a pure function of the phase, so nothing it would have produced is lost).
    P.edit(f"{INC}/CathodeModMatrix.h", "E8 active-slot list + source mask", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/ForkConfig.h>\n"),
        ("            slotRef.active = true;\n            if (!sameDest)\n                rebuildTouched();\n",
         "            slotRef.active = true;\n            if (!sameDest)\n                rebuildTouched();\n"
         "            rebuildActive(); // FIREFORK E8: the source may change while the destination stays\n"),
        ("            slots_[static_cast<std::size_t> (slot)] = Slot {};\n            rebuildTouched();\n",
         "            slots_[static_cast<std::size_t> (slot)] = Slot {};\n            rebuildTouched();\n            rebuildActive(); // FIREFORK E8\n"),
        ("            for (auto& slot : slots_)\n                slot = Slot {};\n            rebuildTouched();\n",
         "            for (auto& slot : slots_)\n                slot = Slot {};\n            rebuildTouched();\n            rebuildActive(); // FIREFORK E8\n"),
        ("            if (userSlotGain_ == 1.0)\n            {\n                for (const auto& slot : slots_)\n",
         "            if constexpr (fork::kExact) // FIREFORK E8: the active slots only, in slot order (the sums are unchanged)\n            {\n"
         "                if (userSlotGain_ == 1.0)\n                {\n"
         "                    for (int a = 0; a < activeCount_; ++a)\n                    {\n"
         "                        const auto& slot = slots_[static_cast<std::size_t> (activeIdx_[static_cast<std::size_t> (a)])];\n"
         "                        destValue_[static_cast<std::size_t> (slot.dest)] += sourceValue_[static_cast<std::size_t> (slot.src)] * slot.depth;\n"
         "                    }\n                    return;\n                }\n"
         "                for (int a = 0; a < activeCount_; ++a)\n                {\n"
         "                    const int i = activeIdx_[static_cast<std::size_t> (a)];\n"
         "                    const auto& slot = slots_[static_cast<std::size_t> (i)];\n"
         "                    const Real v = sourceValue_[static_cast<std::size_t> (slot.src)] * slot.depth;\n"
         "                    destValue_[static_cast<std::size_t> (slot.dest)] += i < kUserSlots ? v * userSlotGain_ : v;\n"
         "                }\n                return;\n            }\n"
         "            if (userSlotGain_ == 1.0)\n            {\n                for (const auto& slot : slots_)\n"),
        ("        [[nodiscard]] bool idle() const noexcept { return touchedCount_ == 0; }\n",
         "        [[nodiscard]] bool idle() const noexcept { return touchedCount_ == 0; }\n"
         "        // FIREFORK E8: whether any active slot reads `src` (always true without NEMESIS_FORK_EXACT).\n"
         "        [[nodiscard]] bool reads (Source src) const noexcept\n        {\n"
         "            return !fork::kExact || ((srcMask_ >> static_cast<unsigned> (src)) & 1u) != 0u;\n        }\n"),
        ("        int touchedCount_ = 0;\n        Real userSlotGain_ = 1.0;",
         "        int touchedCount_ = 0;\n"
         "        // FIREFORK E8: the active slots in slot order, and the sources they read.\n"
         "        static_assert (kSourceStorage <= 64 && kMaxSlots <= 255, \"FIREFORK E8: the source mask is 64 bits, slot indices 8\");\n"
         "        std::array<std::uint8_t, kMaxSlots> activeIdx_ {};\n        int activeCount_ = 0;\n        std::uint64_t srcMask_ = 0;\n"
         "        void rebuildActive() noexcept\n        {\n            activeCount_ = 0;\n            srcMask_ = 0;\n"
         "            for (int i = 0; i < kMaxSlots; ++i)\n            {\n                const auto& slot = slots_[static_cast<std::size_t> (i)];\n"
         "                if (!slot.active)\n                    continue;\n"
         "                activeIdx_[static_cast<std::size_t> (activeCount_++)] = static_cast<std::uint8_t> (i);\n"
         "                srcMask_ |= std::uint64_t { 1 } << static_cast<unsigned> (slot.src);\n            }\n        }\n"
         "        Real userSlotGain_ = 1.0;")])
    P.edit(f"{INC}/Lfo.h", "E8 Lfo::tick", [(
        "            const Real out = depth_ * shapeValue (phase_);\n            advance();\n            return static_cast<float> (out);\n        }\n",
        "            const Real out = depth_ * shapeValue (phase_);\n            advance();\n            return static_cast<float> (out);\n        }\n\n"
        "        // FIREFORK E8: processSample without computing the value nobody reads (the hold and the advance are the same).\n"
        "        void tick() noexcept\n        {\n"
        "            if (delayCounter_ > 0)\n            {\n                --delayCounter_;\n                if (runThroughHold_)\n                    advance();\n                return;\n            }\n"
        "            advance();\n        }\n")])
    P.edit(f"{INC}/CathodeModulatedVoice.h", "E8 unread LFOs only advance", [(
        "            const float l1 = lfo1_.processSample();\n            const float l2 = lfo2_.processSample();\n"
        "            const float l3 = lfo3_.processSample();\n            const float st = step_.processSample();\n            const float sh = sh_.processSample();\n",
        "            // FIREFORK E8: an LFO whose value no active slot reads (or nothing reads: the idle skip) only advances.\n"
        "            const bool fed = !(fork::Config::idleMatrixSkip && matrix_.idle());\n"
        "            const auto tickLfo = [fed, this] (Lfo& lfo, Source src) noexcept {\n"
        "                if (fed && matrix_.reads (src))\n                    return lfo.processSample();\n"
        "                lfo.tick();\n                return 0.0f;\n            };\n"
        "            const float l1 = tickLfo (lfo1_, Source::Lfo1);\n            const float l2 = tickLfo (lfo2_, Source::Lfo2);\n"
        "            const float l3 = tickLfo (lfo3_, Source::Lfo3);\n            const float st = step_.processSample();\n"
        "            const float sh = tickLfo (sh_, Source::SampleHold);\n")])
    # E9: request flags (DRAW on/off, DRAW lengths, F2 on, route, balance) were polled per voice per sample: ~11 loads, and
    # applyDrawEdge even twice. Each request setter now also raises a per-object dirty flag; the poll runs only when it is up.
    # The polls are idempotent while nothing changed (each compares the request with what it latched), so skipping them is exact.
    # The flag is cleared BEFORE the requests are read, so a request stored while the poll runs raises it again.
    DIRTY_CLEAR = ("                if (!{f}.load (std::memory_order_acquire))\n                    return;\n"
                   "                {f}.store (false, std::memory_order_relaxed);\n")
    P.edit(f"{INC}/CathodeOutputStage.h", "E9 OutputStage draw dirty flag", [
        ("        void setAmpDraw (bool on) noexcept { ampDrawReq_.store (on, std::memory_order_relaxed); }\n",
         "        void setAmpDraw (bool on) noexcept\n        {\n            ampDrawReq_.store (on, std::memory_order_relaxed);\n"
         "            drawDirty_.store (true, std::memory_order_release); // FIREFORK E9\n        }\n"),
        ("        void setAmpDrawLengthMs (Real ms) noexcept { draw_.requestLengthMs (ms); }\n",
         "        void setAmpDrawLengthMs (Real ms) noexcept\n        {\n            draw_.requestLengthMs (ms);\n"
         "            drawDirty_.store (true, std::memory_order_release); // FIREFORK E9\n        }\n"),
        ("        void applyDrawEdge() noexcept\n        {\n            draw_.applyLengthRequest();\n",
         "        void applyDrawEdge() noexcept\n        {\n            if constexpr (fork::kExact) // FIREFORK E9\n            {\n"
         + DIRTY_CLEAR.format(f="drawDirty_") + "            }\n            draw_.applyLengthRequest();\n"),
        ("        std::atomic<bool> ampDrawReq_ { false };",
         "        std::atomic<bool> drawDirty_ { true }; // FIREFORK E9: a draw request may be pending\n        std::atomic<bool> ampDrawReq_ { false };")])
    P.edit(f"{INC}/CathodeModulatedVoice.h", "E9 ModulatedVoice env draw dirty flag", [
        ("                envDrawReq_[static_cast<std::size_t> (n - 2)].store (on, std::memory_order_relaxed); // [row 303]\n",
         "                envDrawReq_[static_cast<std::size_t> (n - 2)].store (on, std::memory_order_relaxed); // [row 303]\n"
         "            envDrawDirty_.store (true, std::memory_order_release); // FIREFORK E9\n"),
        ("                envDraw_[static_cast<std::size_t> (n - 2)].requestLengthMs (ms);\n",
         "                envDraw_[static_cast<std::size_t> (n - 2)].requestLengthMs (ms);\n"
         "            envDrawDirty_.store (true, std::memory_order_release); // FIREFORK E9\n"),
        ("            outputStage_.applyDrawEdge();\n            for (std::size_t i = 0; i < envDraw_.size(); ++i)\n",
         "            outputStage_.applyDrawEdge();\n            if constexpr (fork::kExact) // FIREFORK E9\n            {\n"
         + DIRTY_CLEAR.format(f="envDrawDirty_") + "            }\n"
         "            for (std::size_t i = 0; i < envDraw_.size(); ++i)\n"),
        ("        std::array<std::atomic<bool>, 2> envDrawReq_ {};",
         "        std::atomic<bool> envDrawDirty_ { true }; // FIREFORK E9\n        std::array<std::atomic<bool>, 2> envDrawReq_ {};")])
    P.edit(f"{INC}/CathodeVoiceStage.h", "E9 VoiceStage filter request dirty flag", [
        ("        void setFilter2On (bool on) noexcept { f2OnReq_.store (on, std::memory_order_relaxed); }\n",
         "        void setFilter2On (bool on) noexcept\n        {\n            f2OnReq_.store (on, std::memory_order_relaxed);\n"
         "            filterReqDirty_.store (true, std::memory_order_release); // FIREFORK E9\n        }\n"),
        ("        void setFilterRoute (FilterRoute route) noexcept { routeReq_.store (route, std::memory_order_relaxed); }\n",
         "        void setFilterRoute (FilterRoute route) noexcept\n        {\n            routeReq_.store (route, std::memory_order_relaxed);\n"
         "            filterReqDirty_.store (true, std::memory_order_release); // FIREFORK E9\n        }\n"),
        ("            balanceReq_.store (b < -1.0f ? -1.0f : (b > 1.0f ? 1.0f : b), std::memory_order_relaxed);\n",
         "            balanceReq_.store (b < -1.0f ? -1.0f : (b > 1.0f ? 1.0f : b), std::memory_order_relaxed);\n"
         "            filterReqDirty_.store (true, std::memory_order_release); // FIREFORK E9\n"),
        ("        void applyFilter2Edge() noexcept\n        {\n",
         "        void applyFilter2Edge() noexcept\n        {\n            if constexpr (fork::kExact) // FIREFORK E9\n            {\n"
         + DIRTY_CLEAR.format(f="filterReqDirty_") + "            }\n"),
        ("        std::atomic<bool> f2OnReq_ { false };",
         "        std::atomic<bool> filterReqDirty_ { true }; // FIREFORK E9\n        std::atomic<bool> f2OnReq_ { false };")])
    # E10: processCore dispatched through an if-chain (~25 compares before LowPass). One switch, the same calls per mode.
    P.edit(f"{INC}/CathodeFilterBank.h", "E10 processCore switch", [(
        "        [[nodiscard]] float processCore (float toFilter) noexcept\n        {\n            float out = 0.0f;\n",
        "        [[nodiscard]] float processCore (float toFilter) noexcept\n        {\n"
        "            if constexpr (fork::kExact) // FIREFORK E10: one jump table instead of the if-chain; each arm is the chain's call\n"
        "            {\n                switch (filterMode_)\n                {\n"
        "                    case FilterMode::LowPass: return filter_.processSample (toFilter).lp;\n"
        "                    case FilterMode::BandPass: return filter_.processSample (toFilter).bp;\n"
        "                    case FilterMode::HighPass: return filter_.processSample (toFilter).hp;\n"
        "                    case FilterMode::Notch: return filter_.processSample (toFilter).notch;\n"
        "                    case FilterMode::Formant: return formant_.processSample (toFilter);\n"
        "                    case FilterMode::Comb: return comb_.processSample (toFilter);\n"
        "                    case FilterMode::Ladder24: return ladder_.processSample (toFilter).lp24;\n"
        "                    case FilterMode::Ladder12: return ladder_.processSample (toFilter).lp12;\n"
        "                    case FilterMode::ScreamLp: case FilterMode::ScreamHp: return scream_.processSample (toFilter);\n"
        "                    case FilterMode::Acid: return acid_.processSample (toFilter);\n"
        "                    case FilterMode::SovietLp: case FilterMode::SovietBp: case FilterMode::SovietHp: return soviet_.processSample (toFilter);\n"
        "                    case FilterMode::ClassicLp24: case FilterMode::ClassicLp12: case FilterMode::ClassicBp: case FilterMode::ClassicHp:\n"
        "                        return classic_.processSample (toFilter);\n"
        "                    case FilterMode::ReedLp: case FilterMode::ReedBp: case FilterMode::ReedHp: case FilterMode::ReedMorph:\n"
        "                        return reed_.processSample (toFilter);\n"
        "                    case FilterMode::StateLp: case FilterMode::StateBp: case FilterMode::StateHp: case FilterMode::StateNotch:\n"
        "                    case FilterMode::StateAp: case FilterMode::StateMorph: return state_.processSample (toFilter);\n"
        "                }\n                return 0.0f;\n            }\n"
        "            float out = 0.0f;\n")])
    # E12: without lanes (fork::Config::lanes = false, the M7) the PD staging only moves the generated OS steps through a member
    # array and two flags. pdPlain then runs the slot's processSample(), which the header documents as bit-identical to
    # generateSteps + decimateSteps. That processSample keeps the Saw generator's state in locals: the stores into the OS history
    # may alias the float members, so the compiler reloaded phase, increment, knee and slopes on every OS step.
    P.edit(f"{INC}/CathodeOscCore.h", "E12 no PD staging without lanes", [(
        "            PdStage& st = pdStage_[static_cast<std::size_t> (slot)];\n            st.staged = false;\n            st.done = false;\n",
        "            PdStage& st = pdStage_[static_cast<std::size_t> (slot)];\n            st.staged = false;\n            st.done = false;\n"
        "            if constexpr (fork::kExact) // FIREFORK E12: no lane will read the staged steps\n"
        "                if (!fork::Config::lanes)\n                    return;\n")])
    P.edit(f"{INC}/PhaseDistortionOscillator.h", "E12 PD Saw steps from locals", [(
        "        float processSample() noexcept\n        {\n            return os_.decimateSample ([this]() noexcept { return generateOsSample(); });\n        }\n",
        "        float processSample() noexcept\n        {\n"
        "            if constexpr (fork::kExact) // FIREFORK E12: the Saw generator from locals, then the same decimation\n"
        "                if (type_ == PdType::Saw && factor_ > 1 && factor_ <= kMaxOsStepsFork)\n                {\n"
        "                    float steps[kMaxOsStepsFork];\n"
        "                    Wide ph = phase_;\n                    const Wide inc = incOs_;\n"
        "                    const Real knee = knee_, s0 = seg0Slope_, s1 = seg1Slope_;\n"
        "                    for (int p = 0; p < factor_; ++p)\n                    {\n"
        "                        ph = wrap01 (ph + inc);\n"
        "                        const Real x = static_cast<Real> (ph);\n"
        "                        const Real w = x < knee ? s0 * x : 0.5 + s1 * (x - knee);\n"
        "                        steps[p] = static_cast<float> (sine (w) - kTwoOverPi * (2.0 * knee - 1.0));\n                    }\n"
        "                    phase_ = ph;\n                    int q = 0;\n"
        "                    return os_.decimateSample ([&q, &steps]() noexcept { return steps[q++]; });\n                }\n"
        "            return os_.decimateSample ([this]() noexcept { return generateOsSample(); });\n        }\n"
        "        static constexpr int kMaxOsStepsFork = 8; // FIREFORK E12\n")])
    # E14: every Oversampler held its own copy of one of three coefficient designs (one per factor): ~1,650 instances, ~1.8 MB.
    # The designs are a pure function of the factor, so they are computed once, kept in one static pool, and shared. The
    # coefficients are the same numbers, so the output is bit-identical. The pool can be placed in fast memory (NEMESIS_FORK_FAST_DATA).
    P.edit(f"{INC}/Oversampler.h", "E14 shared coefficient tables", [
        ("        std::vector<Real> up_; // flat [factor_ * phaseTaps_] polyphase up coefficients\n"
         "        std::vector<Real> down_; // [downTaps_] decimation FIR coefficients\n",
         "#if NEMESIS_FORK_EXACT\n"
         "    public:\n"
         "        // FIREFORK E14: a view of a design held in the shared pool (Oversampler.cpp), with the vector calls this class uses.\n"
         "        struct SharedTaps\n        {\n"
         "            const Real* p = nullptr;\n            std::size_t n = 0;\n"
         "            [[nodiscard]] const Real* data() const noexcept { return p; }\n"
         "            [[nodiscard]] std::size_t size() const noexcept { return n; }\n"
         "            void clear() noexcept\n            {\n                p = nullptr;\n                n = 0;\n            }\n        };\n"
         "        using TapStore = SharedTaps;\n"
         "    private:\n"
         "#else\n    public:\n        using TapStore = std::vector<Real>;\n    private:\n#endif\n"
         "        TapStore up_; // flat [factor_ * phaseTaps_] polyphase up coefficients\n"
         "        TapStore down_; // [downTaps_] decimation FIR coefficients\n"),
        ("    static_assert (sizeof (Oversampler) == 8 * sizeof (int) + 4 * sizeof (std::vector<Real>),",
         "    static_assert (sizeof (Oversampler) == 8 * sizeof (int) + 2 * sizeof (Oversampler::TapStore) + 2 * sizeof (std::vector<Real>),")])
    P.edit(f"{INC}/Oversampler.h", "E14 the shared pool", [(
        "namespace nemesis::dsp\n{\n",
        "namespace nemesis::dsp\n{\n"
        "#if NEMESIS_FORK_EXACT\n"
        "    // FIREFORK E14/N1: one pool for every oversampler's coefficients (and, with NEMESIS_FORK_APPROX, the compact kernels'\n"
        "    // coefficients and tap offsets). Defined in Oversampler.cpp; NEMESIS_FORK_FAST_DATA places it (the firmware: DTCM).\n"
        "    struct ForkTapPool\n    {\n"
        "        static constexpr std::size_t kSize = 1024;\n"
        "        static Real pool[kSize];\n        static std::uint16_t idx[kSize];\n        static std::size_t used;\n    };\n"
        "#endif\n", )])
    P.edit("src/Oversampler.cpp", "E14 pool storage", [(
        "namespace nemesis::dsp\n{\n",
        "namespace nemesis::dsp\n{\n"
        "#if NEMESIS_FORK_EXACT\n#ifndef NEMESIS_FORK_FAST_DATA\n#define NEMESIS_FORK_FAST_DATA\n#endif\n"
        "    NEMESIS_FORK_FAST_DATA Real ForkTapPool::pool[ForkTapPool::kSize];\n"
        "    NEMESIS_FORK_FAST_DATA std::uint16_t ForkTapPool::idx[ForkTapPool::kSize];\n"
        "    std::size_t ForkTapPool::used = 0;\n#endif\n")])
    P.edit("src/Oversampler.cpp", "E14 intern the designs", [
        ("        constexpr int kHalfTapsPerPhase = 8;\n",
         "        constexpr int kHalfTapsPerPhase = 8;\n"
         "#if NEMESIS_FORK_EXACT\n"
         "        // FIREFORK E14: the shared designs. Filled by prepare() (off the audio thread; not thread-safe, the firmware prepares\n"
         "        // from one thread), never freed, never moved, so the views stay valid. Factors 2, 4, 8 need 465 coefficients in all.\n"
         "        constexpr std::size_t kPoolSize = ForkTapPool::kSize;\n"
         "        Real* const gPool = ForkTapPool::pool;\n"
         "        std::size_t& gPoolUsed = ForkTapPool::used;\n"
         "        struct Design\n        {\n            int factor = 0;\n            std::size_t up = 0, nUp = 0, down = 0, nDown = 0;\n        };\n"
         "        Design gDesigns[8];\n        int gDesignCount = 0;\n"
         "        Oversampler::SharedTaps intern (const std::vector<Real>& v) noexcept\n        {\n"
         "            if (gPoolUsed + v.size() > kPoolSize)\n                __builtin_trap(); // a design that does not fit is a build error in disguise: fail loudly\n"
         "            Real* dst = gPool + gPoolUsed;\n            for (std::size_t i = 0; i < v.size(); ++i)\n                dst[i] = v[i];\n"
         "            gPoolUsed += v.size();\n            return { dst, v.size() };\n        }\n"
         "#endif\n"),
        ("        up_.assign (static_cast<std::size_t> (l) * static_cast<std::size_t> (phaseTaps_), 0.0);\n",
         "#if NEMESIS_FORK_EXACT\n"
         "        for (int k = 0; k < gDesignCount; ++k) // FIREFORK E14: designed before: share it\n"
         "            if (gDesigns[k].factor == l)\n            {\n"
         "                up_ = { gPool + gDesigns[k].up, gDesigns[k].nUp };\n"
         "                down_ = { gPool + gDesigns[k].down, gDesigns[k].nDown };\n"
         "                xhist_.assign (2 * static_cast<std::size_t> (phaseTaps_), 0.0);\n"
         "                ohist_.assign (2 * static_cast<std::size_t> (ohistLen_), 0.0);\n"
         "                xpos_ = 0;\n                opos_ = 0;\n                return;\n            }\n"
         "        std::vector<Real> upW;\n#else\n        std::vector<Real>& upW = up_;\n#endif\n"
         "        upW.assign (static_cast<std::size_t> (l) * static_cast<std::size_t> (phaseTaps_), 0.0);\n"),
        ("                    up_[dst] = static_cast<Real> (l) * h[static_cast<std::size_t> (idx)];\n",
         "                    upW[dst] = static_cast<Real> (l) * h[static_cast<std::size_t> (idx)];\n"),
        ("        down_ = h;\n",
         "#if NEMESIS_FORK_EXACT\n"
         "        {\n            const auto u = intern (upW);\n            const auto d = intern (h);\n"
         "            up_ = u;\n            down_ = d;\n"
         "            if (u.data() != nullptr && d.data() != nullptr && gDesignCount < 8)\n"
         "                gDesigns[gDesignCount++] = { l, static_cast<std::size_t> (u.data() - gPool), u.size(),\n"
         "                                             static_cast<std::size_t> (d.data() - gPool), d.size() };\n        }\n"
         "#else\n        down_ = h;\n#endif\n")])
    # B5: startRightChain copied the whole bank into the twin in one sample, the Comb ring (fs/20 + 4 floats per bank) included.
    # A ring that is not in circuit never changes on the left either (the comb only runs in Comb mode, and every write reaches
    # both banks through editFilter*), so the twin may take it LATER, at the moment Comb becomes its mode. Same contents, same
    # moment of use: exact. Moving the rings out of the way makes the bank copy itself allocation- and copy-free for them.
    P.edit(f"{INC}/CombFilter.h", "B5 ring swap", [(
        "        // Control-rate parameters + derived coefficients.\n        Real fs_ = 44100.0;\n",
        "    public:\n        // FIREFORK B5: hand the delay ring over without copying it (the twin takes it lazily, see CathodeFilterBank).\n"
        "        void swapRing (std::vector<Real>& v) noexcept { buf_.swap (v); }\n"
        "        void copyRingFrom (const CombFilter& o) noexcept\n        {\n"
        "            if (buf_.size() == o.buf_.size())\n                std::copy (o.buf_.begin(), o.buf_.end(), buf_.begin());\n        }\n\n"
        "    private:\n        // Control-rate parameters + derived coefficients.\n        Real fs_ = 44100.0;\n")])
    P.edit(f"{INC}/CathodeFilterBank.h", "B5 twin copy without the comb ring", [(
        "    public:\n        // FIREFORK E4: everything the cutoff setters",
        "    public:\n"
        "        // FIREFORK B5: `*this = o` for the stereo twin, except the Comb ring, which is taken when Comb becomes the mode.\n"
        "        void assignForTwin (const CathodeFilterBank& o) noexcept\n        {\n"
        "            if (o.filterMode_ == FilterMode::Comb)\n            {\n                *this = o;\n                combRingStale_ = false;\n                return;\n            }\n"
        "            std::vector<Real> mine, theirs;\n"
        "            comb_.swapRing (mine);\n            const_cast<CombFilter&> (o.comb_).swapRing (theirs); // audio thread only; restored below\n"
        "            *this = o; // the comb's ring is empty on both sides here: nothing copied, nothing allocated\n"
        "            const_cast<CombFilter&> (o.comb_).swapRing (theirs);\n            comb_.swapRing (mine);\n"
        "            combRingStale_ = true;\n        }\n"
        "        // FIREFORK B5: called after every edit while the twin runs: Comb just became the mode, so take the ring now.\n"
        "        void refreshCombRingFrom (const CathodeFilterBank& o) noexcept\n        {\n"
        "            if (combRingStale_ && filterMode_ == FilterMode::Comb)\n            {\n                comb_.copyRingFrom (o.comb_);\n                combRingStale_ = false;\n            }\n        }\n"
        "        bool combRingStale_ = false;\n\n"
        "        // FIREFORK E4: everything the cutoff setters")])
    P.edit(f"{INC}/CathodeVoiceStage.h", "B5 lazy comb ring in the twin", [
        ("                r.f1 = f1_;\n                r.f2 = f2_;\n",
         "                if constexpr (fork::kExact) // FIREFORK B5\n                {\n"
         "                    r.f1.assignForTwin (f1_);\n                    r.f2.assignForTwin (f2_);\n                }\n"
         "                else\n                {\n                    r.f1 = f1_;\n                    r.f2 = f2_;\n                }\n"),
        ("        template <typename Fn>\n        void editFilter1 (Fn&& fn) noexcept\n        {\n            fn (f1_);\n            if (rightRun_)\n                fn (right_->f1);\n        }\n",
         "        template <typename Fn>\n        void editFilter1 (Fn&& fn) noexcept\n        {\n            fn (f1_);\n            if (rightRun_)\n            {\n"
         "                fn (right_->f1);\n                if constexpr (fork::kExact)\n                    right_->f1.refreshCombRingFrom (f1_); // FIREFORK B5\n            }\n        }\n"),
        ("        template <typename Fn>\n        void editFilter2 (Fn&& fn) noexcept\n        {\n            fn (f2_);\n            if (rightRun_)\n                fn (right_->f2);\n        }\n",
         "        template <typename Fn>\n        void editFilter2 (Fn&& fn) noexcept\n        {\n            fn (f2_);\n            if (rightRun_)\n            {\n"
         "                fn (right_->f2);\n                if constexpr (fork::kExact)\n                    right_->f2.refreshCombRingFrom (f2_); // FIREFORK B5\n            }\n        }\n")])

    # ==== 11. NEARLY EXACT optimizations (NEMESIS_FORK_APPROX; docs/DSP_REVIEW.md section 4). About 1 ulp .. 1e-8. ============
    A = "fork::kApprox"
    # N2: divisions by constants in the cubic soft clip (every oversampled step of every drive and non-linear core) become
    # multiplies by the reciprocal, and inside the knee the ADAA divided difference uses its closed form,
    # (F1(a) - F1(b)) / (a - b) = (a + b) * (1/2 - (a^2 + b^2) / 12), which needs no division and no near-equal fallback (and is more
    # accurate in float: the divided difference cancels catastrophically when a ~ b).
    P.edit(f"{INC}/Waveshaper.h", "N2 soft clip without divisions", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/ForkConfig.h>\n"),
        ("            const Real f1x0 = antiderivative1 (x0);\n\n            Real y;\n            if (diff > kEps || diff < -kEps)\n",
         "            const Real f1x0 = antiderivative1 (x0);\n\n            Real y;\n"
         f"            if ({A} && x0 < 1.0 && x0 > -1.0 && x1_ < 1.0 && x1_ > -1.0) // FIREFORK N2: the closed form inside the knee\n"
         "                y = (x0 + x1_) * (0.5 - (x0 * x0 + x1_ * x1_) * (1.0 / 12.0));\n"
         "            else if (diff > kEps || diff < -kEps)\n"),
        ("            return x - x * x * x / 3.0;\n",
         f"            if constexpr ({A}) // FIREFORK N2\n                return x - x * x * x * (1.0 / 3.0);\n            return x - x * x * x / 3.0;\n"),
        ("            if (x >= 1.0)\n                return 2.0 * x / 3.0 - 0.25;\n            if (x <= -1.0)\n                return -2.0 * x / 3.0 - 0.25;\n"
         "            const Real x2 = x * x;\n            return x2 * 0.5 - x2 * x2 / 12.0;\n",
         f"            if constexpr ({A}) // FIREFORK N2\n            {{\n"
         "                if (x >= 1.0)\n                    return x * (2.0 / 3.0) - 0.25;\n                if (x <= -1.0)\n                    return -x * (2.0 / 3.0) - 0.25;\n"
         "                const Real x2 = x * x;\n                return x2 * 0.5 - x2 * x2 * (1.0 / 12.0);\n            }\n"
         "            if (x >= 1.0)\n                return 2.0 * x / 3.0 - 0.25;\n            if (x <= -1.0)\n                return -2.0 * x / 3.0 - 0.25;\n"
         "            const Real x2 = x * x;\n            return x2 * 0.5 - x2 * x2 / 12.0;\n")])
    P.edit(f"{INC}/SelfOscLimiter.h", "N2 limiter reciprocal", [
        ("            const float limited = clip_.processSample (x * kLimitDrive) / kLimitDrive;\n",
         f"            const float limited = {A} ? clip_.processSample (x * kLimitDrive) * kInvLimitDrive // FIREFORK N2\n"
         "                                         : clip_.processSample (x * kLimitDrive) / kLimitDrive;\n"),
        ("                const float limited = p.clip.processSample (u * kLimitDrive) / kLimitDrive;\n",
         f"                const float limited = {A} ? p.clip.processSample (u * kLimitDrive) * kInvLimitDrive // FIREFORK N2\n"
         "                                             : p.clip.processSample (u * kLimitDrive) / kLimitDrive;\n"),
        ("        static constexpr float kLimitDrive = 2.0f / 3.0f;\n",
         "        static constexpr float kLimitDrive = 2.0f / 3.0f;\n        static constexpr float kInvLimitDrive = 1.0f / kLimitDrive; // FIREFORK N2\n")])
    P.edit(f"{INC}/SovietFilter.h", "N2 Soviet knee reciprocal", [(
        "                            : kNegKnee * AdaaSoftClip::shape (x / kNegKnee);",
        f"                            : kNegKnee * AdaaSoftClip::shape ({A} ? x * (1.0 / kNegKnee) : x / kNegKnee); // FIREFORK N2")])
    # N1: the oversampler kernels. The prototype is a windowed sinc at 1/L, so every L-th tap from the centre is a sinc zero (in
    # exact maths; ~1e-8 after the window and the float rounding): upsampler phase 0 is a pure delay (one tap), the other phases
    # end in an exact 0, and the decimator multiplies 1 in L of its taps by ~0. The decimator is also symmetric, so pairs are
    # folded (one multiply per pair, the design symmetrised: the two halves differ by rounding only). Four accumulators instead
    # of one serial FMA chain. Needs the shared pool (NEMESIS_FORK_EXACT): the compact decimator follows its taps in the pool.
    P.edit("src/Oversampler.cpp", "N1 compact decimator in the pool", [(
        "            const auto d = intern (h);\n",
        "            const auto d = intern (h);\n"
        "#if NEMESIS_FORK_APPROX\n"
        "            // FIREFORK N1: right after the taps: [centre coef][pair count][pair coefs...], offsets in ForkTapPool::idx.\n"
        "            {\n                const int c = nTaps / 2;\n                std::vector<Real> blk { h[static_cast<std::size_t> (c)], 0.0 };\n"
        "                std::vector<int> offs { 0, 0 };\n"
        "                for (int j = 0; j < c; ++j)\n                    if ((c - j) % l != 0)\n                    {\n"
        "                        blk.push_back (0.5 * (h[static_cast<std::size_t> (j)] + h[static_cast<std::size_t> (nTaps - 1 - j)]));\n"
        "                        offs.push_back (j);\n                    }\n"
        "                blk[1] = static_cast<Real> (blk.size() - 2);\n"
        "                const std::size_t at = gPoolUsed;\n                (void) intern (blk);\n"
        "                for (std::size_t i = 0; i < offs.size(); ++i)\n                    ForkTapPool::idx[at + i] = static_cast<std::uint16_t> (offs[i]);\n            }\n"
        "#endif\n")])
    P.edit(f"{INC}/Oversampler.h", "N1 compact kernels", [
        ("                    os[p - p0] = fir::dot (row, newest, phaseTaps_);\n",
         "                    os[p - p0] = (fork::kApprox && fork::kExact) ? upRow (p, newest) : fir::dot (row, newest, phaseTaps_); // FIREFORK N1\n"),
        ("            for (int p = 0; p < factor_; ++p)\n"
         "                shape (static_cast<float> (fir::dot (up_.data() + static_cast<std::size_t> (p) * static_cast<std::size_t> (phaseTaps_), newest, phaseTaps_)));\n",
         "            for (int p = 0; p < factor_; ++p)\n                shape (static_cast<float> (upRow (p, newest)));\n"),
        ("                pushOs (static_cast<Real> (shape (static_cast<float> (fir::dot (up_.data() + static_cast<std::size_t> (p) * static_cast<std::size_t> (phaseTaps_), newest, phaseTaps_)))));\n",
         "                pushOs (static_cast<Real> (shape (static_cast<float> (upRow (p, newest)))));\n"),
        ("            const Real* ref = ohist_.data() + opos_ + ohistLen_ - phase0;\n"
         "            return static_cast<float> (fir::dot (down_.data(), ref, downTaps_));\n        }\n",
         "            const Real* ref = ohist_.data() + opos_ + ohistLen_ - phase0;\n"
         "#if NEMESIS_FORK_EXACT && NEMESIS_FORK_APPROX\n"
         "            { // FIREFORK N1: the compact, folded decimator\n"
         "                const Real* blk = down_.data() + downTaps_;\n"
         "                const std::uint16_t* off = ForkTapPool::idx + (blk - ForkTapPool::pool) + 2;\n"
         "                const Real* pc = blk + 2;\n                const int n = static_cast<int> (blk[1]);\n"
         "                const int last = downTaps_ - 1;\n"
         "                Real a0 = blk[0] * ref[-(downTaps_ / 2)], a1 = 0.0, a2 = 0.0, a3 = 0.0;\n                int i = 0;\n"
         "                for (; i + 3 < n; i += 4)\n                {\n"
         "                    a0 += pc[i] * (ref[-off[i]] + ref[-(last - off[i])]);\n"
         "                    a1 += pc[i + 1] * (ref[-off[i + 1]] + ref[-(last - off[i + 1])]);\n"
         "                    a2 += pc[i + 2] * (ref[-off[i + 2]] + ref[-(last - off[i + 2])]);\n"
         "                    a3 += pc[i + 3] * (ref[-off[i + 3]] + ref[-(last - off[i + 3])]);\n                }\n"
         "                for (; i < n; ++i)\n                    a0 += pc[i] * (ref[-off[i]] + ref[-(last - off[i])]);\n"
         "                return static_cast<float> ((a0 + a1) + (a2 + a3));\n            }\n"
         "#else\n"
         "            return static_cast<float> (fir::dot (down_.data(), ref, downTaps_));\n#endif\n        }\n\n"
         "        // One upsampling phase. FIREFORK N1 (approx builds): phase 0 is the centre tap alone, the others drop their exact-0\n"
         "        // last tap and sum in four accumulators. Otherwise the pinned fir::dot.\n"
         "        [[nodiscard]] Real upRow (int p, const Real* newest) const noexcept\n        {\n"
         "            const Real* row = up_.data() + static_cast<std::size_t> (p) * static_cast<std::size_t> (phaseTaps_);\n"
         "            if constexpr (fork::kApprox && fork::kExact)\n            {\n"
         "                const int m = (phaseTaps_ - 1) / 2;\n"
         "                if (p == 0)\n                    return row[m] * newest[-m];\n"
         "                const int n = phaseTaps_ - 1;\n                Real a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;\n                int j = 0;\n"
         "                for (; j + 3 < n; j += 4)\n                {\n"
         "                    a0 += row[j] * newest[-j];\n                    a1 += row[j + 1] * newest[-(j + 1)];\n"
         "                    a2 += row[j + 2] * newest[-(j + 2)];\n                    a3 += row[j + 3] * newest[-(j + 3)];\n                }\n"
         "                for (; j < n; ++j)\n                    a0 += row[j] * newest[-j];\n"
         "                return (a0 + a1) + (a2 + a3);\n            }\n"
         "            return fir::dot (row, newest, phaseTaps_);\n        }\n")])
    # N3: increments divided the frequency by the sample rate (a double division, ~30 cycles on the M7) on every retune, which is
    # every sample while Pitch is modulated, in every slot. Multiply by a cached reciprocal instead (<= 1 ulp of a double). The
    # oscillator core's sync increments use the same reciprocal as the wavetable, so master and copy still agree to the bit.
    INV = "        Wide invSampleRate_ = 1.0 / 48000.0; // FIREFORK N3: 1 / sampleRate_\n"
    P.edit(f"{INC}/PhaseDistortionOscillator.h", "N3 PD inc", [
        ("            inc_ = sampleRate_ > 0.0 ? freqHz_ / sampleRate_ : 0.0;\n",
         f"            inc_ = sampleRate_ > 0.0 ? ({A} ? freqHz_ * invSampleRate_ : freqHz_ / sampleRate_) : 0.0; // FIREFORK N3\n"),
        ("        Wide sampleRate_ = 48000.0;\n", "        Wide sampleRate_ = 48000.0;\n" + INV)])
    P.edit("src/PhaseDistortionOscillator.cpp", "N3 PD inv rate", [(
        "        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;\n",
        "        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;\n        invSampleRate_ = 1.0 / sampleRate_; // FIREFORK N3\n")])
    P.edit(f"{INC}/BitmaskOscillator.h", "N3 BM inc", [
        ("            inc_ = sampleRate_ > 0.0 ? freqHz_ / sampleRate_ : 0.0;\n",
         f"            inc_ = sampleRate_ > 0.0 ? ({A} ? freqHz_ * invSampleRate_ : freqHz_ / sampleRate_) : 0.0; // FIREFORK N3\n"),
        ("        Wide sampleRate_ = 48000.0;\n", "        Wide sampleRate_ = 48000.0;\n" + INV)])
    P.edit("src/BitmaskOscillator.cpp", "N3 BM inv rate", [(
        "        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;\n",
        "        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;\n        invSampleRate_ = 1.0 / sampleRate_; // FIREFORK N3\n")])
    P.edit(f"{INC}/WavetableOscillator.h", "N3 WT inc", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/ForkConfig.h>\n"),
        ("            inc_ = freqHz_ / sampleRate_;\n            resolveMip();\n",
         f"            inc_ = {A} ? freqHz_ * invSampleRate_ : freqHz_ / sampleRate_; // FIREFORK N3\n            resolveMip();\n"),
        ("            sampleRate_ = sampleRate > 0.0 ? sampleRate : 1.0;\n",
         "            sampleRate_ = sampleRate > 0.0 ? sampleRate : 1.0;\n            invSampleRate_ = 1.0 / sampleRate_; // FIREFORK N3\n"),
        ("        Wide sampleRate_ = 48000.0;\n", "        Wide sampleRate_ = 48000.0;\n" + INV)])
    P.edit(f"{INC}/CathodeOscCore.h", "N3 core increments", [
        ("                syncMasterInc_ = s.freqHz / sampleRate_;\n",
         f"                syncMasterInc_ = {A} ? s.freqHz * invSampleRate_ : s.freqHz / sampleRate_; // FIREFORK N3\n"),
        ("                syncSlaveInc_ = s.freqHz / sampleRate_;\n",
         f"                syncSlaveInc_ = {A} ? s.freqHz * invSampleRate_ : s.freqHz / sampleRate_; // FIREFORK N3\n"),
        ("                s.incCached = sampleRate_ > 0.0 ? s.freqHz / sampleRate_ : 0.0;\n",
         f"                s.incCached = sampleRate_ > 0.0 ? ({A} ? s.freqHz * invSampleRate_ : s.freqHz / sampleRate_) : 0.0;\n"),
        ("            const Wide incOs = (sampleRate_ > 0.0 ? s.freqHz / sampleRate_ : 0.0)\n                                 / static_cast<Real> (factor);\n",
         f"            const Wide incOs = {A} ? fork::divByFactor (sampleRate_ > 0.0 ? s.freqHz * invSampleRate_ : 0.0, factor) // FIREFORK N3\n"
         "                                   : (sampleRate_ > 0.0 ? s.freqHz / sampleRate_ : 0.0) / static_cast<Real> (factor);\n"),
        ("        Wide sampleRate_ = 48000.0;\n        Wide noteHz_ = 0.0;\n",
         "        Wide sampleRate_ = 48000.0;\n" + INV + "        Wide noteHz_ = 0.0;\n")])
    P.edit("src/CathodeOscCore.cpp", "N3 core inv rate", [(
        "        sampleRate_ = sampleRate > 0.0 ? sampleRate : 1.0;\n        for (Slot& s : slots_)\n",
        "        sampleRate_ = sampleRate > 0.0 ? sampleRate : 1.0;\n        invSampleRate_ = 1.0 / sampleRate_; // FIREFORK N3\n        for (Slot& s : slots_)\n")])
    # N4: the PD Pulse / SawPulse shapes call powf on every oversampled step (4 per slot-sample at x4). x^k = 2^(k log2 x) through
    # the fast exp2/log2 (relative error ~2e-7); x = 0 gives ~1e-30 instead of 0 (k >= 1).
    P.edit(f"{INC}/PhaseDistortionOscillator.h", "N4 PD pow", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/FastMath.h>\n"),
        ("                    return sine (std::pow (ph, powExp_));\n",
         f"                    return sine ({A} ? fm::exp2 (powExp_ * fm::log2 (ph)) : std::pow (ph, powExp_)); // FIREFORK N4\n"),
        ("                    return sine (std::pow (w, powExp_));\n",
         f"                    return sine ({A} ? fm::exp2 (powExp_ * fm::log2 (w)) : std::pow (w, powExp_)); // FIREFORK N4\n")])
    # N5: the filter drive, the output drive and the Ladder core oversampled x4 whatever filterOversample said, so a driven Ladder
    # was the heaviest core of all. fork::Config::driveOversample (default 4 = upstream) sets them; Classic can keep its own
    # factor (classicOversample, 0 = follow filterOversample) because its top cutoff at full resonance scales with the factor.
    P.edit(f"{INC}/CathodeFilterBank.h", "N5 drive and ladder factor", [
        ("            drive_.prepare();\n", "            drive_.prepare (fork::Config::driveOversample); // FIREFORK N5 (upstream: the default 4)\n"),
        ("            ladder_.prepare (sampleRate);\n", "            ladder_.prepare (sampleRate, fork::Config::driveOversample); // FIREFORK N5\n")])
    P.edit(f"{INC}/CathodeOutputStage.h", "N5 output drive factor", [
        ("#include <nemesis/dsp/Real.h>\n", "#include <nemesis/dsp/Real.h>\n#include <nemesis/dsp/ForkConfig.h>\n"),
        ("            outDrive_.prepare(); //", "            outDrive_.prepare (fork::Config::driveOversample); // FIREFORK N5;"),
        ("            outDriveR_.prepare(); //", "            outDriveR_.prepare (fork::Config::driveOversample); // FIREFORK N5;")])
    P.edit(f"{INC}/CathodeVoiceStage.h", "N5 classic factor", [(
        "            int classicOversample = fork::Config::filterOversample,\n",
        "            int classicOversample = fork::Config::classicOversample > 0 ? fork::Config::classicOversample : fork::Config::filterOversample,\n")])

    # ==== 12. memory (docs/DSP_REVIEW.md section 1) =========================================================================
    # P2: the voice pool size from the build (NEMESIS_FORK_MAX_VOICES, default 32 = upstream).
    P.edit(f"{INC}/CathodeVoiceEngine.h", "P2 voice pool from the build", [(
        "        static constexpr int kMaxVoices = 32;\n",
        "        static constexpr int kMaxVoices = NEMESIS_FORK_MAX_VOICES; // FIREFORK P2: upstream 32\n"
        "        static_assert (kMaxVoices >= 4 && kMaxVoices % 4 == 0, \"FIREFORK P2: the lane groups need a multiple of 4 voices\");\n")])
    # P3: every voice prepares a stereo right chain (two more filter banks, HPF, reducer, drift, resonator, noise). With
    # fork::Config::stereoTwin = false it is never made: the chain then never starts (followRightChain already checks the pointer).
    P.edit(f"{INC}/CathodeVoiceStage.h", "P3 optional stereo twin", [
        ("            if (right_ == nullptr)\n                right_ = std::make_unique<RightChain>();\n"
         "            right_->f1.prepare (sampleRate, screamOversample, acidOversample, sovietOversample, classicOversample, reedOversample);\n",
         "            if (!fork::Config::stereoTwin) // FIREFORK P3: no twin, so width and the Stereo route play mono\n                right_.reset();\n"
         "            else\n            {\n"
         "            if (right_ == nullptr)\n                right_ = std::make_unique<RightChain>();\n"
         "            right_->f1.prepare (sampleRate, screamOversample, acidOversample, sovietOversample, classicOversample, reedOversample);\n"),
        ("            right_->noiseSide.setLevel (0.0f); // as noise_ is primed below\n",
         "            right_->noiseSide.setLevel (0.0f); // as noise_ is primed below\n            } // FIREFORK P3\n"),
        ("            right_->gritDry.assign (cap, 0.0f); // [stereo voice S3]",
         "            if (right_ != nullptr) // FIREFORK P3\n                right_->gritDry.assign (cap, 0.0f); // [stereo voice S3]")])
    # P4: the PD oscillator's shared sine table (8 KB, read on every OS step of every PD slot) can be placed in fast memory too.
    P.edit("src/PhaseDistortionOscillator.cpp", "P4 sine table placement", [(
        "            return t;\n        }();\n        return table.data();\n",
        "            return t;\n        }();\n"
        "#if NEMESIS_FORK_EXACT // FIREFORK P4: a copy of the table (the same floats: the compiler may fold the original at build time,\n"
        "                      // a run-time sin would round differently), in storage the build may place (the firmware: DTCM)\n"
        "#ifndef NEMESIS_FORK_FAST_DATA\n#define NEMESIS_FORK_FAST_DATA\n#endif\n"
        "        NEMESIS_FORK_FAST_DATA static std::array<float, kSineTableSize + 1> fastTable;\n"
        "        static bool copied = false;\n"
        "        if (!copied)\n        {\n            fastTable = table;\n            copied = true;\n        }\n"
        "        return fastTable.data();\n#else\n        return table.data();\n#endif\n")])

    # ==== 13. firmware API (docs/FIRMWARE_UI_PLAN.md): additive methods, nothing upstream calls them ==================
    # The firmware's sequencer ends ONE note: when two notes of the same pitch overlap (a long gate, swing), the engine's
    # noteOff(pitch) releases the newest, cutting the note that should go on. noteOffVoice releases the group the given voice
    # belongs to, only if it is still the group that was started then (not stolen and reused since).
    P.edit(f"{INC}/CathodeVoiceEngine.h", "firmware noteOffVoice", [(
        "        [[nodiscard]] int lastTriggeredVoice() const noexcept { return lastTriggered_; }\n",
        "        [[nodiscard]] int lastTriggeredVoice() const noexcept { return lastTriggered_; }\n\n"
        "        // FIREFORK: the note group a voice belongs to (changes when the voice is given a new note), and a key-up for exactly\n"
        "        // that note: the held group of `voice` is released if it is still `group`; otherwise nothing (it was stolen). While a\n"
        "        // cut is deferring note events it falls back to noteOff(pitch), which keeps the deferred order.\n"
        "        [[nodiscard]] std::uint64_t voiceGroupAt (int i) const noexcept { return voiceGroup_[static_cast<std::size_t> (i)]; }\n"
        "        void noteOffVoice (int voice, std::uint64_t group, int midiNote) noexcept\n"
        "        {\n"
        "            if (voice < 0 || voice >= kMaxVoices || isMonophonic() || (cutLeft_ > 0 && deferredCount_ > 0))\n"
        "            {\n"
        "                noteOff (midiNote);\n"
        "                return;\n"
        "            }\n"
        "            const auto v = static_cast<std::size_t> (voice);\n"
        "            if (!voiceHeld_[v] || voiceGroup_[v] != group || voiceNote_[v] != midiNote)\n"
        "                return;\n"
        "            for (int i = 0; i < renderSpan_; ++i)\n"
        "            {\n"
        "                const auto u = static_cast<std::size_t> (i);\n"
        "                if (voiceHeld_[u] && voiceNote_[u] == midiNote && voiceGroup_[u] == group)\n"
        "                {\n"
        "                    voiceHeld_[u] = false;\n"
        "                    voiceReleaseAt_[u] = sampleClock_;\n"
        "                    voices_[u].gateOff();\n"
        "                }\n"
        "            }\n"
        "        }\n")])

    # ---- E16. lazy filter reset at note-on (exact; T1, 2026-10-28). Measured on the board: engine noteOn 311k -> 107k cycles, worst block 164% -> 115%.
    P.edit(f"{INC}/CathodeFilterBank.h", "E16 lazy reset", [
        (
            "            dryCeiling_.prepare (sampleRate); // [D-M-0373] the dry term's ceiling detectors\n        }\n",
            "            dryCeiling_.prepare (sampleRate); // [D-M-0373] the dry term's ceiling detectors\n            if constexpr (fork::kExact) // FIREFORK E16: every core's prepare cleared it; only the selected one can run next\n                lazyDirty_ = lazyBitFor (filterMode_);\n        }\n"),
        (
            '            filterMode_ = mode;\n            // A mode change moves where `synth.filt.drive` belongs',
            '            filterMode_ = mode;\n            if constexpr (fork::kExact) // FIREFORK E16: the core now selected may run, so its state may leave "freshly reset"\n                lazyDirty_ = static_cast<std::uint8_t> (lazyDirty_ | lazyBitFor (mode));\n            // A mode change moves where `synth.filt.drive` belongs'),
        (
            '        [[nodiscard]] ClassicFilter& classicCore() noexcept { return classic_; }\n',
            '        [[nodiscard]] ClassicFilter& classicCore() noexcept\n        {\n            if constexpr (fork::kExact) // FIREFORK E16: the lanes run this core by reference, whatever the mode says\n                lazyDirty_ = static_cast<std::uint8_t> (lazyDirty_ | kLzClassic);\n            return classic_;\n        }\n'),
        (
            '            filter_.reset();\n            formant_.reset();\n            comb_.reset();\n            ladder_.reset();\n            scream_.reset();\n            acid_.reset();\n            soviet_.reset();\n            classic_.reset();\n            reed_.reset();\n            state_.reset();\n        }\n',
            '            filter_.reset();\n            formant_.reset();\n            state_.reset();\n            if constexpr (fork::kExact)\n            {\n                // FIREFORK E16: lazy reset. A core that was not selected since the last reset has not run, so its state is\n                // still exactly what a reset leaves; clearing it again writes the same bytes. `lazyDirty_` holds the cores\n                // that were selected (setFilterMode) or are selected now; those are cleared, the rest are skipped.\n                const std::uint8_t d = lazyDirty_;\n                if (d & kLzComb) { comb_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);\n                if (d & kLzLadder) { ladder_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);\n                if (d & kLzScream) { scream_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);\n                if (d & kLzAcid) { acid_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);\n                if (d & kLzSoviet) { soviet_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);\n                if (d & kLzClassic) { classic_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);\n                if (d & kLzReed) { reed_.reset(); NEMESIS_FORK_COUNT (lazyCleared); } else NEMESIS_FORK_COUNT (lazySkipped);\n                lazyDirty_ = lazyBitFor (filterMode_); // the selected core runs next\n            }\n            else\n            {\n                comb_.reset();\n                ladder_.reset();\n                scream_.reset();\n                acid_.reset();\n                soviet_.reset();\n                classic_.reset();\n                reed_.reset();\n            }\n        }\n'),
        (
            '    private:\n        // FIREFORK E3a\n        static constexpr bool isSvfMode',
            '    private:\n        // FIREFORK E16: the cores whose reset is worth skipping (a comb ring, oversampler histories) and the model each is selected by.\n        static constexpr std::uint8_t kLzComb = 1, kLzLadder = 2, kLzScream = 4, kLzAcid = 8, kLzSoviet = 16, kLzClassic = 32, kLzReed = 64;\n        static constexpr std::uint8_t lazyBitFor (FilterMode m) noexcept\n        {\n            switch (m)\n            {\n                case FilterMode::Comb: return kLzComb;\n                case FilterMode::Ladder24: case FilterMode::Ladder12: return kLzLadder;\n                case FilterMode::ScreamLp: case FilterMode::ScreamHp: return kLzScream;\n                case FilterMode::Acid: return kLzAcid;\n                case FilterMode::SovietLp: case FilterMode::SovietBp: case FilterMode::SovietHp: return kLzSoviet;\n                case FilterMode::ClassicLp24: case FilterMode::ClassicLp12: case FilterMode::ClassicBp: case FilterMode::ClassicHp: return kLzClassic;\n                case FilterMode::ReedLp: case FilterMode::ReedBp: case FilterMode::ReedHp: case FilterMode::ReedMorph: return kLzReed;\n                default: return 0;\n            }\n        }\n    public:\n        // bits of the cores that may hold state: selected since the last reset (read by the tests)\n        std::uint8_t lazyDirty_ = 0;\n    private:\n        // FIREFORK E3a\n        static constexpr bool isSvfMode'),
    ])

    # ---- E17. skip the cutoff/Q push when its operands are unchanged (exact; T3, 2026-10-28). Env2 -> cutoff settles to a constant after its decay;
    # the same operands give the same Hz and the same coefficients, so the exp2 and the filter's tan/divisions are not redone. Measured: -2 to -4 points of a part.
    P.edit(f"{INC}/CathodeModulatedVoice.h", "E17 push-skip", [
        ("            organicCutoffMult_ = 1.0f; // [T-CMP-11] a hard reset owes no note a stale Organic offset\n",
         "            organicCutoffMult_ = 1.0f; // [T-CMP-11] a hard reset owes no note a stale Organic offset\n"
         "            pushValid_ = false; // FIREFORK E17: nothing has been pushed since this reset\n"
         "            cutValid_ = false;\n"),
        ("            // sentence true rather than aspirational.\n            genericControlPhase_ = 0;\n        }\n",
         "            // sentence true rather than aspirational.\n            genericControlPhase_ = 0;\n            cutValid_ = false; // FIREFORK E17\n        }\n"),
        ("            // sentence true rather than aspirational.\n            genericControlPhase_ = 0;\n        }\n",
         "            // sentence true rather than aspirational.\n            genericControlPhase_ = 0;\n            cutValid_ = false; // FIREFORK E17\n        }\n"),
        ("            // WHOLE NOTE on a value up to 63 samples stale rather than one sample stale.\n            genericControlPhase_ = 0;\n",
         "            // WHOLE NOTE on a value up to 63 samples stale rather than one sample stale.\n            genericControlPhase_ = 0;\n            cutValid_ = false; // FIREFORK E17\n"),
        ("            genericControlPhase_ = 0; // [T-CMP-16 F3] phase 0 writes, so a rebuilt live list is current\n",
         "            genericControlPhase_ = 0; // [T-CMP-16 F3] phase 0 writes, so a rebuilt live list is current\n            cutValid_ = false; // FIREFORK E17\n"),
        ("        void pushFilterCutoffReso (float cutoffHz, float q) noexcept\n        {\n            outputStage_.voice().setCutoff (cutoffHz, q);\n",
         "        void pushFilterCutoffReso (float cutoffHz, float q) noexcept\n        {\n"
         "            if constexpr (fork::kExact) // FIREFORK E17: the same operands into the same model leave every coefficient where it is\n"
         "            {\n"
         "                const std::uint32_t mw = outputStage_.voice().filterModeWrites();\n"
         "                const bool resoLive = destLive_[static_cast<std::size_t> (Dest::FiltReso)];\n"
         "                NEMESIS_FORK_COUNT (pushCalls);\n"
         "                if (pushValid_ && cutoffHz == pushHz_ && q == pushQ_ && mw == pushModeW_ && resoLive == pushResoLive_)\n"
         "                {\n"
         "                    NEMESIS_FORK_COUNT (pushSkips);\n"
         "                    return;\n"
         "                }\n"
         "                pushValid_ = true;\n"
         "                pushHz_ = cutoffHz;\n"
         "                pushQ_ = q;\n"
         "                pushModeW_ = mw;\n"
         "                pushResoLive_ = resoLive;\n"
         "            }\n"
         "            outputStage_.voice().setCutoff (cutoffHz, q);\n"),
        ("                    if (cutoffLive)\n                        effectiveCutoffHz_ = cutoffForMod (baseCutoffHz_, cutoffSlew_.process (matrix_.modulation (Dest::FiltCutoff)))\n                                             * organicCutoffMult_;\n",
         "                    if (cutoffLive)\n"
         "                    {\n"
         "                        const float cutMod = cutoffSlew_.process (matrix_.modulation (Dest::FiltCutoff));\n"
         "                        NEMESIS_FORK_COUNT (cutCalls);\n"
         "                        if constexpr (fork::kExact) // FIREFORK E17: the same operands give the same Hz, so skip the exp2\n"
         "                        {\n"
         "                            if (!(cutValid_ && cutMod == cutModLast_ && baseCutoffHz_ == cutBaseLast_ && organicCutoffMult_ == cutOrgLast_))\n"
         "                            {\n"
         "                                cutValid_ = true;\n"
         "                                cutModLast_ = cutMod;\n"
         "                                cutBaseLast_ = baseCutoffHz_;\n"
         "                                cutOrgLast_ = organicCutoffMult_;\n"
         "                                effectiveCutoffHz_ = cutoffForMod (baseCutoffHz_, cutMod) * organicCutoffMult_;\n"
         "                            }\n"
         "                            else\n"
         "                                NEMESIS_FORK_COUNT (expSkips);\n"
         "                        }\n"
         "                        else\n"
         "                            effectiveCutoffHz_ = cutoffForMod (baseCutoffHz_, cutMod) * organicCutoffMult_;\n"
         "                    }\n"),
        ("        int genericControlPhase_ = 0;\n        bool genericIterating_ = false;",
         "        int genericControlPhase_ = 0;\n"
         "        bool pushValid_ = false; // FIREFORK E17: what the last push delivered\n"
         "        bool pushResoLive_ = false;\n"
         "        bool cutValid_ = false; // FIREFORK E17: the operands of the last effectiveCutoffHz_\n"
         "        float cutModLast_ = 0.0f, cutBaseLast_ = 0.0f, cutOrgLast_ = 0.0f;\n"
         "        float pushHz_ = 0.0f, pushQ_ = 0.0f;\n"
         "        std::uint32_t pushModeW_ = 0;\n"
         "        bool genericIterating_ = false;"),
    ])
    P.edit(f"{INC}/CathodeVoiceStage.h", "E17 filterModeWrites", [(
        "        void setCutoff (float cutoffHz, float q) noexcept\n        {\n            editFilter1Coefficients ([&] (CathodeFilterBank& b) noexcept { b.setCutoff (cutoffHz, q); });",
        "        [[nodiscard]] std::uint32_t filterModeWrites() const noexcept { return f1_.filterModeWrites(); } // FIREFORK E17\n"
        "        void setCutoff (float cutoffHz, float q) noexcept\n        {\n            editFilter1Coefficients ([&] (CathodeFilterBank& b) noexcept { b.setCutoff (cutoffHz, q); });")])

    # ---- E18. pad the voice object so the voices of a part do not all map onto the same D-cache sets (bit-identical: only the array stride changes).
    # sizeof(CathodeModulatedVoice) is 16,536 = 2 x 8 KB + 152, so every voice's hot fields repeat at a multiple of the cache way size (8 KB): on the board,
    # 4 voices cost 232% of the block budget, 156-166% with a pad, 136% would be linear. Off (0) = upstream layout. The firmware sets NEMESIS_FORK_VOICE_PAD.
    P.edit(f"{INC}/CathodeModulatedVoice.h", "E18 voice stride pad", [(
        "        bool liveRefreshOwed_ = false; // [S5R B31 dsp2 F4] a `refreshLive` deferred to that loop's end\n    };\n",
        "        bool liveRefreshOwed_ = false; // [S5R B31 dsp2 F4] a `refreshLive` deferred to that loop's end\n"
        "#if NEMESIS_FORK_VOICE_PAD > 0\n"
        "        // FIREFORK E18: pure padding, never read or written. See ForkConfig.h.\n"
        "        alignas (8) std::uint8_t forkStridePad_[NEMESIS_FORK_VOICE_PAD] {};\n"
        "#endif\n    };\n")])

    # ---- E19. control-rate modulation (T3, 2026-10-28). The matrix sum and the named destinations (cutoff, pitch, amp, PD, ...) are evaluated every
    # NEMESIS_FORK_CTL_RATE-th sample while the voice sounds, and every NEMESIS_FORK_IDLE_CTL_RATE-th while the idle skip is ticking a silent voice; the sources
    # (LFOs, envelopes, S&H, drifters) still tick EVERY sample, so they stay in lockstep (D-M-0574). Both default to 1 = today's code, bit-identical.
    # CTL_RATE > 1 is INEXACT (the destinations are held between ticks: a staircase at fs/N; the step limiter on the cutoff is rescaled to match), and needs the
    # user's ears. The idle rate leaves a silent voice's output at exactly 0; it can only differ through the state a note inherits (the step limiter). Board
    # (Sub Pressure, one voice 39.2%): CTL_RATE 8 -> 33.9%; an idle configured voice 11.4 -> 5.7 (CTL 8) or 5.4 (IDLE 64); a second sounding voice unchanged.
    P.edit(f"{INC}/CathodeModulatedVoice.h", "E19 control rate", [
        ("            cutoffSlew_.prepare (sampleRate, kCutoffSlewSeconds, kCutoffSlewOnsetSeconds); // [T-CGUI-83]\n"
         "            cutoffSlew_.setThreshold (cutoffSlewThreshold());\n"
         "            f2CutoffSlew_.prepare (sampleRate, kCutoffSlewSeconds, kCutoffSlewOnsetSeconds); // [D-M-0158]\n"
         "            f2CutoffSlew_.setThreshold (static_cast<float> (kCutoffSlewStepOctaves));\n",
         "            // FIREFORK E19: the limiters run once per control tick, so their ramp length (in samples) and step threshold scale with the rate (1 = unchanged)\n"
         "            cutoffSlew_.prepare (sampleRate / kCtlRate, kCutoffSlewSeconds, kCutoffSlewOnsetSeconds); // [T-CGUI-83]\n"
         "            cutoffSlew_.setThreshold (cutoffSlewThreshold() * kCtlRate);\n"
         "            f2CutoffSlew_.prepare (sampleRate / kCtlRate, kCutoffSlewSeconds, kCutoffSlewOnsetSeconds); // [D-M-0158]\n"
         "            f2CutoffSlew_.setThreshold (static_cast<float> (kCutoffSlewStepOctaves) * kCtlRate);\n"),
        ("cutValid_ = false; // FIREFORK E17\n", "cutValid_ = false; ctlPhase_ = 0; // FIREFORK E17/E19 (the next sample is a control tick)\n"),
        ("cutValid_ = false; // FIREFORK E17\n", "cutValid_ = false; ctlPhase_ = 0; // FIREFORK E17/E19\n"),
        ("cutValid_ = false; // FIREFORK E17\n", "cutValid_ = false; ctlPhase_ = 0; // FIREFORK E17/E19\n"),
        ("cutValid_ = false; // FIREFORK E17\n", "cutValid_ = false; ctlPhase_ = 0; // FIREFORK E17/E19\n"),
        ("        static constexpr int kGenericControlDecimation = 64;\n",
         "        static constexpr int kGenericControlDecimation = 64;\n"
         "        // FIREFORK E19: control rate of the named destinations (1 = every sample, today) and, for a voice the idle skip is ticking, of the idle voice\n"
         "        static constexpr int kCtlRate = NEMESIS_FORK_CTL_RATE;\n"
         "        static constexpr int kIdleCtlRate = NEMESIS_FORK_IDLE_CTL_RATE;\n"),
        ("        void tickIdleSources() noexcept { modulateSample(); }\n",
         "        void tickIdleSources() noexcept\n"
         "        {\n"
         "            if constexpr (kIdleCtlRate != kCtlRate) // FIREFORK E19: a silent voice needs its sources ticking, not its destinations every sample\n"
         "            {\n"
         "                idleTick_ = true;\n"
         "                modulateSample();\n"
         "                idleTick_ = false;\n"
         "            }\n"
         "            else\n"
         "                modulateSample();\n"
         "        }\n"),
        ("            applyDrawEdges(); // [S5R2] a requested DRAW flip lands here, on the audio thread\n",
         "            applyDrawEdges(); // [S5R2] a requested DRAW flip lands here, on the audio thread\n"
         "            // FIREFORK E19: the sources below tick every sample; the matrix sum and the named destinations run on tick 0 of each control period\n"
         "            bool ctlTick = true;\n"
         "            if constexpr (kCtlRate > 1 || kIdleCtlRate > 1)\n"
         "            {\n"
         "                const int period = idleTick_ ? kIdleCtlRate : kCtlRate;\n"
         "                ctlTick = (ctlPhase_ == 0);\n"
         "                ctlPhase_ = (ctlPhase_ + 1 >= period) ? 0 : ctlPhase_ + 1;\n"
         "            }\n"),
        ("            const auto tickLfo = [fed, this] (Lfo& lfo, Source src) noexcept {\n                if (fed && matrix_.reads (src))\n",
         "            const auto tickLfo = [fed, ctlTick, this] (Lfo& lfo, Source src) noexcept {\n                if (fed && ctlTick && matrix_.reads (src))\n"),
        ("            if (!(fork::Config::idleMatrixSkip && matrix_.idle()))\n            {\n                feedSources (",
         "            if (ctlTick && !(fork::Config::idleMatrixSkip && matrix_.idle()))\n            {\n                feedSources ("),
        ("            // 4. Apply the live wired destinations (un-live ones are never touched → no-op bit-exact).\n",
         "            // 4. Apply the live wired destinations (un-live ones are never touched → no-op bit-exact).\n"
         "            if (ctlTick) // FIREFORK E19 (closed before the generic destinations)\n            {\n"),
        ("            // [T-CMP-07 leaf 4a] The GENERIC destinations, and the loop is over the LIVE ones only —",
         "            } // FIREFORK E19\n"
         "            // [T-CMP-07 leaf 4a] The GENERIC destinations, and the loop is over the LIVE ones only —"),
        ("        int genericControlPhase_ = 0;\n",
         "        int genericControlPhase_ = 0;\n"
         "        bool idleTick_ = false; // FIREFORK E19: the call comes from tickIdleSources\n"
         "        int ctlPhase_ = 0; // FIREFORK E19: position inside the control period (0 = this sample is a control tick)\n"),
    ])

    # ---- E20. a voice pool that is not a multiple of 4 (T8, 2026-10-28): the multiple-of-4 rule only served the NEON lane groups (off on the M7), whose arrays are rounded up.
    # Pool 6 and pool 5 change 0 of 2,643 golden scenarios (T8); the firmware uses 6 (loses nothing for the shipped library) and gets ~0.84 MB of SDRAM back.
    P.edit(f"{INC}/CathodeVoiceEngine.h", "E20 voice pool any n", [
        ('        static_assert (kMaxVoices >= 4 && kMaxVoices % 4 == 0, "FIREFORK P2: the lane groups need a multiple of 4 voices");\n',
         '        static_assert (kMaxVoices >= 4, "FIREFORK P2/E20: the lane groups are rounded up, so the pool need not be a multiple of 4");\n'),
        ("        static constexpr int kLaneGroups = kMaxVoices / 4;\n", "        static constexpr int kLaneGroups = (kMaxVoices + 3) / 4;\n"),
    ])

    # ---- E21. panel topology (T8/T9, 2026-10-28): leave out what the Drumboy panel cannot reach. Both switches default to 0 = upstream, bit-identical; the firmware turns them on.
    # The reachability audit (every engine call our code can make, panel/MIDI/project file/p-lock/governor; docs/BRINGUP.md) found no path to these, and a golden set of 2,643
    # scenarios (every lockable parameter at min/mid/max, all models, fuzzed panels, MIDI, previews, 40 s sessions) changes in 0 of them with all of it removed.
    # E21a PANEL_FILTERS: the five filter cores no model of the panel selects (Formant, Comb, Scream, Reed, State) are not prepared (4 comb rings of 8.8 KB per voice among them,
    #      about 0.7-0.95 MB of SDRAM) and a mode that would reach one plays as the SVF low-pass. E21b PANEL_SOURCES: LFO2, LFO3, S&H, the step modulator, Env3 and the delta-decay
    #      envelopes only tick while some active slot reads them (the panel routes none), and the engine's three drifters are not ticked (nothing reads them): about 2-4 points of a voice.
    P.edit(f"{INC}/CathodeFilterBank.h", "E21a panel filters", [
        ("            formant_.prepare (sampleRate);\n",
         "            if constexpr (!fork::kPanelFilters) // FIREFORK E21a\n                formant_.prepare (sampleRate);\n"),
        ("            comb_.prepare (sampleRate);\n",
         "            if constexpr (!fork::kPanelFilters)\n                comb_.prepare (sampleRate);\n"),
        ("            comb_.setType (CombType::Both);\n",
         "            if constexpr (!fork::kPanelFilters)\n                comb_.setType (CombType::Both);\n"),
        ("            scream_.prepare (sampleRate, screamOversample); // designs the Scream OS FIRs\n            scream_.setCutoff (kDefaultCutoffHz, 0.0f);\n",
         "            if constexpr (!fork::kPanelFilters)\n            {\n                scream_.prepare (sampleRate, screamOversample); // designs the Scream OS FIRs\n                scream_.setCutoff (kDefaultCutoffHz, 0.0f);\n            }\n"),
        ("            reed_.prepare (sampleRate, reedOversample); // designs the Reed OS FIRs\n            reed_.setCutoff (kDefaultCutoffHz, 0.0f);\n",
         "            if constexpr (!fork::kPanelFilters)\n            {\n                reed_.prepare (sampleRate, reedOversample); // designs the Reed OS FIRs\n                reed_.setCutoff (kDefaultCutoffHz, 0.0f);\n            }\n"),
        ("            state_.prepare (sampleRate);\n            state_.setCutoff (kDefaultCutoffHz, 0.0f);\n",
         "            if constexpr (!fork::kPanelFilters)\n            {\n                state_.prepare (sampleRate);\n                state_.setCutoff (kDefaultCutoffHz, 0.0f);\n            }\n"),
        ("        void setFilterMode (FilterMode mode) noexcept\n        {\n",
         "        void setFilterMode (FilterMode mode) noexcept\n        {\n"
         "            if constexpr (fork::kPanelFilters) // FIREFORK E21a: a model whose core is not built plays as the SVF low-pass (the panel cannot select one)\n"
         "                if (mode == FilterMode::Formant || mode == FilterMode::Comb || (mode >= FilterMode::ScreamLp && mode <= FilterMode::ScreamHp)\n"
         "                    || (mode >= FilterMode::ReedLp && mode <= FilterMode::StateMorph) || mode == FilterMode::ReedMorph)\n"
         "                    mode = FilterMode::LowPass;\n"),
        ("            filter_.reset();\n            formant_.reset();\n            state_.reset();\n",
         "            filter_.reset();\n            if constexpr (!fork::kPanelFilters) // FIREFORK E21a\n            {\n                formant_.reset();\n                state_.reset();\n            }\n"),
    ])
    P.edit(f"{INC}/CathodeModulatedVoice.h", "E21b panel sources", [
        ("            const float e3 = tickModEnv (1);\n",
         "            // FIREFORK E21b: a source no active slot reads does not advance (the panel routes LFO1, Env2, key tracking, wheel and aftertouch only)\n"
         "            const float e3 = (fork::kPanelSources && !matrix_.reads (Source::Env3)) ? 0.0f : tickModEnv (1);\n"),
        ("            const float l2 = tickLfo (lfo2_, Source::Lfo2);\n            const float l3 = tickLfo (lfo3_, Source::Lfo3);\n"
         "            const float st = step_.processSample();\n            const float sh = tickLfo (sh_, Source::SampleHold);\n",
         "            const float l2 = (fork::kPanelSources && !matrix_.reads (Source::Lfo2)) ? 0.0f : tickLfo (lfo2_, Source::Lfo2);\n"
         "            const float l3 = (fork::kPanelSources && !matrix_.reads (Source::Lfo3)) ? 0.0f : tickLfo (lfo3_, Source::Lfo3);\n"
         "            const float st = (fork::kPanelSources && !matrix_.reads (Source::StepMod)) ? 0.0f : step_.processSample();\n"
         "            const float sh = (fork::kPanelSources && !matrix_.reads (Source::SampleHold)) ? 0.0f : tickLfo (sh_, Source::SampleHold);\n"),
        ("            const float dd1 = dd_[0].processSample();\n            const float dd2 = dd_[1].processSample();\n"
         "            const float dd3 = dd_[2].processSample(); // [D-M-0193] the SUB's\n",
         "            const float dd1 = (fork::kPanelSources && !matrix_.reads (Source::DeltaDecay1)) ? 0.0f : dd_[0].processSample();\n"
         "            const float dd2 = (fork::kPanelSources && !matrix_.reads (Source::DeltaDecay2)) ? 0.0f : dd_[1].processSample();\n"
         "            const float dd3 = (fork::kPanelSources && !matrix_.reads (static_cast<Source> (CathodeModMatrix::kSubMaskEnvSource))) ? 0.0f\n"
         "                                                                                                                            : dd_[2].processSample(); // [D-M-0193] the SUB's\n"),
    ])
    P.edit(f"{INC}/CathodeVoiceEngine.h", "E21b drifters", [(
        "        void tickDrifters() noexcept\n        {\n",
        "        void tickDrifters() noexcept\n        {\n"
        "            if constexpr (fork::kPanelSources) // FIREFORK E21b: nothing the panel can route reads a drifter, so the three random walks are not run\n"
        "                return;\n")])
