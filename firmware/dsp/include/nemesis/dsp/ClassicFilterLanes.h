#pragma once

#include <nemesis/dsp/ClassicFilter.h>
#include <nemesis/dsp/OversamplerLanes.h>

#include <array>
#include <cstddef>
#include <cstdint>

// [T-CMP-37 L1] LANE-WISE (ACROSS-VOICES) CLASSIC FILTER. [L2] Wired into the CATHODE engine for F1 (CathodeVoiceEngine.h): a voice
// is lane-resident while it is batch-eligible and moves in and out through `importLane` / `exportLane`.
//
// N `ClassicFilter`s advanced in lock step as float64x2 lanes, BIT-IDENTICAL to N scalar objects: `ClassicFilter::processSample`
// -> `Oversampler::processSample` -> `coreStep`, each lane operation for operation (see OversamplerLanes.h for the
// contract). Per-lane cutoff / resonance / drive / loop-drive / type. The scalar header is not touched.
//
// WHERE THE SCALAR FUSES (-ffp-contract=on: ONE fused multiply-add per `a*b +- c` expression whose LEFT operand is a product,
// else whose RIGHT is) — read off the source, then pinned by the bit-exact test, which would red on any other choice:
//   a_i  = oneMinusG*s_i - g*psi_i                 = fma (oneMinusG, s_i, -(g*psi_i))
//   S    = g3*a1 + g2*a2 + g*a3 + a4               = fma (g, a3, fma (g3, a1, g2*a2)) + a4
//   node = (compGain*in - k*S) * gComp             = fma (compGain, in, -(k*S)) * gComp
//   blend= driven + mix*(bounded - driven)         = fma (mix, bounded - driven, driven)
// Everything else (the one-pole updates, c_i - psi_i, the BP/HP mixes, `2.0*x` which is exact) is a plain operation.
// shape (x) = x - x*x*x/3.0 keeps its divide (a reciprocal multiply would round differently).
//
// Coefficients come from a scratch scalar ClassicFilter (its tan / sqrt / drive law are not re-implemented) plus the
// handful of derived products the scalar's private members hold, written with the scalar's own expressions.
namespace nemesis::dsp::lanes
{
    template <class B = DefaultBackend, int N = 4>
    class ClassicFilterLanes
    {
    public:
        static constexpr int kLanes = N;
        static constexpr std::size_t kNV = N / 2;
        using V = typename B::V;
        using Type = ClassicFilter::Type;

        // Designs the oversampler FIRs and clears every lane; every lane starts at cutoff 1 kHz, reso 0, drive 0, LP24
        // (a lane's setters run before it is compared with a scalar object). Allocates.
        void prepare (double sampleRate, int oversampleFactor = ClassicFilter::kDefaultOversample)
        {
            scratch_.prepare (sampleRate, oversampleFactor);
            os_.prepare (oversampleFactor);
            for (int l = 0; l < N; ++l)
            {
                setLaneCutoff (l, 1000.0f, 0.0f);
                setLaneDrive (l, 0.0f);
                setLaneType (l, Type::Lp24);
            }
            reset();
        }

        void reset() noexcept
        {
            for (int l = 0; l < N; ++l)
                clearLaneState (l);
            os_.reset();
        }
        void resetLane (int lane) noexcept
        {
            clearLaneState (lane);
            os_.resetLane (lane);
        }

