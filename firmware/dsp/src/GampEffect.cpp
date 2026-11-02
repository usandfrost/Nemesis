#include <nemesis/dsp/Real.h>
#include <nemesis/dsp/FastMath.h>
#include <nemesis/dsp/GampEffect.h>

#include <algorithm>
#include <cassert>
#include <cmath>

namespace nemesis::dsp
{
    namespace
    {
        [[nodiscard]] Real clampD (Real v, Real lo, Real hi) noexcept
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        // §0.7's DC blocker, at BASE rate after the whole nonlinear core. 15 Hz is below the
        // lowest note a bass amp is asked for and above the rate at which a bias step would be
        // audible as a thump.
        constexpr Real kDcBlockHz = 15.0;

        // [leaf 2] The Q of the three SHELF sections (the bell has its own, per model): Butterworth,
        // i.e. the flattest second-order response, so a shelf's own skirt adds no ripple of its
        // own to the band next door.
        constexpr float kShelfQ = 0.70710678118654752f;

        // The upper rail on `gainForDb`. The `level` row stops at +12 dB and the makeup is a
        // trim, so this is a caller-bug guard rather than a musical limit.
        constexpr Real kMaxGainDb = 60.0;

        // [D-M-0304] S(u) = u − u⁹/9 saturates at ±8/9 — see the header's transformer note.
        constexpr float kXfmrCeiling = 8.0f / 9.0f;
        // …and the crossover's ADAA ill-conditioning guard (AdaaSoftClip's own figure).
        constexpr Real kXoverEps = 1.0e-5;

        // ===================== §8's VOICING SELECTION SUMMARY, AS DATA =====================
        // One row per `Model`, in `kAmpModels` storage order. See the header's `Voicing` note:
        // amp_modelling.md gives each field's SHAPE and a range, so the figures are picks and
        // the tests pin the properties they exist for.
        //
        //  stages  §8's "gain stages" column verbatim: Clean 1–2 mostly linear · Crunch 2 ·
        //          Lead 3–4 · Hi-gain 4–6 · Bass "clean + driven high band" · Acoustic none.
        //          Hi-gain takes the bottom of its range because four cascaded ADAA clips
        //          already reach §4's "enormous gain" and the fifth costs 8× its own arithmetic.
        //  os      §0.2's table: 2–4× clean/acoustic · 4× crunch · 4–8× lead · 8–16× hi-gain.
        //          `AdaaSoftClip` carries part of the floor by construction (M1-02's whole
        //          point), so each voicing takes the LOW end of its bracket and the
        //          `[gamp][alias]` case is what says that was enough.
        //  gainOff the model's gain STRUCTURE — what the row's dB means here. At the shipped
        //          row (15.31 dB) this puts Clean 1.3 dB into one stage (§1.2's "gain stages
        //          run well below clipping") and Hi-gain 15.3 dB into four.
        //  stgGain §0.3's inter-stage gain ("progressively more saturated", §3.2), applied at
        //          stages 2..N so the cascade compounds rather than repeating one curve.
        //  bias    §0.3's grid-bias offset → asymmetry → EVEN harmonics. Crunch is the voicing
        //          §2.2 makes the strongest claim for ("asymmetric clipping → strong even
        //          harmonics → the warm, vocal grind"); Clean is nearly symmetric because §1
        //          wants clarity; Acoustic has none because §6 has no clipper at all.
        //  interHp §0.3/§4.2's TIGHTNESS, and the field with the widest spread on purpose:
        //          12 Hz on Clean is a plain coupling cap, while Hi-gain's 220 Hz is §4.2's
        //          "remove the lows BEFORE they are distorted" — the single most important
        //          high-gain lever the spec names. Crunch stays modest per §2.2 ("this voicing
        //          wants some low-end push, not surgical tightness").
        //  pwrDrive §0.5's class-AB pair, in dB — §2.3 makes Crunch's power-amp drive "the source
        //          of the crunch" (the hottest of the six), §5 gives the bass amp "lots of
        //          headroom" (unity) and Acoustic none at all (the stage is skipped).
        //  sag/atk/rel §0.5's B+ droop and its RC, inside §0.5's own 10–100 ms — BOTH directions,
        //          which is what makes the droop audible as a gesture: a supply that collapsed
        //          instantly would just be a quieter amp, while one that takes tens of
        //          milliseconds is the "compresses then blooms back" §0.5 describes (a first
        //          measurement used 2–10 ms attacks and the droop had finished before the first
        //          window opened — 0.01 dB, indistinguishable from a static clipper). The column where
        //          §8's voicings disagree most: §2.2 "prominent and fast-ish" (Crunch, the touch
        //          response), §3.2 "heavy, slow sag" (Lead, the sustain), §4.2 "tight (fast/no
        //          sag)" (Hi-gain, definition over bloom), §1.2 "mild" (Clean, the bloom).
        //  stageLp §0.3's plate-load/Miller roll-off (">10–20 kHz" at the clean end), coming
        //          down as the gain climbs because a dark cascade is what keeps a high-gain
        //          model from fizzing before the cab even sees it.
        //  makeup  the measured per-model output trim — see the header. Fitted on a 110 Hz
        //          band-limited saw at the shipped `gain` row, the tone stack at NOON and the
        //          shipped cabinet (4×12 through a dynamic mic), so the six sit within a dB of
        //          each other and of dry (`[gamp][level]` is the assertion). [leaf 3] re-fitted
        //          them once, by the ~3–4 dB §0.6's LF-resonance and presence peaks add: the CAB
        //          row keeps its own level character on purpose (an 8×10 really is louder than a
        //          DI), so what is normalised is the MODEL axis, at the shipped cabinet.
        //  xfmr    [D-M-0304] §0.5's output transformer, 1/(core flux at the knee). §5.2 makes the
        //          SVT's LF saturation "more audible here and musically useful" — Bass carries
        //          the most; the guitar voicings a touch, Hi-gain least (§4.2 wants definition).
        //  xover   [D-M-0304] §0.5's class-AB crossover, the slope dip at zero. Hi-gain's pair is
        //          biased coldest; Clean's is where it is HEARD (§1's quiet picked notes).
        constexpr GampEffect::Voicing kVoicing[GampEffect::kNumModels] = {
            // stages, os, gainOff, stgGain, bias, interHp, stageLp, makeup, pwrDrive, sag, atk, rel, xfmr, xover
            { 1, 2, -14.0, 0.0, 0.03, 12.0, 12000.0, -3.15, 1.5, 0.12, 30.0, 60.0, 1.0, 0.20 }, // Clean
            { 2, 4, -6.0, 6.0, 0.16, 60.0, 10000.0, -6.85, 6.8, 0.35, 25.0, 40.0, 1.3, 0.15 }, // Crunch
            { 3, 4, -4.0, 8.0, 0.10, 120.0, 8000.0, -5.80, 6.0, 0.45, 40.0, 120.0, 1.3, 0.15 }, // Lead
            { 4, 8, 0.0, 10.0, 0.20, 220.0, 7000.0, -2.45, 4.0, 0.05, 8.0, 20.0, 1.0, 0.25 }, // Hi-gain
            { 2, 4, -10.0, 4.0, 0.08, 40.0, 6000.0, -3.30, 1.0, 0.15, 20.0, 80.0, 1.9, 0.20 }, // Bass
            { 0, 1, -16.0, 0.0, 0.00, 20.0, 18000.0, -7.95, 0.0, 0.0, 10.0, 60.0, 0.0, 0.0 }, // Acoustic
        };

        // ==================== §0.4's TONE STACK + §0.5's PRESENCE, AS DATA ====================
        // Same storage order, same owner-tunable-default status. §0.4 names the TOPOLOGIES and
        // their character rather than figures, so what is spec-derived here is the ORDERING and
        // the SIGNS, and those are what the tests pin:
        //   * `midHz` climbs 400 → 800 Hz across the guitar voicings, because §0.4's Fender
        //     stack scoops around 400 Hz while a Marshall's midrange presence sits higher and
        //     §3.2's lead hump is "~500 Hz–1 kHz".
        //   * `midOffsetDb` is the BASELINE tilt at noon and it is the sharpest per-model
        //     statement in the table: §1's Fender is "pronounced mid scoop" (−3), §2's
        //     Marshall/Vox is mid-FORWARD (0), §3.2's lead wants the hump (+2), §4.3 offers a
        //     modern scoop (−4) and §5's bass amp a mild contour (−2). Acoustic is flat (§6.3).
        //   * `presenceHz` is the NFB shelf's corner, ~3–4.5 kHz on an amp; on Acoustic the same
        //     filter is §6.3's "air / sparkle" and sits at 8 kHz.
        constexpr GampEffect::Tone kTone[GampEffect::kNumModels] = {
            // bassHz, midHz, midQ, trebleHz, presenceHz, rangeDb, midOffsetDb
            { 90.0, 400.0, 0.70, 3000.0, 4500.0, 10.0, -3.0 }, // Clean    — Fender, scooped
            { 110.0, 650.0, 0.80, 2600.0, 3800.0, 12.0, 0.0 }, // Crunch   — Marshall/Vox
            { 120.0, 800.0, 0.90, 2800.0, 4000.0, 12.0, 2.0 }, // Lead     — the hump
            { 100.0, 700.0, 0.90, 3200.0, 4200.0, 14.0, -4.0 }, // Hi-gain — modern scoop
            { 60.0, 500.0, 0.70, 2200.0, 3000.0, 12.0, -2.0 }, // Bass     — contour
            { 120.0, 900.0, 0.70, 5000.0, 8000.0, 9.0, 0.0 }, // Acoustic  — flat, full-range
        };

