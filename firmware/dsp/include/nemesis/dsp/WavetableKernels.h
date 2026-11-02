#pragma once

#include <nemesis/dsp/WavetableRecipe.h>

namespace nemesis::dsp
{
    // THE KERNEL DISPATCH (T-WT-11; ADR-041 decision 2). `WavetableRecipe.h` states what a recipe
    // IS; this is what renders one.
    //
    // Every law here is the law that already shipped, lifted out of its category class and given
    // its parameters — owner ruling R1, recorded in full at the top of `WavetableRecipe.h`. The
    // six generator classes keep their enums, their constants, their law accessors and every
    // public signature; their `renderFrame` bodies become a recipe lookup plus a call to
    // `render()` below, and their law accessors delegate to the `…At` helpers so a bank's law is
    // stated exactly once and read by both.
    //
    // Build-time only. `render()` allocates and calls libm, exactly as the bodies it replaces did:
    // generation runs on SELECTION, off the audio thread. Nothing here may be called at render
    // time. (`MaskTableGenerator::dcOffsetFor` IS reached from the audio thread and is deliberately
    // NOT part of this seam — it keeps its own static table.)
    class WavetableKernels
    {
    public:
        // Renders `recipe`'s frame `frame` into `out` (`WavetableBank::kFrameLength` floats).
        // `frame` is clamped into [0, recipe.frames) — the clamp lives here, at the one place a
        // frame index becomes a law's morph, rather than being re-spelled in six generators.
        static void render (const WavetableRecipe& recipe, int frame, float* out);

        // Renders one MASK STATE of a COMPLEX law directly, bypassing the frame→Gray step.
        // `MaskTableGenerator::renderMask` is a public signature that takes a mask rather than a
        // frame (the `mask` param is a modulation destination, so a mask that is not any frame's
        // is a legitimate request), so the seam has to expose the same door.
        static void renderMask (kernel::MaskLaw law, int mask, float* out);

        // ─── The law helpers the generators' own accessors delegate to ────────────────────────
        //
        // Each is the value a law reads at `frame`, computed from the recipe. They exist because
        // the shipped classes publish these as public law accessors that the tests assert
        // against; delegating keeps one statement of each law instead of two that can drift —
        // which is the whole reason ADR-041 gives for the seam.

        // The morph parameter: 0 at the first frame, 1 at the last. Every factory category uses
        // this shape, so "frame 0 is one end of the morph" means one thing across the catalogue.
        [[nodiscard]] static double morphAt (const WavetableRecipe& recipe, int frame) noexcept;

        [[nodiscard]] static double combSpacingAt (const WavetableRecipe& recipe, int frame) noexcept;
        [[nodiscard]] static int harmonicCountAt (const WavetableRecipe& recipe, int frame) noexcept;
        [[nodiscard]] static double tiltAt (const WavetableRecipe& recipe, int frame) noexcept;
        [[nodiscard]] static double quintFadeAt (const WavetableRecipe& recipe, int frame) noexcept;
        [[nodiscard]] static double chordBlendAt (const WavetableRecipe& recipe, int frame) noexcept;
        [[nodiscard]] static double dutyAt (const WavetableRecipe& recipe, int frame) noexcept;
        [[nodiscard]] static int dutySixtyFourthsAt (const WavetableRecipe& recipe, int frame) noexcept;
        [[nodiscard]] static double partnerLevelAt (const WavetableRecipe& recipe, int frame) noexcept;
        [[nodiscard]] static double fmIndexAt (const WavetableRecipe& recipe, int frame) noexcept;
    };
} // namespace nemesis::dsp
