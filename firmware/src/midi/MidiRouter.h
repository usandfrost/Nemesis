#ifndef __MIDI_ROUTER_H
#define __MIDI_ROUTER_H

#include "midi/MidiTypes.h"

namespace midi {

// Turns parsed events into calls on a Sink, adding the performance behaviour MIDI leaves to the receiver:
// sustain pedal (note-offs are held until the pedal lifts), all-notes-off, controller reset.
//
// Sink must provide:
//   void noteOn(int note, float velocity01);     void noteOff(int note);
//   void pitchBend(float bendMinus1To1);          void modWheel(float wheel01);
//   void channelPressure(float pressure01);       void allNotesOff();
template <class Sink>
class Router {
   public:
    void apply(const Event& e, Sink& s) {
        switch (e.type) {
            case Type::NoteOn:
                clearPending(e.data1);
                s.noteOn(e.data1, e.data2 / 127.0f);
                break;
            case Type::NoteOff:
                if (sustain_) setPending(e.data1);
                else s.noteOff(e.data1);
                break;
            case Type::PitchBend: {
                const int v = (static_cast<int>(e.data2) << 7) | e.data1;   // 0..16383, centre 8192
                s.pitchBend(static_cast<float>(v - 8192) / 8192.0f);
                break;
            }
            case Type::ChannelPressure:
                s.channelPressure(e.data1 / 127.0f);
                break;
            case Type::ControlChange:
                control(e.data1, e.data2, s);
                break;
            default:
                break;   // poly pressure and program change are not routed yet
        }
    }

    bool sustainDown() const { return sustain_; }

   private:
    void control(uint8_t cc, uint8_t value, Sink& s) {
        switch (cc) {
            case 1: s.modWheel(value / 127.0f); break;
            case 64: {
                const bool down = value >= 64;
                if (sustain_ && !down) releasePending(s);
                sustain_ = down;
                break;
            }
            case 120:   // all sound off
            case 123:   // all notes off
                clearAllPending();
                s.allNotesOff();
                break;
            case 121:   // reset all controllers
                if (sustain_) releasePending(s);
                sustain_ = false;
                s.pitchBend(0.0f);
                s.modWheel(0.0f);
                s.channelPressure(0.0f);
                break;
            default:
                break;
        }
    }

    void setPending(uint8_t n) { pending_[n >> 5] |= (1u << (n & 31)); }
    void clearPending(uint8_t n) { pending_[n >> 5] &= ~(1u << (n & 31)); }
    void clearAllPending() { for (auto& w : pending_) w = 0; }
    void releasePending(Sink& s) {
        for (int n = 0; n < 128; ++n)
            if (pending_[n >> 5] & (1u << (n & 31))) s.noteOff(n);
        clearAllPending();
    }

    bool sustain_ = false;
    uint32_t pending_[4] = {0, 0, 0, 0};   // notes released while the pedal was down
};

}  // namespace midi

#endif
