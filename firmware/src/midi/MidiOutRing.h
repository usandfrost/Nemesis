#ifndef __MIDI_OUT_RING_H
#define __MIDI_OUT_RING_H

#include <stdint.h>

// The bytes waiting for the MIDI out socket: one producer (the audio interrupt) puts whole messages in, one consumer (the UART
// interrupt) takes bytes out. A note-on is only taken while the ring still has room for the note-off of every note that can be out at
// once (`reserve` bytes), so a note-off never finds it full and no note is left hanging on the synth listening. Hardware-free, so it is
// tested on the host (tests/app/test_audio.cpp).
namespace midi {

constexpr int kMaxNotesOut = 63;                    // notes the instrument can have out at once (audio/Instrument.h)
constexpr uint32_t kNoteOffReserve = 3 * (kMaxNotesOut + 1);   // room kept for all their note-offs

template <uint32_t N>
class OutRing {
    static_assert((N & (N - 1)) == 0 && N >= 16, "N must be a power of two");

   public:
    explicit OutRing(uint32_t reserve) : reserve_(reserve) {}

    // Whole 3-byte channel messages, all or none (a retrigger: a note-off and its note-on). False: they did not fit (a note-on beyond
    // the reserve, or no room at all) and nothing was queued.
    bool put(const uint8_t* bytes, uint32_t n) {
        const uint32_t h = head_;
        const uint32_t room = N - (h - tail_);
        bool noteOn = false;
        for (uint32_t i = 0; i + 2 < n; i += 3) noteOn = noteOn || ((bytes[i] & 0xF0) == 0x90 && bytes[i + 2] > 0);
        if (room < n || (noteOn && room < n + reserve_)) { ++dropped_; return false; }
        for (uint32_t i = 0; i < n; ++i) buf_[(h + i) & (N - 1)] = bytes[i];
        barrier();   // the bytes before the new head
        head_ = h + n;
        return true;
    }
    bool put(uint8_t status, uint8_t d1, uint8_t d2) {
        const uint8_t m[3] = {status, d1, d2};
        return put(m, 3);
    }
    bool empty() const { return tail_ == head_; }
    bool take(uint8_t& b) {
        const uint32_t t = tail_;
        if (t == head_) return false;
        barrier();
        b = buf_[t & (N - 1)];
        barrier();
        tail_ = t + 1;
        return true;
    }
    uint32_t size() const { return head_ - tail_; }
    uint32_t dropped() const { return dropped_; }

   private:
    static inline void barrier() {
#if defined(__arm__)
        __asm volatile("dmb" ::: "memory");
#else
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    }
    uint8_t buf_[N] = {};
    volatile uint32_t head_ = 0, tail_ = 0;
    volatile uint32_t dropped_ = 0;
    const uint32_t reserve_;
};

// One long message (SysEx, from the main loop) sharing the socket with that ring. Nothing else may go out in the middle of a SysEx, so it waits for the ring's message
// in progress to finish, then runs to its end while the ring waits: 60 ms at the longest, and the ring has room for that. A new one is only taken once the ring has
// gone out, so a long transfer leaves room for the notes between its messages. The transmit interrupt calls next().
template <uint32_t Max>
class OutMerge {
   public:
    // The main loop. False: the last one is still going out.
    template <class Ring>
    bool set(const Ring& ring, const uint8_t* bytes, uint32_t n) {
        if (n == 0 || n > Max || left_ != 0 || !ring.empty()) return false;
        for (uint32_t i = 0; i < n; ++i) buf_[i] = bytes[i];
        at_ = 0;
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        left_ = n;
        return true;
    }
    bool busy() const { return left_ != 0; }
    template <class Ring>
    bool next(Ring& ring, uint8_t& b) {
        if (rest_ == 0 && left_ != 0) {
            b = buf_[at_++];
            left_ = left_ - 1;
            return true;
        }
        if (!ring.take(b)) return false;
        if (b >= 0x80) rest_ = lengthOf(b) - 1;
        else if (rest_ > 0) --rest_;
        return true;
    }

   private:
    static uint32_t lengthOf(uint8_t status) {
        if (status >= 0xF8) return 1;                                     // real time
        const uint8_t hi = status & 0xF0;
        return hi == 0xC0 || hi == 0xD0 ? 2 : 3;
    }
    uint8_t buf_[Max];
    volatile uint32_t left_ = 0;
    uint32_t at_ = 0, rest_ = 0;
};

}  // namespace midi

#endif
