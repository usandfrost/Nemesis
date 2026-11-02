#ifndef __MIDI_OUT_H
#define __MIDI_OUT_H

#include <stdint.h>

// MIDI output on USART6 (the DIN out socket, 31250 baud, transmitter only), as the stock firmware wires it.
//
// Threading: the audio interrupt QUEUES messages (the lanes' notes, sample-accurate to the block); the USART6 interrupt SENDS them one
// byte at a time from a ring (midi/MidiOutRing.h). One producer, one consumer, no lock. At 31250 baud a 3-byte note takes ~1 ms; when
// more notes are played than the socket can carry, note-ons are dropped (and counted) but their note-offs always fit.
namespace midi {

struct OutStats {
    volatile uint32_t bytes = 0;   // bytes sent
};
extern OutStats outStats;
uint32_t outDropped();             // messages that did not fit

void startOut();                                    // the transmitter on; call once, after the HAL is up and before the audio starts
constexpr uint32_t kLongMax = 200;
bool sendLong(const uint8_t* bytes, int n);         // one SysEx message, up to kLongMax (main loop); false: the last one is still going out, try again
bool send(const uint8_t* bytes, int n);   // whole 3-byte channel messages, all or none (audio interrupt); false: not queued

}  // namespace midi

#endif