        // [D-M-0305] §0.4's PASSIVE STACK per topology (`ToneNetwork`): C1 C2 C3 (treble, bass, mid caps) and
        // R1 R2 R3 R4 (treble, bass, mid pots, slope resistor). Clean is Yeh & Smith's own '59 Bassman (§1's
        // Fender scoop: treble up lowers the low mids); Crunch the JCM800 and Lead the plexi-era Marshall
        // (§2's mid-forward stack, a lighter scoop); Hi-gain the Mesa-style blackface variant with the 25 k mid
        // pot (§4.3's deepest scoop on a mid cut); Bass the blackface Twin's 250 k bass pot (§5's most bass
        // authority). Acoustic has none (§6.3's EQ is active, not a passive network).
        constexpr GampEffect::ToneNetwork kNetwork[GampEffect::kNumModels - 1] = {
            { 250.0e-12, 20.0e-9, 20.0e-9, 250.0e3, 1.0e6, 25.0e3, 56.0e3 }, // Clean    — '59 Bassman 5F6-A
            { 470.0e-12, 22.0e-9, 22.0e-9, 220.0e3, 1.0e6, 22.0e3, 33.0e3 }, // Crunch   — JCM800
            { 500.0e-12, 22.0e-9, 22.0e-9, 250.0e3, 1.0e6, 25.0e3, 33.0e3 }, // Lead     — plexi Marshall
            { 250.0e-12, 100.0e-9, 47.0e-9, 250.0e3, 250.0e3, 25.0e3, 100.0e3 }, // Hi-gain — Mesa-style
            { 250.0e-12, 100.0e-9, 47.0e-9, 250.0e3, 250.0e3, 10.0e3, 100.0e3 }, // Bass  — blackface Twin
        };
        // The bass pot's audio taper, exp((p − 1)·k): k = 3.4 puts noon at 18 % of the track, a real log pot's.
        constexpr Real kBassTaper = 3.4;

        // Yeh & Smith's coefficients of H(s) = (b1 s + b2 s² + b3 s³) / (1 + a1 s + a2 s² + a3 s³) at pot positions
        // (l, m, t) = (bass, mid, treble). `b[0]` is the zero at DC, kept for the indexing.
        void networkPolys (const GampEffect::ToneNetwork& n, const Real l, const Real m, const Real t, Real (&b)[4], Real (&a)[4]) noexcept
        {
            const Real c1 = n.c1, c2 = n.c2, c3 = n.c3, r1 = n.r1, r2 = n.r2, r3 = n.r3, r4 = n.r4;
            b[0] = 0.0;
            b[1] = t * c1 * r1 + m * c3 * r3 + l * (c1 * r2 + c2 * r2) + (c1 * r3 + c2 * r3);
            b[2] = t * (c1 * c2 * r1 * r4 + c1 * c3 * r1 * r4) - m * m * (c1 * c3 * r3 * r3 + c2 * c3 * r3 * r3)
                   + m * (c1 * c3 * r1 * r3 + c1 * c3 * r3 * r3 + c2 * c3 * r3 * r3)
                   + l * (c1 * c2 * r1 * r2 + c1 * c2 * r2 * r4 + c1 * c3 * r2 * r4)
                   + l * m * (c1 * c3 * r2 * r3 + c2 * c3 * r2 * r3)
                   + (c1 * c2 * r1 * r3 + c1 * c2 * r3 * r4 + c1 * c3 * r3 * r4);
            const Real ccc = c1 * c2 * c3;
            b[3] = l * m * (ccc * r1 * r2 * r3 + ccc * r2 * r3 * r4) - m * m * (ccc * r1 * r3 * r3 + ccc * r3 * r3 * r4)
                   + m * (ccc * r1 * r3 * r3 + ccc * r3 * r3 * r4) + t * ccc * r1 * r3 * r4 - t * m * ccc * r1 * r3 * r4
                   + t * l * ccc * r1 * r2 * r4;
            a[0] = 1.0;
            a[1] = (c1 * r1 + c1 * r3 + c2 * r3 + c2 * r4 + c3 * r4) + m * c3 * r3 + l * (c1 * r2 + c2 * r2);
            a[2] = m * (c1 * c3 * r1 * r3 - c2 * c3 * r3 * r4 + c1 * c3 * r3 * r3 + c2 * c3 * r3 * r3)
                   + l * m * (c1 * c3 * r2 * r3 + c2 * c3 * r2 * r3) - m * m * (c1 * c3 * r3 * r3 + c2 * c3 * r3 * r3)
                   + l * (c1 * c2 * r2 * r4 + c1 * c2 * r1 * r2 + c1 * c3 * r2 * r4 + c2 * c3 * r2 * r4)
                   + (c1 * c2 * r1 * r4 + c1 * c3 * r1 * r4 + c1 * c2 * r3 * r4 + c1 * c2 * r1 * r3 + c1 * c3 * r3 * r4 + c2 * c3 * r3 * r4);
            a[3] = l * m * (ccc * r1 * r2 * r3 + ccc * r2 * r3 * r4) - m * m * (ccc * r1 * r3 * r3 + ccc * r3 * r3 * r4)
                   + m * (ccc * r3 * r3 * r4 + ccc * r1 * r3 * r3 - ccc * r1 * r3 * r4) + l * ccc * r1 * r2 * r4 + ccc * r1 * r3 * r4;
        }

        // A(s) = a3 (s + ρ)(s² + α s + β): an RC network's poles are REAL and negative, so the monic cubic has three
        // real roots (the trigonometric case); ρ is the fastest, α/β carry the other two. A double root (the
        // boundary of that case) is caught by the clamp. Deterministic, so the same pots give the same split —
        // which is what makes a correction exactly 0 at noon.
        struct CubicSplit
        {
            Real rho, alpha, beta;
        };
        CubicSplit splitCubic (const Real (&a)[4]) noexcept
        {
            const Real p2 = a[2] / a[3], p1 = a[1] / a[3], p0 = a[0] / a[3];
            const Real q = (p2 * p2 - 3.0 * p1) / 9.0;
            const Real r = (2.0 * p2 * p2 * p2 - 9.0 * p2 * p1 + 27.0 * p0) / 54.0;
            const Real sq = std::sqrt (std::max (q, 0.0));
            const Real cosArg = sq > 0.0 ? std::clamp (r / (sq * sq * sq), -1.0, 1.0) : 1.0;
            const Real th = std::acos (cosArg);
            constexpr Real kTwoPi = 6.283185307179586;
            Real roots[3];
            for (int k = 0; k < 3; ++k)
                roots[k] = -2.0 * sq * std::cos ((th + kTwoPi * k) / 3.0) - p2 / 3.0;
            // the fastest pole (most negative root) is the one-pole; the other two are the quadratic
            int f = 0;
            for (int k = 1; k < 3; ++k)
                if (roots[k] < roots[f])
                    f = k;
            const Real x = roots[(f + 1) % 3], y = roots[(f + 2) % 3];
            return { -roots[f], -(x + y), x * y };
        }

        // ======================== §0.6's SPEAKER SIM, AS DATA ========================
        // Four sections per cabinet, in §0.6's own order and inside §0.6's own ranges. What is
        // SPEC-DERIVED is the set of sections and the orderings the tests pin; which figure in
        // each range belongs to which box is the pick.
        //   * the 4×12 is the closed-back reference: the strongest LF resonance, the classic
        //     upper-mid presence peak and a 5 kHz top limit.
        //   * the 2×12 is OPEN-backed, so its LF resonance is lower AND weaker (an open box has
        //     no cabinet air spring to peak against) and its top reaches slightly further.
        //   * the 1×12 and 1×10 climb in LF resonance and lose weight as the cone shrinks — the
        //     1×10 is the thinnest and brightest of the five.
        //   * the 8×10 is §0.6's bass cab, and it is the one row §0.6 states outright: "bass cabs
        //     push the LP HIGHER and the LF resonance LOWER (15"/8×10)". Both are asserted.
        //   * the cone-breakup dip sits roughly half an octave UNDER each box's presence peak and
        //     is the narrowest section of the four. That gap is load-bearing rather than
        //     cosmetic: the peak is wider than the dip and +5 dB against its −5, so a notch
        //     placed inside the peak's skirt produces no net dip anywhere and could only be
        //     asserted by reading the table back (a measurement found exactly that at 1800 Hz
        //     against a 2400 Hz peak — the response there was a 1.4× BOOST).
        //   * `Direct` carries zeroes because it is never read — the stage early-returns.
        constexpr GampEffect::Cab kCab[GampEffect::kNumCabs] = {
            // lfHz, lfDb, lfQ,  presHz, presDb, presQ,  lpHz,  notchHz, notchDb, notchQ
            { 95.0, 4.0, 1.10, 2400.0, 5.0, 0.90, 5000.0, 1300.0, -5.0, 1.60 }, // 4×12
            { 85.0, 2.5, 0.80, 2000.0, 3.5, 0.80, 5500.0, 1100.0, -4.0, 1.40 }, // 2×12 open
            { 110.0, 3.0, 1.00, 2600.0, 4.0, 1.00, 5200.0, 1400.0, -4.5, 1.50 }, // 1×12
            { 140.0, 2.0, 1.00, 3200.0, 5.0, 1.10, 6000.0, 1700.0, -4.0, 1.60 }, // 1×10
            { 60.0, 5.0, 0.90, 1500.0, 2.0, 0.70, 7000.0, 800.0, -3.0, 1.20 }, // 8×10 bass
            { 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 }, // Direct — bypassed
        };

