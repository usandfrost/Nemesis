#ifndef __APP_UI_H
#define __APP_UI_H

#include <stdint.h>

#include "app/CostBridge.h"
#include "app/Fmt.h"
#include "app/Model.h"
#include "app/PresetFile.h"
#include "app/SampleLib.h"
#include "app/Services.h"

// The panel's behaviour: modes, the eight knob bindings, the keys, p-locks, undo, toasts. A port of the mockup's S object, bindings(),
// pressKey(), layerDown()/layerUp() and their helpers; the toasts say the same words. It edits the Project in place and talks to the
// sequencer through Transport. Main loop only.
namespace app {

enum class Mode : uint8_t { Play, Step, Sound, Mixer, Browse, ProjectM, Song, Fx, Save, Record };

enum class Key : uint8_t {
    Stop, Play, Rec, Up, Add, Down, Left, Ok, Right, Copy, Clear, Paste, Mute, Fill, StepK,   // left keypad
    File, PartK, Mixer, Osc1, Osc2, Sub, Flt1, Flt2, Grit, Amp, ModK, Glob, Lane, Info, SongK, Shift,   // right keypad
    Spare, Count
};

// Shared with the sequencer (audio interrupt). The UI writes the requests; the sequencer owns the rest while playing.
struct Transport {
    volatile uint8_t playing = 0;
    volatile uint8_t songPlaying = 0;
    volatile uint8_t pat = 0;          // the pattern playing (and on screen)
    volatile int8_t nextPat = -1;      // queued: switches at the end of the current pattern
    volatile uint8_t fill = 0;         // FILL held
    volatile uint8_t songIdx = 0;
    volatile uint8_t clockExt = 0;     // the steps come from outside: GLOBAL's CLOCK is MIDI or SYNC
    volatile uint16_t extSteps = 0;    // steps the clock has given and the sequencer has not played
    volatile int8_t startRow = -1;     // the SONG screen is open: a START from outside plays the song from this row (the UI keeps it up to date)
    volatile uint8_t playRow = 0, repLeft = 0;
    volatile int8_t jumpTo = -1;       // a song row picked with OK while the song plays
    volatile uint16_t pstep = 0;       // step within the pattern
    volatile int32_t gstep = -1;       // steps since PLAY (swing alternates on it)
    volatile int16_t shownStep = -1;   // the step sounding now (the playhead)
    volatile uint32_t patChanges = 0;  // pattern switches made by the sequencer (the UI drops an open lock on each)
    volatile uint32_t songEnds = 0;    // a song that stops has ended
    volatile uint32_t playGen = 0;     // counts PLAY presses: a STOP + PLAY between two audio blocks is still a new start
    volatile uint32_t hitAtMs[kLanes] = {};   // when each lane last played (the layer LED flash)
};

// One knob as the screen shows it.
struct Cell {
    Str<20> label;
    Text value;
    Str<8> dest;           // the destination note after a step offset, drawn in the accent colour
    Str<16> tag;           // T1..B4, LOCK, INFO, SHIFT, a cost badge
    uint32_t col = 0;      // accent override (0xRRGGBB), 0 = the page accent
    float bar = -1.0f;     // the value bar 0..1, < 0 = none
    bool idle = false;     // dimmed (it has no effect in the patch as it stands) but still turnable
    bool pend = false;     // a sample on show that has not been picked yet (the folder knob has moved)
    bool dis = false, ro = false, lock = false, warn = false, costTag = false;
};

constexpr int kNoSlot = -128;   // Ui::loadedSlot: what is in memory was not loaded from anywhere

class Ui {
   public:
    Ui(Project& pj, Transport& tp, const SampleLib& lib, Services& sv) : pj_(pj), tp_(tp), lib_(lib), sv_(sv) { syncLoaded(); for (int16_t& s : catLast_) s = -1; }

