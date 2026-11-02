#ifndef __AUDIO_SONGEXPORT_H
#define __AUDIO_SONGEXPORT_H

#include <stddef.h>
#include <stdint.h>

#include "app/Model.h"
#include "app/SampleLib.h"
#include "app/Services.h"
#include "app/Ui.h"
#include "audio/Instrument.h"

// A song to files: one WAV, a WAV for each lane that plays, or a MIDI file. No HAL in here, and no card: the files go through ExportFiles. It runs in slices from the main
// loop, so the screen stays alive and STOP works. The caller has taken the instrument off the audio interrupt, so nothing can be heard, and nothing can be dropped either:
// the governor is off and a block takes as long as it takes.
//
// A stem is the whole song played once more with only one lane let through. The sequencer runs all of it each time from the same seed, so a step's PROB falls the same
// way in every stem and in the mix. Each stem keeps its effects. The stems do not add up to the mix exactly: the master compressor hears one lane at a time.
namespace audio {

class ExportFiles {
   public:
    virtual ~ExportFiles() {}
    virtual bool open(const char* folder, const char* name) = 0;   // a new file, never one that is there
    virtual bool write(const void* data, size_t n) = 0;
    virtual bool rewrite(uint32_t at, const void* data, size_t n) = 0;   // (the WAV header, once the length is known)
    virtual bool close(bool keep) = 0;                             // keep: false means the file is taken away. Keeping checks what reached the file
};

class SongExport {
   public:
    // `scratch` is a project this may write over (the MIDI file needs a copy)
    SongExport(app::Project& pj, app::Transport& tp, Instrument& inst, const app::SampleLib& lib, app::Project& scratch, ExportFiles& files)
        : pj_(pj), tp_(tp), inst_(inst), lib_(lib), scratch_(scratch), files_(files) {}

    bool start(int kind, int song);
    const char* why() const { return why_; }     // why it did not start, when that is not the card's doing
    void setMissing(int samples) { missing_ = samples; }   // samples the project uses that could not be loaded: it renders without them, and says so at the end
    bool step(app::ExportStatus& s, int blocks = 120);   // some more of it. False: it is over, and s.result says how
    void cancel() { cancel_ = true; }
    static bool lanePlays(const app::Project& pj, int song, int lane);

   private:
    bool nextFile();
    void beginPass();
    void startSong();
    void endPass();
    bool finishFile();
    void finish(uint8_t result);

    app::Project& pj_;
    app::Transport& tp_;
    Instrument& inst_;
    const app::SampleLib& lib_;
    app::Project& scratch_;
    ExportFiles& files_;

    int kind_ = 0, song_ = 0, lane_ = -1, stem_ = 0, stems_ = 0;
    uint8_t lanes_[app::kLanes] = {};
    uint8_t savedLoop_ = 0, savedPat_ = 0, savedSong_ = 0, savedRow_ = 0, savedRep_ = 0;
    int8_t savedNext_ = -1;
    bool savedDegrade_ = true, drain_ = false;
    uint32_t drained_ = 0;
    int missing_ = 0;
    const char* why_ = nullptr;
    bool cancel_ = false, open_ = false, tail_ = false;
    uint8_t result_ = app::EXPORT_RUNNING;
    uint32_t frames_ = 0, songFrames_ = 0, quiet_ = 0, tailFrames_ = 0;
    int have_ = 0;
    static constexpr int kBufferFrames = 1024;
    int16_t buffer_[2 * kBufferFrames];
};

}  // namespace audio

#endif