        void setLaneCutoff (int lane, float cutoffHz, float resonance) noexcept
        {
            genKnown_[static_cast<std::size_t> (lane)] = false; // the arrays no longer mirror any scalar's generation
            scratch_.setCutoff (cutoffHz, resonance);
            const double g = scratch_.coefficientG();
            const double k = scratch_.feedbackK();
            const auto i = static_cast<std::size_t> (lane);
            // The scalar's own expressions (ClassicFilter.cpp setCutoff), so they round — and fuse — the same way.
            g_[i] = g;
            oneMinusG_[i] = 1.0 - g;
            const double g2 = g * g;
            const double g3 = g2 * g;
            const double g4 = g3 * g;
            g2_[i] = g2;
            g3_[i] = g3;
            k_[i] = k;
            compGain_[i] = 1.0 + k;
            invComp_[i] = 1.0 / compGain_[i];
            gComp_[i] = 1.0 / (1.0 + k * g4);
        }
        void setLaneDrive (int lane, float amount) noexcept
        {
            genKnown_[static_cast<std::size_t> (lane)] = false;
            scratch_.setDrive (amount);
            takeDrive (lane, false);
        }
        void setLaneLoopDrive (int lane, float amount) noexcept
        {
            genKnown_[static_cast<std::size_t> (lane)] = false;
            scratch_.setLoopDrive (amount);
            takeDrive (lane, true);
        }
        void setLaneType (int lane, Type t) noexcept
        {
            const auto i = static_cast<std::size_t> (lane);
            genKnown_[i] = false;
            lp12_[i] = t == Type::Lp12 ? ~0ull : 0ull;
            bp_[i] = t == Type::Bp ? ~0ull : 0ull;
            hp_[i] = t == Type::Hp ? ~0ull : 0ull;
            types_[i] = t;
            uniform_ = true;
            for (int l = 1; l < N; ++l)
                uniform_ = uniform_ && types_[static_cast<std::size_t> (l)] == types_[0];
            uniformType_ = types_[0];
        }

        // ---- [T-CMP-37 L2] THE ENGINE'S SEAM: a lane that mirrors a live scalar ClassicFilter ----------------------------------
        // The scalar's setters (setCutoff's tan, setDrive, setLoopDrive, setType) already ran in the voice's modulation half;
        // the lane takes their RESULTS, so nothing is re-derived and nothing can round differently.
        //
        // importLane: the scalar's eight states and both FIR windows become the lane's (call when the voice becomes
        // lane-resident). exportLane: the lane's become the scalar's, and the lane is cleared (call when it leaves — or the
        // lane would run on as a stale, though finite, voice). A lane that is not a member of a batch holds zeros and is fed zero.
        bool importLane (int lane, const ClassicFilter& f) noexcept
        {
            if (!os_.importLane (lane, f.oversampler()))
                return false;
            const auto i = static_cast<std::size_t> (lane);
            const auto st = f.state();
            s1_[i] = st[0];
            s2_[i] = st[1];
            s3_[i] = st[2];
            s4_[i] = st[3];
            uEst_[i] = st[4];
            y1Est_[i] = st[5];
            y2Est_[i] = st[6];
            y3Est_[i] = st[7];
            return true;
        }
        bool exportLane (int lane, ClassicFilter& f) noexcept
        {
            if (!os_.exportLane (lane, f.oversampler()))
                return false;
            {
                const auto st = state (lane); // FIREFORK: lane state is double, the scalar's is Real
                std::array<Real, 8> r {};
                for (std::size_t k = 0; k < 8; ++k)
                    r[k] = static_cast<Real> (st[k]);
                f.setState (r);
            }
            resetLane (lane);
            return true;
        }
        // Whether `f` is a core this batch can stand in for (same oversampling design).
        [[nodiscard]] bool matches (const ClassicFilter& f) const noexcept { return f.factor() == os_.factor() && os_.factor() > 1; }

        // Takes the scalar's current coefficients into lane `lane`: every sample, for every member lane (a cutoff can move on
        // each one). Plain copies; `refreshShape` then sets what the batch as a whole needs.
        // [T-CMP-37 L3] Skipped while the scalar's `coefficientsGeneration` is the one this lane last took (nothing that sets a coefficient
        // has run on it since): the arrays already hold exactly those values. A cutoff that moves every sample pays the copy as before.
        void loadLaneCoefficients (int lane, const ClassicFilter& f) noexcept
        {
            const auto i = static_cast<std::size_t> (lane);
            const std::uint32_t gen = f.coefficientsGeneration();
            if (genKnown_[i] && gen_[i] == gen)
                return;
            gen_[i] = gen;
            genKnown_[i] = true;
            const ClassicFilter::Coefficients c = f.coefficients();
            g_[i] = c.g;
            oneMinusG_[i] = c.oneMinusG;
            g2_[i] = c.g2;
            g3_[i] = c.g3;
            k_[i] = c.k;
            compGain_[i] = c.compGain;
            invComp_[i] = c.invComp;
            gComp_[i] = c.gComp;
            drive_[i] = c.drive;
            mix_[i] = static_cast<double> (c.boundMix);
            types_[i] = c.type;
            lp12_[i] = c.type == Type::Lp12 ? ~0ull : 0ull;
            bp_[i] = c.type == Type::Bp ? ~0ull : 0ull;
            hp_[i] = c.type == Type::Hp ? ~0ull : 0ull;
        }
        // After the members' coefficients are loaded: the batch-wide switches, over the MEMBER lanes only (bit `l` of
        // `members`) — a non-member lane is fed zero from zero state and its output is never read, so it must not decide
        // whether the batch takes the uniform-type or the bounded-drive path.
        void refreshShape (unsigned members) noexcept
        {
            bool uniform = true;
            bool bound = false;
            bool first = true;
            for (int l = 0; l < N; ++l)
            {
                if (((members >> static_cast<unsigned> (l)) & 1u) == 0u)
                    continue;
                const auto i = static_cast<std::size_t> (l);
                if (first)
                    uniformType_ = types_[i];
                first = false;
                uniform = uniform && types_[i] == uniformType_;
                bound = bound || mix_[i] > 0.0;
            }
            uniform_ = uniform;
            anyBound_ = bound;
        }

