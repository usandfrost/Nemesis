// Host tests for firmware/src/cost/*: the cost model, the one-heavy-filter rule, the degrade ladder and the load governor.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "cost/CostModel.h"
#include "cost/Degrade.h"
#include "cost/Governor.h"

using namespace cost;

static int gFail = 0, gChecks = 0;
#define CHECK(cond)                                                                                   \
    do {                                                                                              \
        ++gChecks;                                                                                    \
        if (!(cond)) { ++gFail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }       \
    } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs((double)(a) - (double)(b)) <= (tol))

static uint32_t rngState = 12345;
static uint32_t rnd() { rngState = rngState * 1664525u + 1013904223u; return rngState >> 8; }
static float rnd01() { return (rnd() & 0xFFFF) / 65535.0f; }

static VoiceDesc randomVoice() {
    VoiceDesc d;
    d.slot[0].kind = static_cast<Osc>(rnd() % 5); d.slot[1].kind = static_cast<Osc>(rnd() % 5);
    d.slot[0].audible = true; d.slot[1].audible = rnd() % 2;
    d.slot[0].unison = 1 + rnd() % 8; d.slot[1].unison = rnd() % 3 ? 1 : 1 + rnd() % 4;
    d.f1 = static_cast<Filter>(rnd() % static_cast<int>(Filter::Count)); d.f2 = static_cast<Filter>(rnd() % static_cast<int>(Filter::Count));
    d.f2On = rnd() % 2; d.route = static_cast<Route>(rnd() % 3);
    d.drive = rnd() % 2 ? rnd01() : 0; d.fm = rnd() % 3 == 0 ? rnd01() : 0; d.ring = rnd() % 3 == 0 ? rnd01() : 0; d.sync = rnd() % 4 == 0 ? 1 : 0;
    d.width = rnd() % 2 ? rnd01() : 0; d.companding = rnd() % 3 == 0 ? rnd01() : 0; d.fold = rnd() % 3 == 0 ? rnd01() : 0;
    return d;
}

#if !DBOY_COST_LEAN
static void testTable() {
    Settings fw; fw.skipSilentSlots = true;
    NEAR(voiceCost(VoiceDesc(), fw), 1.0, 1e-6);                              // the plain voice (one audible slot, the silent one skipped) defines the unit
    NEAR(voiceCost(VoiceDesc(), Settings()), 1.0f + table::kSlotCost, 1e-6);  // with the skip off both slots are computed
    int heavy = 0;
    for (int i = 0; i < static_cast<int>(Filter::Count); ++i) {
        const Filter f = static_cast<Filter>(i);
        const float e = table::kFilterExtra[i];
        CHECK(e >= 0.0f);
        CHECK(isHeavy(f) == (extraAtX4(f) >= table::kHeavyThreshold));
        if (isHeavy(f)) { ++heavy; CHECK(!isHeavy(lightFallback(f))); CHECK(lightFallback(f) != f); }
        else { CHECK(lightFallback(f) == f); CHECK(oversampledCore(f) || e == 0.0f); }   // a plain light model is exactly free
    }
    // which models are heavy is MEASURED (the table); the two biggest cores must be, the plain ones never
    CHECK(heavy >= 1 && isHeavy(Filter::ClassicLp24) && isHeavy(Filter::ClassicHp));
    CHECK(!isHeavy(Filter::Ladder24) && !isHeavy(Filter::StateNotch) && !isHeavy(Filter::Formant) && !isHeavy(Filter::Comb) && !isHeavy(Filter::LowPass)
          && !isHeavy(Filter::StateMorph));
    CHECK(lightFallback(Filter::ClassicHp) == Filter::HighPass && lightFallback(Filter::ClassicBp) == Filter::BandPass);
    // every model has a light twin with the same response, so the fallback never depends on which side of the threshold a model lands
    for (int i = 0; i < static_cast<int>(Filter::Count); ++i) {
        const Filter f = static_cast<Filter>(i), t = lightTwin(f);
        CHECK(!isHeavy(lightFallback(f)));                                    // never a heavy model, whatever the table says
        if (t != f) CHECK(!isHeavy(t));
    }
    CHECK(lightTwin(Filter::ReedLp) == Filter::StateLp && lightTwin(Filter::ReedMorph) == Filter::StateMorph && lightTwin(Filter::SovietBp) == Filter::BandPass);
    CHECK(static_cast<int>(Filter::Count) == 28);
}

