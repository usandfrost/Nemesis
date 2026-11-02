#ifndef __MIDI_TYPES_H
#define __MIDI_TYPES_H

#include <stddef.h>
#include <stdint.h>

// Hardware-independent MIDI types. Nothing in this directory except MidiIn.cpp touches the HAL, so the parser, the
// queue and the router are unit-tested on the host (tests/test_midi.cpp).
namespace midi {

enum class Type : uint8_t {
    None,
    NoteOff,
    NoteOn,
    PolyPressure,
    ControlChange,
    ProgramChange,
    ChannelPressure,
    PitchBend,   // data1 = LSB, data2 = MSB (14-bit, centre 8192)
    Clock, Start, Continue, Stop,   // real time: no channel
};

struct Event {
    Type type = Type::None;
    uint8_t channel = 0;   // 0..15
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

// Single-producer / single-consumer ring. The producer is the UART interrupt, the consumer is the audio interrupt, and
// either can preempt the other (different priorities), so the indices are the only shared state and each has exactly one
// writer. A full queue drops the NEW event and counts it: losing a note-on is better than corrupting the ring.
template <size_t N>
class SpscQueue {
    static_assert((N & (N - 1)) == 0 && N >= 2, "N must be a power of two");

   public:
    bool push(const Event& e) {
        const uint32_t h = head_;
        if (h - tail_ >= N) {
            ++dropped_;
            return false;
        }
        buf_[h & (N - 1)] = e;
        barrier();            // the slot must be visible before the new head is
        head_ = h + 1;
        return true;
    }

    bool pop(Event& e) {
        const uint32_t t = tail_;
        if (t == head_) return false;
        barrier();
        e = buf_[t & (N - 1)];
        barrier();            // finish reading the slot before handing it back
        tail_ = t + 1;
        return true;
    }

    uint32_t dropped() const { return dropped_; }
    uint32_t size() const { return head_ - tail_; }

   private:
    static void barrier() {
#if defined(__GNUC__)
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    }
    Event buf_[N];
    volatile uint32_t head_ = 0;   // written only by the producer
    volatile uint32_t tail_ = 0;   // written only by the consumer
    volatile uint32_t dropped_ = 0;
};

}  // namespace midi

#endif
