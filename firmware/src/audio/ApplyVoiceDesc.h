#ifndef __APPLY_VOICE_DESC_H
#define __APPLY_VOICE_DESC_H

#include <nemesis/dsp/CathodeVoiceEngine.h>

#include "cost/CostModel.h"

// Pushes the cost-relevant features of a patch (cost::VoiceDesc) onto every voice of a CATHODE engine. This is the one place that turns a
// degrade step into engine settings; CathodeRenderer uses it on the audio thread and bench/degrade_check runs the same code on the host.
// Only the features the degrade ladder can bypass are touched, so the rest of the patch is left alone.
//
// `prev` is what the engine already has. Only the fields that differ are pushed, so a degrade step (one field) costs one setter per voice
// inside the audio interrupt, not twelve. Pass nullptr to push everything (start-up, a new patch).
namespace audio {

template <class Engine>
inline void applyVoiceDesc(Engine& engine, const cost::VoiceDesc& d, const cost::VoiceDesc* prev = nullptr) {
    using FilterMode = nemesis::dsp::CathodeVoiceStage::FilterMode;
    using FilterRoute = nemesis::dsp::CathodeVoiceStage::FilterRoute;
    const bool all = prev == nullptr;
    const auto changed = [&](auto a, auto b) { return all || a != b; };
    const bool uni0 = changed(d.slot[0].unison, all ? 0 : prev->slot[0].unison), uni1 = changed(d.slot[1].unison, all ? 0 : prev->slot[1].unison);
    const bool width = changed(d.width, all ? 0.0f : prev->width), drive = changed(d.drive, all ? 0.0f : prev->drive);
    const bool fm = changed(d.fm, all ? 0.0f : prev->fm), ring = changed(d.ring, all ? 0.0f : prev->ring), sync = changed(d.sync, all ? 0.0f : prev->sync);
    const bool fold = changed(d.fold, all ? 0.0f : prev->fold), comp = changed(d.companding, all ? 0.0f : prev->companding);
    const bool f1 = changed(d.f1, all ? cost::Filter::Count : prev->f1), f2 = changed(d.f2, all ? cost::Filter::Count : prev->f2);
    const bool f2On = changed(d.f2On, all ? !d.f2On : prev->f2On), route = changed(d.route, all ? cost::Route::Series : prev->route) || all;
    for (int i = 0; i < Engine::kMaxVoices; ++i) {
        auto& t = engine.voiceAt(i).outputStage().voice();
        if (uni0) t.setUnisonVoices(0, d.slot[0].unison);
        if (uni1) t.setUnisonVoices(1, d.slot[1].unison);
        if (width) { t.setUnisonWidth(0, d.width); t.setUnisonWidth(1, d.width); }
        if (drive) t.setDrive(d.drive);
        if (fm) t.setXmodFm(d.fm);
        if (ring) t.setXmodRing(d.ring);
        if (sync) t.setSyncAmount(d.sync);
        if (fold) t.setFoldAmount(d.fold);
        if (comp) t.setCompanding(d.companding);
        if (f1) t.setFilterMode(static_cast<FilterMode>(d.f1));
        if (f2) t.editFilter2([&](nemesis::dsp::CathodeFilterBank& b) noexcept { b.setFilterMode(static_cast<FilterMode>(d.f2)); });
        if (route) t.setFilterRoute(static_cast<FilterRoute>(d.route));
        if (f2On) t.setFilter2On(d.f2On);
    }
}

}  // namespace audio

#endif