static void testCost() {
    Settings s4, s2; s2.filterOversample = 2;
    s4.skipSilentSlots = s2.skipSilentSlots = true;                            // the firmware configuration
    VoiceDesc d;
    const float plain = voiceCost(d, s4);
    d.f1 = Filter::ClassicLp24;
    const float c4 = voiceCost(d, s4), c2 = voiceCost(d, s2);
    CHECK(c4 > plain + 0.3f);
    NEAR(c2 - plain, (c4 - plain) / 2.0f, 1e-4);                            // the heavy extra scales with the oversampling factor
    VoiceDesc l = VoiceDesc(); l.f1 = Filter::Ladder24; NEAR(voiceCost(l, s2), 1.0, 1e-6);   // light filters are free at any factor
    VoiceDesc hd = VoiceDesc(); hd.f1 = Filter::ClassicLp24; VoiceDesc hdd = hd; hdd.drive = 0.6f;
    NEAR(voiceCost(hdd, s4), voiceCost(hd, s4), 1e-6);                         // drive adds nothing on a heavy core (it has its own)
    const float driveAt = [](int os) { return table::kDrive * static_cast<float>(os) / static_cast<float>(table::kTableOversample); }(4);
    VoiceDesc ld = VoiceDesc(); ld.drive = 0.6f; NEAR(voiceCost(ld, s4) - 1.0f, driveAt, 1e-5);   // but costs on a light one
    Settings d2 = s4; d2.driveOversample = 2; NEAR(voiceCost(ld, d2) - 1.0f, driveAt / 2.0f, 1e-5);  // ...scaled with its own factor

    // a silent slot that is skipped costs nothing, whatever oscillator kind it is set to (it is not computed)
    for (int k = 0; k < 5; ++k) {
        VoiceDesc sk = VoiceDesc(); sk.slot[1].kind = static_cast<Osc>(k); sk.slot[1].macroHeavy = true;
        NEAR(voiceCost(sk, s4), 1.0f, 1e-6);
        VoiceDesc au = sk; au.slot[1].audible = true;                          // audible, it is computed: its kind counts
        NEAR(voiceCost(au, s4), 1.0f + table::kSlotCost + oscKindDelta(au.slot[1]), 1e-5);
    }

    // monotonic: switching a costly feature on never makes a voice cheaper
    for (int i = 0; i < 400; ++i) {
        VoiceDesc a = randomVoice(), b = a;
        b.drive = 0.5f; CHECK(voiceCost(b, s4) >= voiceCost(a, s4) - 1e-5f);
        // ring, FM and sync are the exception that proves a measured rule: they collapse slot 0's unison stack, so with a stack present they can
        // make a voice CHEAPER. Without one they never do.
        VoiceDesc a1 = a; a1.slot[0].unison = 1; a1.width = 0.0f;
        VoiceDesc r = a1; r.ring = 0.5f; CHECK(voiceCost(r, s4) >= voiceCost(a1, s4) - 1e-5f);
        VoiceDesc f = a1; f.fm = 0.5f; CHECK(voiceCost(f, s4) >= voiceCost(a1, s4) - 1e-5f);
        VoiceDesc u = a; u.slot[0].unison = 8; CHECK(voiceCost(u, s4) >= voiceCost(a, s4) - 1e-5f);
        CHECK(voiceCost(a, s4) >= 0.3f);
        CHECK(voiceCost(a, s2) <= voiceCost(a, s4) + 1e-5f);                // fewer oversampling steps never cost more
    }

    // a silent slot is free only when nothing couples it, and only with the skip enabled
    Settings skip = s4; skip.skipSilentSlots = true;
    VoiceDesc v;                                                              // slot 0 audible, slot 1 silent
    Settings noSkip = s4; noSkip.skipSilentSlots = false;
    NEAR(voiceCost(v, skip), 1.0f, 1e-6);
    NEAR(voiceCost(v, noSkip), 1.0f + table::kSlotCost, 1e-5);               // without the skip a silent slot is still computed
    VoiceDesc ring = v; ring.ring = 0.5f;
    NEAR(voiceCost(ring, skip), 1.0f + table::kSlotCost + table::kRing, 1e-5);   // coupled: the silent slot is computed, so it counts
    VoiceDesc uni = v; uni.slot[1].unison = 2;
    NEAR(voiceCost(uni, skip), 1.0f + table::kSlotCost + kUnisonSlot1Factor * table::kUnison2, 1e-5); // a silent slot with a unison stack is not skipped either (and slot 1 stacks cost double)
    VoiceDesc both = v; both.slot[1].audible = true;
    NEAR(voiceCost(both, skip), voiceCost(v, noSkip), 1e-6);                  // two audible slots cost what two computed slots cost

    // width engages the right chain: a fixed part plus one more copy of the filter's own extra, and only when a unison stack exists
    VoiceDesc w = v; w.slot[0].unison = 4; w.width = 0.8f;
    VoiceDesc nw = w; nw.width = 0.0f;
    NEAR(voiceCost(w, s4) - voiceCost(nw, s4), table::kWidthFixed, 1e-5);
    w.f1 = nw.f1 = Filter::ClassicLp24;
    NEAR(voiceCost(w, s4) - voiceCost(nw, s4), table::kWidthFixed + filterExtra(Filter::ClassicLp24, s4), 1e-5);
    VoiceDesc co = w; co.ring = 0.5f; VoiceDesc con = co; con.width = 0.0f; con.slot[0].unison = 1;
    NEAR(voiceCost(co, s4), voiceCost(con, s4), 1e-6);                          // ring collapses the stack: unison and width cost nothing while it is on
    VoiceDesc fmv = co; fmv.ring = 0.0f; fmv.fm = 0.5f; VoiceDesc fmn = fmv; fmn.slot[0].unison = 1; fmn.width = 0.0f; NEAR(voiceCost(fmv, s4), voiceCost(fmn, s4), 1e-6);
    VoiceDesc sy = co; sy.ring = 0.0f; sy.sync = 1.0f; VoiceDesc syn = sy; syn.slot[0].unison = 1; syn.width = 0.0f; NEAR(voiceCost(sy, s4), voiceCost(syn, s4), 1e-6);
    VoiceDesc s1u = co; s1u.slot[1].unison = 4;                                // but a stack on slot 1 is not collapsed by that
    CHECK(voiceCost(s1u, s4) > voiceCost(co, s4) + 0.1f);
    VoiceDesc a1 = VoiceDesc(); a1.slot[1].audible = true; a1.slot[1].unison = 4; VoiceDesc a0 = VoiceDesc(); a0.slot[1].audible = true; a0.slot[0].unison = 4;
    NEAR(voiceCost(a1, s4) - voiceCost(VoiceDesc(), s4), (voiceCost(a0, s4) - voiceCost(VoiceDesc(), s4) - table::kSlotCost) * kUnisonSlot1Factor + table::kSlotCost, 1e-4);   // slot 1 unison: x the measured factor
    VoiceDesc nu = w; nu.slot[0].unison = 1; NEAR(voiceCost(nu, s4) - voiceCost(w, s4), -unisonExtra(4) - table::kWidthFixed - filterExtra(Filter::ClassicLp24, s4), 1e-5);
}
#endif

