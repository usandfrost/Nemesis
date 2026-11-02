#pragma once

#include <nemesis/dsp/Real.h>
#include <algorithm>
#include <cmath>

namespace nemesis::dsp
{
    // FilterResonance — the ONE law that turns a filter Q into the [0,1] resonance every
    // model-zoo core documents. [T-CMP-26]
    //
    // WHY IT EXISTS AS ITS OWN HEADER. The law was written for `FltrEffect` and lived as its
    // private business, while the SYNTH voice reached the same nine cores through a different
    // law entirely — `CathodeMachine`'s `normalised01 ("synth.filt.reso", plain)`, linear in the
    // 0–127 knob rather than in damping. Measured 2026-09-08: the two diverge by up to 0.281 on
    // the cores' own axis, and by 0.370 vs 0.509 at the shipped default of plain 47 — so the same
    // Ladder at the same knob position was audibly more resonant as an FLTR insert than as the
    // synth's own filter. Owner ruled to unify on THIS law. A shared header rather than a call
    // into `FltrEffect::` from the voice path, because the name of the home is the thing that
    // stops the next author reinventing the second copy.
    //
    // LINEAR IN DAMPING (1/Q) — the axis the cores interpolate on, which is why equal steps of
    // the returned value are equal steps of what you hear rather than of what the knob reads.
    // Both rails land exactly on 0 and 1 because they are the endpoints of the same expression.
    //
    // ⚠ THE RAILS ARE A SECOND COPY OF THE REGISTRY'S, AND DELIBERATELY SO. `dsp` sits below
    // `core`, so it cannot read `MachineRegistry`; `app/tests/test_fx_seam.cpp` asserts these
    // constants against the real `kResoQ` curve's ends instead, so the copy cannot drift
    // silently. Do not "fix" this by reaching upward — that is a layer violation, not a tidy-up.
    namespace resonance
    {
        inline constexpr Real kQMin = 0.5;
        inline constexpr Real kQMax = 20.0;

        [[nodiscard]] inline float resonance01ForQ (const Real q) noexcept
        {
            const Real clamped = std::clamp (q, kQMin, kQMax);
            const Real dampHi = 1.0 / kQMin; // least resonant
            const Real dampLo = 1.0 / kQMax; // most resonant
            return static_cast<float> ((dampHi - 1.0 / clamped) / (dampHi - dampLo));
        }

        // [T-CGUI-47] The KNOB POSITION a Q sits at — the exact inverse of the registry's reso curve.
        // [T-CGUI-18] That curve is `ExpZero` with noon at `kKnobNoonQ`:
        //   Q = (kQMin + b)·((kQMax + b)/(kQMin + b))^u − b,   b = noonBias (kQMin, kQMax, noon)
        // [T-CMP-29] It is NOT the Comb's feedback law any more: every Comb takes `resonance01ForQ`,
        // the damping law FLTR's comb always had. The noon is a checked copy of `core::calibration::kResoNoonQ`, for the
        // kQMin/kQMax reason above; `app/tests/test_apply_table.cpp` asserts the two are equal and
        // that this inverts core's REAL curve.
        inline constexpr Real kKnobNoonQ = 1.5;
        inline constexpr Real kKnobBias = (kKnobNoonQ * kKnobNoonQ - kQMin * kQMax) / (kQMin + kQMax - 2.0 * kKnobNoonQ);

        [[nodiscard]] inline float knob01ForQ (const Real q) noexcept
        {
            const Real lo = kQMin + kKnobBias;
            const Real ratio = (kQMax + kKnobBias) / lo;
            return static_cast<float> (std::log ((std::clamp (q, kQMin, kQMax) + kKnobBias) / lo) / std::log (ratio));
        }