    // ---- input (main loop)
    // For the debug hook (board/DebugHook.h): put `part` on screen, find preset `id` in the BROWSE list and load it, as OK does. False if there is
    // no such synth part or preset.
    bool debugLoadPreset(int part, int id);
    // For the debug hook: set a synth part's VOICES directly (the MIXER knob's setting), 1..8.
    void debugSetVoices(int part, int n);
    void keyDown(Key k);
    void keyUp(Key k);
    void layerDown(int i);
    void layerUp(int i);
    void turn(int encoder, int detents);
    void tick();                         // timers: the long press, transport events from the sequencer

    // ---- what the screen and the LEDs read
    void cells(Cell out[8]);
    // Where its value sits between its limits, 0..1, or below 0 if it has none. For a panel whose knobs have stops (the web version). `stop`: how far apart its values are on that
    // travel, 0 when they are too close to matter, so a drag between two values of a short list can go to the nearer one.
    float knobPlace(int encoder, float* stop = nullptr);
    Mode mode() const { return mode_; }
    int page() const { return page_; }
    int sel() const { return sel_; }
    int part() const { return part_; }
    int stepPage() const { return stepPage_; }
    int cursor() const { return cursor_; }
    int stepSub() const { return stepSub_; }
    // ---- the tracker: STEP's third view, the same steps as rows (UiTracker.cpp, docs/TRACKER.md)
    bool tracker() const { return mode_ == Mode::Step && stepSub_ == 2; }
    int trackerRow() const { return trRow_; }                 // the cursor's row. Its step is cursor(): a lane shorter than the row repeats
    int trackerRows() const { return patLen(pattern()); }
    bool popover() const { return trPop_ && tracker(); }
    int trackerStop() const { return trStop_; }               // the last move would have left +-24 semitones: +1 above, -1 below, else 0
    int trackerNote() const;                                  // the cursor's note, or the one an empty step would get
    bool inScale() const { return trScale_ && pj_.scale > 0 && !isSamplerPart(lane().part); }
    void trackerSample(Text& name, Text& folder, bool& pending) const;   // a sampler lane's popover: the sample of the cursor's step
    // The loop of the sample on screen, as it plays now. `lap`: one time round. `xfade`: the XFADE shown (a step can lock it). `sample`: the one shown (a step can lock that too).
    bool shownLoop(uint32_t& start, uint32_t& end, double& fade, double& lap, int xfade, int sample) const;
    const SliceMap* shownSlices() const;   // the slices of the sample on screen (nullptr: none)
    int sliceSel() const { return sliceSel_; }
    bool sliceEdit() const { return sliceEdit_ && !lockOn_; }
    const RecStatus& recording() const { return rec_; }
    const RecSettings& recSettings() const { return recSet_; }
    float recLevel() const { return recLevel_; }   // the meter, 0..1
    int mixSub() const { return mixSub_; }
    void settle();   // the UI's state made valid for the model as it is now (also before drawing)
    int fxView() const { return fxView_; }     // FX: which of the six views (FxView)
    // SAVE PRESET: the name being typed (20 characters, padded with spaces), the cursor in it, the category, whether it replaces the preset the part
    // was loaded from, and the id a new preset would get (-1: no room or no card)
    const char* saveName() const { return saveName_; }
    int saveCursor() const { return saveCur_; }
    int saveCategory() const { return saveCat_; }
    bool saveReplaces() const { return saveOver_ && saveCanReplace(); }
    int saveSource() const { return saveSrc_; }
    int saveTargetId() const { return saveReplaces() ? saveSrc_ : presetNextId(); }
    int projSub() const { return projSub_; }   // PROJECT: 0 key, scale and card; 1 the lanes' MIDI
    int midiLane() const { return midiLane_; }
    bool shift() const { return shift_; }
    bool fill() const { return tp_.fill != 0; }
    bool lockOpen() const { return lockOn_ && lockPat_ == tp_.pat; }
    int lockLane() const { return lockLane_; }
    int lockIdx() const { return lockIdx_; }
    int slot() const { return slot_; }
    int loadedSlot() const { return loadedSlot_; }
    bool exporting() const { return exporting_; }
    const char* exportNote() const { return exportNote_.c_str(); }   // how the last one ended, on EXPORT until the next
    const char* exportPlace() { return sv_.exportPlace(); }
    const ExportStatus& exportStatus() const { return export_; }
    int exportSongIndex() const { return exportSong_; }
    int exportKind() const { return exportKind_; }
    int deleteAsked() const { return deleteAsked_; }   // the slot CLEAR is asking about, -1: none
    int browseCat() const { return browseCat_; }
    int browseIdx() const { return browseIdx_; }
    int browseLoaded(int part) const { return loaded_[part]; }
    int songRow() const { return songRow_; }
    bool autoDegrade() const { return autoDegrade_; }
    const char* toast() const;           // null when no toast is showing
    bool laneFlashing(int lane) const;
    int flashLeftMs(int lane) const { return laneFlashing(lane) ? (int)(flashUntil_ - sv_.nowMs()) : 0; }
    uint32_t revision() const { return rev_; }   // changes whenever the screen may need redrawing