#if !DBOY_COST_LEAN
static void testHeavyRule() {
    VoiceDesc d; d.f1 = Filter::ClassicLp24; d.f2On = true; d.f2 = Filter::LowPass;
    CHECK(heavyRuleOk(d) && heavyCount(d) == 1);
    CHECK(setFilter(d, FilterSlot::F2, Filter::Acid) == true);                // the new choice wins, F1 is displaced
    CHECK(d.f2 == Filter::Acid && d.f1 == Filter::Ladder24 && heavyRuleOk(d));
    CHECK(setFilter(d, FilterSlot::F1, Filter::ClassicHp) == true);           // and the other way round
    CHECK(d.f1 == Filter::ClassicHp && d.f2 == lightFallback(Filter::Acid) && heavyRuleOk(d));
    CHECK(setFilter(d, FilterSlot::F1, Filter::Ladder12) == false && d.f1 == Filter::Ladder12);   // a light choice displaces nothing

    VoiceDesc e; e.f1 = Filter::Acid; e.f2 = Filter::ClassicLp24; e.f2On = false;
    CHECK(heavyRuleOk(e));                                                    // F2 is off: only one filter runs, so two heavy settings are fine
    CHECK(setFilter2On(e, true) == true && e.f2 == Filter::Ladder24 && e.f1 == Filter::Acid);   // turning F2 on demotes F2, the newcomer
    VoiceDesc f; f.f1 = Filter::LowPass; f.f2 = Filter::Acid; f.f2On = true;
    CHECK(setFilter2On(f, false) == false && heavyRuleOk(f));

    // p-locks: a locked model that would make a second heavy filter becomes its light fallback; the lane's base is never touched
    VoiceDesc b; b.f1 = Filter::ClassicLp24; b.f2On = true; b.f2 = Filter::LowPass;
    CHECK(allowedLock(b, FilterSlot::F2, Filter::Acid) == Filter::Ladder24);
    CHECK(allowedLock(b, FilterSlot::F1, Filter::Acid) == Filter::Acid);      // replacing the heavy one with another heavy one is fine
    CHECK(allowedLock(b, FilterSlot::F2, Filter::Comb) == Filter::Comb);
    CHECK(b.f1 == Filter::ClassicLp24 && b.f2 == Filter::LowPass);
    VoiceDesc nb = b; nb.f2On = false;
    CHECK(allowedLock(nb, FilterSlot::F2, Filter::Acid) == Filter::Acid);     // F2 is not running, so no conflict yet

    // property: any sequence of edits leaves the rule satisfied
    VoiceDesc p;
    for (int i = 0; i < 20000; ++i) {
        switch (rnd() % 3) {
            case 0: setFilter(p, FilterSlot::F1, static_cast<Filter>(rnd() % static_cast<int>(Filter::Count))); break;
            case 1: setFilter(p, FilterSlot::F2, static_cast<Filter>(rnd() % static_cast<int>(Filter::Count))); break;
            default: setFilter2On(p, rnd() % 2); break;
        }
        if (!heavyRuleOk(p)) { CHECK(false); break; }
    }
    CHECK(true);
}
#else
// The lean table: the sum renderVoice runs, in the units of a plain voice. The numbers are the board's (tools/fit_lean_cost.py), so this checks the shape, not them.
static void testLeanTable() {
    using namespace lean_table;
    const Settings fw;
    NEAR(voiceCost(VoiceDesc(), fw), 1.0, 1e-6);
    const float opts[] = {kOsc2, kBank, kPd, kUnison, kWidth, kSub, kNoise, kLadder, kAcid, kSoviet, kClassic, kDrive, kHpf, kF2, kRing, kSync, kGrit, kClip};
    for (float o : opts) CHECK(o >= 0.0f);                                     // nothing gets cheaper for being switched on
    CHECK(kPlainCycles > 0 && kVoiceBudget >= 15);                             // the board holds the 3 x 5 voices the panel allows
    CHECK(kSamplerVoice > 0.0f && kCostReverb > 0.0f && kCostAmp[0] == 0.0f);
    VoiceDesc o2; o2.slot[1].audible = true; NEAR(voiceCost(o2, fw), 1.0f + kOsc2, 1e-5);
    VoiceDesc bank; bank.slot[0].kind = Osc::Wavetable; NEAR(voiceCost(bank, fw), 1.0f + kBank, 1e-5);
    VoiceDesc uni; uni.slot[0].unison = 4; NEAR(voiceCost(uni, fw), 1.0f + 3.0f * kUnison, 1e-5);
    VoiceDesc wide = uni; wide.width = 1.0f; NEAR(voiceCost(wide, fw), 1.0f + 3.0f * kUnison + kWidth, 1e-5);
    VoiceDesc flat; flat.width = 1.0f; NEAR(voiceCost(flat, fw), 1.0f, 1e-6);  // width needs a stack
    VoiceDesc ring = wide; ring.slot[1].audible = true; ring.ring = 1.0f;      // a live ring collapses the stack, and its width with it
    NEAR(voiceCost(ring, fw), 1.0f + kOsc2 + kRing, 1e-5);
    VoiceDesc dead = uni; dead.ring = 1.0f; NEAR(voiceCost(dead, fw), voiceCost(uni, fw), 1e-6);   // no second oscillator: no ring
    VoiceDesc sync = uni; sync.sync = 1.0f; NEAR(voiceCost(sync, fw), 1.0f + kSync, 1e-5);
    VoiceDesc f2; f2.f2On = true; f2.f2 = Filter::Ladder24; NEAR(voiceCost(f2, fw), 1.0f + kF2 + kLadder, 1e-5);
    VoiceDesc f2Off = f2; f2Off.f2On = false; NEAR(voiceCost(f2Off, fw), 1.0f, 1e-6);
    VoiceDesc lamps; lamps.pd1 = lamps.sub = lamps.noise = lamps.hpf = lamps.clip = true;
    NEAR(voiceCost(lamps, fw), 1.0f + kPd + kSub + kNoise + kHpf + kClip, 1e-5);
    // the conditions of renderVoice: no osc 1, no stack and no ring or sync; PD on osc 1 turns sync off and leaves the stack running, per member
    VoiceDesc dead1 = uni; dead1.slot[0].audible = false; dead1.width = 1.0f; dead1.sync = 1.0f; NEAR(voiceCost(dead1, fw), 1.0f, 1e-6);
    VoiceDesc pdSync = uni; pdSync.pd1 = true; pdSync.sync = 1.0f; NEAR(voiceCost(pdSync, fw), 1.0f + 3.0f * kUnison + 4.0f * kPd, 1e-5);
    // a stereo voice pays for the chain twice
    VoiceDesc wideF = wide; wideF.f1 = Filter::ClassicLp24; wideF.hpf = true;
    NEAR(voiceCost(wideF, fw), 1.0f + 3.0f * kUnison + kWidth + 2.0f * (kClassic + kHpf), 1e-5);
    // the ladder: the width goes before the unison, and ring (free) stays when the sync goes
    {
        PartDesc p; p.voices = 1; p.voice = wide;
        Step l[kMaxSteps]; const int n = buildLadder(&p, 1, fw, l, kMaxSteps);
        CHECK(n == 2 && l[0].action == Action::WidthOff && l[1].action == Action::UnisonOff);
        PartDesc q; q.voice.slot[1].audible = true; q.voice.ring = 1.0f; q.voice.sync = 1.0f;
        CHECK(apply(q, Action::XmodOff) && q.voice.sync == 0.0f && q.voice.ring > 0.0f);
    }
    // no model is heavy, so the one-heavy-filter rule never fires and nothing has a fallback
    for (int i = 0; i < static_cast<int>(Filter::Count); ++i) { CHECK(!isHeavy(static_cast<Filter>(i))); CHECK(lightFallback(static_cast<Filter>(i)) == static_cast<Filter>(i)); }
    VoiceDesc two; two.f1 = Filter::ClassicLp24; two.f2 = Filter::Acid; two.f2On = true; CHECK(heavyRuleOk(two));
    // monotonic, without the two that collapse a stack
    for (int i = 0; i < 400; ++i) {
        VoiceDesc a = randomVoice(), b = a;
        b.drive = 0.5f; CHECK(voiceCost(b, fw) >= voiceCost(a, fw) - 1e-5f);
        CHECK(voiceCost(a, fw) >= 1.0f - 1e-5f);
    }
}
#endif