        // …and the three microphones, as the three things a mic does (see the header). §0.6 asks
        // only that the mic be CAPTURED; the character of each is the pick, and the orderings the
        // tests pin are the ones every engineer would recognise:
        //   Dynamic   — a moving-coil's upper-presence peak and its proximity weight; the
        //               reference reach (`lpScale` exactly 1).
        //   Condenser — flatter and further: an air peak an octave up and a top that reaches
        //               ~35 % higher, with no proximity lift.
        //   Ribbon    — the dark one: body rather than presence, a top that stops ~25 % SHORT of
        //               the cab's own limit, and the most low-end weight.
        constexpr GampEffect::Mic kMic[GampEffect::kNumMics] = {
            // bellHz, bellDb, bellQ,  lpScale, lowShelfDb
            { 4500.0, 4.0, 1.20, 1.00, 1.5 }, // Dynamic
            { 8000.0, 2.5, 0.80, 1.35, 0.0 }, // Condenser
            { 1200.0, 1.5, 0.70, 0.75, 2.5 }, // Ribbon
        };
    }

    const GampEffect::Voicing& GampEffect::voicingFor (const Model m) noexcept
    {
        const int i = static_cast<int> (m);
        return kVoicing[i < 0 ? 0 : (i >= kNumModels ? kNumModels - 1 : i)];
    }

    const GampEffect::Tone& GampEffect::toneFor (const Model m) noexcept
    {
        const int i = static_cast<int> (m);
        return kTone[i < 0 ? 0 : (i >= kNumModels ? kNumModels - 1 : i)];
    }

    const GampEffect::ToneNetwork* GampEffect::toneNetworkFor (const Model m) noexcept
    {
        const int i = static_cast<int> (m);
        return i >= 0 && i < kNumModels - 1 ? &kNetwork[i] : nullptr;
    }

    Real GampEffect::bassPot (const Real percent) noexcept
    {
        return std::exp ((clampD (percent, 0.0, kTonePercentMax) / kTonePercentMax - 1.0) * kBassTaper);
    }

    Real GampEffect::linearPot (const Real percent) noexcept
    {
        return clampD (percent, 0.0, kTonePercentMax) / kTonePercentMax;
    }

    std::complex<Real> GampEffect::toneNetworkResponse (const Model m, const Real bassPercent, const Real midPercent, const Real treblePercent, const Real hz) noexcept
    {
        const ToneNetwork* n = toneNetworkFor (m);
        if (n == nullptr)
            return { 1.0, 0.0 };
        // The pots as the hot path holds them (float), so noon here IS the hot path's noon.
        const auto pots = [] (Real b, Real mid, Real t, Real (&out)[3]) {
            out[0] = static_cast<Real> (static_cast<float> (bassPot (b)));
            out[1] = static_cast<Real> (static_cast<float> (linearPot (mid)));
            out[2] = static_cast<Real> (static_cast<float> (linearPot (t)));
        };
        const std::complex<Real> s (0.0, 6.283185307179586 * hz);
        const auto h = [&] (const Real (&p)[3]) {
            Real b[4], a[4];
            networkPolys (*n, p[0], p[1], p[2], b, a);
            return (b[1] * s + b[2] * s * s + b[3] * s * s * s) / (a[0] + a[1] * s + a[2] * s * s + a[3] * s * s * s);
        };
        Real at[3], noon[3];
        pots (bassPercent, midPercent, treblePercent, at);
        pots (kToneNoonPercent, kToneNoonPercent, kToneNoonPercent, noon);
        if (at[0] == noon[0] && at[1] == noon[1] && at[2] == noon[2])
            return { 1.0, 0.0 }; // exactly, as the hot path is (a complex division of equals is not)
        return h (at) / h (noon);
    }

    const GampEffect::Cab& GampEffect::cabFor (const Cabinet c) noexcept
    {
        const int i = static_cast<int> (c);
        return kCab[i < 0 ? 0 : (i >= kNumCabs ? kNumCabs - 1 : i)];
    }

    const GampEffect::Mic& GampEffect::micFor (const Microphone m) noexcept
    {
        const int i = static_cast<int> (m);
        return kMic[i < 0 ? 0 : (i >= kNumMics ? kNumMics - 1 : i)];
    }

    //--------------------------------------------------------------------------
    // The conversion laws. See the header for WHY each one is shaped this way.
    //--------------------------------------------------------------------------

    float GampEffect::preGainFor (const Model m, const Real gainDb) noexcept
    {
        const Real db = clampD (gainDb, 0.0, kGainDbMax) + voicingFor (m).gainOffsetDb;
        return static_cast<float> (std::pow (10.0, db / 20.0));
    }

    float GampEffect::stageGainFor (const Model m) noexcept
    {
        return static_cast<float> (std::pow (10.0, voicingFor (m).stageGainDb / 20.0));
    }

    float GampEffect::gainForDb (const Real db) noexcept
    {
        // `!(db > kSilenceDb)` rather than `db <= kSilenceDb` so the row's own −∞ rail AND a NaN
        // both land on real silence instead of a denormal or a propagated NaN.
        if (!(db > kSilenceDb))
            return 0.0f;
        return static_cast<float> (std::pow (10.0, clampD (db, kSilenceDb, kMaxGainDb) / 20.0));
    }

    float GampEffect::bandGainForPercent (const Real percent, const Real rangeDb, const Real offsetDb) noexcept
    {
        const Real p = clampD (percent, 0.0, kTonePercentMax);
        // NOON IS EXACTLY UNITY when there is no baseline tilt: `p/50 − 1` is exactly 0 at 50,
        // `0 · range + 0` is exactly 0, and `pow(10, 0)` is exactly 1 — so the section it feeds
        // is bit-exactly transparent rather than nearly so.
        const Real db = (p / kToneNoonPercent - 1.0) * rangeDb + offsetDb;
        return static_cast<float> (std::pow (10.0, db / 20.0));
    }

    //--------------------------------------------------------------------------
    // Lifecycle
    //--------------------------------------------------------------------------

    void GampEffect::prepare (const Real sampleRate, const int maxBlockSamples)
    {
        prepare (sampleRate, maxBlockSamples, 0);
    }

    void GampEffect::prepare (const Real sampleRate, const int maxBlockSamples, const int factorOverride)
    {
        (void) maxBlockSamples; // every stage sizes itself off the SAMPLE RATE, not the block

        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        factorOverride_ = factorOverride;

        // [T-FX-24] the dry-alignment line — allocated HERE and never again.
        for (auto& line : dryLine_)
            line.assign (static_cast<std::size_t> (kDryDelayCapacity), 0.0f);
        dryWrite_ = 0;

        // EVERY model's oversampler, on both channels: `Oversampler::prepare` designs
        // coefficients and allocates, and `gamp.model` is a live edit, so selecting a voicing
        // must not be the moment one is built (§2.3 — the trade `FltrEffect` takes for its
        // nine cores). ~3 KB each at factor 8, so twelve of them is tens of KB.
        for (Channel& c : ch_)
            for (int m = 0; m < kNumModels; ++m)
            {
                const int own = kVoicing[m].oversample;
                c.os[m].prepare (factorOverride_ > 0 ? factorOverride_ : own);
            }

        // [leaf 4] The sag detector, at BASE rate (the rail moves over tens of milliseconds, so
        // there is nothing for it to gain from the OS grid). Detector and domain are stated here
        // rather than inherited: §0.5's droop follows the current the amp delivers, which is a
        // PEAK reading in the LEVEL domain.
        sag_.setDetector (EnvelopeFollower::Detector::Peak);
        sag_.setDomain (EnvelopeFollower::Domain::Level);
        sag_.prepare (sampleRate_);
        // …so `refresh` re-prepares (and CLEARS) every filter, and the glides snap: nothing sounds yet.
        osPreparedRate_ = 0.0;
        basePreparedRate_ = 0.0;
        fresh_ = true;

        // AFTER the oversamplers, because the stage coefficients are resolved at the ACTIVE
        // model's OS rate and `refresh` reads the factor back off the oversampler.
        // …which also primes the shapers to the model's bias point, because the bias it resolves
        // differs from the zero this object was constructed with.
        refresh();
        prepared_ = true;
    }