    Pattern& pattern() { return pj_.patterns[tp_.pat]; }
    const Pattern& pattern() const { return pj_.patterns[tp_.pat]; }
    Lane& lane() { return pattern().lanes[sel_]; }
    const Lane& lane() const { return pattern().lanes[sel_]; }
    bool isSamplerPart(int p) const { return isSampler(pj_, p); }
    // the lane whose sampler pages are edited for the current part (the selected lane if it plays on it)
    int curLaneIndex() const;
    // the page set being edited: a part's synth pages, or a sampler lane's pages plus the part's GLOBAL page
    bool pageExists(int page) const;
    bool partHasLane(int part) const;
    void refreshLoops();    // the lanes on an AUTO loop have its points (one search a call, when the knobs have stopped)
    void refreshSlices();   // the lanes that want slices have them, for the sample as it is; one cut a call
    bool cutSlices(Lane& l, int smp, bool asked);
    void rememberSlices(const SliceMap& m, bool fresh);
    void recKey();                 // REC, on the REC screen: start, stop, or start over
    void recLeave();               // off the REC screen: a take is kept, a wait given up, the input off
    void recKeepTake(bool toLane);
    int16_t& pageValue(int page, int param);
    int shownValue(int page, int param);   // what a cell shows: the open step lock's value if it has one for this knob, else the part's
    int paramMax(int page, int param) const;
    int paramMin(int page, int param) const;
    // BROWSE: the list on screen (presets for a synth part, samples for a sampler part)
    int browseCount() const;
    int browseItem(int i) const;         // preset or sample index of list row i
    int browseCatCount() const;
    const char* browseCatName(int c) const;
    // the step the open lock edits
    Step* lockStep();
    // song helpers
    Song& song() { return pj_.songs[tp_.songIdx]; }
    const Song& song() const { return pj_.songs[tp_.songIdx]; }
    double stepDur() const { return 60.0 / pj_.bpm / 4.0; }
    double songBars(const Song& s) const;

    // ---- the cost model's view, for the MIXER
    const Ladder& ladder();              // rebuilt when the cost-relevant settings change
    int degradeLevel() { return autoDegrade_ ? sv_.degradeLevel() : 0; }

    Project& project() { return pj_; }
    const Project& project() const { return pj_; }
    Transport& transport() { return tp_; }
    const SampleLib& samples() const { return lib_; }
    Services& services() { return sv_; }

    void say(const char* msg);           // a toast
    // the card's slots as last read (after boot, a save, a load, an undo); the PROJECT screen draws from this, not from the card
    bool slotInfo(int slot, SlotInfo& out);
    void refreshCard();
    void dirty() { ++rev_; }