        [[nodiscard]] int factor() const noexcept { return os_.factor(); }

        // One lane's eight double states (s1..s4, uEst, y1Est, y2Est, y3Est), for a bit-level comparison with the scalar's.
        [[nodiscard]] std::array<double, 8> state (int lane) const noexcept
        {
            const auto i = static_cast<std::size_t> (lane);
            return { s1_[i], s2_[i], s3_[i], s4_[i], uEst_[i], y1Est_[i], y2Est_[i], y3Est_[i] };
        }

        // One base-rate sample for all N lanes: in/out are N floats.
        void processSample (const float* in, float* out) noexcept
        {
            os_.process (in, out, [this] (V (&u)[kNV]) noexcept { coreStep (u); });
        }

        // [n][N] interleaved.
        void processBlock (const float* in, float* out, int n) noexcept
        {
            for (int i = 0; i < n; ++i)
                processSample (in + static_cast<std::size_t> (i) * N, out + static_cast<std::size_t> (i) * N);
        }

    private:
        // ClassicFilter keeps boundMix_ private: it is 0 after setDrive and LoopDriveBound::mixFor (driveGain) after
        // setLoopDrive (both public inputs), so the lane tracks the same two numbers.
        void takeDrive (int lane, bool loop) noexcept
        {
            const auto i = static_cast<std::size_t> (lane);
            drive_[i] = scratch_.driveGain();
            mix_[i] = loop ? static_cast<double> (LoopDriveBound::mixFor (drive_[i])) : 0.0;
            anyBound_ = false;
            for (int l = 0; l < N; ++l)
                anyBound_ = anyBound_ || mix_[static_cast<std::size_t> (l)] > 0.0;
        }

        void clearLaneState (int lane) noexcept
        {
            const auto i = static_cast<std::size_t> (lane);
            s1_[i] = s2_[i] = s3_[i] = s4_[i] = 0.0;
            uEst_[i] = y1Est_[i] = y2Est_[i] = y3Est_[i] = 0.0;
        }

        // AdaaSoftClip::shape: x >= 1 -> 2/3, x <= -1 -> -2/3, else x - x*x*x/3.0 (the divide stays a divide).
        static V shapeV (V x) noexcept
        {
            const V one = B::set1 (1.0);
            const V c23 = B::set1 (2.0 / 3.0);
            const V mid = B::sub (x, B::div (B::mul (B::mul (x, x), x), B::set1 (3.0)));
            return B::select (B::ge (x, one), c23, B::select (B::le (x, B::neg (one)), B::neg (c23), mid));
        }