    void GampEffect::reset() noexcept
    {
        // AUDIO thread (the chain's §DSP D6 hard-switch flush). Every model's oversampler, not
        // only the active one: a tail left in an idle one would surface the moment a later
        // model switch selected it — the stuck-tail class `reset()` exists to close off.
        for (Channel& c : ch_)
        {
            for (Oversampler& os : c.os)
                os.reset();
            for (int s = 0; s < kMaxStages; ++s)
            {
                c.clip[s].reset();
                c.couple[s].reset();
                c.plate[s].reset();
            }
            c.dcBlock.reset();
            c.power.reset();
            c.bassSvf.reset();
            c.midSvf.reset();
            c.trebleSvf.reset();
            c.netZero.reset();
            c.netPole.reset();
            c.netLp = 0.0;
            c.presenceSvf.reset();
            c.cabLf.reset();
            c.cabPres.reset();
            c.cabNotch.reset();
            c.cabLp1.reset();
            c.cabLp2.reset();
            c.micBell.reset();
            c.micShelf.reset();
            c.xoverLo1.reset();
            c.xoverLo2.reset();
            c.xoverHi1.reset();
            c.xoverHi2.reset();
            c.piezoBell.reset();
            c.piezoShelf.reset();
            c.bodyBell.reset();
            c.xfmrFlux.reset();
            c.xoverX1 = 0.0;
            c.xoverG1 = 0.0;
        }
        // [leaf 4] …and the rail, which is state too: a follower still holding a loud passage
        // would leave the amp compressed after the chain's hard-switch flush. RT-safe.
        sag_.reset();
        lastOut_[0] = 0.0f;
        lastOut_[1] = 0.0f;
        rail_ = 1.0f;
        // [T-FX-24] the dry line IS signal memory, so a reset drops it (FLTR's reset does the same).
        for (auto& line : dryLine_)
            std::fill (line.begin(), line.end(), 0.0f);
        dryWrite_ = 0;
        // …and every glide lands: after the hard-switch flush there is no note to click.
        for (Glide* g : glides())
            g->snap (g->target);
        for (Glide* g : { &gNetBass_, &gNetMid_, &gNetTreble_ })
            g->snap (g->target);
        resolveNetwork (gNetBass_.cur, gNetMid_.cur, gNetTreble_.cur);
        if (cornerLeft_ > 0)
        {
            cornerLeft_ = 0;
            cornerFrom_ = cornerTo_;
            applyCorners (cornerTo_);
        }
        gliding_ = false;
        fresh_ = true;
        primeClips();
    }

    void GampEffect::primeClips() noexcept
    {
        // THE IDLE OPERATING POINT, and it is physics rather than a test convenience: §0.3's
        // stage sits at its grid bias at rest, so the shaper's one-sample memory must hold the
        // bias and not zero. `AdaaSoftClip` is an ANTIDERIVATIVE shaper — its output is the
        // average of the curve ACROSS the sample interval — so a cold state (x[n−1] = 0)
        // followed by a first input of `bias` reads `F1(bias)/bias`, exactly HALF of
        // `shape(bias)` for a small bias, and `core`'s constant subtraction then leaves a
        // one-sample −0.1 spike out of pure silence. One discarded evaluation at the bias
        // removes it, and after it a DC-silent input maps to EXACTLY zero at every stage.
        for (Channel& c : ch_)
            for (int s = 0; s < kMaxStages; ++s)
                (void) c.clip[s].processSample (static_cast<float> (bias_));
    }

    int GampEffect::latencySamples() const noexcept
    {
        return ch_[0].os[static_cast<int> (model_)].latencySamples();
    }

    //--------------------------------------------------------------------------
    // The audio path
    //--------------------------------------------------------------------------

    float GampEffect::core (Channel& c, const float u) noexcept
    {
        // §0.3's stage, N times. Runs at OS RATE (the whole nonlinear core inside one up/down
        // cascade — the M1-09fh idiom), so the couplings and plate roll-offs were resolved at
        // `factor · fs` in `refresh`.
        const int stages = voicingFor (model_).stages;
        float y = u;
        // [leaf 2/5] §0.4 puts the stack AFTER the first gain stage, so the linear voicing (no
        // stages at all) gets it here — after §6.2's two corrective sections, which is §6.1's own
        // order (the piezo correction and the body come before the semi-parametric EQ).
        if (stages == 0)
            return tone (c, acousticVoicing (c, y));

        // [leaf 5] §5.1's band split, on the one voicing whose topology differs: only the HIGH
        // band is driven, and §5.1 puts the tone stack and the power amp after the recombine.
        if (model_ == Model::Bass)
        {
            // A REAL LR4 CROSSOVER — two cascaded Butterworth sections per half — and NOT the
            // complementary `x − lp` subtraction this leaf tried first. That subtraction
            // reconstructs bit-exactly and isolates NOTHING: a cascaded low-pass at 250 Hz is
            // 0.997 in magnitude at 60 Hz but −38° in PHASE, so `|1 − 0.997∠−38°|` is 0.65 — the
            // fundamental survives in the "high" band at two thirds of full scale, gets §5.3's
            // 20 dB of high-band gain, and clips. Measured 0.69 THD at 60 Hz, i.e. precisely the
            // mush §5.2 exists to avoid, and 0.65 is what the arithmetic predicts. Two cascaded
            // high-passes leave it 49 dB down instead.
            const float low = c.xoverLo2.processSample (c.xoverLo1.processSample (y).lp).lp;
            // …and §5.3's own reading of the `gain` row — "Gain / drive: applied to the HIGH
            // band" — which is the other half of keeping the fundamental clean: pre-gaining before
            // the split would drive the low band into the power amp's knee.
            float high = c.xoverHi2.processSample (c.xoverHi1.processSample (y).hp).hp * gPre_.cur;
            for (int s = 0; s < stages; ++s)
                high = stage (c, s, high);
            return powerAmp (c, tone (c, low + high));
        }

        for (int s = 0; s < stages; ++s)
        {
            y = stage (c, s, y);
            // [leaf 2] §0.4's position: "after the first gain stage(s) … so it shapes what feeds
            // later gain". On a one-stage voicing that is also the end of the cascade.
            if (s == 0)
                y = tone (c, y);
        }
        // [leaf 4] §0.5's power amp, LAST inside the nonlinear region — §7's "distortion is
        // distributed", and the reason it is not just a second preamp stage is the rail.
        return powerAmp (c, y);
    }

    float GampEffect::stage (Channel& c, const int s, const float x) noexcept
    {
        // ONE §0.3 gain stage. The coupling cap FIRST, so what a stage distorts is what survived
        // the previous stage's HP — §4.2's whole point, and also §0.7's DC block between stages
        // (the previous clip's bias offset never reaches this one's knee).
        float h = c.couple[s].hp (x);
        if (s > 0)
            h *= stageGain_; // INTER-stage gain; the row's own pre-gain fed stage 1
        // …the grid bias, then the plate curve: §0.3's option-1 transfer function, "apply a DC
        // offset, pass through soft-clip, REMOVE DC" — and the removal is LOCAL, `− shape(bias)`,
        // not left to the blockers downstream. Two reasons, and the first is a defect this caught:
        // the offset is a CONSTANT, so a stage fed silence emits `shape(bias)` and four cascaded
        // stages amplify it into a 0.74-peak thump on the first sample after a cold start
        // (`reset()` is called on the chain's hard switch, so that is a click on every reorder).
        // Subtracting it makes silence in EXACTLY silence out, at every stage, and the asymmetry
        // is untouched because only the curve's offset moved — the even harmonics are a property
        // of its SHAPE. The blockers still earn their place: an asymmetric clip on a real SIGNAL
        // makes a program-dependent DC term this static subtraction cannot know about.
        return c.plate[s].lp (
            c.clip[s].processSample (h + static_cast<float> (bias_)) - biasOffset_);
    }

    float GampEffect::acousticVoicing (Channel& c, const float x) noexcept
    {
        // §6.2's core transformation, in §6.1's order: cut the piezo's harsh spike and restore the
        // lows it never had, then put a body resonance back where the pickup cannot hear one. Both
        // are FIXED — §6.3's rows for them do not exist in this type's registry block (ledgered).
        float y = x + piezoBellK_ * (piezoBellGain_ - 1.0f) * c.piezoBell.processSample (x).bp;
        y += (piezoShelfGain_ - 1.0f) * c.piezoShelf.processSample (y).lp;
        return y + bodyK_ * (bodyGain_ - 1.0f) * c.bodyBell.processSample (y).bp;
    }

