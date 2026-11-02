#ifndef __CYCLE_COUNTER_H
#define __CYCLE_COUNTER_H

#include <stdint.h>

// DWT cycle counter (Cortex-M7). One cycle = 1/550 MHz = 1.82 ns. Wraps after ~7.8 s; only ever used
// for differences, which stay correct across a single wrap.
namespace perf {

inline void cycleCounterEnable() {
    *(volatile uint32_t*)0xE000EDFC |= (1u << 24);   // DEMCR.TRCENA
    *(volatile uint32_t*)0xE0001FB0 = 0xC5ACCE55;    // DWT_LAR unlock (harmless where not present)
    *(volatile uint32_t*)0xE0001004 = 0;             // CYCCNT
    *(volatile uint32_t*)0xE0001000 |= 1u;           // DWT_CTRL.CYCCNTENA
}

inline uint32_t cycles() { return *(volatile uint32_t*)0xE0001004; }

// Running min / max / average of cycles-per-block, reset on read. Written from the audio interrupt,
// read from the main loop: single-writer 32-bit fields, so no lock is needed.
struct BlockStats {
    volatile uint32_t last = 0;
    volatile uint32_t min = 0xFFFFFFFFu;
    volatile uint32_t max = 0;
    volatile uint64_t sum = 0;
    volatile uint32_t count = 0;
    volatile uint32_t overruns = 0;   // blocks that took longer than the sample period allows

    void add(uint32_t c, uint32_t budgetCycles) {
        last = c;
        if (c < min) min = c;
        if (c > max) max = c;
        sum += c;
        ++count;
        if (c > budgetCycles) ++overruns;
    }
    uint32_t average() const { return count ? (uint32_t)(sum / count) : 0; }
    void reset() { min = 0xFFFFFFFFu; max = 0; sum = 0; count = 0; }
};

}  // namespace perf

#endif
