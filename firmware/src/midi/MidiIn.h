#ifndef __MIDI_IN_H
#define __MIDI_IN_H

#include "midi/MidiTypes.h"
#include "midi/SysexIn.h"

// MIDI input on USART1 (the DIN socket's opto-isolator, 31250 baud, half-duplex), the same pin and mode as the stock firmware.
//
// Threading: the UART interrupt parses bytes and PUSHES events; the audio interrupt POPS them at the start of each block
// (AudioRenderer::beginBlock) and is the only context that touches the synth engine. So there is exactly one producer and one
// consumer and no lock anywhere.
namespace midi {

struct Stats {
    volatile uint32_t bytes = 0;      // bytes received
    volatile uint32_t messages = 0;   // complete channel messages that passed the filter
    volatile uint32_t errors = 0;     // UART framing / overrun / noise errors
};

extern SpscQueue<64> rxQueue;
extern Stats stats;
extern SysexIn sysexIn;                    // the preset protocol's messages, for the main loop

void setChannel(uint8_t ch1to16OrOmni);   // 0 = omni (default)
void start();                              // enable the receiver and arm the first byte; call once, after the HAL is up
void onByteReceived();                     // HAL_UART_RxCpltCallback for USART1
void onReceiveError();                     // HAL_UART_ErrorCallback for USART1

}  // namespace midi

#endif