   private:
    // ---- the binding table: what each knob does in this mode
    enum class B : uint8_t {
        None, Tempo, Swing, PlayedBy, PatternSel, Root, LaneSample, Folder, Level, Length, Hits, Rotate, Note, Velocity, GateB, Prob,
        ExportSong, ExportKindB, SongSel, SongRowB, SongPat, SongRep, SongEnd, SongLen, SongTime, KeyB, ScaleB, CardSlot, StepSample, Pitch,
        ArpB, ArpRate, ArpOct, ArpGate, ChordB, Inv, Voicing, Strum, Param, Prio, AutoDeg, CpuLoad, DegradeB,
        MixLevel, Master, Voices, CostB, Bank, Category, BrowsePart, Instrument, Scroll, Scroll3, BrowseOk, BrowsePreview, MidiLane, MidiCh, MidiNote, MidiShared, FxParam, FxPart,
        SvCursor, SvChar, SvCat, SvMode, SvTarget, SvVoices, SvPart,
        StepSlice, SliceSel, SliceMove, SlicePitch, SliceLevel, SliceRev, SliceOn, SliceSplit, SliceMerge,
        RecSource, RecGain, RecCapture, RecThreshold, RecChannel, RecMonitor, RecStopB, RecRoom,
        TrNote, TrOctave, TrInScale
    };
    struct Bind {
        B id = B::None;
        int8_t arg = 0;
    };
    void bindings(Bind out[8]) const;
    void baseBindings(Bind out[8]) const;
    void describe(const Bind& b, int i, Cell& c);
    void apply(const Bind& b, int d);

    // ---- actions (the mockup's functions of the same names)
    void pressKey(Key k);
    void setMode(Mode m, int page = -1);
    void goBack();
    void selectLane(int i);
    void selectPattern(int p);
    void pressLayer(int i);
    void fixPage();
    void fixSongRow();
    void syncLoaded() { for (int i = 0; i < kParts; ++i) loaded_[i] = pj_.parts[i].preset; }   // the BROWSE list's PART tags
    void ensurePage();
    bool listMove(int dir);
    void browseOpen();
    void browseCatMove(int dir);
    void browseCatStep(int dir);
    void previewBrowse();
    // ---- the sample knobs: a folder, then a sample in it (docs/SAMPLE_BROWSER.md). SHIFT makes a sample knob the folder knob.
    struct Pick { int lane = 0, step = -1; };      // what a knob picks for: a lane's own sample, or (step >= 0) a step's sample lock
    enum : int8_t { PickLane, PickStep, PickPage };   // which knob: a B::Root, a B::StepSample, the SAMPLE page's
    Pick pickFor(int knob) const;
    int laneSampleNow(int lane) const;             // the sample a lane plays, in range
    int pickNow(const Pick& p) const;              // the sample it has, -1: a step with no lock of its own, it plays the lane's
    int pickFolder(const Pick& p) const;           // the folder the knob is in
    bool pickMoved(const Pick& p) const;           // the folder knob has left the sample's folder: nothing is picked there yet
    int pickShown(const Pick& p) const;            // what the sample knob takes next if it has nothing to step from
    void pickTurn(const Pick& p, int dir);
    void pickFolderTurn(const Pick& p, int dir);
    void pickPut(const Pick& p, int sample);
    void pickCell(const Pick& p, Cell& c);
    void pickForget();                             // a new project: every knob starts in the folder of its sample again
    void setLaneSample(int lane, int sample, bool run);   // `run`: a run of turns is one undo
    void auditionSoon(int sample);
    void auditionGo(int sample);
    void start();
    void stop();
    void loadSlot();
    void saveSlot();
    void deleteSlot();
    void exportSong();
    void loaded();
    void refreshSlot(int i);
    void clearKey();
    void emptyPattern();
    void copyKey();
    void pasteKey();
    void okKey();
    void addKey();
    // ---- the tracker (UiTracker.cpp)
    void trackerFix();                   // from settle(): the cursor's step is the row's, a popover goes when its cell does
    void trackerEnter();                 // STEP pressed into the view: the row of the step the editor was on
    bool trackerKey(Key k);              // false: the key does what it does elsewhere
    void trackerOkUp();
    void trackerClear();
    void trackerLayerDown(int i);
    void trackerTouch(B id);             // a knob is about to edit the cursor's step
    void trackerMove(int d, bool octave);
    void trackerOpen();
    void trackerClose();
    void trackerNoTap() { if (trOkDown_) trOkUsed_ = true; }
    void trackerSnap(const char* label, bool fresh);
    Step& trackerBegin();
    // ---- SAVE PRESET (SHIFT + OK in BROWSE or on the sound pages): the part's sound goes to the card as a new preset or over the one it was loaded from
    bool saveCanReplace() const { return saveSrc_ >= kPresetIdMin && presetExists(saveSrc_); }
    void openSave();
    void saveInitName();
    void saveStepChar(int d);
    void saveCommit();
    void saveRecord(PresetRecord& rec) const;
    bool sendUsedElsewhere(int e) const;

