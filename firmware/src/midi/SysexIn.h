#ifndef __MIDI_SYSEX_IN_H
#define __MIDI_SYSEX_IN_H

#include <stdint.h>
#include <string.h>

// The preset protocol's messages out of the MIDI stream (docs/PRESET_SYSEX.md), byte by byte in the UART interrupt, handed whole to the main loop. Everything else
// on the wire is the parser's. Two slots: the main loop can be away at the card for seconds, and the other side waits for an answer, so a second message means a
// late ABORT at most. A message that does not fit, is cut off, or finds both slots full is left out and counted: the other side's timeout deals with it.
namespace midi {

constexpr int kSysexMax = 200;   // the protocol's largest message is 188 bytes

class SysexIn {
   public:
    // The interrupt
    void feed(uint8_t b) {
        if (b >= 0xF8) return;   // real time is legal anywhere
        if (b == 0xF0) {
            at_ = 0;
            collecting_ = true;
        } else if (!collecting_) {
            return;
        }
        if (b >= 0x80 && b != 0xF0 && b != 0xF7) { collecting_ = false; ++cut_; return; }   // a status byte in the middle: it was cut off
        if (at_ >= kSysexMax) { collecting_ = false; ++cut_; return; }
        static const uint8_t kHead[4] = {0xF0, 0x7D, 0x4E, 0x50};
        if (at_ < 4 && b != kHead[at_]) { collecting_ = false; return; }   // somebody else's
        const uint32_t h = head_;
        uint8_t* slot = data_[h & 1];
        if (h - tail_ >= 2) { collecting_ = false; ++cut_; return; }
        slot[at_++] = b;
        if (b == 0xF7) {
            collecting_ = false;
            length_[h & 1] = at_;
            __atomic_signal_fence(__ATOMIC_SEQ_CST);
            head_ = h + 1;
        }
    }
    void reset() { collecting_ = false; }   // a framing or overrun error
    // The main loop: the next whole message, or false. `out` holds kSysexMax bytes.
    bool take(uint8_t* out, int& n) {
        const uint32_t t = tail_;
        if (t == head_) return false;
        n = length_[t & 1];
        memcpy(out, data_[t & 1], (size_t)n);
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        tail_ = t + 1;
        return true;
    }
    uint32_t cut() const { return cut_; }

   private:
    uint8_t data_[2][kSysexMax];
    volatile int length_[2] = {0, 0};
    volatile uint32_t head_ = 0, tail_ = 0;
    uint32_t cut_ = 0;
    int at_ = 0;
    bool collecting_ = false;
};

}  // namespace midi

#endif
