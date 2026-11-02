"use strict";
// The Learn tab: lessons beside the real panel. A lesson is a list of steps; a step lights one control and waits until the firmware's own state (wb_state, read with every
// screen frame) shows it was done. Nothing here simulates the Drumboy: each lesson starts from a Drumboy as it boots (freshDrumboy), then its `project` is keyed in on the panel.
const MODE = { Play: 0, Step: 1, Sound: 2, Mixer: 3, Browse: 4, Project: 5, Song: 6, Fx: 7, Save: 8, Record: 9 };
const PAGE = { OSC1: 0, OSC2: 1, SUB: 2, FLT1: 3, FLT2: 4, GRIT: 5, AMP: 6, MOD: 7, GLOB: 8, SMP: 9, SFLT: 10, SAMP: 11, SLC: 12, LOOP: 13 };
const readState = (a) => ({ mode: a[0], part: a[1], lane: a[2], page: a[3], playing: a[4], step: a[5], pattern: a[6], shift: a[7], fxView: a[8], stepPage: a[9],
  cursor: a[10], browseCat: a[11], browseIdx: a[12], steps: a[13], muted: a[14], cutoff: a[15], level: a[16], length: a[17], euclid: a[18], hits: a[19],
  preset: a.slice(20, 23), partLevel: a.slice(23, 26), voices: a.slice(26, 29), master: a[29], bpm: a[30], songRows: a[31], songPlaying: a[32], songRow: a[33], rowPattern: a[34], rowRepeat: a[35],
  lockOpen: a[36], lockStep: a[37], locks: a[38], note: a[39], velocity: a[40], gate: a[41], mixView: a[42], stepView: a[43], sends: a.slice(44, 47), amp: a[47], knob: a.slice(48, 56),
  partType: a[56], lanePart: a[57], sample: a[58], sliceCount: a[59], sliceEdit: a[60], sliceSel: a[61], prob: a[62], stepSlice: a[63], loopFound: a[64], row: a[65], popover: a[66], chordType: a[67], root: a[68], laneSampler: a[69], projSub: a[80],
  rec: a[70], recSource: a[71], recGain: a[72], recAuto: a[73], recMonitor: a[74], recStop: a[75], slot: a[76], loaded: a[77], slotUsed: a[78], deleteAsked: a[79], exportsDone: a[81], exportKind: a[82] });
const stepOn = (s, n) => (s.steps >>> n) & 1;
const emptyLane = (s) => s.steps === 0;
// A lesson's `project`: what it starts from, pressed on the panel before it opens. A key's name, or [a knob's name, detents], or [a knob's name, detents, "SHIFT"].
const muted = (lanes) => lanes.flatMap((n) => [`LAYER ${n}`, "MUTE"]);
// Lane 10 boots on Hat Closed. SHIFT and the sample knob go from folder to folder, and the sample knob's first turn takes the sample on show.
const laneTenSample = (folders, samples) => ["LAYER 10", "OSC 1", ["T1", folders, "SHIFT"], ["T1", samples], "OSC 1"];
// A knob's values read as zero off their own page and a step's as another lane's on another lane, so a `done` says where it is looking.
const onPage = (s, page) => s.mode === MODE.Sound && s.page === page && !s.sliceEdit;
const inEditor = (s, view) => s.mode === MODE.Step && s.stepView === view;