static std::vector<PartDesc> randomParts(int n) {
    std::vector<PartDesc> v;
    for (int i = 0; i < n; ++i) {
        PartDesc p; p.voice = randomVoice(); p.voices = 1 + rnd() % 5; p.priority = rnd() % 3; p.sampler = rnd() % 4 == 0;
        setFilter2On(p.voice, p.voice.f2On);
        v.push_back(p);
    }
    return v;
}

static void testLadder() {
    Settings s2; s2.filterOversample = 2; s2.skipSilentSlots = true;
    for (int trial = 0; trial < 600; ++trial) {
        auto parts = randomParts(1 + rnd() % 4);
        Step lad[kMaxSteps];
        const int n = static_cast<int>(parts.size());
        const int steps = buildLadder(parts.data(), n, s2, lad, kMaxSteps);
        float cost = totalCost(parts.data(), n, s2);
        const float start = cost;
        for (int i = 0; i < steps; ++i) {
            CHECK(lad[i].saved > 0.0f);                                         // every step buys something
            // the recorded saving equals the real change in total cost
            std::vector<PartDesc> deg(parts);
            for (int p = 0; p < n; ++p) deg[p] = degraded(parts[p], p, lad, steps, i + 1);
            const float after = totalCost(deg.data(), n, s2);
            CHECK(after < cost + 1e-4f);
            NEAR(cost - after, lad[i].saved, 1e-3);
            cost = after;
        }
        // fully degraded: no costly option and one voice per part
        std::vector<PartDesc> all(parts);
        for (int p = 0; p < n; ++p) {
            all[p] = degraded(parts[p], p, lad, steps, steps);
            CHECK(all[p].voices == 1);
            // nothing that still costs anything is left to bypass (an option that is free, e.g. width with no unison stack, may remain set)
            for (int a = 1; a < static_cast<int>(Action::Count); ++a) {
                PartDesc t = all[p];
                if (apply(t, static_cast<Action>(a))) CHECK(partCost(all[p], s2) - partCost(t, s2) <= 0.001f);
            }
            if (!all[p].sampler) CHECK(heavyCount(all[p].voice) == 0);
        }
        CHECK(totalCost(all.data(), n, s2) <= start + 1e-4f);
        for (int p = 0; p < n; ++p) { const PartDesc d0 = degraded(parts[p], p, lad, steps, 0); CHECK(d0.voices == parts[p].voices && voiceCost(d0.voice, s2) == voiceCost(parts[p].voice, s2)); }
    }
}

