#ifndef __AUDIO_WAVE_STORE_H
#define __AUDIO_WAVE_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "app/Model.h"
#include "audio/PatchMap.h"
#include "audio/WaveFile.h"

// The wavetable banks in memory. A bank is a finished table (the band-limited, decimated pyramid the host tool wt_pyramid wrote as a .wtp file:
// no FFT on the device) in one of a few fixed slots of an arena the board allocates once (its heap never frees). Main loop only; the audio
// interrupt reads the banks through SharedTables::bank[], which this publishes into. A slot is only re-used after the audio interrupt has had
// time to let go of it: a bank nobody asks for is first un-published (the voices re-bind to the basic wave at the next block), and its slot is
// free after kRetireMs.
namespace audio {

class WaveStore {
   public:
    static constexpr int kMaxSlots = 6;
    static constexpr uint32_t kRetireMs = 25;
    static constexpr int kFrames = 32, kFrameLen = 2048;

    // The samples one table takes (as nemesis::dsp::Real); the arena holds `slots` of these.
    static size_t slotSamples();

    // `arena`: slots x slotSamples() of Real, from the board's SDRAM (or a host buffer). `staging`: a buffer of at least slotSamples() int16 the
    // file is read into when it cannot be read into the arena itself (the host builds in double; the board passes nullptr and reads into the
    // slot's own second half).
    void init(nemesis::dsp::Real* arena, int slots, int16_t* staging, SharedTables* pub, WaveFile* file);
    int slots() const { return slots_; }
    // How many slots may be used now (the shared memory, audio/MemoryPool.h): a bank past that is refused like one past the last slot. A new bank takes the lowest free slot.
    void setSlotLimit(int n) { limit_ = n; }
    int highSlot() const;   // the highest slot in use + 1

    // Which banks the project wants in memory (a bit per bank): from here on those load, the rest are let go. Returns true if every wanted bank
    // is resident or unavailable (no file); false while one is waiting for a free slot or for the audio interrupt to let go of one.
    bool want(const uint8_t bits[(app::kWaveBanks + 7) / 8], uint32_t nowMs);
    // The bank that could not be loaded for lack of a slot at the last want() (-1: none), and which banks are resident.
    int refused() const { return refused_; }
    bool resident(int bank) const;
    int residentCount() const;
    uint32_t loadedBytes() const { return bytesRead_; }

   private:
    auto slotBase(int i) const { return arena_ + (size_t)(slots_ - 1 - i) * slotSamples(); }   // slot 0 is the top of the arena
    struct Slot {
        int bank = -1;                 // -1 free
        bool published = false;
        uint32_t freeAt = 0;           // after un-publishing: usable again from this time
        nemesis::dsp::Wavetable wt;
    };
    bool load(int slot, int bank);
    Slot slot_[kMaxSlots];
    int slots_ = 0, limit_ = kMaxSlots;
    nemesis::dsp::Real* arena_ = nullptr;
    int16_t* staging_ = nullptr;
    SharedTables* pub_ = nullptr;
    WaveFile* file_ = nullptr;
    int refused_ = -1;
    uint32_t bytesRead_ = 0;
};

// The banks a project plays: every CATHODE part's OSC 1 and OSC 2 on a bank.
void neededBanks(const app::Project& pj, uint8_t bits[(app::kWaveBanks + 7) / 8]);

}  // namespace audio

#endif