    // ---- undo: one level, for the actions that throw work away (SHIFT + CLEAR)
    enum class U : uint8_t { None, LaneClear, LaneSnap, PatternSnap, SongSnap, SongRow, PartPages, LaneSample, StepSample, PartType, ProjectSnap, SlotDelete, CardSave, FxSnap, Slices, StepSnap };
    struct Undo {
        U kind = U::None;
        Str<40> label;
        uint8_t pat = 0, lane = 0, part = 0, idx = 0;
        uint8_t eu = 0, ov[kMaxSteps] = {};
        int16_t i16a = 0;
        int8_t i8a = 0, i8b = 0;
        int16_t i16b = 0;
        SongRow row = {};
    };
    void remember(U kind, const char* label);
    void undo();

    Project& pj_;
    Transport& tp_;
    const SampleLib& lib_;
    Services& sv_;

    Mode mode_ = Mode::Play;
    int page_ = PG_OSC1;
    Mode backMode_ = Mode::Play;
    int backPage_ = PG_OSC1;
    bool browseSmp_ = false;   // the part type the browse category was picked for
    int sel_ = 0, part_ = 0, stepPage_ = 0, cursor_ = 0, stepSub_ = 0, mixSub_ = 0, projSub_ = 0, midiLane_ = 0, fxView_ = 0;
    bool shift_ = false;
    bool lockOn_ = false, lockEdited_ = false;
    int lockPat_ = 0, lockLane_ = 0, lockIdx_ = 0;
    int trRow_ = 0, trPopRow_ = 0, trPopLane_ = 0, trPopPat_ = 0;   // the tracker's cursor row, and the cell the popover belongs to
    bool trPop_ = false, trOkDown_ = false, trOkUsed_ = false;      // OK is a tap when it comes up with nothing done while it was down
    bool trRun_ = false;        // a run of edits under one undo: it ends when the popover closes
    bool trScale_ = false;      // IN SCALE (not in the project: it is how you are working, not what the song is)
    int8_t trStop_ = 0;
    int8_t trLast_[kLanes] = {};   // the last note entered on each lane (lanes have roots of their own)
    Step undoStep_ = {};
    bool laneDown_ = false;     // the L key is held: a long hold goes home from wherever the player is
    uint32_t laneDownAt_ = 0;
    bool clearDown_ = false;    // CLEAR on a lane is held, nothing else has been touched: a long hold empties the pattern
    uint32_t clearDownAt_ = 0;
    Undo clearFrom_;            // what the press that started the hold took (kind None: nothing), and on which pattern
    struct Held { bool on = false; int i = 0, idx = 0; bool latch = false, edited = false, lng = false; uint32_t at = 0; bool tracker = false; } held_;   // tracker: `i` is a lane, and it holds one cell
    uint32_t lastClear_ = 0;
    bool hasClearPress_ = false;
    int browseCat_ = 0, browseIdx_ = 0;
    char saveName_[kPresetNameMax + 1] = {};   // SAVE PRESET
    int saveCur_ = 0, saveCat_ = 1, saveSrc_ = -1;
    bool saveOver_ = false, saveEdited_ = false, saveTouched_ = false;
    uint32_t saveArmUntil_ = 0, saveOpenedAt_ = 0;                // a replace asks for OK twice, within a few seconds
    int16_t loaded_[kParts] = {-1, -1, -1, -1, -1, -1, -1};   // the preset each part last loaded in BROWSE (the list's PART tags)
    int slot_ = 0, loadedSlot_ = kNoSlot;   // below 0: a factory project (F1 is -kFactoryProjects)
    int deleteAsked_ = -1;              // a delete asks for CLEAR twice, within a few seconds
    int exportSong_ = 0, exportKind_ = EXPORT_WAV;
    bool exporting_ = false;            // only STOP does anything meanwhile
    ExportStatus export_;
    Text exportNote_;
    uint32_t deleteAskedAt_ = 0;
    bool deleteUnread_ = false;         // it cannot be read, so it would go for good: that has been said
    int songRow_ = 0;
    bool autoDegrade_ = true;
    Str<96> toast_;
    uint32_t toastUntil_ = 0;
    int flashLane_ = -1;
    uint32_t flashUntil_ = 0;
    uint32_t seenPatChanges_ = 0, seenSongEnds_ = 0;
    int previewPending_ = -1;
    uint32_t previewAt_ = 0;
    int auditionWait_ = -1;                    // the sample to audition when the knob has stopped (each audition reads the card: tens of ms)
    uint32_t auditionAt_ = 0, auditionLast_ = 0;
    uint8_t laneFolder_[kLanes] = {};          // where the folder knob of a lane's own sample has gone, 0: the sample's own folder
    uint8_t stepFolder_[kLanes] = {};          // the folder the lane's steps were last picked in, 0: none yet, they start in the lane's
    int stepPlace(const Pick& p) const { return (tp_.pat * kLanes + p.lane) * kMaxSteps + p.step; }
    int stepMoved_ = -1;                       // the locked step the folder knob has taken out of its lock's folder (stepPlace), -1: none
    int16_t catLast_[kMaxSampleCats];          // the sample last picked in each folder, -1: none yet
    uint32_t rev_ = 1;
    bool toastShown_ = false, flashShown_ = false;
    int lastShown_ = -1;
    uint32_t lastLive_ = 0, lastIdle_ = 0, busyUntil_[kParts] = {};
    int lastDegrade_ = 0;
    SlotInfo slots_[kCardSlots];
    bool slotHas_[kCardSlots] = {};
    bool cardRead_ = false;

