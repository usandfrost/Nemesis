#ifndef __AUDIO_RENDERER_H
#define __AUDIO_RENDERER_H

// Anything that can fill one block of stereo float samples in [-1, 1]. The CATHODE voice engine and the whole instrument plug in
// behind this; the first milestone used a test tone. No HAL: the same renderers run in the host tests.
class AudioRenderer {
   public:
    virtual ~AudioRenderer() {}
    // Called once at the start of every block, in the audio interrupt, before render(). The place to apply queued control
    // changes (MIDI): it runs in the same context as render(), so the engine needs no locking.
    virtual void beginBlock() {}
    virtual void render(float* left, float* right, int frames) = 0;
};

#endif
