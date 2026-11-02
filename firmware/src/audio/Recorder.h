#ifndef __AUDIO_RECORDER_H
#define __AUDIO_RECORDER_H

#include <stdint.h>

// Takes sound into memory: what comes in at the line input, or what the Drumboy itself is playing. No HAL in here. The audio interrupt calls block() once per audio
// block; the main loop starts it, stops it and takes the result. A word each way, so nothing has to be held except the start.
namespace audio {

class Recorder {
   public:
    enum State : uint8_t { Idle, Armed, Capturing, Done };   // Armed: waiting for the input to pass the threshold
    enum Source : uint8_t { Line, Output };
    enum Channel : uint8_t { Both, Left, Right };

    static constexpr int kPreRoll = 2205;   // 50 ms before the trigger stay in the take: what crosses the threshold is rarely the very start of the sound

    struct Config {
        uint8_t source = Line, channel = Both;
        bool autoCapture = false;     // start when the input passes `threshold`
        float threshold = 0.05f;      // 0..1 of full scale
        float stopSeconds = 0.0f;     // stop after this long below the threshold, and leave that silence out. 0: only REC or a full memory stops it
        uint32_t rate = 44100;
    };

    // Main loop, with the audio held for the moment it takes. `buffer` holds `capacity` frames of mono.
    void start(int16_t* buffer, uint32_t capacity, const Config& c);
    void stop();              // Armed: back to Idle with nothing taken. Capturing: Done with what there is
    void clear();             // back to Idle

    void setMonitor(float gain) { monitor_ = gain; }     // the line input goes on to the outputs at this level (0: not at all)
    void setListening(bool on) { listening_ = on; }      // the meter runs, and the input is looked at at all
    void setSource(uint8_t s) { source_ = s; }           // what the meter shows while nothing is being taken

    // Audio interrupt, after the block is rendered. `rx`: the codec's words, left then right, for `frames` frames. `left` and `right`: the block going out.
    void block(const uint32_t* rx, float* left, float* right, int frames);

    State state() const { return (State)state_; }
    uint32_t frames() const { return used_; }
    uint32_t capacity() const { return capacity_; }
    const int16_t* data() const { return buffer_; }
    float peak() { const float p = peak_; peak_ = 0.0f; return p; }   // the highest level since the last look
    bool clipped() const { return clipped_; }

   private:
    void take(float x);
    int16_t pre_[kPreRoll];
    uint32_t preAt_ = 0;
    bool preFull_ = false;
    int16_t* buffer_ = nullptr;
    volatile uint32_t used_ = 0;
    uint32_t capacity_ = 0;
    Config c_;
    volatile uint8_t state_ = Idle;
    volatile uint8_t source_ = Line;
    volatile bool listening_ = false, clipped_ = false;
    volatile float monitor_ = 0.0f;
    volatile float peak_ = 0.0f;
    float env_ = 0.0f, decay_ = 0.999f;
    float dcInL_ = 0.0f, dcOutL_ = 0.0f, dcInR_ = 0.0f, dcOutR_ = 0.0f;
    int above_ = 0;
    uint32_t quiet_ = 0, quietNeed_ = 0;
};

}  // namespace audio

#endif