static void testLadderOrder() {
    Settings s2; s2.filterOversample = 2;
    PartDesc a, b, c;                                                           // a: lowest priority, c: highest
    a.priority = 0; b.priority = 1; c.priority = 2;
    a.voices = b.voices = c.voices = 3;
    for (PartDesc* p : {&a, &b, &c}) { p->voice.drive = 0.5f; p->voice.ring = 0.5f; p->voice.f1 = Filter::ClassicLp24; }
    PartDesc parts[3] = {c, a, b};                                              // deliberately not in priority order: index 1 is the lowest
    Step lad[kMaxSteps];
    const int n = buildLadder(parts, 3, s2, lad, kMaxSteps);
    CHECK(n > 3);
    CHECK(lad[0].action == Action::StealVoice && lad[0].part == 1);            // the first step releases a voice on the lowest-priority part
    int lastPartRank = -1;                                                      // then the options go part by part, lowest priority first
    auto rank = [&](int part) { return parts[part].priority; };
    for (int i = 1; i < n && lad[i].action != Action::StealVoice; ++i) { CHECK(rank(lad[i].part) >= lastPartRank); lastPartRank = rank(lad[i].part); }
    CHECK(lad[1].part == 1);
    // within a part the biggest saving comes first: whatever bypass saves the most on part 1 (after its released voice) is step 1
    {
        PartDesc p1 = parts[1]; apply(p1, Action::StealVoice);
        Action best = Action::Count; float bestSaved = 0.0f;
        for (int a = 1; a < static_cast<int>(Action::Count); ++a) {
            PartDesc t = p1;
            if (!apply(t, static_cast<Action>(a))) continue;
            const float sv = partCost(p1, s2) - partCost(t, s2);
            if (sv > bestSaved) { bestSaved = sv; best = static_cast<Action>(a); }
        }
        CHECK(lad[1].action == best);
        NEAR(lad[1].saved, bestSaved, 1e-6);
    }
    // the last steps are voice releases only
    int tail = n - 1; while (tail >= 0 && lad[tail].action == Action::StealVoice) --tail;
    for (int i = tail + 1; i < n; ++i) CHECK(lad[i].action == Action::StealVoice);

    PartDesc t1, t2; t1.voices = t2.voices = 2; t1.priority = t2.priority = 1; t1.voice.drive = t2.voice.drive = 0.5f;
    PartDesc tie[2] = {t1, t2}; const int tn = buildLadder(tie, 2, s2, lad, kMaxSteps);
    CHECK(tn > 0 && lad[0].part == 1);                                          // equal priority: the later part degrades first

    PartDesc smp; smp.sampler = true; smp.voices = 4; smp.priority = 0;
    PartDesc syn; syn.voices = 2; syn.priority = 5; syn.voice.drive = 0.5f;
    PartDesc two[2] = {smp, syn};
    const int sn = buildLadder(two, 2, s2, lad, kMaxSteps);
    for (int i = 0; i < sn; ++i) if (lad[i].part == 0) CHECK(lad[i].action == Action::StealVoice);   // a sampler part only loses voices
    CHECK(lad[0].part == 0 && lad[0].action == Action::StealVoice);

    PartDesc one; one.voices = 1; PartDesc none[1] = {one};
    CHECK(buildLadder(none, 1, s2, lad, kMaxSteps) == 0);                      // nothing to give up: an empty ladder
}