        // ── THE RESONANCE CEILINGS ─────────────────────────────────────────────────────────
        //
        // Every model-zoo core stops SHORT of its own analytic self-oscillation point, and the
        // constant that says where was, until T-CMP-31, in an unnamed namespace inside its own
        // `.cpp` — one per core, seven of them, readable by nothing outside that TU. Verified
        // before the lift: outside `dsp/src/*.cpp` these names appeared ONLY in prose comments.
        // **So a self-oscillation THRESHOLD law could not be written against them at all**, which
        // is the missing mechanism CAT-4a kept being blocked on — not a missing number.
        //
        // They are TWO laws, not seven. Lifting them here rather than into each header follows
        // the argument this file's own preamble makes: the name of the home is what stops the
        // next author reinventing the second copy.
        //
        // ⚠ THE LIFT IS BIT-EXACT UNDER IEEE SEMANTICS — same `double` literals, same expression,
        // same operand order. Verified by diffing generated assembly old-vs-new for all seven cores
        // at -O1/-Os/-O2/-O3/-O2-flto and with -ffp-contract both fast and off: identical, same FMA
        // fusion on both sides. If a golden moves, the extraction is wrong; do not re-bless it.
        // ⚠ IT IS *NOT* BIT-EXACT UNDER REASSOCIATING FAST-MATH, and the difference is real: at
        // -Ofast the family-B fold crosses the (inlined) function boundary differently and the three
        // cores diverge by 1 ulp (2.22e-16 at 1.0). Unreachable today — `ENABLE_FAST_MATH` is off and
        // applies to the plugin target, while these compile into the `nemesis_dsp` static lib — so it
        // is a QUALIFIER, not a defect. If dsp sources are ever added to a fast-math target, this
        // paragraph is the thing to read first.
        namespace ceiling
        {
            // FAMILY A — FEEDBACK CEILING. `k = kMax · reso`, with `kMax` just under the loop gain
            // at which the topology sings. The headroom is what makes max resonance a sharp,
            // bounded ring rather than an oscillator.
            //
            // The thresholds are DERIVED, not chosen — each core's headnote carried the derivation
            // before the lift and it is kept here whole:
            //   Ladder/Classic, 4-pole: four one-poles each at -45 deg give -180 deg where
            //     |H| = 1/sqrt(2) per pole, so loop gain k·(1/sqrt2)^4 = k/4 = 1 -> **k = 4**.
            //   Acid, 3-pole: three one-poles each at -60 deg give -180 deg where |H| = 1/2 per
            //     pole, so loop gain k·(1/2)^3 = 1 -> **k = 8**.
            //   Scream, Sallen-Key: K = 1 + r, and **K = 3** is the Q -> inf point, so r -> 2.
            // In every case the ceiling sits JUST under the threshold, so max resonance gives a
            // sharp bounded ring (poles inside the unit circle) rather than blowing up. ⚠ An
            // in-loop clip caps the large-signal loop only WHERE ONE IS IN CIRCUIT — Ladder's is
            // gated on the drive predicate (`LadderFilter.h`:78, and `synth.filt.drive` ships 0),
            // so at the shipped default POLE PLACEMENT IS THE ONLY BOUND. Do not treat the clip as
            // a safety net when choosing a ceiling.
            // 4-pole ladder sings at k = 4. filter.md §3/§11
            inline constexpr Real kLadderMaxK = 3.99;
            // Same topology as Ladder; Classic "stays clean nearly to self-oscillation". filter.md §2/§11
            inline constexpr Real kClassicMaxK = 3.9;
            // 0.975x of the 3-pole threshold k = 8. filter.md §6/§11
            inline constexpr Real kAcidMaxK = 7.8;
            // Keeps K < 3; the diode clip in the feedback caps a max-reso ring. filter.md §4/§11
            inline constexpr Real kScreamMaxR = 1.98;

            [[nodiscard]] inline constexpr Real feedbackK (const Real kMax, const Real reso) noexcept
            {
                return kMax * reso;
            }

            // FAMILY B — DAMPING FLOOR. `k = 1/Q`, interpolated from well-damped down to the
            // model's own floor. The three expressions were byte-identical before the lift and
            // differ ONLY in `kDampMin`, which is the voicing decision each core is making.
            inline constexpr Real kDampMax = 2.0; // reso 0 -> Q = 0.5, well damped. Shared by all three.
            //
            // In all three the LINEAR poles stay inside the unit circle for k > 0, which is what
            // makes them stable. ⚠ Soviet and Reed also clip in-loop; **State does NOT — it has no
            // in-loop saturator and no oversampler at all** (`StateFilter.h`:22), so for State pole
            // placement is the ONLY bound and this floor is the whole of it. The floor IS the voicing:
            // Q 50. filter.md §1/§11
            inline constexpr Real kSovietDampMin = 0.02;
            // Q 100 — the highest-Q core. The "slightly unstable" Steiner character: a sharper,
            // longer ring than Soviet. filter.md §7/§11
            inline constexpr Real kReedDampMin = 0.01;
            // Q 20 — deliberately the tamest, and the reason State is NOT in the osc-capable six
            // (owner ruling 8, 2026-09-07). filter.md §5/§11
            inline constexpr Real kStateDampMin = 0.05;

            [[nodiscard]] inline constexpr Real dampingK (const Real dampMin, const Real reso) noexcept
            {
                return kDampMax * (1.0 - reso) + dampMin * reso;
            }

            // ⚠ **WHAT THESE CEILINGS DO *NOT* GIVE YOU, AND IT IS THE POINT OF THE WHOLE SEAM.**
            // Every core here stops below its threshold, so max resonance RINGS — sharp, bounded,
            // always decaying, because the LINEAR POLES stay inside the unit circle for k > 0.
            // That is the bound that always holds; an in-loop clip is an extra one where a core
            // has it in circuit, and State never does. **True sustained, pitch-tracked
            // self-oscillation is a DEFERRED FOLLOW-ON (CAT-4a), not something this file delivers.**
            // That sentence stood in `LadderFilter.cpp`, `ClassicFilter.cpp` and `AcidFilter.cpp`
            // before T-CMP-31 lifted the constants out; it is re-homed here rather than lost, and it
            // is the signpost to the one card this seam exists to unblock.
            //
            // State is the sharpest case of the same point: at max reso it rings musically at Q ~ 20
            // yet ALWAYS DECAYS — mild and clamped, musical, never fully singing (filter.md §11) —
            // which is the stability claim behind ruling 8, not merely a voicing preference. It is
            // also the core with no clip and no oversampler, so that decay rests on pole placement
            // alone: lowering `kStateDampMin` toward 0 has nothing underneath it.
        }
    }
}
