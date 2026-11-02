#pragma once

namespace nemesis::dsp
{
    // [D-M-0243] The §22.8 LEVEL DOMAIN's ceiling, in ONE home: linear gain in [0, 10^(12/20)], unity at v = 100 and
    // make-up above. Seven rows across five engines share the law (`synth.level` · `prism.level` · the sampler's
    // three gains · `magma.amp.level` · `dross.body.level`); `CathodeOutputStage` and `PrismVoice` alias these
    // rather than transcribing them, so the two voice families cannot drift apart.
    struct LevelLaw
    {
        static constexpr float kMaxLevelDb = 12.0f;
        static constexpr float kMaxLevel = 3.98107170553497f; // 10^(12/20)
    };
}
