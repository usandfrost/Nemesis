#ifndef __COST_GOVERNOR_H
#define __COST_GOVERNOR_H

// Decides WHEN to climb or descend the degrade ladder, from the measured load of each audio block (cycles used / cycles available).
// Climbing is quick (a dropout is audible), descending is slow and needs a clear margin (so it never flaps).
//
//   panic      one block this heavy climbs a step at once, even while settling (an overrun is a dropout); after a climb it waits
//              `panicHoldBlocks` so the new level can show in the measurement first
//   enter      the smoothed load stays at or above this for `enterBlocks` blocks -> climb a step
//   exit       the smoothed load stays at or below this for `exitBlocks` blocks -> descend a step
//   settle     blocks to ignore after a change, so the smoothed load reflects the new level before it is judged again
// A block is 32 samples = 0.73 ms at 44.1 kHz, so the defaults are: panic immediately, climb after ~47 ms, descend after ~1 s.
// All thresholds are guesses until the board is measured; they are plain constants for that reason.
namespace cost {

struct GovernorConfig {
    float panic = 0.97f;
    float enter = 0.85f;
    float exit = 0.60f;
    int enterBlocks = 64;
    int exitBlocks = 1400;
    int settleBlocks = 48;
    int panicHoldBlocks = 8;     // ~6 ms
    float smoothing = 1.0f / 16.0f;
};

class Governor {
   public:
    explicit Governor(const GovernorConfig& c = GovernorConfig()) : c_(c) {}

    // Call once per audio block. `maxLevel` = ladder length. Returns the (possibly new) level.
    int update(float load, int maxLevel) {
        ema_ += (load - ema_) * c_.smoothing;
        if (maxLevel < 0) maxLevel = 0;
        if (level_ > maxLevel) level_ = maxLevel;
        if (panicHold_ > 0) --panicHold_;
        else if (load >= c_.panic) { climb(maxLevel); return level_; }
        if (settle_ > 0) { --settle_; return level_; }
        if (ema_ >= c_.enter) { if (++high_ >= c_.enterBlocks) climb(maxLevel); } else high_ = 0;
        // low_ only counts while there is something to give back, so it cannot grow without bound at level 0
        if (level_ > 0 && ema_ <= c_.exit) {
            if (++low_ >= c_.exitBlocks) { --level_; low_ = 0; settle_ = c_.settleBlocks; panicHold_ = 0; }
        } else low_ = 0;
        return level_;
    }

    // Keep judging off for at least `blocks` (panic still acts). The renderer uses it after releasing a voice: its release tail keeps
    // costing CPU until it has faded, so the load would otherwise look unchanged and the governor would climb again for nothing.
    void holdFor(int blocks) { if (blocks > settle_) settle_ = blocks; }

    int level() const { return level_; }
    float smoothedLoad() const { return ema_; }
    void reset() { level_ = 0; ema_ = 0.0f; high_ = low_ = settle_ = panicHold_ = 0; }

   private:
    void climb(int maxLevel) {
        high_ = low_ = 0;
        if (level_ < maxLevel) { ++level_; settle_ = c_.settleBlocks; panicHold_ = c_.panicHoldBlocks; }
    }
    GovernorConfig c_;
    int level_ = 0;
    float ema_ = 0.0f;
    int high_ = 0, low_ = 0, settle_ = 0, panicHold_ = 0;
};

}  // namespace cost

#endif
