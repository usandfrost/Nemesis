#include <nemesis/dsp/PulseOscillator.h>

#include <nemesis/dsp/WavetableBank.h>
#include <nemesis/dsp/WavetableKernels.h>
#include <nemesis/dsp/WavetableRecipe.h>

#include <vector>

namespace nemesis::dsp
{
    namespace
    {
        constexpr int kFrameLength = WavetableBank::kFrameLength;

        // THE SAW IS A SHIPPED KERNEL LAW, NOT NEW GEOMETRY (ADR-041's seam, used as a consumer — the
        // `SubOscillator::buildTable` precedent). `SawSquareTriSine` saturates saw → square → triangle
        // → sine and `BasicTableGenerator`'s own note records that "frame 0 is the saw", so morph = 0
        // lands on the saw member alone. The recipe declares `frames = 2` because
        // `WavetableKernels::morphAt` divides by `frames − 1`; rendering it at frame 0 pins the morph at
        // that end, which is the honest spelling of "this law, at this constant".
        //
        // Reaching for the SAW rather than a `Pulse` recipe is the point of the card: a `Pulse` kernel
        // would bake ONE duty into the table, which is precisely the frame quantisation this mode
        // exists to escape. The duty lives in the read offset instead.
        constexpr WavetableRecipe kSawRecipe {
            "cathode.pulse.saw",
            "Pulse Saw",
            WavetableCategory::Basic,
            2,
            WavetableKernel::ShapeMorph,
            kernel::ShapeMorph::make (kernel::ShapeSet::SawSquareTriSine),
            FrameOrdering::Morph
        };
    } // namespace

    void PulseOscillator::buildTable (Wavetable& table)
    {
        // ONE frame. The sub needs three because its enum selects between three waves; a pulse's
        // control is the read offset, so a second frame would be content nothing can reach.
        std::vector<float> frame (static_cast<std::size_t> (kFrameLength));
        WavetableKernels::render (kSawRecipe, 0, frame.data());
        table.build (frame.data(), 1, kFrameLength);
    }
}
