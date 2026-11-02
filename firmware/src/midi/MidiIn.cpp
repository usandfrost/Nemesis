#include "midi/MidiIn.h"

#include "main.h"
#include "midi/MidiParser.h"
#include "usart.h"

// One byte lands here each time; the HAL's interrupt-driven receive is re-armed from the completion callback. At 31250 baud
// that is at most ~3100 interrupts a second, which is nothing. (The stock firmware re-arms with DMA after the first byte;
// plain interrupts are simpler and equally robust at this rate.)
uint8_t midiRxData = 0;

namespace midi {

SpscQueue<64> rxQueue;
Stats stats;

static Parser parser;
SysexIn sysexIn;

void setChannel(uint8_t ch) { parser.setChannel(ch); }

void start() {
    HAL_HalfDuplex_EnableReceiver(&huart1);
    HAL_UART_Receive_IT(&huart1, &midiRxData, 1);
}

void onByteReceived() {
    const uint8_t b = midiRxData;
    HAL_UART_Receive_IT(&huart1, &midiRxData, 1);   // re-arm first: never lose the next byte to our own processing
    ++stats.bytes;
    sysexIn.feed(b);
    Event e;
    if (parser.feed(b, e)) {
        if (e.type < Type::Clock) ++stats.messages;   // (a clock is not somebody playing: it does not keep the screen awake)
        rxQueue.push(e);   // a full queue drops the new event and counts it (rxQueue.dropped())
    }
}

void onReceiveError() {
    ++stats.errors;
    __HAL_UART_CLEAR_FLAG(&huart1, UART_CLEAR_OREF | UART_CLEAR_FEF | UART_CLEAR_NEF | UART_CLEAR_PEF);
    parser.reset();   // a corrupted byte may sit in the middle of a message
    sysexIn.reset();
    HAL_UART_Receive_IT(&huart1, &midiRxData, 1);
}

}  // namespace midi