    float GampEffect::powerAmp (Channel& c, const float x) noexcept
    {
        // SYMMETRIC (a push-pull pair clips both halves alike), so no bias, no injected DC and
        // nothing to prime — `shape(0)` is 0, so silence stays silence by construction.
        //
        // The rail scales the clip's input DOWN and its output back UP, which is what makes the
        // droop a HEADROOM change rather than a volume change: as the rail falls the effective
        // threshold falls with it, so a constant loud input is compressed progressively over the
        // attack and released over the recovery. `rail_` is resolved once per BASE sample, so
        // every OS sub-sample of one frame sees the same supply — which is physically right (the
        // rail cannot move at 8× the audio rate) and keeps the division off the OS path.
        float v = x * powerDrive_ / rail_;
        // [D-M-0304] §0.5's CROSSOVER, in the pair's grid domain before it clips (see the header).
        // Skipped bit-exactly at 0 (the seam's OFF side, the linear voicing).
        if (xoverDepth_ > 0.0f)
            v -= crossover (c, v);
        const float z = rail_ * c.power.processSample (v);
        // [D-M-0304] …then §0.5's OUTPUT TRANSFORMER (see the header for the model). Skipped
        // bit-exactly at 0, which is the A/B seam's OFF side and the linear voicing.
        if (xfmrDrive_ <= 0.0f)
            return z;
        const float phi = xfmrK_ * c.xfmrFlux.processSample (z).bp; // the core flux
        const float u = xfmrDrive_ * phi;
        // e(φ) = φ − S(gφ)/g, the flux the core could not carry: u⁹/(9g) under the knee (computed
        // directly — no cancellation), and everything past ±8/(9g) above it.
        float excess;
        if (u >= 1.0f)
            excess = phi - kXfmrCeiling * xfmrInvDrive_;
        else if (u <= -1.0f)
            excess = phi + kXfmrCeiling * xfmrInvDrive_;
        else
        {
            const float u2 = u * u;
            const float u4 = u2 * u2;
            excess = u4 * u4 * u * (xfmrInvDrive_ / 9.0f);
        }
        return z - excess;
    }

    float GampEffect::crossover (Channel& c, const float v) noexcept
    {
        // The DEVIATION from the straight line, D(v) = depth·v·(1 − u²)³ inside the band, through
        // first-order ADAA: its antiderivative G(v) = −depth·(w²/8)·(1 − u²)⁴ is a polynomial, so the
        // divided difference costs no libm. A loud note crosses the band in a fraction of an OS sample
        // and the bump would otherwise land as a broadband spike (measured: +2.4 dB on Bass's hot alias
        // floor without it). Both ends outside the band ⇒ G = 0 both ⇒ exactly 0: a loud note away from
        // zero, and a crossing that jumps the whole band (D is odd, so its mean across ±w is 0).
        const Real w = kXoverWidth;
        const Real depth = static_cast<Real> (xoverDepth_);
        const Real x0 = static_cast<Real> (v);
        const Real u0 = x0 / w;
        const Real t0 = u0 < 1.0 && u0 > -1.0 ? 1.0 - u0 * u0 : 0.0;
        const Real g0 = -depth * (w * w / 8.0) * (t0 * t0) * (t0 * t0);
        const Real diff = x0 - c.xoverX1;
        Real d;
        if (diff > kXoverEps || diff < -kXoverEps)
            d = (g0 - c.xoverG1) / diff;
        else
        {
            const Real m = 0.5 * (x0 + c.xoverX1);
            const Real um = m / w;
            const Real tm = um < 1.0 && um > -1.0 ? 1.0 - um * um : 0.0;
            d = depth * m * tm * tm * tm;
        }
        c.xoverX1 = x0;
        c.xoverG1 = g0;
        return static_cast<float> (d);
    }

    float GampEffect::tone (Channel& c, const float x) noexcept
    {
        // Three sections over the SIMULTANEOUS taps of three `TptSvf` frames — see the header for
        // each law and for why a neutral section is bit-exactly transparent.
        const TptSvf::Outputs b = c.bassSvf.processSample (x);
        const TptSvf::Outputs m = c.midSvf.processSample (x);
        const TptSvf::Outputs t = c.trebleSvf.processSample (x);
        // Each section is fed the SAME input and their offsets are summed, which is what makes
        // the three independent (a serial cascade would multiply their skirts together and no
        // band's gain would be exact anywhere).
        const float y = x + gBass_.cur * b.lp + gMid_.cur * m.bp + gTreble_.cur * t.hp;
        if (!netOn_)
            return y;
        // [D-M-0305] …and on a network voicing the pots act through the stack instead (the sections above carry
        // only the model's noon `midOffsetDb` bell there): H(l, m, t) / H(noon), three sections in series. At noon
        // every correction is exactly 0 and both gains exactly 1, so this returns `y` bit for bit.
        const TptSvf::Outputs z = c.netZero.processSample (y);
        float v = netC0_ * y + netC1_ * z.bp + netC2_ * z.lp;
        const TptSvf::Outputs p = c.netPole.processSample (v);
        v = v + netD1_ * p.bp + netD2_ * p.lp;
        const Real u = (static_cast<Real> (v) - c.netLp) * netG_; // TPT one-pole at the fastest pole
        const Real lp = u + c.netLp;
        c.netLp = lp + u;
        v = v + netE_ * static_cast<float> (lp);
        return netGain_ * v;
    }

    void GampEffect::resolveNetwork (const float l, const float m, const float t) noexcept
    {
        const ToneNetwork* n = toneNetworkFor (model_);
        netOn_ = n != nullptr;
        if (n == nullptr)
            return;
        Real b[4], a[4];
        networkPolys (*n, static_cast<Real> (l), static_cast<Real> (m), static_cast<Real> (t), b, a);
        // S1 — N'(s) / N'_noon(s), N' = b1 + b2 s + b3 s², over the SVF at the noon zeros (ω1² = b1n/b3n): written
        // c0·x + c1·bp + c2·lp with hp = x − k·bp − lp folded in, each difference formed so it is exactly 0 at noon.
        const Real w1 = std::sqrt (netB1n_ / netB3n_);
        netC0_ = static_cast<float> (b[3] / netB3n_);
        netC1_ = static_cast<float> ((b[2] * netB3n_ - b[3] * netB2n_) / (netB3n_ * netB3n_ * w1));
        netC2_ = static_cast<float> ((b[1] * netB3n_ - b[3] * netB1n_) / (netB1n_ * netB3n_));
        // S2/S3 — A_noon(s) / A(s), split at the fastest pole.
        const CubicSplit k = splitCubic (a);
        const Real w = std::sqrt (k.beta);
        netD1_ = static_cast<float> ((netAlphaN_ - k.alpha) / w);
        netD2_ = static_cast<float> ((netBetaN_ - k.beta) / k.beta);
        netE_ = static_cast<float> ((netRhoN_ - k.rho) / k.rho);
        netGain_ = static_cast<float> (netA3n_ / a[3]);
        constexpr Real kTwoPi = 6.283185307179586;
        const Real g = fm::tan (std::min (k.rho / (2.0 * netOsRate_), 0.49 * 3.141592653589793));
        netG_ = g / (1.0 + g);
        for (Channel& c : ch_)
            c.netPole.setCutoff (static_cast<float> (w / kTwoPi), static_cast<float> (w / k.alpha));
    }

    float GampEffect::cabinet (Channel& c, const float x) noexcept
    {
        // §0.6's roster ends in a DI, which never met a speaker — so the whole stage is a bit-exact
        // bypass there, MIC INCLUDED (see the header). None of the seven filters is advanced, so a
        // `Direct` patch also costs nothing. [D-M-0513] SETTLED on it, that is: mid-switch the box's
        // share `gCabMix_` crossfades the stage against its input (below).
        const float mix = gCabMix_.cur;
        if (mix == 0.0f)
            return x;

        // The cab's three peaks/dips are summed over the SAME input, like the tone stack's, so
        // each one's gain is exact at its own centre.
        const TptSvf::Outputs lf = c.cabLf.processSample (x);
        const TptSvf::Outputs pr = c.cabPres.processSample (x);
        const TptSvf::Outputs nt = c.cabNotch.processSample (x);
        float y = x + gCabLf_.cur * lf.bp + gCabPres_.cur * pr.bp + gCabNotch_.cur * nt.bp;
        // …then the MIC's own bell and weight, in SERIES with them rather than summed: the mic is
        // a second transducer downstream of the speaker, not a fourth resonance of it.
        y += gMicBell_.cur * c.micBell.processSample (y).bp;
        y += gMicShelf_.cur * c.micShelf.processSample (y).lp;
        // …and last, §0.6's steep top limit — the thing that "kills fizz", so it has to come AFTER
        // every peak that could add some. Two cascaded 2-pole sections = 24 dB/oct.
        const float wet = c.cabLp2.processSample (c.cabLp1.processSample (y).lp).lp;
        return mix == 1.0f ? wet : x + mix * (wet - x);
    }

