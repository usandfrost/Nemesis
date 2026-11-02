#ifndef __BOARD_CARD_H
#define __BOARD_CARD_H

#include <stddef.h>
#include <stdint.h>

#include "app/Model.h"
#include "app/SampleLib.h"
#include "app/Params.h"
#include "app/Services.h"
#include "app/WaveLib.h"
#include "audio/MemoryPool.h"
#include "audio/WaveFile.h"

// The SD card (SDMMC2 + FatFs, both as the stock firmware sets them up): the the project slots in /CATHODE, and the stock card's
// samples in /Sample/<category>/*.wav. The samples are indexed at boot (name, folder: sorted, so a project's sample numbers stay the
// same while the card does) and their audio is loaded into an SDRAM cache when a project or a lane needs it; a sample browsed or
// auditioned goes to a separate buffer that the next audition overwrites, so browsing never fills the cache. Main loop only.
//
// Starting up reads the card and writes nothing. Everything that writes goes through beginWrite(): the first time after a start that reads the whole file system
// (storage/CardCheck.h), and a card that fails it, or one that gave a write error, is only read from then on, until the next start. Nothing is ever repaired or
// formatted. A save either completes, read back from the card, or leaves the previous version in place: docs/STORAGE.md.
namespace board {

class Card {
   public:
    bool mount();                       // false: no card, or not readable
    bool mounted();                     // also notices a card that was pulled, or that a write failed
    bool beginWrite();                  // false: the card must not be written; see writeProblem()
    bool writesCleared() const { return mounted_ && checked_ && !readOnly_; }   // beginWrite() has passed (and will not start a check now)
    const char* writeProblem();         // why not, in a few words for the screen; nullptr if nothing is known to be wrong
    const char* checkAgain();           // the whole file system read again now (a test): what is wrong with it, or nullptr

    enum class Load : uint8_t { Ok, Bad, Full, NoCard };

    // ---- samples
    // Index every WAV on the card into `lib` (after the built-in ones it already holds). Returns how many were found.
    int indexSamples(app::SampleLib& lib);
    // Sample `i`'s audio into the cache (reads it now if needed: tens of ms).
    Load ensureLoaded(app::SampleLib& lib, int i);
    // Sample `i`'s audio somewhere it can be auditioned: the cache if it is there, else the audition buffer.
    Load loadForAudition(app::SampleLib& lib, int i);
    // Every sample a project uses (lane samples, sample locks), in a fresh cache. Call with the sound stopped.
    void loadProjectSamples(app::SampleLib& lib, const app::Project& pj);
    // The cache is full: drop the samples the project no longer uses and close the gaps. Call with the samplers stopped (their voices
    // read from memory that moves). Returns true if room was made.
    bool evictUnused(app::SampleLib& lib, const app::Project& pj);
    bool canEvict(const app::SampleLib& lib, const app::Project& pj);   // the cache holds something the project no longer uses

    // ---- the memory the samples and the wavetables share
    // Sizes the pool from what SDRAM has left. `arena`: where the wavetable slots go, the top of it, `slots` of them at most. Call once, before anything loads.
    void openPool(size_t slotBytes, int& slots, void*& arena);   // `slots` comes back as how many fit
    audio::MemoryPool& pool() { return pool_; }
    // The free memory between the samples and the wavetables, for a recording. While it is lent out nothing loads or moves.
    int16_t* lendPool(size_t& frames);
    void endLend() { lent_ = false; }
    // A recording that ends here, in the lent memory, becomes a sample: saved to /Sample/REC when there is a card, on the list either way. The sample's number, or -1.
    int keepRecording(app::SampleLib& lib, uint32_t frames, uint32_t rate);
    int lastSaveError() const { return saveError_; }   // why the last take did not reach the card (0: it did)

    // ---- wavetable banks: /CATHODE/WT/wtNNN.wtp (tools/wt_pyramid writes them)
    int scanWaves();                      // marks app::waveSet().present for the banks the card has; returns how many
    audio::WaveFile& waves();             // the files, for audio::WaveStore (main loop only; it shares the card's one file object)

    // ---- the preset library: /CATHODE/PRESETS/*.PRS and INDEX.PIX (app::presetsOpen reads them through this)
    app::PresetStorage& presets();

    // ---- project slots (app::Services)
    bool slotInfo(int slot, app::SlotInfo& out);
    bool saveSlot(int slot, const app::Project& pj, const app::SampleLib& lib);
    bool loadSlot(int slot, app::Project& pj, app::Project& scratch, const app::SampleLib& lib, int& missing);
    bool undoSave(int slot);
    bool deleteSlot(int slot);
    // A file too long for memory, written as it is made (audio/SongExport.h): a new file, never one that is there. A WAV's header is put right at the end, by exportRewrite.
    // Closing reads all of it back. False: the file is taken away if the card still takes writes. Otherwise part of it stays, for a computer to delete.
    bool exportOpen(const char* folder, const char* name);
    bool exportWrite(const void* data, size_t n);
    bool exportRewrite(uint32_t at, const void* data, size_t n);
    bool exportClose(bool keep);
    const char* exportProblem() const;  // why the last of these failed, when it was not the card as a whole
    int ensureProjectSamples(app::SampleLib& lib, const app::Project& pj);   // what a render needs, loaded without dropping anything: how many could not be
    bool loadFellBack() const { return fellBack_; }   // the last load's newest save did not hold up, and the one before it was loaded

   private:
    void allocate();
    bool writeWav(const char* path, const int16_t* data, uint32_t frames, uint32_t rate);
    Load readWav(const app::SampleItem& it, int16_t* out, size_t capacity, uint32_t& frames, uint32_t& rate);
    void resetCache(app::SampleLib& lib);
    void finishPresetReplace();
    bool mounted_ = false, readOnly_ = false, checked_ = false;
    bool fellBack_ = false;
    uint32_t savedGeneration_[app::kCardSlots] = {};   // what undoSave may take back: the save this session made, and nothing else
    // the sample cache: one SDRAM block filled from the start, emptied as a whole (sized from what SDRAM has left)
    int16_t* cache_ = nullptr;
    size_t cacheFrames_ = 0, cacheUsed_ = 0;
    audio::MemoryPool pool_;
    bool lent_ = false;
    int saveError_ = 0;
    // the audition buffer: the last sample browsed, overwritten by the next
    int16_t* audition_ = nullptr;
    size_t auditionFrames_ = 0;
    int auditionItem_ = -1;
    // the file paths of the indexed samples ("Kick/Altair_1.wav"), one pool
    char* paths_ = nullptr;
    size_t pathsCap_ = 0, pathsUsed_ = 0;
    int firstCardSample_ = 0;
};

extern Card card;

}  // namespace board

#endif
