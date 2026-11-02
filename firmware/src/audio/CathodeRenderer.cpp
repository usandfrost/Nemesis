#include "audio/CathodeRenderer.h"
#include "audio/ApplyVoiceDesc.h"

#include <nemesis/dsp/ForkConfig.h>

using namespace nemesis::dsp;

// cost::Filter mirrors CathodeVoiceStage::FilterMode one to one; if upstream ever reorders the enum, this stops the build.
#define COST_FILTER_MATCHES(name) static_assert(static_cast<int>(cost::Filter::name) == static_cast<int>(CathodeVoiceStage::FilterMode::name), "cost::Filter::" #name " != FilterMode::" #name)
COST_FILTER_MATCHES(LowPass); COST_FILTER_MATCHES(BandPass); COST_FILTER_MATCHES(HighPass); COST_FILTER_MATCHES(Notch); COST_FILTER_MATCHES(Formant);
COST_FILTER_MATCHES(Comb); COST_FILTER_MATCHES(Ladder24); COST_FILTER_MATCHES(Ladder12); COST_FILTER_MATCHES(ScreamLp); COST_FILTER_MATCHES(ScreamHp);
COST_FILTER_MATCHES(Acid); COST_FILTER_MATCHES(SovietLp); COST_FILTER_MATCHES(SovietBp); COST_FILTER_MATCHES(SovietHp); COST_FILTER_MATCHES(ClassicLp24);
COST_FILTER_MATCHES(ClassicLp12); COST_FILTER_MATCHES(ClassicBp); COST_FILTER_MATCHES(ClassicHp); COST_FILTER_MATCHES(ReedLp); COST_FILTER_MATCHES(ReedBp);
COST_FILTER_MATCHES(ReedHp); COST_FILTER_MATCHES(StateLp); COST_FILTER_MATCHES(StateBp); COST_FILTER_MATCHES(StateHp); COST_FILTER_MATCHES(StateNotch);
COST_FILTER_MATCHES(StateAp); COST_FILTER_MATCHES(StateMorph); COST_FILTER_MATCHES(ReedMorph);
// ...and has no model beyond it: a new FilterMode appended upstream fails here instead of being cast past the cost table.
static_assert(static_cast<int>(cost::Filter::Count) == static_cast<int>(CathodeVoiceStage::FilterMode::ReedMorph) + 1,
              "cost::Filter and FilterMode differ in length");
static_assert(sizeof(cost::table::kFilterExtra) / sizeof(cost::table::kFilterExtra[0]) == static_cast<size_t>(cost::Filter::Count),
              "the generated cost table and cost::Filter differ in length");

namespace {

using OM = CathodeVoiceStage::OscMode;
using FM = CathodeVoiceStage::FilterMode;

// Same dual phase-distortion voice the engine's own tests use (no wavetable dependency), so the
// first on-target numbers are comparable with the host benchmark's "PD x2 / Ladder24".
void configurePatch(CathodeOutputStage& v) {
    CathodeVoiceStage& t = v.voice();
    for (int s = 0; s < 2; ++s) {
        t.setOscMode(s, OM::PhaseDistortion);
        t.setPdShape(s, 0.3f + 0.2f * s);
        t.setPdAmount(s, 0.6f);
    }
    t.setSemitones(1, 7);
    t.setLevel(0, 0.5f);   // per-slot levels (the voice no longer has a single osc mix)
    t.setLevel(1, 0.5f);
    t.setFilterMode(FM::Ladder24);
    v.amp().setDelayMs(0.0f);
}

constexpr float kPatchCutoffHz = 2000.0f, kPatchQ = 3.0f;
constexpr float kAttackMs = 5.0f, kDecayMs = 30.0f, kSustain = 0.8f, kReleaseMs = 200.0f;

}  // namespace

void CathodeRenderer::init(double sampleRate, int voices) {
    engine_ = new nemesis::dsp::CathodeVoiceEngine;   // SDRAM heap (memory/SdramHeap.h)
    engine_->prepare(sampleRate);
    engine_->setVoiceCount(voices);
    for (int i = 0; i < CathodeVoiceEngine::kMaxVoices; ++i) configurePatch(engine_->voiceAt(i).outputStage());
    // Cutoff and amp envelope through the engine's PATCH setters: they set each voice's modulation base, so a cutoff route that is added
    // and later cleared returns to this cutoff (setting the stage directly left the voice's own default base, 20 kHz, to come back).
    engine_->setPatchCutoff(kPatchCutoffHz, kPatchQ);
    engine_->setPatchAmpEnv(kAttackMs, kDecayMs, kSustain, kReleaseMs);
    releaseSeconds_ = kReleaseMs * 0.001f;
    // A stolen voice crossfades over 3 ms instead of jumping to the new pitch in the middle of its release tail (the engine default is 0).
    engine_->setStealFade(0.003f);
    engine_->reset();
    // The lane-wise voice batching is a NEON desktop optimisation (T-CMP-37/38); the M7 has no NEON, so run every voice scalar.
    // (main.cpp sets fork::Config::lanes = false, so prepare() never built the lanes; this keeps it true for any other caller.)
    engine_->setLaneSitesForTest(0u);
    engine_->setBendRange(2.0);   // the MIDI default: pitch wheel = +/- 2 semitones

    // The cost model describes this patch (configurePatch above): two audible phase-distortion slots into a Ladder24, nothing costly switched on.
    costSettings_.filterOversample = fork::Config::filterOversample;
    costSettings_.driveOversample = fork::Config::driveOversample;
    costSettings_.skipSilentSlots = fork::Config::skipSilentSlots;
    PatchRequest r;
    r.voice.slot[1].audible = true;
    r.voice.f1 = cost::Filter::Ladder24;
    r.voices = voices;
    applyPatch(r);   // before the audio interrupt runs, so no request is needed
}