// A step: `say` what to do, `why` it matters, `target` the control to light (a name from window.plateKeys, or a list), `done(state, stateAtStart)`.
// How Show me does it: a press of the target; `turn` detents, stopping when the step is done; `aim(state)` how far the value is from the one asked for, so it can turn back past
// an overshoot; `hold` ms held down; `twice` two quick presses; `chord` the first target held while the second is pressed; `repeat` presses until done; `pause` ms before the press; `again(state)` a press first, when the step's earlier press has run out; `next` a step that only
// asks you to look, with a Next button; `wait` a step that finishes by itself, with nothing to press. `cursor` the step of the lane a step works on. A lesson's `lane`: the one lane all of it happens on (a step passes only on that lane).
// Rules for the text: plain words, no term before it is explained, and `why` is read before the action, so it says what will happen. A key is named by what is printed on it
// the first time a lesson uses it (panel.js KEYS; web/test/site.mjs checks this). Knobs here have stops: never ask for "more turns" of a knob, ask for a value or a direction.
const LESSONS = [
  { title: "First sound", minutes: 2,
    intro: "Nemesis is a groovebox: a drum machine and a synthesizer in one box, built to make loops. The panel on the right is the real Nemesis firmware running in your browser, so what you learn here works the same on the hardware. The control to use always flashes, so you never have to hunt for it. These lessons start from a demo project. A Drumboy starts with an empty one. To follow along there, before each lesson press the I key, press + until F1 DEMO is marked, press the three bars, then the L key to go home, and press key 1.",
    outro: "You started a pattern, picked out two of its instruments and reshaped a sound while it played. That is most of what using Nemesis feels like. On the hardware the knobs turn without an end and the screen shows the value. Nearly everything else works as it does here.",
    steps: [
      { say: "Press PLAY (the triangle).", why: "Everything Nemesis plays is a pattern: a short loop that repeats. Click the flashing key, or press the space bar on your computer.",
        target: "PLAY", done: (s) => s.playing },
      { say: "Press key 8 on the row of ten keys.", why: "These ten keys are the lanes, and a lane is one instrument's track. Lane 8 is the kick drum, and its light flashes every time it plays.",
        target: "LAYER 8", done: (s) => s.lane === 7 },
      { say: "Press key 1.", why: "Lane 1 is the bass line, played by a synthesizer. Choosing a lane doesn't change the music, it only chooses what the knobs and keys work on.",
        target: "LAYER 1", done: (s) => s.lane === 0 },
      { say: "Press FILTER 1 (the first of the two keys with a curve).", why: "Each key in this group opens one page of settings for the sound. The filter decides how bright or dark it is, and the screen will show its page.",
        target: "FILTER 1", done: (s) => s.mode === MODE.Sound && s.page === PAGE.FLT1 },
      { say: "Turn the flashing knob down, and listen to the bass.", why: "This knob is CUTOFF. It moves the filter you see on the screen, and turning it down makes the sound darker. To turn a knob here, drag it up or down: the line on the knob shows the value, and a drag of 160 pixels covers its whole range. The mouse wheel moves it one step at a time, and so do the arrow keys once you've clicked the knob.",
        target: "T2", turn: -24, done: (s, at) => s.cutoff <= at.cutoff - 12 },
      { say: "Press STOP (the square).", why: "This stops the pattern. Your change stays until you change it again.",
        target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "Lanes and parts", minutes: 3,
    intro: "Nemesis has ten lanes and seven parts. Lanes are the tracks that play the rhythm. Parts are the instruments that make the sound. In this lesson you'll silence a lane, change how loud it is, and meet the first three parts.",
    outro: "Ten lanes play the notes and seven parts make the sound. Several lanes can share a part, and you can give each synth lane a part of its own: on the home screen, hold SHIFT (the nine dots at the bottom right) and turn the third knob on the top row, PLAYED BY, to move a lane to another part.",
    steps: [
      { say: "Press PLAY.", why: "Listen to the whole pattern for a moment, so you can hear what changes next.", target: "PLAY", done: (s) => s.playing },
      { say: "Press key 8.", why: "That picks the kick drum's lane. The screen and the keys will work on this lane.", target: "LAYER 8", done: (s) => s.lane === 7 },
      { say: "Press MUTE (the M key).", why: "The kick will drop out. A muted lane keeps its place in the pattern and just stays silent, so you can take instruments in and out while the music plays.",
        target: "MUTE", done: (s) => (s.muted >>> 7) & 1 },
      { say: "Press MUTE again.", why: "The kick comes back, exactly in time.", target: "MUTE", done: (s) => !((s.muted >>> 7) & 1) },
      { say: "Turn the flashing knob down a little.", why: "On this home screen, the bottom knobs work on the selected lane. This one is LEVEL: how loud the lane is compared with the others.",
        target: "B2", turn: -20, done: (s, at) => s.level <= at.level - 12 },
      { say: "Press key 1.", why: "Lane 1 is played by part A. A part is the instrument that makes a lane's sound, and the screen shows which part you're on.", target: "LAYER 1", done: (s) => s.lane === 0 },
      { say: "Press PART (the D key).", why: "There are seven parts, A to G. Part A is a synthesizer, and it plays the bass. This moves you to part B, another synthesizer, which plays the melodic lanes 4 to 7.",
        target: "PART", done: (s) => s.part === 1 },
      { say: "Press PART again.", why: "Part C is a sampler, which means it plays recorded sounds. Here it plays the drums on lanes 8 to 10, and each lane has its own sample.",
        target: "PART", done: (s) => s.part === 2 },
      { say: "Press STOP.", why: "That is the whole layout: lanes for the rhythm, parts for the sound. Parts D to G are spare synthesizers, ready for when you give one a lane.", target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "Your first beat", minutes: 4,
    intro: "Now you'll program a drum beat yourself. The step sequencer divides a bar, one measure of music, into 16 steps, and you switch each step on or off. You'll build a classic pattern: kick, snare and hi-hat.",
    outro: "You wrote a beat from nothing. Every lane works this way, and a lane can be up to 32 steps long.",
    steps: [
      { say: "Press key 8.", why: "We'll start with the kick drum.", target: "LAYER 8", done: (s) => s.lane === 7 },
      { say: "Press STEP (the S key on the left).", why: "This opens the step editor. Keys 1 to 8 on the row will become steps instead of lanes, and a light shows each step that plays.", target: "STEP", done: (s) => s.mode === MODE.Step },
      { say: "Press CLEAR (the small ring) twice, quickly.", why: "The first press removes your edits and the second empties the lane, so you start from nothing. Holding CLEAR for a second empties every lane in the pattern.", target: "CLEAR", twice: true, done: emptyLane },
      { say: "Press key 1.", why: "Step 1 is the start of the bar, and most beats have a kick here.", target: "LAYER 1", done: (s) => stepOn(s, 0) },
      { say: "Press key 5.", why: "Each group of four steps is one beat, so step 5 is the second beat.", target: "LAYER 5", done: (s) => stepOn(s, 4) },
      { say: "Press key 10.", why: "Only eight steps fit on the keys at a time. Keys 9 and 10 move back and forward between pages, so this shows steps 9 to 16.", target: "LAYER 10", done: (s) => s.stepPage === 1 },
      { say: "Press key 1.", why: "On this page, key 1 is step 9, the third beat.", target: "LAYER 1", done: (s) => stepOn(s, 8) },
      { say: "Press key 5.", why: "And key 5 is step 13, the fourth beat. That makes a kick on every beat.", target: "LAYER 5", done: (s) => stepOn(s, 12) },
      { say: "Press PLAY.", why: "Hear your kick with the other instruments. Keep it playing: you can edit while it runs.", target: "PLAY", done: (s) => s.playing },
      { say: "Press DOWN (the minus key).", why: "UP (the + key) and DOWN move to another lane without leaving the editor. Lane 9 is the snare.", target: "DOWN", done: (s) => s.lane === 8 },
      { say: "Press CLEAR (the small ring) twice, quickly.", why: "Empty the snare lane too.", target: "CLEAR", twice: true, done: emptyLane },
      { say: "Press key 5.", why: "The editor stays on steps 9 to 16, so this is step 13: a snare on the fourth beat. You'll hear it straight away.", target: "LAYER 5", done: (s) => stepOn(s, 12) },
      { say: "Press key 9.", why: "Key 9 goes back a page, to steps 1 to 8.", target: "LAYER 9", done: (s) => s.stepPage === 0 },
      { say: "Press key 5.", why: "A snare on the second beat too. A kick on every beat with a snare on two and four is the backbone of a huge amount of music.", target: "LAYER 5", done: (s) => stepOn(s, 4) },
      { say: "Press DOWN.", why: "Lane 10 is the hi-hat.", target: "DOWN", done: (s) => s.lane === 9 },
      { say: "Press CLEAR (the small ring) twice, quickly.", why: "One more empty lane.", target: "CLEAR", twice: true, done: emptyLane },
      { say: "Press ADD (the dot).", why: "This one writes a rhythm for you: it spreads a random number of hits evenly across the lane. Press it again whenever you want a different one.", target: "ADD", done: (s) => s.euclid && s.steps !== 0 },
      { say: "Press STOP.", why: "Your beat stays in the pattern.", target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "Choosing sounds", minutes: 3,
    intro: "Nemesis comes with 200 synth presets, sorted by type. A preset is a ready-made sound for a part. In this lesson you'll find a new bass, load it, undo it, and then change a drum sample.",
    outro: "FILE opens the list for whatever part you're on: presets on a synth part, samples on a sampler lane. Samples are sorted into folders, and LEFT and RIGHT change the folder there too. On a drum lane OSC 1 opens the SAMPLE page, where you don't need the list: its first knob picks the sample, and with SHIFT held it picks the folder. SHIFT with the small ring undoes the last big change.",
    steps: [
      { say: "Press PLAY.", why: "Keep the music running, because then you hear each preset in the pattern. Lane 1 is selected and it belongs to part A, so a preset you load will change part A.", target: "PLAY", done: (s) => s.playing },
      { say: "Press FILE (the F key at the top right).", why: "This opens the sound browser, and the screen will list the presets.", target: "FILE", done: (s) => s.mode === MODE.Browse },
      { say: "Press RIGHT (the triangle pointing right).", why: "LEFT and RIGHT change the category. The next one is BASS.", target: "RIGHT", done: (s) => s.browseCat === 1 },
      { say: "Press DOWN (the minus key).", why: "UP and DOWN walk through the list. Pause on a preset for a moment and Nemesis plays you a preview.", target: "DOWN", done: (s) => s.browseIdx > 0 },
      { say: "Press OK (the three bars).", why: "OK loads the preset onto the part and takes you back. The bass line will play with the new sound.", target: "OK", done: (s) => s.preset[0] >= 0 && s.mode !== MODE.Browse },
      { say: "Click SHIFT (the nine dots at the bottom right), then CLEAR (the small ring).", why: "SHIFT and the small ring is undo, so the old sound comes back and you can try presets without any risk. On the Drumboy Pro you hold SHIFT down while you press. Here you can click it first, or hold Shift on your computer.", target: ["SHIFT", "CLEAR"], chord: true, done: (s) => s.preset[0] < 0 },
      { say: "Press key 8.", why: "Drum lanes are different: each one picks its own sample.", target: "LAYER 8", done: (s) => s.lane === 7 },
      { say: "Press FILE.", why: "On a sampler lane the browser lists samples instead of presets.", target: "FILE", done: (s) => s.mode === MODE.Browse },
      { say: "Press DOWN.", why: "Move to another sample. Each one plays as you land on it.", target: "DOWN", done: (s) => s.browseIdx > 0 },
      { say: "Press OK.", why: "Lane 8 will play the new sample. This one change touches only this lane.", target: "OK", done: (s, at) => s.mode !== MODE.Browse && s.sample !== at.sample },
      { say: "Press STOP.", why: "On the hardware, the same browser reaches the samples on the SD card. Here there are the twelve built-in ones.", target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "Shaping a sound", minutes: 4,
    intro: "A synth sound moves through stages: oscillators make the raw tone, a filter shapes it, and an envelope decides how it starts and fades. The sound keys open one page for each stage, and the eight knobs always match the eight boxes on the screen.",
    outro: "The same path makes every sound here: oscillator, filter, envelope, then movement. Turn any knob and the screen tells you what it does, so exploring is safe.",
    steps: [
      { say: "Press PLAY.", why: "You learn sound by listening, so keep it running. Lane 1 is selected, so you'll be reshaping the bass, which is part A.", target: "PLAY", done: (s) => s.playing },
      { say: "Press OSC 1 (the sine wave key).", why: "The oscillator makes the raw tone. Look at the top row of boxes on the screen: each box belongs to the knob above it.", target: "OSC 1", done: (s) => s.mode === MODE.Sound && s.page === PAGE.OSC1 },
      { say: "Turn the flashing knob up a little.", why: "This is WAVE, the basic shape of the tone. Sine is pure, triangle is soft, saw is bright and square is hollow. On the hardware, wavetables from the SD card come after those.", target: "T1", turn: 1, done: (s, at) => s.knob[0] !== at.knob[0] },
      { say: "Press FILTER 1 (the first of the two keys with a curve).", why: "The filter takes away part of the tone. You met CUTOFF in lesson 1.", target: "FILTER 1", done: (s) => s.mode === MODE.Sound && s.page === PAGE.FLT1 },
      { say: "Turn the flashing knob up.", why: "This is RESO, resonance. It boosts the sound right around the cutoff, which gives it a sharp, vocal edge.", target: "T3", turn: 30, done: (s, at) => s.knob[2] >= at.knob[2] + 20 },
      { say: "Press AMP (the first of the two target keys).", why: "The amp envelope sets the volume over time: how fast a note starts and how it fades. The screen draws its shape.", target: "AMP", done: (s) => s.mode === MODE.Sound && s.page === PAGE.AMP },
      { say: "Turn the flashing knob up.", why: "This is RELEASE: how long a note rings after it ends. Listen for the notes running into each other.", target: "T4", turn: 30, done: (s, at) => s.knob[3] >= at.knob[3] + 20 },
      { say: "Press MOD (the second target key).", why: "Modulation makes a sound move by itself. An LFO is a slow wave that pushes a setting up and down.", target: "MOD", done: (s) => s.mode === MODE.Sound && s.page === PAGE.MOD },
      { say: "Turn the flashing knob up.", why: "This is LFO DEPTH: how far the LFO moves the sound. Here it moves the cutoff, so the bass starts to sway.", target: "T2", turn: 40, done: (s, at) => s.knob[1] >= at.knob[1] + 25 },
      { say: "Press the L key.", why: "L takes you back. Hold it down for a second and it takes you home from any screen, which is good to remember if you ever get lost.", target: "LANE", hold: 800, done: (s) => s.mode === MODE.Play },
      { say: "Press STOP.", why: "Your new sound stays in this project.", target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "Melody", minutes: 4,
    intro: "Drums only need on and off, but a melody needs notes. In the step editor every step has its own pitch, loudness and length. You'll write a short bass line and change how long the loop is.",
    outro: "Each step carries its own note, velocity (how hard it's played), gate and chance of playing. Lanes can have different lengths, so a short lane falls across the others until the longest one ends, and then they all start again. On the PROJECT screen (the I key) you can set a key and a scale, and every synth note then stays inside it.",
    steps: [
      { say: "Press STEP (the S key on the left).", why: "The step editor again, on lane 1, the bass line. The bottom row of boxes on the screen shows the step you last touched.", target: "STEP", done: (s) => s.mode === MODE.Step },
      { say: "Press CLEAR (the small ring) twice, quickly.", why: "Start from an empty lane.", target: "CLEAR", twice: true, done: emptyLane },
      { say: "Press key 1.", why: "A note on step 1. It plays the lane's ROOT, its home note, shown at the top left of the screen.", target: "LAYER 1", done: (s) => stepOn(s, 0) },
      { say: "Press key 3.", why: "A second note.", target: "LAYER 3", done: (s) => stepOn(s, 2) },
      { say: "Turn the flashing knob up a little.", why: "This is NOTE. It changes the pitch of the step you just pressed, so step 3 will play a higher note.", target: "B1", turn: 3, done: (s) => s.cursor === 2 && s.note > 0 },
      { say: "Press key 6.", why: "A third note.", target: "LAYER 6", done: (s) => stepOn(s, 5) },
      { say: "Turn the flashing knob down a little.", why: "NOTE again, this time below the root. Each click is one key on a piano.", target: "B1", turn: -2, done: (s) => s.cursor === 5 && s.note < 0 },
      { say: "Press PLAY.", why: "That's your bass line. You can press steps and turn NOTE as much as you like while it plays.", target: "PLAY", done: (s) => s.playing },
      { say: "Turn the flashing knob up.", why: "This is GATE: how long the selected step's note is held. Short notes bounce and long ones flow together.", target: "B3", turn: 30, done: (s, at) => s.gate >= at.gate + 20 },
      { say: "Turn the flashing knob down, to 8.", why: "This is LENGTH, the number of steps before this lane loops. The other lanes keep their own lengths, so the bass will repeat twice as often.", target: "T2", turn: -8, done: (s) => s.length <= 8 },
      { say: "Press STOP.", why: "Try a length of 7 or 5 some time: the lane falls across the others until the longest one ends, which sounds much bigger than one short loop.", target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "P-locks", minutes: 3,
    intro: "A p-lock (parameter lock) changes a setting for one single step. The filter can open on the third note only, for example, and close again straight after. It's what makes a sequence sound played rather than repeated.",
    outro: "Hold a step, let go, pick a page and turn: that locks a setting to that step, up to eight per step. Most sound settings can be locked, and so can a part's effect sends and its amp. In the editor, a locked step is marked on the grid. The factory projects TECHNO and MASCHINE are built on locks, and the Projects lesson opens one.",
    steps: [
      { say: "Press PLAY.", why: "You'll hear the lock happen. Lane 1, the bass, is selected.", target: "PLAY", done: (s) => s.playing },
      { say: "Press STEP (the S key on the left).", why: "Locks belong to steps, so they start in the step editor.", target: "STEP", done: (s) => s.mode === MODE.Step },
      { say: "Hold key 5 for a second, then let go.", why: "Holding a step opens its lock. The screen will tell you that you're locking step 5, and everything you turn then applies only to that step.", target: "LAYER 5", hold: 800, done: (s) => s.lockOpen },
      { say: "Press FILTER 1 (the first of the two keys with a curve).", why: "With a lock open, you choose a page as usual.", target: "FILTER 1", done: (s) => s.mode === MODE.Sound && s.page === PAGE.FLT1 },
      { say: "Turn the flashing knob up, well past where it was.", why: "That's CUTOFF again, but only for step 5. Listen: one note in the bar becomes brighter than the others.", target: "T2", turn: 30, done: (s, at) => s.locks > 0 && s.knob[1] >= at.knob[1] + 20 },
      { say: "Press key 5.", why: "Tapping the same step closes the lock. The change stays on step 5, and your knob moves go back to changing the whole sound.", target: "LAYER 5", done: (s) => !s.lockOpen },
      { say: "Press STOP.", why: "With a lock open, the small ring removes that step's locks, so you can always take one back.", target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "Tracker", minutes: 4,
    intro: "A tracker shows the steps as text, with all ten lanes side by side, so you see the whole pattern at once and can write notes quickly. It isn't a second sequencer: it shows the same steps as the step editor, so what you write here is there too. In this lesson you'll enter notes on lane 1, the bass line.",
    outro: "Plus, minus and the two triangles move, OK writes the last note, the bottom knobs change it and the small ring takes it away. The top knobs set the step's velocity, gate and chance of playing. With a key and a scale set on PROJECT, T4 turns IN SCALE on, and NOTE then walks only the notes of the scale. Keys 1 to 10 jump to a lane, and holding one opens the lock of the step on that row, as in the step editor. A lane shorter than the longest one repeats, dimmed, so the rows always line up with what you hear. With 32 steps there are two pages, and SHIFT with plus or minus turns the page.",
    steps: [
      { say: "Press STEP (the S key on the left).", why: "The step editor, as before.", target: "STEP", done: (s) => inEditor(s, 0) },
      { say: "Press STEP again.", why: "Its second view, for chords and the arpeggio.", target: "STEP", done: (s) => inEditor(s, 1) },
      { say: "Press STEP once more.", why: "The third view is the tracker. Each row is a step and each column a lane, and a cell shows a step's note and how hard it plays, like C-2 100. Dashes are an empty step.", target: "STEP", done: (s) => inEditor(s, 2) },
      { say: "Press DOWN (the minus key).", why: "Plus, minus and the two triangles move the white box. One row down is step 2 of lane 1, which is empty.", target: "DOWN", done: (s) => inEditor(s, 2) && s.row === 1 },
      { say: "Press OK (the three bars).", why: "On an empty step, OK writes the last note you entered. This lane has none yet, so you get its root note. A box opens beside the cell to show what's on the step.", target: "OK", cursor: 1, done: (s) => stepOn(s, 1) && s.popover },
      { say: "Turn the flashing knob up a little, a few notes at most.", why: "B1 is NOTE. Each click is a semitone, and the box shows the note and how far it is from the root.", target: "B1", cursor: 1, turn: 3, done: (s) => s.popover && s.note > 0 && s.note <= 12 },
      { say: "Turn the flashing knob up.", why: "B2 is OCTAVE, twelve semitones at a time. A note can only be two octaves from the root, so near the limit the knob stops and the box says so.", target: "B2", cursor: 1, turn: 1, done: (s, at) => s.note >= at.note + 12 },
      { say: "Turn the flashing knob up.", why: "B3 is CHORD: the step will play a chord built on its note. The NOTES row of the box lists its notes. Part A plays two notes at once, so the line under it warns you in red: lesson 13 shows how to give a part more. B4 is the inversion, which reorders the notes.", target: "B3", cursor: 1, turn: 1, done: (s) => s.chordType > 0 },
      { say: "Turn the flashing knob down.", why: "The top knobs work on the step too. T1 is VELOCITY, how hard the note plays, and the number in the cell follows it. T2 is GATE, how long it's held, and T3 is PROB, the chance it plays at all.", target: "T1", cursor: 1, turn: -20, done: (s, at) => s.velocity <= at.velocity - 12 },
      { say: "Press DOWN.", why: "Moving on closes the box.", target: "DOWN", done: (s) => s.row === 2 && !s.popover },
      { say: "Press OK.", why: "The last note again, the same pitch as the step above but without the chord. That's how you write a run of the same note quickly.", target: "OK", cursor: 2, done: (s) => stepOn(s, 2) && s.note > 0 },
      { say: "Press CLEAR (the small ring).", why: "This takes the step away, all of it.", target: "CLEAR", cursor: 2, done: (s) => !stepOn(s, 2) },
      { say: "Click SHIFT (the nine dots at the bottom right), then the small ring.", why: "SHIFT and the small ring is undo, here as everywhere. The step comes back.", target: ["SHIFT", "CLEAR"], chord: true, done: (s) => stepOn(s, 2) && !s.shift },
      { say: "Read how the hardware moves a note, then press Next.", why: "On the Drumboy you can also hold OK and press the keys around it: the triangles move the note a semitone, plus and minus an octave. One mouse can't hold two keys, but your computer can: hold Enter and press the arrow keys. In the tracker the letter keys are a piano as well, and each note you type goes in and moves down a row.", next: true },
      { say: "Press RIGHT (the triangle pointing right).", why: "Left and right move between lanes, on the same row.", target: "RIGHT", done: (s) => s.lane === 1 },
      { say: "Press PLAY.", why: "A bright row runs down the page and shows the step that's playing. Only that row moves: the page stays still, and the display has little to redraw.", target: "PLAY", done: (s) => s.playing },
      { say: "Press STOP.", why: "Everything you wrote is in the step editor too, because it's the same steps. Use whichever view suits the job.", target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "Slices", minutes: 5,
    lane: 9, project: [...muted([1, 2, 3, 4, 5, 6, 7, 9]), ...laneTenSample(3, 1), ["B3", -8], ["B4", -6]],   // (one hit a turn of the lane: lane 10 is in a choke group, so a second hit would cut the sweep short)
    intro: "A slice is one piece of a recording. Cut a loop into slices and the sequencer can play them in any order, so one recording becomes a new rhythm. This web version has twelve short built-in sounds and no SD card, so this lesson has no drum loop to cut. You'll cut FX Sweep, a noise sweep just over a second long. On the hardware you'd do the same with a loop from the card. This lesson's project puts FX Sweep on lane 10, keeps the kick, and mutes the rest. On the Drumboy, set that up yourself.",
    outro: "You cut a sample, played its slices by step, locked one to a step, gave a step a chance of playing, and let SYNC keep it in time at a new tempo. With a drum loop from the card, leave GRID on AUTO and the cuts land on the hits. Press OSC 2 again on the SLICE page for SLICE EDIT, where each slice has its own pitch, level and direction. A take you record yourself cuts up the same way, and that's coming up two lessons from now.",
    steps: [
      { say: "Press PLAY.", why: "Lane 10 plays the whole sweep once every two beats, over the kick. Listen to it rise and fall before you cut it.", target: "PLAY", done: (s) => s.playing && s.mode === MODE.Play },
      { say: "Press OSC 2 (the fork key).", why: "On a synth lane this key opens the second oscillator. On a sampler lane it opens the SLICE page. The screen will say that slicing is off, until you turn it on.", target: "OSC 2", done: (s) => onPage(s, PAGE.SLC) },
      { say: "Turn the flashing knob up to STEP.", why: "This is MODE. At STEP, a step that plays gets one slice instead of the whole sample. Nemesis draws the sample as a wave, cuts it at the sudden hits it finds, and marks each cut with a line. FX Sweep fades in and out, so it finds no hit and there is one slice: the whole sample.", target: "T1", turn: 1, aim: (s) => s.knob[0] - 1, done: (s) => onPage(s, PAGE.SLC) && s.knob[0] === 1 && s.sliceCount >= 1 },
      { say: "Turn the flashing knob up to 8.", why: "This is GRID. It cuts the sample into equal parts, for when there are no hits to find. The screen will show eight lines and 8 SLICES, and the hit on step 1 will shrink to slice 1, the quiet start of the sweep. On a drum loop you'd leave GRID on AUTO.", target: "B4", turn: 2, aim: (s) => s.knob[7] - 2, done: (s) => onPage(s, PAGE.SLC) && s.knob[7] === 2 && s.sliceCount === 8 },
      { say: "Press STEP (the S key on the left).", why: "The step editor. This lane has eight steps and the sample has eight slices, so each key plays its own slice: step 3 plays slice 3.", target: "STEP", done: (s) => inEditor(s, 0) },
      { say: "Press CLEAR (the small ring) twice, quickly.", why: "Empty the lane, so you hear one slice at a time.", target: "CLEAR", twice: true, done: (s) => inEditor(s, 0) && emptyLane(s) },
      { say: "Press key 4.", why: "Step 4 plays slice 4, from the loud middle of the sweep: one short burst each time round.", target: "LAYER 4", done: (s) => inEditor(s, 0) && stepOn(s, 3) },
      { say: "Press key 3.", why: "Step 3 plays slice 3, from earlier in the sweep, so it is darker and quieter.", target: "LAYER 3", done: (s) => inEditor(s, 0) && stepOn(s, 2) },
      { say: "Press key 6.", why: "Step 6 plays slice 6, from the far side of the sweep. Which steps you switch on picks where the sample is chopped.", target: "LAYER 6", done: (s) => inEditor(s, 0) && stepOn(s, 5) },
      { say: "Press STEP again.", why: "The step editor's second view. On a sliced lane its STEP SLICE knob locks a slice to the step you last pressed, here step 6. That is a p-lock, as in lesson 7.", target: "STEP", cursor: 5, done: (s) => inEditor(s, 1) },
      { say: "Turn the flashing knob up to 4.", why: "Step 6 will now play slice 4 instead of slice 6. Each click plays you the slice you landed on, so you can choose by ear.", target: "T2", turn: 4, cursor: 5, aim: (s) => s.stepSlice - 4, done: (s) => inEditor(s, 1) && s.cursor === 5 && s.stepSlice === 4 },
      { say: "Turn the flashing knob down to about 50%.", why: "This is PROB, the chance that the step plays at all. At 50%, step 6 plays about every second time, so the lane keeps changing.", target: "B4", turn: -50, cursor: 5, aim: (s) => s.prob - 50, done: (s) => inEditor(s, 1) && s.cursor === 5 && s.prob <= 50 && s.prob >= 30 },
      { say: "Press OSC 2.", why: "Back to the SLICE page.", target: "OSC 2", done: (s) => onPage(s, PAGE.SLC) },
      { say: "Turn the flashing knob up.", why: "This is RANDOM: the chance that a hit plays any slice instead of its own. The lane starts to wander through the sweep.", target: "T3", turn: 60, done: (s) => onPage(s, PAGE.SLC) && s.knob[2] >= 50 },
      { say: "Turn the flashing knob all the way down.", why: "Back to 0%, so that the next change is easy to hear.", target: "T3", turn: -70, done: (s) => onPage(s, PAGE.SLC) && s.knob[2] === 0 },
      { say: "Turn the flashing knob up to SYNC.", why: "This is MODE again. At SYNC the sample plays straight through by itself, each slice at its own place in the beat, whichever steps are on. BEATS says how long that takes, here 2 beats. Your locked step keeps its slice and its chance of playing.", target: "T1", turn: 1, aim: (s) => s.knob[0] - 2, done: (s) => onPage(s, PAGE.SLC) && s.knob[0] === 2 },
      { say: "Hold the L key for a second, then let go.", why: "A short press goes back one screen. Held, it goes home, where the tempo is.", target: "LANE", hold: 800, done: (s) => s.mode === MODE.Play },
      { say: "Turn the flashing knob up.", why: "This is TEMPO. In SYNC the sweep stays with the beat at any tempo: it comes round sooner and the slices keep their pitch.", target: "T1", turn: 30, done: (s, at) => s.mode === MODE.Play && s.bpm >= at.bpm + 20 },
      { say: "Press STOP.", why: "That ends a SYNC loop too, and MUTE does the same for its lane.", target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "Sample loops", minutes: 4,
    lane: 9, project: [...muted([1, 2, 3, 4, 5, 6, 7, 8, 9]), ...laneTenSample(1, 3), ["B4", -3]],
    intro: "A loop plays a stretch of a sample round and round, so a short sound can last as long as its note. You choose where the stretch starts and ends, and then how its end joins its start, because a bad join clicks. The twelve built-in samples are all short hits with no held sound, so you'll loop Tom Low, a drum with a clear pitch that fades. This lesson's project puts it on lane 10, on every beat, and mutes the rest so that you can hear the join. On the Drumboy, set that up yourself.",
    outro: "START and END pick the stretch, LOOP goes round it, XFADE melts the join and AUTO looks for a better one. A sound with a pitch mostly wants EQ AMP and no fade. A sound without one, like noise, wants a fade of 50 ms or more. On the LOOP page, CURVE sets the shape of the fade.",
    steps: [
      { say: "Press PLAY.", why: "Lane 10 plays Tom Low on every beat, and each hit fades away by itself.", target: "PLAY", done: (s) => s.playing && s.mode === MODE.Play },
      { say: "Press OSC 1 (the sine wave key).", why: "On a sampler lane this key opens the SAMPLE page: where the sample starts and ends, and how it plays. The screen draws the wave.", target: "OSC 1", done: (s) => onPage(s, PAGE.SMP) },
      { say: "Turn the flashing knob up to about 10%.", why: "This is START, where playing begins. Moving it in skips the hit at the front and leaves the ringing tone.", target: "T2", turn: 13, aim: (s) => s.knob[1] - 13, done: (s) => onPage(s, PAGE.SMP) && s.knob[1] >= 13 && s.knob[1] <= 40 },
      { say: "Turn the flashing knob down to about 50%.", why: "This is END, where playing stops. Each note gets much shorter, and there is a gap after it.", target: "T3", turn: -63, aim: (s) => s.knob[2] - 64, done: (s) => onPage(s, PAGE.SMP) && s.knob[2] <= 64 && s.knob[2] >= 40 },
      { say: "Turn the flashing knob up to LOOP.", why: "This is PLAY. At LOOP, a note that reaches END jumps back to START and goes round until the note is over, so the short stretch fills the gap. Listen for a tick each time it goes round: that's the join.", target: "T4", turn: 2, aim: (s) => s.knob[3] - 2, done: (s) => onPage(s, PAGE.SMP) && s.knob[3] === 2 },
      { say: "Press SUB (the ring with two dots).", why: "On a sampler lane this key opens the LOOP page, which says how the end joins the start. It lights up only when PLAY is LOOP. The line under the wave gives the time of one lap, once round.", target: "SUB", done: (s) => onPage(s, PAGE.LOOP) },
      { say: "Turn the flashing knob up.", why: "This is XFADE. The end of the stretch melts into the sound just before START, so the tick goes away.", target: "T2", turn: 30, done: (s) => onPage(s, PAGE.LOOP) && s.knob[1] >= 20 },
      { say: "Turn the flashing knob all the way down.", why: "Back to OFF, and the tick comes back. Now let Nemesis fix the join by moving the points instead.", target: "T2", turn: -40, done: (s) => onPage(s, PAGE.LOOP) && s.knob[1] === 0 },
      { say: "Turn the flashing knob up to EQ AMP.", why: "This is AUTO. It looks near START and END for better points. EQ AMP looks for two places where the wave matches, and the screen will say LOOP FOUND when it has them. The tick goes, with no fade. AUTO also sets CURVE, the shape of a fade, to MATCH, which suits the points it found.", target: "T1", turn: 1, aim: (s) => s.knob[0] - 1, done: (s) => onPage(s, PAGE.LOOP) && s.knob[0] === 1 && s.loopFound && s.knob[3] === 3 },
      { say: "Press STOP.", why: "You may still hear the level step at each lap. Tom Low fades, so its stretch is louder at the start than at the end, and no setting makes a fading note steady.", target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "Recording", minutes: 5,
    lane: 9, project: [...muted([1, 2, 3, 4, 5, 6, 7, 9]), "LAYER 10", ["B4", -6]],   // (one hit a bar, so the take plays out)
    intro: "Recording turns sound into a sample, which plays on a lane like any other. One recording is called a take. On the Drumboy a cable goes into the line input. A browser has no socket for that, so here a short drum loop stands in for the cable: it plays for two seconds, stops for two, and plays again. Everything else is the real recorder. This lesson's project selects lane 10, gives it one hit a bar and mutes the lanes other than the kick. On the Drumboy, set that up yourself.",
    outro: "REC opens the screen, then starts and stops a take. OK keeps it and the small ring drops it. A take is a sample like any other, so you can slice it, loop it and lock it, and FILE lists it under REC. On the Drumboy it's saved to the card, in Sample/REC. Here it lasts until the next lesson starts or you close the page. If the screen says a take clipped, lower GAIN and go again.",
    steps: [
      { say: "Press REC (the large ring).", why: "This opens the recording screen. Nothing is being recorded yet. The bar is a meter that shows how loud the sound coming in is.", target: "REC", done: (s) => s.mode === MODE.Record },
      { say: "Turn the flashing knob up.", why: "This is MONITOR: how much of the input you hear. Turn it up and you'll hear the loop that stands in for the cable.", target: "B2", turn: 50, done: (s) => s.mode === MODE.Record && s.recMonitor >= 30 },
      { say: "Turn the flashing knob up to about +6 dB.", why: "This is GAIN, how much the input is boosted. Watch the meter: you want it high without reaching the end, because a sound that hits the end is distorted for good.", target: "T2", turn: 4, aim: (s) => s.recGain - 4, done: (s) => s.recGain >= 3 && s.recGain <= 6 },
      { say: "Turn the flashing knob up to AUTO.", why: "This is CAPTURE. At MANUAL a take starts when you press REC. At AUTO it waits for the sound, so you can have your hands on another instrument.", target: "T3", turn: 1, done: (s) => s.recAuto === 1 },
      { say: "Turn the flashing knob up to 1 second.", why: "This is AUTO STOP. The take ends after this long without sound, and that silence is left out.", target: "B3", turn: 2, aim: (s) => s.recStop - 2, done: (s) => s.recStop === 2 },
      { say: "Press REC.", why: "Nemesis is now waiting, and the screen says so. The line on the meter is THRESHOLD: the take starts when the sound passes it. A little from just before that moment is kept too, so the first hit isn't cut.", target: "REC", done: (s) => s.rec >= 1 },
      { say: "Wait for the loop to come round.", why: "The take starts by itself on the first hit and ends a second after the loop stops. There's nothing to press.", wait: true, done: (s) => s.rec === 3 },
      { say: "Press OK (the three bars).", why: "OK keeps the take on the selected lane, because lane 10 is on the sampler. On a synth lane it would wait in the sample list, under REC, for FILE on a drum lane.", target: "OK", done: (s, at) => s.rec === 0 && s.sample !== at.sample },
      { say: "Press PLAY.", why: "Lane 10 now plays your take, once a bar, over the kick on lane 8. You'll still hear the input too, because MONITOR is up.", target: "PLAY", done: (s) => s.playing },
      { say: "Turn the flashing knob up to OUTPUT.", why: "This is SOURCE. LINE IN is the socket. OUTPUT records Nemesis itself, effects and all, which is how you freeze a part into a sample.", target: "T1", turn: 1, done: (s) => s.recSource === 1 },
      { say: "Turn the flashing knob down to MANUAL.", why: "Back to CAPTURE. This time you start and stop the take yourself.", target: "T3", turn: -1, done: (s) => s.recAuto === 0 },
      { say: "Press REC.", why: "The take starts at once, and the screen counts up the time.", target: "REC", done: (s) => s.rec === 2 },
      { say: "Press REC again.", why: "That ends the take. It stays on the screen until you decide.", target: "REC", done: (s) => s.rec === 3 },
      { say: "Press CLEAR (the small ring).", why: "This one you don't want: the small ring drops a take. If you press REC instead, it asks first, and a second press drops the take and starts a new one.", target: "CLEAR", done: (s) => s.rec === 0 },
      { say: "Press STOP.", why: "Your first take is still on lane 10.", target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "Effects", minutes: 3,
    intro: "Effects put your sounds in a space. The reverb, delay and chorus are shared: each part decides how much of itself to send to them. Each part also has its own amp simulator, for warmth or dirt.",
    outro: "Sends decide how much of a part reaches a shared effect. The FX key steps through six views: delay, reverb, chorus, compressor, amp and sends. Effects are also where a lot of the processing power goes, which comes up in the next lesson.",
    steps: [
      { say: "Press PLAY.", why: "Effects are easiest to hear with music running. Lane 1 is selected, so you'll be working on part A, the bass.", target: "PLAY", done: (s) => s.playing },
      { say: "Press the flashing key until SENDS is highlighted on the screen.", why: "This is the FX key at the bottom left, a ring with a line. Each press moves to the next effects view, shown at the top right of the screen.", target: "FX", repeat: true, done: (s) => s.mode === MODE.Fx && s.fxView === 5 },
      { say: "Turn the flashing knob up.", why: "This is the REVERB send: how much of part A goes into the reverb. The bass will sit in a room.", target: "T3", turn: 50, done: (s, at) => s.sends[1] >= at.sends[1] + 25 },
      { say: "Turn the flashing knob up.", why: "This is the DELAY send: echoes of the bass, in time with the music.", target: "T2", turn: 40, done: (s, at) => s.sends[0] >= at.sends[0] + 20 },
      { say: "Press the FX key until DELAY is highlighted.", why: "This view sets the delay itself. These settings are shared by every part that sends to it.", target: "FX", repeat: true, done: (s) => s.mode === MODE.Fx && s.fxView === 0 },
      { say: "Press the FX key until AMP SIM is highlighted.", why: "Each part has its own amp, so this one belongs to part A alone.", target: "FX", repeat: true, done: (s) => s.mode === MODE.Fx && s.fxView === 4 },
      { say: "Turn the flashing knob up.", why: "This is MODEL. It switches the amp on and picks its character, and the bass will get thicker and dirtier.", target: "T1", turn: 2, done: (s) => s.amp > 0 },
      { say: "Press STOP.", why: "The reverb and delay ring out for a moment after the music stops.", target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "Mixing and CPU", minutes: 3,
    intro: "The mixer balances the parts. It also shows something special to Nemesis: a small computer, the CPU, plays every voice, so sounds have a cost. When you ask for too much, it makes careful choices instead of crackling.",
    outro: "If Nemesis runs short of CPU it gives things up in a fixed order: first a voice, then costly extras like effects and unison, starting with the part that matters least. The PRIORITY view ranks the parts. Nothing is deleted, and it all comes back when the load drops.",
    steps: [
      { say: "Press PLAY.", why: "Mix with your ears, while the music plays.", target: "PLAY", done: (s) => s.playing },
      { say: "Press MIXER (the R key).", why: "Every knob is one part's level. The top row is parts A to D, the bottom row is E to G, and the last knob is the master.", target: "MIXER", done: (s) => s.mode === MODE.Mixer },
      { say: "Turn the flashing knob down.", why: "This is part A's level. It turns down everything part A plays, on all of its lanes at once.", target: "T1", turn: -30, done: (s, at) => s.partLevel[0] <= at.partLevel[0] - 20 },
      { say: "Turn the flashing knob up.", why: "This is part C, the drums. Balancing the bass against the drums is most of what mixing is.", target: "T3", turn: 10, done: (s, at) => s.partLevel[2] >= at.partLevel[2] + 5 || s.partLevel[2] === 127 },
      { say: "Press MIXER again.", why: "The MIXER key steps through four views. This one is VOICES: how many notes each part can play at the same time.", target: "MIXER", done: (s) => s.mode === MODE.Mixer && s.mixView === 1 },
      { say: "Turn the flashing knob up a little.", why: "This is part A's voices. Chords need more voices, and each one costs CPU.", target: "T1", turn: 1, done: (s, at) => s.voices[0] > at.voices[0] },
      { say: "Press MIXER again.", why: "This view is PRIORITY. It says which parts matter most if Nemesis runs short of CPU.", target: "MIXER", done: (s) => s.mode === MODE.Mixer && s.mixView === 2 },
      { say: "Press MIXER again.", why: "This last view answers one question: will it fit? It shows how much of the CPU budget your project needs, and what it would give up first.", target: "MIXER", done: (s) => s.mode === MODE.Mixer && s.mixView === 3 },
      { say: "Read the WILL IT FIT line, then press Next.", why: "It says YES while everything fits, and the bar shows how much of the budget you're using. If it says OVER BUDGET, take a voice away or lower a send.", next: true },
      { say: "Press STOP.", why: "That's the mix. A good habit is to set levels first and add voices only where you can hear them.", target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "Patterns and songs", minutes: 4,
    intro: "One loop doesn't make a track. A project holds 16 patterns, and a song is a list that plays them in order: verse, chorus, and so on. In this lesson you'll switch patterns and arrange a song.",
    outro: "Patterns hold the notes and a song puts them in order. COPY and PASTE (the C and P keys) duplicate a lane, and with SHIFT a whole pattern, which is the quick way to make a variation. Holding the small ring for a second on the home screen or in the step editor (not in the tracker) empties the pattern, when you'd rather start over.",
    steps: [
      { say: "Click SHIFT (the nine dots at the bottom right), then key 2.", why: "SHIFT with keys 1 to 10 picks patterns, so this takes you to pattern 2.", target: ["SHIFT", "LAYER 2"], chord: true, done: (s) => s.pattern === 1 },
      { say: "Press PLAY.", why: "This is a different pattern, with its own notes on every lane. The synth sounds are the same, because parts belong to the project. A drum lane's sample belongs to the pattern.", target: "PLAY", done: (s) => s.playing },
      { say: "Click SHIFT, then key 1.", why: "While playing, the change waits for the end of the pattern, so it always lands in time. The screen tells you what's coming next.", target: ["SHIFT", "LAYER 1"], chord: true, done: (s) => s.pattern === 0 },
      { say: "Press STOP.", why: "Now let's arrange them.", target: "STOP", done: (s) => !s.playing },
      { say: "Press SONG (the S key on the right).", why: "A song is a list of rows, and each row plays one pattern a number of times. This one already has a few rows.", target: "SONG", done: (s) => s.mode === MODE.Song },
      { say: "Press ADD (the dot).", why: "That adds a new row after the selected one.", target: "ADD", done: (s, at) => s.songRows > at.songRows },
      { say: "Turn the flashing knob up.", why: "This picks which pattern the row plays.", target: "T3", turn: 1, done: (s, at) => s.rowPattern !== at.rowPattern },
      { say: "Turn the flashing knob up.", why: "And this is how many times the row repeats before the song moves on.", target: "T4", turn: 1, done: (s, at) => s.rowRepeat !== at.rowRepeat },
      { say: "Press OK (the three bars).", why: "OK plays the song from the selected row. Watch the row move down the list.", target: "OK", done: (s) => s.songPlaying },
      { say: "Press STOP.", why: "One thing left: how a project is kept.", target: "STOP", done: (s) => !s.playing },
    ] },

  { title: "Projects", minutes: 4, projSub: 0,
    intro: "A project holds everything: the patterns, the songs, the sounds and the effects. A Drumboy starts with an empty project, and three factory projects are built into it, ready to play or to take apart. In this lesson you'll load one, save your own copy, delete it and bring it back.",
    outro: "That's Nemesis: patterns of lanes, parts that make the sound, effects, and songs to arrange it all, in projects you keep on the card. On the Drumboy a save stays when the power goes. Here the card is a pretend one, and it's gone when another lesson starts or you close the page.",
    steps: [
      { say: "Press PROJECT (the I key).", why: "This list starts with F1, F2 and F3, the factory projects, and then come thirty slots on the SD card. The B1 knob walks the list too.", target: "PROJECT", done: (s) => s.mode === MODE.Project && s.slot === 0 },
      { say: "Press UP (the + key).", why: "One row up is F3, MASCHINE: a straight, robotic electro pattern.", target: "UP", done: (s) => s.slot === -1 },
      { say: "Press OK (the three bars).", why: "OK loads the row. A factory project loads as a copy, so you can change anything and the original stays safe in the firmware. If you load one by mistake, SHIFT and the small ring brings back what you had.", target: "OK", done: (s) => s.loaded === -1 },
      { say: "Press PLAY.", why: "Listen to the first pattern: a bass and drums, with the bass filter stepping up on every note. Nobody turned a knob. Those are p-locks, from lesson 7.", target: "PLAY", done: (s) => s.playing },
      { say: "Press the L key.", why: "L takes you back to the home screen. The small marks on the steps show where the locks are.", target: "LANE", done: (s) => s.mode === MODE.Play },
      { say: "Click SHIFT (the nine dots at the bottom right), then key 3.", why: "Pattern 3 adds a clean lead and short bleeps that jump from left to right. Both come from the same part. The bleeps are the lead's sound, made short and panned by locks alone.", target: ["SHIFT", "LAYER 3"], chord: true, done: (s) => s.pattern === 2 },
      { say: "Press STOP.", why: "Now keep a copy of your own.", target: "STOP", done: (s) => !s.playing },
      { say: "Press PROJECT.", why: "Back to the list, on F3.", target: "PROJECT", done: (s) => s.mode === MODE.Project },
      { say: "Press DOWN (the minus key).", why: "Slot 1, the first on the card. A factory project can't be saved over, so a change you want to keep goes in a slot.", target: "DOWN", done: (s) => s.slot === 0 },
      { say: "Click SHIFT, then OK.", why: "SHIFT and OK saves the whole project to the slot. The row will show its name, and LOADED, because this is now the project you're working on.", target: ["SHIFT", "OK"], chord: true, done: (s) => s.slotUsed && s.loaded === 0 },
      { say: "Press CLEAR (the small ring).", why: "On this screen the small ring deletes. The first press only asks: the row says so for four seconds, and any other key or knob takes the question back.", target: "CLEAR", done: (s) => s.deleteAsked === 0 },
      { say: "Press CLEAR again.", why: "The second press deletes the project from the card. The project you're working on stays in memory, so nothing changes. If the four seconds have run out, this press asks again and the next one deletes.", target: "CLEAR", pause: 450, again: (s) => s.deleteAsked < 0, done: (s) => !s.slotUsed },
      { say: "Click SHIFT, then the small ring.", why: "SHIFT and the small ring is undo, even for this: the project goes back on the card. The undo lasts until your next undoable action, or until the power goes.", target: ["SHIFT", "CLEAR"], chord: true, done: (s) => s.slotUsed && !s.shift },
    ] },

  { title: "Exporting", minutes: 3,
    intro: "A finished song can leave Nemesis, to go into a computer's music software or out into the world. You can render it to a WAV, a sound file. You can render stems, one file for each lane, so that the kick and the snare can be mixed apart. Or you can export the notes as a MIDI file, to play with another instrument. In this last lesson you'll make all three.",
    outro: "On the Drumboy the files go to the card, in Export, in a folder with the project's name. Rendering is silent and can take a while, and the screen shows how far it has come. STOP gives up. A render never drops a voice, so it can hold more than live playing can. On the librarian page of this website you can also fetch a song's MIDI file from a Drumboy over a MIDI cable.",
    steps: [
      { say: "Press PROJECT (the I key).", why: "This is the project screen, with its list of projects.", target: "PROJECT", done: (s) => s.mode === MODE.Project && s.projSub === 0 },
      { say: "Press PROJECT again.", why: "The key steps through three views. This second one sets the MIDI channel and note of each lane, which the MIDI file will use.", target: "PROJECT", done: (s) => s.mode === MODE.Project && s.projSub === 1 },
      { say: "Press PROJECT once more.", why: "The third view is EXPORT. B1 picks the song and B2 picks what to make. It starts on SONG 1 and SONG WAV.", target: "PROJECT", done: (s) => s.mode === MODE.Project && s.projSub === 2 },
      { say: "Press OK (the three bars).", why: "OK starts. Nemesis plays the song through once, in silence and as fast as it can, and writes every sample to a file. The screen shows how far it has come. On the Drumboy the file goes to the card. Here it comes to your browser as a download.", target: "OK", done: (s, at) => s.exportsDone > at.exportsDone },
      { say: "Turn the flashing knob up to STEMS.", why: "Stems are one WAV for each lane that plays. Each keeps its own effects, and they all start at the same moment, so they line up in any music software.", target: "B2", turn: 1, aim: (s) => s.exportKind - 1, done: (s) => s.exportKind === 1 },
      { say: "Press OK.", why: "The song plays through once per lane, so this takes longer. Here the files arrive together in one zip.", target: "OK", done: (s, at) => s.exportsDone > at.exportsDone },
      { say: "Turn the flashing knob up to MIDI FILE.", why: "A MIDI file holds the notes and no sound. Every lane gets a track, with your rhythms, chords and swing as they play.", target: "B2", turn: 1, aim: (s) => s.exportKind - 2, done: (s) => s.exportKind === 2 },
      { say: "Press OK.", why: "This one takes no time at all, because no sound is made.", target: "OK", done: (s, at) => s.exportsDone > at.exportsDone },
      { say: "Read this, then press Next.", why: "You've finished the course. Online Nemesis is the same panel with nothing flashing, and FILE opens all 200 sounds: it's yours now.", next: true },
    ] },
];

window.Learn = (() => {
  const box = () => document.getElementById("lesson");
  const wait = (ms) => new Promise((r) => setTimeout(r, ms));
  const STORE = "drumboy.learn";
  // what was finished, by title: a lesson's number moves when one is added. (A list from before holds numbers, of the ten lessons there were then.)
  const ADDED = ["Tracker", "Slices", "Sample loops", "Recording", "Projects", "Exporting"], ten = LESSONS.filter((l) => !ADDED.includes(l.title));
  const finished = new Set((() => { try { return (JSON.parse(localStorage.getItem(STORE)) || []).map((f) => (typeof f === "number" ? ten[f]?.title : f)); } catch (e) { return []; } })());
  const SOUND_KEY = ["OSC 1", "OSC 2", "SUB", "FILTER 1", "FILTER 2", "GRIT", "AMP", "MOD", "GLOB", "OSC 1", "FILTER 1", "AMP", "OSC 2", "SUB"];   // (by page: the synth's, then a sampler lane's)
  const MODE_KEY = { [MODE.Step]: "STEP", [MODE.Mixer]: "MIXER", [MODE.Browse]: "FILE", [MODE.Song]: "SONG", [MODE.Fx]: "FX", [MODE.Project]: "PROJECT", [MODE.Record]: "REC" };
  const skipped = [];                  // steps that passed by themselves: a lesson bug, the test reports them
  let lesson = null, index = 0, at = null, last = null, started = false, checked = false, hintTimer = 0, gen = 0, run = 0, showing = false, leftLesson = null, away = "", steady = 0;

  const step = () => LESSONS[lesson].steps[index];
  const key = (n) => window.plateKeys[n];
  // on the lane its lesson happens on, playing that lane's part, in the pattern the lesson made
  const onLane = (n) => { const l = LESSONS[lesson]; return l.lane === undefined || (n.lane === l.lane && n.part === n.lanePart && n.pattern === 0); };
  function coach(names) {
    document.querySelectorAll("#plate .coach").forEach((e) => e.classList.remove("coach"));
    [].concat(names || []).forEach((n) => { if (key(n)) { key(n).el.classList.add("coach"); key(n).el.scrollIntoView({ block: "nearest", inline: "nearest" }); } });
  }
  function quiet() { run++; showing = false; coach(null); clearTimeout(hintTimer); }
  function setHint(text) {
    const h = box().querySelector(".hintBox");
    if (!h) return;
    h.innerHTML = text ? "<p class='hint'></p>" : "";
    if (text) h.firstChild.textContent = text;
  }

  function menu() {
    lesson = null; started = false; away = ""; quiet();
    if (last && last.mode === MODE.Record) { const l = key("LANE"); l.send(true); l.send(false); }   // (the loop that stands in for the cable must not play on over the list)
    box().innerHTML = "<div class='kicker'>Learn Nemesis</div><h2>Level select</h2><p class='why'>Short lessons, one step at a time, on the real panel. Sound helps, so turn your volume up a little. Each lesson starts from a fresh Nemesis with its own project, which clears anything you made before.</p><ol></ol>";
    if (leftLesson !== null) { const n = document.createElement("p"); n.className = "note"; n.textContent = `You left lesson ${leftLesson + 1}. Pick it again to start over.`; box().querySelector("ol").before(n); leftLesson = null; }
    const ol = box().querySelector("ol");
    LESSONS.forEach((l, i) => {
      const li = document.createElement("li"), b = document.createElement("button");
      b.innerHTML = `<span>${i + 1}. ${l.title}</span><span class="${finished.has(l.title) ? "tick" : "muted"}">${finished.has(l.title) ? "done" : l.minutes + " min"}</span>`;
      b.onclick = () => begin(i);
      li.appendChild(b); ol.appendChild(li);
    });
  }
  function view() {
    const l = LESSONS[lesson], s = step();
    away = ""; checked = false; at = last; steady = 0;
    box().innerHTML = `<div class="kicker">Lesson ${lesson + 1}: ${l.title}</div><div class="dots">${l.steps.map((_, i) => `<i class="${i < index ? "done" : i === index ? "now" : ""}"></i>`).join("")}</div>
      <p class="say"></p><p class="why"></p><div class="hintBox"></div><div class="row">${s.next ? '<button class="primary" id="nextStep">Next</button>' : s.wait ? "" : '<button id="showMe">Show me</button>'}<button id="again">Start over</button><button id="leave">All lessons</button></div>`;
    box().querySelector(".say").textContent = s.say;
    box().querySelector(".why").textContent = s.why;
    if (s.next) box().querySelector("#nextStep").onclick = advance;
    else if (!s.wait) box().querySelector("#showMe").onclick = showMe;
    box().querySelector("#again").onclick = () => begin(lesson);
    box().querySelector("#leave").onclick = menu;
    coach(s.target);
    clearTimeout(hintTimer);
    hintTimer = setTimeout(() => { if (!away && !s.next && !s.wait) setHint(s.turn ? "Drag the flashing knob up or down and let go, or use the mouse wheel. Press Show me and the lesson will do this step for you." : "Look for the flashing outline on the panel, or press Show me and the lesson will do this step for you."); }, 12000);
  }
  // the lesson's project, by the panel's own keys and knobs
  async function keyIn(project) {
    if (!project) return;
    for (const a of project) {
      const [name, detents, held] = [].concat(a), k = key(name);
      if (held) key(held).send(true);
      if (detents) for (let d = 0; d < Math.abs(detents); d++) k.turn(Math.sign(detents));
      if (held) key(held).send(false);
      if (detents) await wait(30);
      if (detents) continue;
      k.send(true); k.send(false);
    }
    await wait(200);                     // the state comes with the screen, a frame later
  }
  async function begin(i) {
    const me = ++gen;
    quiet();
    window.plateUnshift();               // (the firmware is about to be replaced: a SHIFT left clicked goes with it)
    lesson = i; index = 0; at = null; last = null; started = false;
    box().innerHTML = `<div class="kicker">Lesson ${i + 1}</div><h2>${LESSONS[i].title}</h2><p class="why">Starting a fresh Nemesis…</p>`;
    await startAudio();   // (it talks to the worker: before the worker is replaced, not during)
    await freshDrumboy();
    if (me !== gen) return;
    await keyIn(LESSONS[i].project);
    if (me !== gen) return;
    box().innerHTML = `<div class="kicker">Lesson ${i + 1}</div><h2>${LESSONS[i].title}</h2><p class="why"></p><div class="row"><button class="primary" id="go">Start</button><button id="leave">All lessons</button></div>`;
    box().querySelector(".why").textContent = LESSONS[i].intro;
    return new Promise((done) => {
      box().querySelector("#go").onclick = () => { started = true; view(); done(); };
      box().querySelector("#leave").onclick = () => { menu(); done(); };
    });
  }
  function finish() {
    const i = lesson, l = LESSONS[i], next = LESSONS[i + 1];
    finished.add(l.title);
    try { localStorage.setItem(STORE, JSON.stringify([...finished])); } catch (e) { /* private window: the progress lasts for the visit */ }
    lesson = null; started = false; quiet();
    box().innerHTML = `<div class="kicker">Lesson ${i + 1} done</div><h2>${l.title}</h2><p class="why"></p><div class="row">${next ? '<button class="primary" id="next" disabled>Next lesson</button>' : ""}<button id="leave" disabled>All lessons</button></div>`;
    box().querySelector(".why").textContent = l.outro;
    const n = box().querySelector("#next"), m = box().querySelector("#leave");
    if (n) n.onclick = () => begin(i + 1);
    m.onclick = menu;
    setTimeout(() => { if (n) n.disabled = false; m.disabled = false; }, 500);   // a second click on Show me must not land here
  }
  function advance() {
    run++; showing = false;
    index++;
    if (index >= LESSONS[lesson].steps.length) finish(); else view();
  }
  // the arrow that goes to `lane` the shorter way round: up and down, in the tracker left and right
  const laneKey = (n, lane) => {
    const forward = (lane - n.lane + 10) % 10 <= 5, tracker = n.mode === MODE.Step && n.stepView === 2;
    return key(tracker ? (forward ? "RIGHT" : "LEFT") : forward ? "DOWN" : "UP");
  };
  // what stands between the panel and the step, when a wrong press moved it: [kind, how to fix it]. Each step started from a known place (`at`), so the way back needs no list per step.
  function detour() {
    const s = step(), n = last;
    if (!n || !at || s.next || s.wait) return null;
    if (n.lockOpen && !at.lockOpen) return ["lock", () => press(key("LAYER " + ((n.lockStep % 8) + 1)), 120)];
    if (!n.lockOpen && at.lockOpen) return ["lockGone", null];
    // the lane comes before the page: a sampler lane's pages do not exist on a synth lane. In the editor the layer keys are steps, and a pattern is picked from the home screen
    if (!onLane(n)) {
      const lane = LESSONS[lesson].lane;
      if (n.pattern !== 0 && n.mode !== MODE.Play) return ["away", () => press(key("LANE"), 800)];
      if (n.pattern !== 0) return ["away", async () => { const sh = key("SHIFT"); sh.send(true); await press(key("LAYER 1"), 140); sh.send(false); }];
      if (n.mode === MODE.Step) return ["away", () => press(laneKey(n, lane), 140)];
      if (n.mode === MODE.Play || n.mode === MODE.Sound) return ["away", () => press(key("LAYER " + (lane + 1)), 140)];
    }
    if ((s.repeat && n.mode === MODE.Fx) || s.target === "LANE") return null;   // (the key this step asks for moves the screen itself)
    if (n.mode !== at.mode || (n.mode === MODE.Sound && n.page !== at.page)) {
      if (at.mode === MODE.Play) return ["away", () => press(key("LANE"), 800)];
      return ["away", () => press(key(at.mode === MODE.Sound ? SOUND_KEY[at.page] : MODE_KEY[at.mode]), 140)];
    }
    if (n.mode === MODE.Sound && n.sliceEdit !== at.sliceEdit) return ["away", () => press(key("OSC 2"), 140)];
    if (n.mode === MODE.Step && n.stepView !== at.stepView) return ["away", async () => { for (let i = (at.stepView - n.stepView + 3) % 3; i > 0; i--) await press(key("STEP"), 140); }];
    if (s.cursor !== undefined && n.mode === MODE.Step && n.cursor !== s.cursor) {   // (they move the cursor without touching a step, and go round)
      const forward = (s.cursor - n.cursor + n.length) % n.length <= n.length / 2;
      return ["away", () => press(key(n.stepView === 2 ? (forward ? "DOWN" : "UP") : forward ? "RIGHT" : "LEFT"), 140)];
    }
    if (n.mode === MODE.Mixer && n.mixView !== at.mixView) return ["away", () => press(key("MIXER"), 140)];
    const view = LESSONS[lesson].projSub !== undefined ? LESSONS[lesson].projSub : at.projSub;   // (a lesson on one view says which, as a quick double press can start a step on another)
    if (n.mode === MODE.Project && n.projSub !== view) return ["away", () => press(key("PROJECT"), 140)];
    if (n.mode === MODE.Fx && n.fxView !== at.fxView && !s.repeat) return ["away", () => press(key("FX"), 140)];
    const picksLane = [].concat(s.target)[0].startsWith("LAYER") && at.mode !== MODE.Step;
    if (n.lane !== at.lane && !picksLane && n.mode !== MODE.Browse && n.mode !== MODE.Song) return ["away", () => press(laneKey(n, at.lane), 140)];
    return null;
  }
  function onState(a) {
    if (!a) return;
    last = readState(a);
    if (lesson === null || !started) return;
    const s = step();
    if (!at) at = last;                  // the state the step started from: "turned down" is measured from here
    if (!checked) {
      checked = true;
      if (s.done && s.done(at, at)) skipped.push(`lesson ${lesson + 1}: "${s.say}" was already done when it came up`);
    }
    if (s.done && onLane(last) && s.done(last, at)) {
      // a knob is done where it comes to rest: a drag or a flick of a trackpad may only pass the value on its way
      if (document.querySelector("#plate .enc.down")) return;
      steady = steady || performance.now();
      if (!s.turn || performance.now() - steady >= 200) advance();
      return;
    }
    steady = 0;
    if (s.wait) {                        // (REC or L gave up waiting: the way back is REC)
      const hint = last.rec !== 0 ? "" : last.mode === MODE.Record ? "That stopped the wait. Press REC to wait again." : "That left the REC screen. Press REC twice: once to open it, and once to wait.";
      if (hint !== away) { away = hint; coach(hint ? "REC" : null); setHint(hint); }
      return;
    }
    const d = detour(), kind = d ? d[0] : "";
    if (kind !== away) {
      away = kind;
      setHint({ away: "That press moved you away from this step. Press Show me to come back.", lock: "A long press opened a lock on a step. Press Show me to close it. A short tap is all this step needs.",
        lockGone: "The lock closed before you finished. Press Start over to try again.", "": "" }[kind]);
    }
  }
  async function press(k, ms) {
    k.el.classList.add("down"); k.send(true);
    await wait(ms);
    k.el.classList.remove("down"); k.send(false);
    await wait(60);
  }
  async function showMe() {
    if (showing || step().wait) return;
    const s = step(), here = index, in_ = lesson, me = ++run;
    const live = () => me === run && lesson === in_ && index === here;
    showing = true;
    const b = box().querySelector("#showMe");
    if (b) b.disabled = true;
    if (last.shift && !s.chord) { window.plateUnshift(); await wait(200); }   // (with SHIFT down the keys below would do something else: the small ring would undo)
    const d = detour();
    const cameBack = !!d;                // (a step it has just come back to is not stuck: the next press finishes it)
    if (last.shift && !s.chord) setHint("Let go of Shift on your computer first.");
    else if (d) {
      if (d[1]) await d[1]();
      await wait(200);                   // the state comes with the screen, a frame later
    } else if (s.next) advance();
    else {
      const keys = [].concat(s.target).map(key);
      if (s.chord) {
        keys[0].el.classList.add("down"); keys[0].send(true);
        await wait(120);
        await press(keys[1], 140);
        keys[0].el.classList.remove("down"); keys[0].send(false);
      } else if (s.turn) {
        for (let i = 0; i < (s.aim ? 140 : Math.abs(s.turn)) && live() && !s.done(last, at); i++) {
          const off = s.aim ? s.aim(last) : 0, dir = s.aim ? -Math.sign(off) : Math.sign(s.turn);
          if (!dir) break;
          keys[0].turn(dir);
          if (!s.aim) { await wait(25); continue; }
          for (let w = 0; w < 40 && live() && s.aim(last) === off; w++) await wait(10);   // (aiming reads the value back, and that comes with a later screen: a detent more on the old value overshoots)
          if (s.aim(last) === off) break;                                                 // (it does not move: the knob is doing something else)
        }
      } else if (s.repeat) {
        for (let i = 0; i < 6 && live(); i++) { await press(keys[0], 120); await wait(220); }
      } else if (s.twice) {
        await press(keys[0], 90); await press(keys[0], 90);
      } else {
        if (s.again && s.again(last)) await press(keys[0], 140);
        if (s.pause) await wait(s.pause);
        await press(keys[0], s.hold || 140);
      }
    }
    if (me === run) { showing = false; const c = box().querySelector("#showMe"); if (c) c.disabled = false; }
    // nothing it can press will finish this: a knob turned off the lesson's project, say. (A pattern change waits for the end of the pattern, and a cut or a search takes a moment.)
    await wait(900);
    if (me === run && live() && !cameBack && !s.next && !s.chord && !last.shift && !detour() && !(s.done(last, at) && onLane(last))) setHint("This step can't finish from here, because something earlier changed. Press Start over to begin the lesson again.");
  }
  // the tab was left: a lesson lives on its tab, and nothing stays lit on the panel
  function leave() {
    gen++;
    if (lesson !== null) leftLesson = lesson;
    lesson = null; started = false; quiet();
  }
  // the test: every lesson through Show me, from a fresh Drumboy, then a wrong press and the way back (web/test/learn.mjs). Returns what went wrong.
  async function runAll() {
    const failed = [];
    const start = async (i) => {
      const going = begin(i);
      await new Promise((r) => { const t = setInterval(() => { const g = box().querySelector("#go"); if (g) { clearInterval(t); g.click(); r(); } }, 20); });
      await going;
    };
    for (let i = 0; i < LESSONS.length; i++) {
      await start(i);
      while (lesson === i) {
        const k = index;
        await wait(200);
        await showMe();
        const t0 = Date.now();
        while (lesson === i && index === k && Date.now() - t0 < 6000) await wait(30);   // (a pattern change waits for the end of the pattern)
        if (lesson === i && index === k) { failed.push(`lesson ${i + 1} step ${k + 1}: ${LESSONS[i].steps[k].say} state ${JSON.stringify(last)}`); break; }
      }
      lesson = null;
    }
    await start(0);
    await wait(200); await showMe(); await wait(400);      // lesson 1, step 2: press key 8
    await press(key("STEP"), 140); await wait(400);
    if (!box().querySelector(".hint")) failed.push("a wrong press: no hint");
    await showMe(); await wait(400);
    if (last.mode !== MODE.Play) failed.push("a wrong press: Show me did not come back, mode " + last.mode);
    await showMe(); await wait(400);
    if (index !== 2) failed.push("a wrong press: the step did not finish after coming back");
    leave();
    return failed.concat(skipped);
  }
  return { open: () => { if (lesson === null) menu(); }, leave, onState, runAll, state: () => last };
})();
