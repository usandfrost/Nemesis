#ifndef __AUDIO_WAVE_FILE_H
#define __AUDIO_WAVE_FILE_H

#include <stddef.h>

namespace audio {

// Where a bank's file comes from: the card on the board, a directory in the host tests.
class WaveFile {
   public:
    virtual ~WaveFile() {}
    virtual bool open(int bank) = 0;                 // wtNNN.wtp
    virtual bool read(void* dst, size_t bytes) = 0;  // exactly that many, or false
    virtual void close() = 0;
};

}  // namespace audio

#endif
