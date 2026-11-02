#ifndef __AUDIO_MEMORY_POOL_H
#define __AUDIO_MEMORY_POOL_H

#include <stddef.h>

// One block of SDRAM for the samples and the wavetables, so a project uses what it needs and nothing sits reserved. Samples fill it from the bottom, wavetable
// slots from the top, and what is left between them is free: a recording goes there. Only the sums are here; no memory is touched. Main loop only. Sizes are in
// int16 frames, the samples' unit.
namespace audio {

class MemoryPool {
   public:
    void init(size_t frames, size_t slotFrames, int maxSlots) { total_ = frames; slot_ = slotFrames; maxSlots_ = maxSlots; samples_ = 0; slots_ = 0; }
    size_t total() const { return total_; }
    size_t samples() const { return samples_; }
    int slots() const { return slots_; }
    size_t top() const { return total_ - (size_t)slots_ * slot_; }      // where the samples must stop
    size_t room() const { return top() - samples_; }

    bool fitsSample(size_t frames) const { return frames <= room(); }
    void useSamples(size_t frames) { samples_ = frames; }
    // How many slots could be in use with the samples as they are: the wavetable store is not given more
    int slotsThatFit() const {
        if (!slot_) return 0;
        const size_t n = (total_ - samples_) / slot_;
        return n < (size_t)maxSlots_ ? (int)n : maxSlots_;
    }
    void useSlots(int slots) { slots_ = slots; }   // the store's highest slot in use + 1

   private:
    size_t total_ = 0, slot_ = 0, samples_ = 0;
    int maxSlots_ = 0, slots_ = 0;
};

}  // namespace audio

#endif
