// Copyright 2012 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Fast 16-bit pseudo random number generator.

#ifndef STMLIB_UTILS_RANDOM_H_
#define STMLIB_UTILS_RANDOM_H_

#include "stmlib/stmlib.h"

namespace stmlib {

class Random {
 public:
  static inline uint32_t state() { return rng_state_; }

  static inline void Seed(uint32_t seed) {
    rng_state_ = seed;
  }

  static inline uint32_t GetWord() {
    rng_state_ = rng_state_ * 1664525L + 1013904223L;
    return state();
  }
  
  static inline int16_t GetSample() {
    return static_cast<int16_t>(GetWord() >> 16);
  }

  static inline float GetFloat() {
    return static_cast<float>(GetWord()) / 4294967296.0f;
  }

 private:
  // >>> UPSTREAM VENDORED PATCH (T-CORE-28) — was `static uint32_t rng_state_;`
  // with its definition in random.cc. A process-global LCG state that GetWord()
  // read-modify-writes is a data race the moment two threads render: found by
  // ThreadSanitizer (290 warnings across random.h:41 Seed and random.h:45
  // GetWord) once the first-party Project seed race stopped aborting the test
  // ahead of it. It is not only formal UB — MacroOscillator::renderBlock Seeds
  // this global per block and Plaits then draws from it for the whole render,
  // so two concurrent renders also cross-contaminate the deterministic stream
  // every golden rests on.
  //
  // thread_local is behaviour-preserving for a single renderer: the state is
  // fully determined by the per-block Seed, so nothing carries across threads
  // that was not already re-seeded. Proven, not assumed — the full debug suite
  // (1483 tests, every dsp golden) is bit-identical with this patch.
  //
  // Declared `static inline` HERE rather than defined in random.cc so the
  // specifier lives on exactly one line: a definition must repeat
  // thread_local, so the two-file form could only be mutated by editing both
  // files at once, and a battery cannot do that — the mutation would always
  // BUILD-FAIL, which is a broken experiment rather than a result (T-B20).
  static inline thread_local uint32_t rng_state_ = 0x21;
  // <<< end patch

  DISALLOW_COPY_AND_ASSIGN(Random);
};

}  // namespace stmlib

#endif  // STMLIB_UTILS_RANDOM_H_