    void GampEffect::process (float* left, float* right, const int numSamples) noexcept
    {
        if (left == nullptr || right == nullptr || numSamples <= 0 || !prepared_)
            return;

        const int mi = static_cast<int> (model_);
        Channel& cl = ch_[0];
        Channel& cr = ch_[1];
        const int dryLenNow = dryLen_.load (std::memory_order_relaxed);

        for (int n = 0; n < numSamples; ++n)
        {
            const float xl = left[n];
            const float xr = right[n];

            // [leaf 4] §0.5's B+ rail, from the PREVIOUS output on both channels — the feedback
            // tap of the follower's contract, LINKED on the max of the two (§0.7). Resolved before
            // the cores run, so both channels' power amps see one supply.
            const float al = lastOut_[0] < 0.0f ? -lastOut_[0] : lastOut_[0];
            const float ar = lastOut_[1] < 0.0f ? -lastOut_[1] : lastOut_[1];
            const float env = sag_.processSample (al > ar ? al : ar);
            const float droop = 1.0f - sagAmount_ * env;
            rail_ = droop < kMinRail ? kMinRail : (droop > 1.0f ? 1.0f : droop);

            // The row's pre-gain is applied at BASE rate — it is linear, so it commutes with the
            // interpolating FIR, and one multiply beats `factor` of them. `inputGain_` is that
            // pre-gain for five of the six voicings and exactly 1 for the sixth: §5.3 applies the
            // `gain` row to the HIGH BAND ONLY, so on Bass it is applied inside `core` after the
            // split (resolved control-rate, so the hot path has no branch).
            // The glides advance once per BASE sample, so every OS sub-sample of a frame sees one value.
            if (gliding_)
                advanceGlides();
            float wl = cl.os[mi].processSample (xl * gInput_.cur,
                [this, &cl] (const float u) { return core (cl, u); });
            float wr = mono_ ? 0.0f : cr.os[mi].processSample (xr * gInput_.cur,
                [this, &cr] (const float u) { return core (cr, u); });

            // §0.7's final DC blocker, at base rate: the last stage's bias would otherwise ride
            // straight out of the effect.
            // …and what the rail will read NEXT sample: the cascade's own output, before the
            // cabinet, because §0.5's droop follows what the power amp delivered rather than what
            // the speaker did with it.
            lastOut_[0] = wl;
            lastOut_[1] = wr;

            wl = cl.dcBlock.hp (wl);
            if (!mono_)
                wr = cr.dcBlock.hp (wr);

            // [leaf 2] §0.5's presence — the NFB high shelf, at BASE rate and AFTER the
            // power-amp position, which is where reducing feedback acts. Transparent at noon
            // (`presenceGain_` is exactly 1), so this costs one `TptSvf` frame and nothing else.
            wl += gPresence_.cur * cl.presenceSvf.processSample (wl).hp;
            if (!mono_)
                wr += gPresence_.cur * cr.presenceSvf.processSample (wr).hp;

            // [leaf 3] §0.6's speaker + mic, last in the chain and at base rate.
            wl = cabinet (cl, wl);
            wr = mono_ ? wl : cabinet (cr, wr);

            // [T-FX-24] the DRY leg at this effect's own reported latency (FLTR's pattern).
            dryLine_[0][static_cast<std::size_t> (dryWrite_)] = xl;
            dryLine_[1][static_cast<std::size_t> (dryWrite_)] = xr;
            const int rawRead = dryWrite_ - dryLenNow;
            const auto readIdx = static_cast<std::size_t> (rawRead < 0 ? rawRead + kDryDelayCapacity : rawRead);
            const float dl = dryLine_[0][readIdx];
            const float dr = dryLine_[1][readIdx];
            dryWrite_ = (dryWrite_ + 1 == kDryDelayCapacity) ? 0 : dryWrite_ + 1;

            left[n] = gWet_.cur * wl + gDry_.cur * dl;
            right[n] = gWet_.cur * wr + gDry_.cur * dr;
        }
        fresh_ = false; // a note may be sounding from here on: edits glide
    }

    //--------------------------------------------------------------------------
    // Control rate
    //--------------------------------------------------------------------------

    void GampEffect::advanceGlides() noexcept
    {
        bool any = false;
        for (Glide* g : glides())
            any = g->advance() || any;
        if (cornerLeft_ > 0)
        {
            --cornerLeft_;
            applyCorners (cornersInForce()); // `cornerTo_` itself on the last step: an exact landing
            any = true;
        }
        bool pots = false;
        for (Glide* g : { &gNetBass_, &gNetMid_, &gNetTreble_ })
            pots = g->advance() || pots;
        if (pots)
            resolveNetwork (gNetBass_.cur, gNetMid_.cur, gNetTreble_.cur); // the targets themselves on the last step
        gliding_ = any || pots;
    }

    GampEffect::Corners GampEffect::cornersInForce() const noexcept
    {
        if (cornerLeft_ == 0)
            return cornerTo_;
        const Real t = 1.0 - static_cast<Real> (cornerLeft_) / static_cast<Real> (cornerLen_);
        Corners k = cornerTo_;
        for (std::size_t i = 0; i < k.size(); ++i)
        {
            const Corner& a = cornerFrom_[i];
            k[i].hz = static_cast<float> (static_cast<Real> (a.hz) * std::exp (t * cornerLnRatio_[i]));
            k[i].q = static_cast<float> (static_cast<Real> (a.q) + t * static_cast<Real> (cornerTo_[i].q - a.q));
        }
        return k;
    }

    void GampEffect::applyCorners (const Corners& k) noexcept
    {
        for (Channel& c : ch_)
        {
            c.cabLf.setCutoff (k[0].hz, k[0].q);
            c.cabPres.setCutoff (k[1].hz, k[1].q);
            c.cabNotch.setCutoff (k[2].hz, k[2].q);
            c.cabLp1.setCutoff (k[3].hz, k[3].q);
            c.cabLp2.setCutoff (k[3].hz, k[3].q);
            c.micBell.setCutoff (k[4].hz, k[4].q);
        }
    }