        // One OS-rate sample through the core for all lanes; `u` in = float-exact upsampled values, out = float-exact.
        void coreStep (V (&u)[kNV]) noexcept
        {
            const V one = B::set1 (1.0);
            const V two = B::set1 (2.0);
            V g[kNV], omg[kNV], g2[kNV], g3[kNV], k[kNV], comp[kNV], gComp[kNV], invComp[kNV];
            for (std::size_t v = 0; v < kNV; ++v)
            {
                const std::size_t o = 2 * v;
                g[v] = B::load (&g_[o]);
                omg[v] = B::load (&oneMinusG_[o]);
                g2[v] = B::load (&g2_[o]);
                g3[v] = B::load (&g3_[o]);
                k[v] = B::load (&k_[o]);
                comp[v] = B::load (&compGain_[o]);
                gComp[v] = B::load (&gComp_[o]);
                invComp[v] = B::load (&invComp_[o]);
            }

            // in = LoopDriveBound::apply (u * driveGain, boundMix)
            V in[kNV];
            for (std::size_t v = 0; v < kNV; ++v)
            {
                const std::size_t o = 2 * v;
                const V driven = B::mul (u[v], B::load (&drive_[o]));
                if (!anyBound_)
                {
                    in[v] = driven;
                    continue;
                }
                const V mix = B::load (&mix_[o]);
                const V bounded = shapeV (driven);
                const V blend = B::fma (mix, B::sub (bounded, driven), driven);
                in[v] = B::select (B::le (mix, B::set1 (0.0)), driven, B::select (B::ge (mix, one), bounded, blend));
            }

            V uE[kNV], y1E[kNV], y2E[kNV], y3E[kNV], s1[kNV], s2[kNV], s3[kNV], s4[kNV];
            for (std::size_t v = 0; v < kNV; ++v)
            {
                const std::size_t o = 2 * v;
                uE[v] = B::load (&uEst_[o]);
                y1E[v] = B::load (&y1Est_[o]);
                y2E[v] = B::load (&y2Est_[o]);
                y3E[v] = B::load (&y3Est_[o]);
                s1[v] = B::load (&s1_[o]);
                s2[v] = B::load (&s2_[o]);
                s3[v] = B::load (&s3_[o]);
                s4[v] = B::load (&s4_[o]);
            }

            V psi1[kNV], psi2[kNV], psi3[kNV], psi4[kNV];
            for (std::size_t v = 0; v < kNV; ++v)
            {
                psi1[v] = B::sub (uE[v], shapeV (uE[v]));
                psi2[v] = B::sub (y1E[v], shapeV (y1E[v]));
                psi3[v] = B::sub (y2E[v], shapeV (y2E[v]));
                psi4[v] = B::sub (y3E[v], shapeV (y3E[v]));
            }

            // a_i = oneMinusG*s_i - g*psi_i  (fused: fma (oneMinusG, s_i, -(g*psi_i)))
            V node[kNV];
            for (std::size_t v = 0; v < kNV; ++v)
            {
                const V a1 = B::fma (omg[v], s1[v], B::neg (B::mul (g[v], psi1[v])));
                const V a2 = B::fma (omg[v], s2[v], B::neg (B::mul (g[v], psi2[v])));
                const V a3 = B::fma (omg[v], s3[v], B::neg (B::mul (g[v], psi3[v])));
                const V a4 = B::fma (omg[v], s4[v], B::neg (B::mul (g[v], psi4[v])));
                // S = ((g3*a1 + g2*a2) + g*a3) + a4, fused as fma (g, a3, fma (g3, a1, g2*a2)) + a4
                const V s = B::add (B::fma (g[v], a3, B::fma (g3[v], a1, B::mul (g2[v], a2))), a4);
                // node = (compGain*in - k*S) * gComp
                node[v] = B::mul (B::fma (comp[v], in[v], B::neg (B::mul (k[v], s))), gComp[v]);
            }

            V y1[kNV], y2[kNV], y3[kNV], y4[kNV];
            for (std::size_t v = 0; v < kNV; ++v)
            {
                const V c1 = B::sub (node[v], psi1[v]);
                const V v1 = B::mul (B::sub (c1, s1[v]), g[v]);
                y1[v] = B::add (v1, s1[v]);
                s1[v] = B::add (y1[v], v1);

                const V c2 = B::sub (y1[v], psi2[v]);
                const V v2 = B::mul (B::sub (c2, s2[v]), g[v]);
                y2[v] = B::add (v2, s2[v]);
                s2[v] = B::add (y2[v], v2);

                const V c3 = B::sub (y2[v], psi3[v]);
                const V v3 = B::mul (B::sub (c3, s3[v]), g[v]);
                y3[v] = B::add (v3, s3[v]);
                s3[v] = B::add (y3[v], v3);

                const V c4 = B::sub (y3[v], psi4[v]);
                const V v4 = B::mul (B::sub (c4, s4[v]), g[v]);
                y4[v] = B::add (v4, s4[v]);
                s4[v] = B::add (y4[v], v4);
            }

            for (std::size_t v = 0; v < kNV; ++v)
            {
                const std::size_t o = 2 * v;
                B::store (&s1_[o], s1[v]);
                B::store (&s2_[o], s2[v]);
                B::store (&s3_[o], s3[v]);
                B::store (&s4_[o], s4[v]);
                B::store (&uEst_[o], node[v]);
                B::store (&y1Est_[o], y1[v]);
                B::store (&y2Est_[o], y2[v]);
                B::store (&y3Est_[o], y3[v]);
            }

            // Tap select, then the float round the scalar's `return static_cast<float> (out)` makes.
            for (std::size_t v = 0; v < kNV; ++v)
            {
                const std::size_t o = 2 * v;
                V out = y4[v];
                if (uniform_)
                {
                    switch (uniformType_)
                    {
                        case Type::Lp24:
                            break;
                        case Type::Lp12:
                            out = y2[v];
                            break;
                        case Type::Bp:
                            out = B::mul (B::mul (two, B::sub (y1[v], y2[v])), invComp[v]);
                            break;
                        case Type::Hp:
                            out = B::mul (B::add (B::sub (node[v], B::mul (two, y1[v])), y2[v]), invComp[v]);
                            break;
                    }
                }
                else
                {
                    const V bp = B::mul (B::mul (two, B::sub (y1[v], y2[v])), invComp[v]);
                    const V hp = B::mul (B::add (B::sub (node[v], B::mul (two, y1[v])), y2[v]), invComp[v]);
                    out = B::select (B::loadMask (&lp12_[o]), y2[v], out);
                    out = B::select (B::loadMask (&bp_[o]), bp, out);
                    out = B::select (B::loadMask (&hp_[o]), hp, out);
                }
                u[v] = B::roundF (out);
            }
        }