static float simulate(Governor& g, const std::vector<float>& costAtLevel, float unitLoad, int blocks, int& changes, float noise = 0.03f) {
    int last = g.level(); changes = 0; float load = 0;
    for (int i = 0; i < blocks; ++i) {
        const int lv = g.level();
        load = costAtLevel[std::min(lv, static_cast<int>(costAtLevel.size()) - 1)] * unitLoad * (1.0f + noise * (rnd01() * 2 - 1));
        g.update(load, static_cast<int>(costAtLevel.size()) - 1);
        if (g.level() != last) { ++changes; last = g.level(); }
    }
    return load;
}

static void testGovernor() {
    // a plant whose cost falls as the ladder is climbed: 8.0 units, each step saves a bit
    std::vector<float> cost = {8.0f, 7.5f, 7.0f, 6.5f, 6.0f, 5.5f, 5.0f, 4.5f, 4.0f, 3.5f, 3.0f};
    {
        Governor g; int ch = 0;
        simulate(g, cost, 0.14f, 20000, ch);                                    // 8.0 x 0.14 = 1.12: far over the limit
        CHECK(g.level() > 0);
        CHECK(cost[g.level()] * 0.14f * 1.03f <= 0.97f);                         // settles where even a +3% block does not panic
        int ch2 = 0; simulate(g, cost, 0.14f, 20000, ch2);
        CHECK(ch2 == 0);                                                         // and then it STAYS there: no flapping
        // the load drops (notes released): it climbs back down to level 0, slowly
        std::vector<float> light = {2.0f, 1.9f, 1.8f};
        int ch3 = 0; Governor g2; g2.update(1.0f, 10);
        for (int i = 0; i < 10; ++i) g.update(0.9f, 10);
        simulate(g, light, 0.14f, 60000, ch3);
        CHECK(g.level() == 0);
    }
    {
        Governor g;                                                              // one panic block climbs one step at once
        for (int i = 0; i < 100; ++i) g.update(0.4f, 5);
        CHECK(g.level() == 0);
        g.update(1.2f, 5); CHECK(g.level() == 1);
        const int hold = GovernorConfig().panicHoldBlocks;
        for (int i = 0; i < hold; ++i) g.update(1.2f, 5);                        // not again until the new level can show in the load
        CHECK(g.level() == 1);
        g.update(1.2f, 5); CHECK(g.level() == 2);                                 // still overrunning: the next step, inside the settle window
    }
    {
        Governor g;                                                              // after a DESCEND an overrun climbs back at once
        for (int i = 0; i < 200; ++i) g.update(1.2f, 5);
        const int lv = g.level(); CHECK(lv > 0);
        int i = 0;
        while (g.level() == lv && i++ < 100000) g.update(0.2f, 5);
        CHECK(g.level() == lv - 1);
        g.update(1.2f, 5); CHECK(g.level() == lv);
    }
    {
        Governor g;                                                              // holdFor: no judging while a released voice fades out
        g.holdFor(300);
        for (int i = 0; i < 300 + GovernorConfig().enterBlocks - 5; ++i) g.update(0.9f, 5);
        CHECK(g.level() == 0);
        for (int i = 0; i < 400; ++i) g.update(0.9f, 5);
        CHECK(g.level() >= 1);
        Governor p; p.holdFor(300); p.update(1.2f, 5); CHECK(p.level() == 1);   // but an overrun still acts
    }
    {
        Governor g;                                                              // sustained 0.9 climbs after enterBlocks; 0.8 never does
        for (int i = 0; i < 5000; ++i) g.update(0.8f, 5);
        CHECK(g.level() == 0);
        for (int i = 0; i < 5000; ++i) g.update(0.9f, 5);
        CHECK(g.level() >= 1);
    }
    {
        Governor g;                                                              // the dead band: between exit and enter nothing moves
        for (int i = 0; i < 200; ++i) g.update(1.2f, 5);
        const int lv = g.level(); CHECK(lv > 0);
        for (int i = 0; i < 30000; ++i) g.update(0.72f, 5);
        CHECK(g.level() == lv);
        for (int i = 0; i < 30000; ++i) g.update(0.3f, 5);                       // clearly idle: restores, one step per exitBlocks
        CHECK(g.level() == 0);
    }
    {
        Governor g;                                                              // a cap of 0 means never climb; a shrinking cap clamps
        for (int i = 0; i < 1000; ++i) g.update(1.5f, 0);
        CHECK(g.level() == 0);
        for (int i = 0; i < 1000; ++i) g.update(1.5f, 6);
        CHECK(g.level() == 6);
        g.update(1.5f, 2); CHECK(g.level() == 2);
    }
    {
        Governor g; int changes = 0, last = 0;                                   // a load that swings across the band every 100 blocks must not thrash
        for (int i = 0; i < 40000; ++i) {
            const float load = ((i / 100) % 2) ? 0.9f : 0.5f;
            g.update(load, 8);
            if (g.level() != last) { ++changes; last = g.level(); }
        }
        CHECK(changes < 2 * 8 + 4);
    }
}

int main() {
#if DBOY_COST_LEAN
    testLeanTable();
#else
    testTable();
    testCost();
    testHeavyRule();
#endif
    testLadder();
    testLadderOrder();
    testGovernor();
    std::printf("%d checks, %d failed\n", gChecks, gFail);
    return gFail ? 1 : 0;
}