    void GampEffect::refresh() noexcept
    {
        const Voicing& v = voicingFor (model_);

        preGain_ = preGainFor (model_, gainDb_);
        // [leaf 5] …and where it is APPLIED, which is per voicing: at the input for five of the
        // six, after the band split for Bass (§5.3's "applied to the high band").
        inputGain_ = model_ == Model::Bass ? 1.0f : preGain_;
        stageGain_ = stageGainFor (model_);
        const Real previousBias = bias_;
        bias_ = v.bias;
        // The clip's answer to the bias ALONE — subtracted per stage so silence stays silence
        // (see `core`). Exact for the ADAA shaper too: at a constant input its divided
        // difference degenerates to `shape` of the midpoint, which is `shape(bias)`.
        biasOffset_ = static_cast<float> (AdaaSoftClip::shape (bias_));
        // …and when the bias MOVES, the shapers' memory has to move with it, or the first sample
        // after the edit reads a divided difference across the bias STEP and the constant
        // subtraction above leaves a spike (that is the same defect `primeClips` exists for,
        // arriving through a model switch instead of a cold start). Gated on the change, because
        // priming is a write to audio state and `gain`/`level`/`mix` edits must not perturb it —
        // only a `model` edit moves the operating point.
        if (bias_ != previousBias)
            primeClips();

        // The stage filters run at OS rate, so their corners are resolved against `factor · fs`
        // — the ScreamFilter rule, and what keeps a voicing's tightness the same MUSICAL corner
        // whatever factor it runs at. Read the factor off the oversampler rather than the table,
        // so the override the alias case uses is honoured here too.
        const Real osRate = sampleRate_
                              * static_cast<Real> (ch_[0].os[static_cast<int> (model_)].factor());
        // `TptSvf::prepare` CLEARS a filter's state, so it runs only when the rate it is resolved
        // against MOVES — `prepare()` (base rate) or a model edit across OS factors. A row edit is a
        // knob turn on a sounding note: it re-resolves coefficients and keeps the state (the old path
        // re-prepared everything on every edit — a click per knob turn).
        const bool osMoved = osRate != osPreparedRate_;
        const bool baseMoved = sampleRate_ != basePreparedRate_;
        osPreparedRate_ = osRate;
        basePreparedRate_ = sampleRate_;
        // [leaf 2] …and the tone stack's own, resolved from the ROWS. The bell's `k` is `1/Q`,
        // which is the same damping term the SVF itself computes — read it back off the filter
        // rather than recomputing it, so the two cannot disagree about the peak gain.
        const Tone& tn = toneFor (model_);
        // [D-M-0305] On a network voicing the three rows drive the passive stack (`resolveNetwork`, below), so the
        // sections here carry only the model's noon tilt: the shelves sit out, the bell holds `midOffsetDb`.
        const ToneNetwork* net = toneNetworkFor (model_);
        bassGain_ = net != nullptr ? 1.0f : bandGainForPercent (bassPercent_, tn.rangeDb);
        midGain_ = bandGainForPercent (net != nullptr ? kToneNoonPercent : midPercent_, tn.rangeDb, tn.midOffsetDb);
        trebleGain_ = net != nullptr ? 1.0f : bandGainForPercent (treblePercent_, tn.rangeDb);
        presenceGain_ = bandGainForPercent (presencePercent_, tn.rangeDb);
        for (Channel& c : ch_)
        {
            for (int s = 0; s < kMaxStages; ++s)
            {
                c.couple[s].setCutoff (v.interHpHz, osRate);
                c.plate[s].setCutoff (v.stageLpHz, osRate);
            }
            c.dcBlock.setCutoff (kDcBlockHz, sampleRate_);
            // The stack runs INSIDE the oversampled region (§0.4's position), so its corners are
            // resolved at `factor · fs` exactly like the cascade's.
            if (osMoved)
                c.bassSvf.prepare (osRate);
            if (osMoved)
                c.midSvf.prepare (osRate);
            if (osMoved)
                c.trebleSvf.prepare (osRate);
            if (osMoved)
            {
                c.netZero.prepare (osRate);
                c.netPole.prepare (osRate);
                c.netLp = 0.0;
            }
            c.bassSvf.setCutoff (static_cast<float> (tn.bassHz), kShelfQ);
            c.midSvf.setCutoff (static_cast<float> (tn.midHz), static_cast<float> (tn.midQ));
            c.trebleSvf.setCutoff (static_cast<float> (tn.trebleHz), kShelfQ);
            // …and presence at BASE rate, because §0.5 puts it after the power amp.
            if (baseMoved)
                c.presenceSvf.prepare (sampleRate_);
            c.presenceSvf.setCutoff (static_cast<float> (tn.presenceHz), kShelfQ);
        }
        midK_ = static_cast<float> (ch_[0].midSvf.dampingK());
        // [D-M-0305] …and the network's NOON, from the pots exactly as the glides will hold them (float), so a pot
        // that lands on noon reproduces these doubles and every correction is exactly 0.
        netOsRate_ = osRate;
        if (net != nullptr)
        {
            netNoonL_ = static_cast<float> (bassPot (kToneNoonPercent));
            const auto mid = static_cast<Real> (static_cast<float> (linearPot (kToneNoonPercent)));
            Real b[4], a[4];
            networkPolys (*net, static_cast<Real> (netNoonL_), mid, mid, b, a);
            netB1n_ = b[1];
            netB2n_ = b[2];
            netB3n_ = b[3];
            netA3n_ = a[3];
            const CubicSplit k = splitCubic (a);
            netRhoN_ = k.rho;
            netAlphaN_ = k.alpha;
            netBetaN_ = k.beta;
            const Real w1 = std::sqrt (b[1] / b[3]);
            for (Channel& c : ch_)
                c.netZero.setCutoff (static_cast<float> (w1 / 6.283185307179586), static_cast<float> (w1 / (b[2] / b[3])));
        }

        // [leaf 3] §0.6's cascade, at BASE rate. The two tables MULTIPLY in exactly one place —
        // the top limit — which is what makes the mic a modifier rather than a second cabinet.
        // [D-M-0513] `Direct` keeps the last real box's coefficients, so a switch to it fades that box out.
        if (cab_ != Cabinet::Direct)
            voicedCab_ = cab_;
        const Cab& cb = cabFor (voicedCab_);
        const Mic& mc = micFor (mic_);
        // …and a box switched in from a SETTLED `Direct` starts from rest: its filters were not run while
        // the stage was out, so their state is whatever the last box left, seconds ago.
        const bool stageOut = gCabMix_.cur == 0.0f && gCabMix_.left == 0;
        const bool reEnter = cab_ != Cabinet::Direct && stageOut;
        cabLfGain_ = gainForDb (cb.lfDb);
        cabPresGain_ = gainForDb (cb.presDb);
        cabNotchGain_ = gainForDb (cb.notchDb);
        micBellGain_ = gainForDb (mc.bellDb);
        micShelfGain_ = gainForDb (mc.lowShelfDb);
        const Real lpHz = cb.lpHz * mc.lpScale;
        for (Channel& c : ch_)
        {
            if (baseMoved)
                c.cabLf.prepare (sampleRate_);
            if (baseMoved)
                c.cabPres.prepare (sampleRate_);
            if (baseMoved)
                c.cabNotch.prepare (sampleRate_);
            if (baseMoved)
                c.cabLp1.prepare (sampleRate_);
            if (baseMoved)
                c.cabLp2.prepare (sampleRate_);
            if (baseMoved)
                c.micBell.prepare (sampleRate_);
            if (baseMoved)
                c.micShelf.prepare (sampleRate_);
            c.micShelf.setCutoff (static_cast<float> (kMicShelfHz), kShelfQ);
            if (reEnter)
            {
                c.cabLf.reset();
                c.cabPres.reset();
                c.cabNotch.reset();
                c.cabLp1.reset();
                c.cabLp2.reset();
                c.micBell.reset();
                c.micShelf.reset();
            }
        }
        // [D-M-0513] The five moving corners. Butterworth per LP section, so the CASCADE of two is
        // maximally flat in its passband and 24 dB/oct beyond — the "natural top limit" §0.6 asks
        // for, not a resonant wall. Nothing sounding through them (before the first block, a model
        // edit's snap, or the stage settled out on `Direct`) and they SNAP; on a sounding note they
        // glide from the corners in force (see the header). An unchanged re-push lets a glide run on.
        const Corners corners { { { static_cast<float> (cb.lfHz), static_cast<float> (cb.lfQ) },
            { static_cast<float> (cb.presHz), static_cast<float> (cb.presQ) },
            { static_cast<float> (cb.notchHz), static_cast<float> (cb.notchQ) },
            { static_cast<float> (lpHz), kShelfQ },
            { static_cast<float> (mc.bellHz), static_cast<float> (mc.bellQ) } } };
        if (fresh_ || model_ != glideModel_ || stageOut || baseMoved)
        {
            cornerFrom_ = corners;
            cornerTo_ = corners;
            cornerLeft_ = 0;
            applyCorners (corners);
        }
        else if (corners != cornerTo_)
        {
            cornerFrom_ = cornersInForce();
            cornerTo_ = corners;
            for (int i = 0; i < kCorners; ++i)
            {
                const auto ui = static_cast<std::size_t> (i);
                cornerLnRatio_[ui] = std::log (static_cast<Real> (cornerTo_[ui].hz) / static_cast<Real> (cornerFrom_[ui].hz));
            }
            cornerLen_ = std::max (1, static_cast<int> (std::lround (kGlideMs * 0.001 * sampleRate_)));
            cornerLeft_ = cornerLen_;
        }
        // [leaf 5] §5.1's crossover and §6.2's two sections — all at OS rate, because all three
        // live inside the nonlinear core (the split feeds the cascade, and the acoustic sections
        // sit where the cascade would have been).
        piezoBellGain_ = gainForDb (kPiezoDb);
        piezoShelfGain_ = gainForDb (kPiezoShelfDb);
        bodyGain_ = gainForDb (kBodyDb);
        for (Channel& c : ch_)
        {
            if (osMoved)
                c.xoverLo1.prepare (osRate);
            if (osMoved)
                c.xoverLo2.prepare (osRate);
            if (osMoved)
                c.xoverHi1.prepare (osRate);
            if (osMoved)
                c.xoverHi2.prepare (osRate);
            if (osMoved)
                c.piezoBell.prepare (osRate);
            if (osMoved)
                c.piezoShelf.prepare (osRate);
            if (osMoved)
                c.bodyBell.prepare (osRate);
            // Butterworth per section, cascaded — the LR4 construction, so each half is
            // 24 dB/oct and the two sum to an allpass (flat in MAGNITUDE, phase rotated).
            c.xoverLo1.setCutoff (static_cast<float> (kBassXoverHz), kShelfQ);
            c.xoverLo2.setCutoff (static_cast<float> (kBassXoverHz), kShelfQ);
            c.xoverHi1.setCutoff (static_cast<float> (kBassXoverHz), kShelfQ);
            c.xoverHi2.setCutoff (static_cast<float> (kBassXoverHz), kShelfQ);
            c.piezoBell.setCutoff (static_cast<float> (kPiezoHz), static_cast<float> (kPiezoQ));
            c.piezoShelf.setCutoff (static_cast<float> (kPiezoShelfHz), kShelfQ);
            c.bodyBell.setCutoff (static_cast<float> (kBodyHz), static_cast<float> (kBodyQ));
        }
        piezoBellK_ = static_cast<float> (ch_[0].piezoBell.dampingK());
        bodyK_ = static_cast<float> (ch_[0].bodyBell.dampingK());

        // [D-M-0513] The bells' `k` at the TARGET corners (the filters may still be gliding toward
        // them), read off the filter's own law through a probe so the two cannot disagree.
        const auto kAt = [] (const Corner& k) {
            TptSvf probe;
            probe.setCutoff (k.hz, k.q);
            return static_cast<float> (probe.dampingK());
        };
        cabLfK_ = kAt (corners[0]);
        cabPresK_ = kAt (corners[1]);
        cabNotchK_ = kAt (corners[2]);
        micBellK_ = kAt (corners[4]);

        // `level` and the per-model makeup are both plain dB, so they add before the one
        // conversion — and a `level` of −∞ (the row's plain 0) still lands on exact silence
        // because `gainForDb`'s silence rail is checked on the SUM.
        // [leaf 4] §0.5's power amp, from the same table. The linear voicing has none (`stages`
        // is 0, so `core` never calls it), which is what keeps §6's "not a distortion model" true
        // of the WHOLE chain rather than only of its preamp.
        powerDrive_ = gainForDb (v.powerDriveDb);
        sagAmount_ = static_cast<float> (clampD (v.sagAmount, 0.0, 0.9));
        sag_.setAttackMs (static_cast<float> (v.sagAttackMs));
        sag_.setReleaseMs (static_cast<float> (v.sagReleaseMs));
        // [D-M-0304] …and its two colours, each gated by the A/B seam. The flux band-pass runs at OS
        // rate inside the core; Q = ½ makes it two coincident real poles, and `k·bp` unity at the
        // corner.
        xfmrDrive_ = xfmrOn_ ? static_cast<float> (clampD (v.xfmrDrive, 0.0, 10.0)) : 0.0f;
        xfmrInvDrive_ = xfmrDrive_ > 0.0f ? 1.0f / xfmrDrive_ : 0.0f;
        xoverDepth_ = xoverOn_ ? static_cast<float> (clampD (v.xoverDepth, 0.0, 0.9)) : 0.0f;
        // …and the flux likewise: a `gain`/tone/`mix` edit must not drop a saturated core's state.
        for (Channel& c : ch_)
        {
            if (osMoved)
                c.xfmrFlux.prepare (osRate);
            c.xfmrFlux.setCutoff (static_cast<float> (kXfmrFluxHz), 0.5f);
        }
        // `k·bp` is unity at the corner and 2w/(1+w²) at w = f/fc; dividing that out at the low-E
        // reference makes φ READ AS AMPLITUDE there, so `xfmrDrive` is 1/(the low-E level at the knee).
        const Real w = kXfmrRefHz / kXfmrFluxHz;
        xfmrK_ = static_cast<float> (ch_[0].xfmrFlux.dampingK() * (1.0 + w * w) / (2.0 * w));

        outGain_ = gainForDb (levelDb_ + v.makeupDb);

        wetGain_ = static_cast<float> (clampD (mixPercent_, 0.0, kMixPercentMax) / kMixPercentMax);
        dryGain_ = 1.0f - wetGain_;
        wetOut_ = wetGain_ * outGain_;

        // THE GLIDES. Every gain the hot path multiplies by moves to its new value over `kGlideMs`
        // (linear, landing EXACTLY on the target) rather than stepping — a stepped band gain on a
        // sounding note is a click. Each is the coefficient AS THE HOT PATH USES IT (`k·(A − 1)`,
        // not `A`), so a glide that has landed is bit-for-bit the old arithmetic. Before the first
        // block after `prepare`/`reset` there is no note to click, so they SNAP (which keeps a
        // configure-then-render bit-identical to the old path).
        const int glideLen = std::max (1, static_cast<int> (std::lround (kGlideMs * 0.001 * sampleRate_)));
        // …and a MODEL edit snaps them too: it swaps the whole gain structure (and possibly the
        // oversampler), so a glide would walk one voicing's pre-gain through another's cascade —
        // measured 3× over the linear voicing's level for 10 ms going Crunch -> Acoustic.
        const bool snap = fresh_ || model_ != glideModel_;
        glideModel_ = model_;
        const auto glideTo = [snap, glideLen] (Glide& g, const float target) {
            if (snap)
                g.snap (target);
            else
                g.moveTo (target, glideLen);
        };
        glideTo (gInput_, inputGain_);
        glideTo (gPre_, preGain_);
        glideTo (gBass_, bassGain_ - 1.0f);
        glideTo (gMid_, midK_ * (midGain_ - 1.0f));
        glideTo (gTreble_, trebleGain_ - 1.0f);
        glideTo (gPresence_, presenceGain_ - 1.0f);
        glideTo (gCabLf_, cabLfK_ * (cabLfGain_ - 1.0f));
        glideTo (gCabPres_, cabPresK_ * (cabPresGain_ - 1.0f));
        glideTo (gCabNotch_, cabNotchK_ * (cabNotchGain_ - 1.0f));
        glideTo (gMicBell_, micBellK_ * (micBellGain_ - 1.0f));
        glideTo (gMicShelf_, micShelfGain_ - 1.0f);
        glideTo (gWet_, wetOut_);
        glideTo (gDry_, dryGain_);
        glideTo (gCabMix_, cab_ == Cabinet::Direct ? 0.0f : 1.0f); // [D-M-0513]
        // [D-M-0305] the pots, which the network re-resolves from per base sample while they move.
        glideTo (gNetBass_, static_cast<float> (bassPot (bassPercent_)));
        glideTo (gNetMid_, static_cast<float> (linearPot (midPercent_)));
        glideTo (gNetTreble_, static_cast<float> (linearPot (treblePercent_)));
        resolveNetwork (gNetBass_.cur, gNetMid_.cur, gNetTreble_.cur);
        gliding_ = cornerLeft_ > 0 || gNetBass_.left > 0 || gNetMid_.left > 0 || gNetTreble_.left > 0;
        for (const Glide* g : glides())
            gliding_ = gliding_ || g->left > 0;

        // The latency follows the ACTIVE model's oversampler, and this is where a model change lands.
        updateDryAlignment();
    }

