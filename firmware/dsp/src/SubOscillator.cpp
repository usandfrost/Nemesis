#include <nemesis/dsp/SubOscillator.h>

#include <nemesis/dsp/WavetableBank.h>
#include <nemesis/dsp/WavetableKernels.h>
#include <nemesis/dsp/WavetableRecipe.h>

#include <cstddef>
#include <vector>

namespace nemesis::dsp
{
    namespace
    {
        constexpr int kFrameLength = WavetableBank::kFrameLength;

        // THE THREE FRAMES ARE SHIPPED KERNEL LAWS, NOT NEW GEOMETRY (ADR-041's seam, used as a
        // consumer). Each recipe below declares `frames = 2` and is rendered at ONE frame, because
        // `WavetableKernels::morphAt` divides by `frames − 1` — a single-frame recipe would divide
        // by zero. Two frames with the morph pinned at an end is the honest spelling of "this law,
        // at this constant": the `Pulse` recipes set `dutyFirst == dutyLast`, so their duty does not
        // depend on which frame is asked for at all.

        // Square — a 50 % pulse. At exactly 50 % duty the even harmonics of a pulse cancel, which
        // IS the square wave; spelling it as a Pulse rather than reaching for `ShapeMorph`'s square
        // member keeps the two edge-driven waves on one law, so `Square` and `Pulse` differ in
        // their duty and in nothing else.
        constexpr WavetableRecipe kSquareRecipe {
            "cathode.sub.square",
            "Sub Square",
            WavetableCategory::Basic,
            2,
            WavetableKernel::Pulse,
            kernel::Pulse::make (kernel::DutyLaw::Sweep,
                0.50, // duty at frame 0 …
                0.50, // … and at the last, so every frame is the same 50 % square
                0.0, // `SixtyFourths` only — unused
                kernel::DutySet::ChipRegisters, // `Staircase` only — unused
                0.0, // no partner …
                0.0,
                0), // … and no partner ratio, which is what says so
            FrameOrdering::Morph
        };

        // Sine — `ShapeMorph`'s PURE end. `SawSquareTriSine` saturates through saw → square →
        // triangle → sine, so morph = 1 lands on the sine member alone: `renderShapeMorph` computes
        // `seg = 1 · (4 − 1) = 3`, `k = 3`, `frac = 0`, and `shapeAt` clamps `k + 1` back to 3, so
        // the crossfade's two operands are the same shape and the blend is exact.
        constexpr WavetableRecipe kSineRecipe {
            "cathode.sub.sine",
            "Sub Sine",
            WavetableCategory::Basic,
            2,
            WavetableKernel::ShapeMorph,
            kernel::ShapeMorph::make (kernel::ShapeSet::SawSquareTriSine),
            FrameOrdering::Morph
        };

        // Pulse — the same law as `Square` at `kPulseDuty`. See the constant's own note for why
        // 25 % and not some second 50 %.
        constexpr WavetableRecipe kPulseRecipe {
            "cathode.sub.pulse",
            "Sub Pulse",
            WavetableCategory::Basic,
            2,
            WavetableKernel::Pulse,
            kernel::Pulse::make (kernel::DutyLaw::Sweep,
                SubOscillator::kPulseDuty,
                SubOscillator::kPulseDuty,
                0.0,
                kernel::DutySet::ChipRegisters,
                0.0,
                0.0,
                0),
            FrameOrdering::Morph
        };
    } // namespace

    void SubOscillator::buildTable (Wavetable& table)
    {
        std::vector<float> frames (static_cast<std::size_t> (kNumWaves)
                                   * static_cast<std::size_t> (kFrameLength));

        // Laid out in `Wave` order, which is what makes `positionFor` a selector: frame 0 = Square,
        // 1 = Sine, 2 = Pulse. The SINE is rendered at its recipe's LAST frame (morph = 1, the pure
        // end); the two pulses at frame 0, where their constant duty already is.
        const auto stride = static_cast<std::ptrdiff_t> (kFrameLength);
        WavetableKernels::render (kSquareRecipe, 0, frames.data());
        WavetableKernels::render (kSineRecipe, kSineRecipe.frames - 1, frames.data() + stride);
        WavetableKernels::render (kPulseRecipe, 0, frames.data() + 2 * stride);

        table.build (frames.data(), kNumWaves, kFrameLength);
    }
}
