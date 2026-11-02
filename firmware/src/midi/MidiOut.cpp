#include "midi/MidiOut.h"

#include "main.h"
#include "midi/MidiOutRing.h"
#include "usart.h"

namespace midi {

OutStats outStats;

// 1 KB: ~0.3 s of notes. The reserve keeps room for the note-off of every note the instrument can have out.
static OutRing<1024> ring(kNoteOffReserve);
static OutMerge<kLongMax> merge;   // and the preset protocol's messages, one at a time

uint32_t outDropped() { return ring.dropped(); }

// USART6's transmit-empty interrupt (the HAL's IRQ handler calls huart->TxISR while TXEIE is set): the next byte, or, with the ring
// empty, the interrupt off. The audio interrupt preempts this one and may queue a message between the check and the clear, so the
// ring is looked at again after the clear.
static void txIsr(UART_HandleTypeDef* huart) {
    uint8_t b;
    if (!merge.next(ring, b)) {
        ATOMIC_CLEAR_BIT(huart->Instance->CR1, USART_CR1_TXEIE_TXFNFIE);
        __DMB();
        if (!merge.next(ring, b)) return;
        ATOMIC_SET_BIT(huart->Instance->CR1, USART_CR1_TXEIE_TXFNFIE);
    }
    huart->Instance->TDR = b;
    ++outStats.bytes;
}

void startOut() {
    huart6.TxISR = txIsr;   // first: TXEIE with no hook would make the HAL's handler return without clearing it, forever
    HAL_HalfDuplex_EnableTransmitter(&huart6);
}

bool send(const uint8_t* bytes, int n) {
    if (n <= 0 || !ring.put(bytes, (uint32_t)n)) return false;
    ATOMIC_SET_BIT(huart6.Instance->CR1, USART_CR1_TXEIE_TXFNFIE);
    return true;
}

bool sendLong(const uint8_t* bytes, int n) {
    if (n <= 0 || !merge.set(ring, bytes, (uint32_t)n)) return false;
    ATOMIC_SET_BIT(huart6.Instance->CR1, USART_CR1_TXEIE_TXFNFIE);
    return true;
}

}  // namespace midi