    // clipboards and undo storage (big: the board allocates the Ui in SDRAM)
    Lane clip_;
    bool hasClip_ = false;
    Pattern clipPat_;
    bool hasClipPat_ = false;
    Song clipSong_;
    bool hasClipSong_ = false;
    Undo undo_;
    Lane undoLane_;
    Pattern undoPattern_;
    Song undoSong_;
    FxSettings undoFxAll_;                 // the effects as they were before a preset was loaded
    FxSettings postFx_;                    // ... and as the load left them (undo gives back only what has not been edited since)
    bool sliceEdit_ = false;
    int sliceSel_ = 0;
    int sliceCutFor_ = -1;     // the sample whose SENS or GRID moved: cut anew when the knob has stopped
    uint32_t sliceCutAt_ = 0;
    RecSettings recSet_;
    RecStatus rec_;
    float recLevel_ = 0.0f;
    uint8_t recSeen_ = REC_IDLE;
    bool recDropAsked_ = false;    // REC on a finished take has said it would drop it
    uint32_t lastRec_ = 0;
    uint32_t loopKnobAt_ = 0;  // the last turn of a knob that moves a loop
    SliceMap undoSlices_ = {};
    int16_t undoFxVals_[kFxParams] = {};   // the values a reset replaced (undo_.idx: the view, undo_.part: the part)
    int16_t undoPages_[kSynthPages][kParamsPerPage];
    Project* undoProject_ = nullptr;     // a whole project (load, a deleted slot): allocated on first use

    Ladder ladder_;
    uint32_t ladderSig_ = 0;
};

}  // namespace app

#endif