void CathodeRenderer::beginBlock() {
    takePatchRequest();
    // The load of the block that just finished, against the cycles it had. The governor climbs the degrade ladder quickly when it is too high
    // and walks back down slowly (cost/Governor.h).
    if (audioEngine.stats.count > 0) {
        const float load = static_cast<float>(audioEngine.stats.last) / static_cast<float>(audioEngine.budgetCycles());
        const int wanted = governor_.update(load, ladderSteps_);
        if (wanted != degradeLevel_) { degradeLevel_ = wanted; applyLevel(wanted); }
    }
    if (demoChord_) {
        static const int kChord[5] = {48, 52, 55, 59, 62};   // Cmaj9
        const uint32_t releaseAt = static_cast<uint32_t>(1.5 * AUDIO_SAMPLE_RATE / I2S_BLOCK_SIZE);
        if (blocks_ == 0)
            for (int n : kChord) noteOn(n, 0.8f);
        if (blocks_ == releaseAt)
            for (int n : kChord) noteOff(n);
    }
    ++blocks_;

    midi::Event e;
    while (midi::rxQueue.pop(e)) router_.apply(e, *this);
}

void CathodeRenderer::render(float* left, float* right, int frames) {
    for (int i = 0; i < frames; ++i) {
        const CathodeVoiceEngine::StereoSample s = engine_->processSample();
        left[i] = s.left * level_;
        right[i] = s.right * level_;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Notes: the renderer owns the polyphony cap, so degrading can release the OLDEST voice without resetting the engine.
void CathodeRenderer::noteOn(int note, float velocity01) {
    while (heldCount_ > 0 && heldCount_ >= voiceCap_) releaseOldest();
    engine_->noteOn(note, velocity01);
    for (int i = 0; i < heldCount_; ++i) if (held_[i] == note) { for (int j = i; j + 1 < heldCount_; ++j) held_[j] = held_[j + 1]; --heldCount_; break; }   // a re-struck note moves to the newest slot
    if (heldCount_ < 16) held_[heldCount_++] = static_cast<uint8_t>(note);
}

void CathodeRenderer::noteOff(int note) {
    engine_->noteOff(note);
    for (int i = 0; i < heldCount_; ++i)
        if (held_[i] == note) { for (int j = i; j + 1 < heldCount_; ++j) held_[j] = held_[j + 1]; --heldCount_; break; }
}

void CathodeRenderer::releaseOldest() {
    if (heldCount_ == 0) return;
    engine_->noteOff(held_[0]);                       // the release tail still sounds; only the note is let go
    for (int j = 0; j + 1 < heldCount_; ++j) held_[j] = held_[j + 1];
    --heldCount_;
}

void CathodeRenderer::allNotesOff() {
    engine_->panic();
    heldCount_ = 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Degrading.
void CathodeRenderer::setPatchCost(const cost::VoiceDesc& voice, int voices, int priority) {
    // Sequence lock, single writer (the main loop), single reader (the audio interrupt, which can preempt the writer but not the reverse).
    const uint32_t s = pendingSeq_.load(std::memory_order_relaxed);
    pendingSeq_.store(s + 1, std::memory_order_relaxed);              // odd: being written
    std::atomic_signal_fence(std::memory_order_seq_cst);
    pending_.voice = voice;
    pending_.voices = voices;
    pending_.priority = priority;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    pendingSeq_.store(s + 2, std::memory_order_release);              // even: complete
}

void CathodeRenderer::takePatchRequest() {
    const uint32_t s1 = pendingSeq_.load(std::memory_order_acquire);
    if (s1 == appliedSeq_ || (s1 & 1u)) return;                        // nothing new, or the writer was interrupted mid-write: next block
    const PatchRequest r = pending_;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    if (pendingSeq_.load(std::memory_order_relaxed) != s1) return;
    appliedSeq_ = s1;
    applyPatch(r);
}

void CathodeRenderer::applyPatch(const PatchRequest& r) {
    base_.voice = r.voice;
    if (!cost::heavyRuleOk(base_.voice)) cost::setFilter2On(base_.voice, true);   // one heavy filter per lane: the newcomer (F2) is demoted
    base_.voices = static_cast<uint8_t>(r.voices);
    base_.priority = static_cast<uint8_t>(r.priority);
    rebuildLadder();
    governor_.reset();
    degradeLevel_ = 0;
    engineHasDesc_ = false;   // a new patch: push every field once
    applyLevel(0);
}

void CathodeRenderer::rebuildLadder() {
    ladderSteps_ = cost::buildLadder(&base_, 1, costSettings_, ladder_, cost::kMaxSteps);
}

void CathodeRenderer::applyVoiceDesc(const cost::VoiceDesc& d, const cost::VoiceDesc* prev) { audio::applyVoiceDesc(*engine_, d, prev); }

void CathodeRenderer::applyLevel(int level) {
    const cost::VoiceDesc before = effective_.voice;
    const int capBefore = voiceCap_;
    effective_ = cost::degraded(base_, 0, ladder_, ladderSteps_, level);
    applyVoiceDesc(effective_.voice, engineHasDesc_ ? &before : nullptr);
    engineHasDesc_ = true;
    voiceCap_ = effective_.voices;
    while (heldCount_ > voiceCap_) releaseOldest();
    // A released voice keeps sounding (and costing) until its release has faded, so do not judge the new level before that.
    if (voiceCap_ < capBefore) governor_.holdFor(static_cast<int>(releaseSeconds_ * AUDIO_SAMPLE_RATE / I2S_BLOCK_SIZE) + 1);
}
