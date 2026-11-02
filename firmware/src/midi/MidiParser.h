#ifndef __MIDI_PARSER_H
#define __MIDI_PARSER_H

#include "midi/MidiTypes.h"

namespace midi {

// Byte-at-a-time MIDI 1.0 channel-message parser.
//   * running status (a data byte with no new status repeats the last channel status)
//   * real-time bytes (0xF8..0xFF) are legal anywhere, even inside a message, and are ignored without disturbing it
//   * system exclusive (0xF0 .. 0xF7) is skipped whole; system common bytes cancel running status
//   * stray data bytes with no status are ignored
//   * note-on with velocity 0 becomes a note-off (velocity 64), as the specification says
//   * channel filter: 0 = omni, 1..16 = that channel only
class Parser {
   public:
    void setChannel(uint8_t ch1to16OrOmni) { filter_ = ch1to16OrOmni > 16 ? 0 : ch1to16OrOmni; }

    // Returns true when `b` completed a message that passes the channel filter; the message is in `out`.
    bool feed(uint8_t b, Event& out) {
        if (b >= 0xF8) {                                // real time: comes between anything, and leaves the state untouched
            const Type t = b == 0xF8 ? Type::Clock : b == 0xFA ? Type::Start : b == 0xFB ? Type::Continue : b == 0xFC ? Type::Stop : Type::None;
            if (t == Type::None) return false;
            Event e;
            e.type = t;
            out = e;
            return true;
        }
        if (b >= 0x80) {
            if (b == 0xF0) {                            // sysex start: skip until 0xF7, cancel running status
                sysex_ = true;
                status_ = 0;
            } else if (b == 0xF7) {                     // sysex end (or stray)
                sysex_ = false;
                status_ = 0;
            } else if (b >= 0xF1) {                     // system common: cancel running status, its data bytes are skipped
                sysex_ = false;
                status_ = 0;
            } else {                                    // channel status
                sysex_ = false;
                status_ = b;
                need_ = needFor(b);
                count_ = 0;
            }
            return false;
        }
        if (sysex_ || status_ == 0) return false;       // data byte with nowhere to go
        data_[count_++] = b;
        if (count_ < need_) return false;
        count_ = 0;                                     // running status: the next data byte starts a fresh message

        const uint8_t ch = status_ & 0x0F;
        if (filter_ != 0 && ch != filter_ - 1) return false;
        Event e;
        e.channel = ch;
        e.data1 = data_[0];
        e.data2 = need_ > 1 ? data_[1] : 0;
        switch (status_ & 0xF0) {
            case 0x80: e.type = Type::NoteOff; break;
            case 0x90:
                e.type = e.data2 == 0 ? Type::NoteOff : Type::NoteOn;
                if (e.data2 == 0) e.data2 = 64;
                break;
            case 0xA0: e.type = Type::PolyPressure; break;
            case 0xB0: e.type = Type::ControlChange; break;
            case 0xC0: e.type = Type::ProgramChange; break;
            case 0xD0: e.type = Type::ChannelPressure; break;
            default: e.type = Type::PitchBend; break;
        }
        out = e;
        return true;
    }

    void reset() { status_ = 0; count_ = 0; sysex_ = false; }

   private:
    static uint8_t needFor(uint8_t status) {
        const uint8_t hi = status & 0xF0;
        return (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
    }

    uint8_t filter_ = 0;
    uint8_t status_ = 0;
    uint8_t need_ = 0;
    uint8_t count_ = 0;
    uint8_t data_[2] = {0, 0};
    bool sysex_ = false;
};

}  // namespace midi

#endif