    void GampEffect::updateDryAlignment() noexcept
    {
        // ONLY THE READ DISTANCE MOVES — the line is written every sample, so a length change reads
        // from further back and never clears (FLTR's `updateDryAlignment` records why a clear is the
        // worse discontinuity). Asserted, not trusted: a clamp that binds would misalign silently.
        assert (latencySamples() < kDryDelayCapacity);
        dryLen_.store (std::min (latencySamples(), kDryDelayCapacity - 1), std::memory_order_relaxed);
    }

    void GampEffect::setOutputStage (const bool transformer, const bool crossover) noexcept
    {
        xfmrOn_ = transformer;
        xoverOn_ = crossover;
        refresh();
    }

    void GampEffect::setModel (const int index) noexcept
    {
        const int clamped = index < 0 ? 0 : (index >= kNumModels ? kNumModels - 1 : index);
        model_ = static_cast<Model> (clamped);
        // Back through `refresh` like every other row: the whole voicing changes, including the
        // OS rate the stage corners are resolved against.
        refresh();
    }

    void GampEffect::setGainDb (const float db) noexcept
    {
        gainDb_ = static_cast<Real> (db);
        refresh();
    }

    void GampEffect::setBassPercent (const float percent) noexcept
    {
        bassPercent_ = static_cast<Real> (percent);
        refresh();
    }

    void GampEffect::setMidPercent (const float percent) noexcept
    {
        midPercent_ = static_cast<Real> (percent);
        refresh();
    }

    void GampEffect::setTreblePercent (const float percent) noexcept
    {
        treblePercent_ = static_cast<Real> (percent);
        refresh();
    }

    void GampEffect::setPresencePercent (const float percent) noexcept
    {
        presencePercent_ = static_cast<Real> (percent);
        refresh();
    }

    void GampEffect::setCab (const int index) noexcept
    {
        const int clamped = index < 0 ? 0 : (index >= kNumCabs ? kNumCabs - 1 : index);
        cab_ = static_cast<Cabinet> (clamped);
        refresh();
    }

    void GampEffect::setMic (const int index) noexcept
    {
        const int clamped = index < 0 ? 0 : (index >= kNumMics ? kNumMics - 1 : index);
        mic_ = static_cast<Microphone> (clamped);
        refresh();
    }

    void GampEffect::setLevelDb (const float db) noexcept
    {
        levelDb_ = static_cast<Real> (db);
        refresh();
    }

    void GampEffect::setMixPercent (const float percent) noexcept
    {
        mixPercent_ = static_cast<Real> (percent);
        refresh();
    }

    void GampEffect::setAppliedUnit (const std::string_view field, const Real value) noexcept
    {
        const auto f = static_cast<float> (value);
        if (field == "model")
            setModel (static_cast<int> (value)); // ENUM: the plain ordinal (T-CORE-06)
        else if (field == "gain")
            setGainDb (f);
        else if (field == "level")
            setLevelDb (f);
        else if (field == "mix")
            setMixPercent (f);
        else if (field == "bass")
            setBassPercent (f);
        else if (field == "mid")
            setMidPercent (f);
        else if (field == "treble")
            setTreblePercent (f);
        else if (field == "presence")
            setPresencePercent (f);
        else if (field == "cab")
            setCab (static_cast<int> (value)); // ENUM: the plain ordinal (T-CORE-06)
        else if (field == "mic")
            setMic (static_cast<int> (value)); // ENUM: likewise
        // Anything else — which is now only `on`, the CHAIN's bypass — is IGNORED.
    }

    std::optional<Real> GampEffect::appliedUnit (const std::string_view field) const noexcept
    {
        if (field == "model")
            return static_cast<Real> (model_);
        if (field == "gain")
            return gainDb_;
        if (field == "level")
            return levelDb_;
        if (field == "mix")
            return mixPercent_;
        if (field == "bass")
            return bassPercent_;
        if (field == "mid")
            return midPercent_;
        if (field == "treble")
            return treblePercent_;
        if (field == "presence")
            return presencePercent_;
        if (field == "cab")
            return static_cast<Real> (cab_);
        if (field == "mic")
            return static_cast<Real> (mic_);
        return std::nullopt;
    }
}
