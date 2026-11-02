#pragma once

// FIREFORK: the sample/state type of the CATHODE render path.
//
//   Real  - signal values, filter state, coefficients, envelopes. double by default (bit-identical to upstream
//           upstream); float when NEMESIS_DSP_FLOAT is defined, which is what the Cortex-M7 firmware uses: its FPU
//           does float in hardware at one op per cycle, and double several times slower.
//   Wide  - always double. Phase accumulators and anything that integrates a tiny increment over millions of
//           samples, where float would audibly detune low notes. Kept double on purpose even in float builds.
//
// Do NOT use unsuffixed literals in Real expressions in float builds without checking: `x * 0.5` is a double
// multiply unless the build passes -fsingle-precision-constant (the firmware does for these files).

namespace nemesis::dsp
{
#ifdef NEMESIS_DSP_FLOAT
    using Real = float;
#else
    using Real = double;
#endif
    using Wide = double;
}