        OversamplerLanes<B, N> os_;
        ClassicFilter scratch_; // coefficient calculator only (setCutoff / setDrive / setLoopDrive / driveGain)

        alignas (16) double g_[N] {}, oneMinusG_[N] {}, g2_[N] {}, g3_[N] {}, k_[N] {}, compGain_[N] {}, invComp_[N] {}, gComp_[N] {};
        alignas (16) double drive_[N] {}, mix_[N] {};
        alignas (16) std::uint64_t lp12_[N] {}, bp_[N] {}, hp_[N] {};
        alignas (16) double s1_[N] {}, s2_[N] {}, s3_[N] {}, s4_[N] {}, uEst_[N] {}, y1Est_[N] {}, y2Est_[N] {}, y3Est_[N] {};
        std::uint32_t gen_[N] {}; // [T-CMP-37 L3] the scalar generation each lane's coefficient arrays were last loaded at …
        bool genKnown_[N] {}; // … and whether they still mirror one (the L1 setters above write the arrays directly)
        Type types_[N] {};
        Type uniformType_ = Type::Lp24; // the batch's type while uniform_ (lane 0's, or the first member's)
        bool uniform_ = true;
        bool anyBound_ = false;
    };

    // [T-CMP-37 L2] The engine's batch of four, and one voice's seat in it. `resident` says the voice's ClassicFilter state
    // LIVES in the lane (the scalar's is stale): whoever is about to read or write the scalar — a copy into the right chain, a
    // reset, a path that runs the scalar core — first calls `pull` (state back to the scalar) or `drop` (the scalar's state was
    // just zeroed, so the lane's is dead).
    using ClassicLanes4 = ClassicFilterLanes<DefaultBackend, 4>;
    struct ClassicLaneSlot
    {
        ClassicLanes4* lanes = nullptr;
        int lane = 0;
        bool resident = false;

        void pull (ClassicFilter& scalar) noexcept
        {
            if (resident)
            {
                lanes->exportLane (lane, scalar);
                resident = false;
            }
        }
        void drop() noexcept
        {
            if (resident)
            {
                lanes->resetLane (lane);
                resident = false;
            }
        }
    };
}
