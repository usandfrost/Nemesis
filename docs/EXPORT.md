# Exporting a song

PROJECT's third view (press INFO until EXPORT is lit) makes files of a song. B1 picks the song, B2 picks what to make, and OK starts. STOP gives up.

| B2 | What it makes |
|---|---|
| SONG WAV | The whole song: stereo, 16-bit, 44.1 kHz, effects and all, with the effects' tail. |
| STEMS | A WAV for each lane that plays. Each starts at the start of the song, so they line up in any editor. |
| MIDI FILE | A Standard MIDI File, type 1: a tempo track and a track for each lane. |

The files go to the card in `Export/<project>/`, as `Song 1.wav`, `Song 1 lane 08 Kick 808.wav` and `Song 1.mid`. A file already there is never touched: the new one becomes `Song 1 (2).wav`.

## How a render works

It is offline. The instrument leaves the audio interrupt, so nothing can be heard, and the main loop drives it in slices, so the screen stays alive and STOP works. The governor is off and a block takes as long as it takes, so a render never gives up a voice, and it can hold what live playing could not.

What was sounding dies away first, so it is not in the file. The song then plays once with its loop off, and goes on until two seconds have been silent, twelve at most, so the effects ring out. Muted lanes stay muted. No MIDI goes out of the socket and what comes in is dropped. Afterwards the pattern and song on screen are as they were.

A stem is the whole song again with one lane let through. The sequencer still plays all ten lanes from the same seed, so a step's PROB falls the same way in every stem and in the mix. Each stem keeps its effects, and the lane's sends reach the shared delay and reverb. The stems do not add up to the mix exactly. Lanes that share a part share its voices, which are stolen differently with one lane playing. A synth does not start from the same state each time, so its stem differs in phase from the mix. And the master compressor, when it is on, hears one lane at a time. A song with nothing to play makes no files.

## MIDI

The notes come from the real sequencer, run on its own and quicker than time, so Euclid rhythms, tapped steps, chords, strums, arpeggios, swing and the scale are in the file as they play. A step with a PROB plays or not once, the same each time the file is made. A synth lane's notes are the notes. Each lane's notes are what its MIDI out sends, from PROJECT's MIDI view: a synth lane plays its notes, a drum lane alone on a channel plays its MIDI note moved by the step's pitch, and a lane on a shared channel is always its trigger note. Each track is on the lane's channel, or a channel of its own if the lane's MIDI is off, and runs to the song's end, rests included. A repeated note ends before it starts again, as at the socket. Locks are not in the file. A file holds 24,000 notes at most, on a 480 PPQ grid, and a song with more says so. What plays plays on while the file is made, which takes a moment.

## On the card

The first write after a start reads the whole card through first (`docs/STORAGE.md`). A file is written as it is made and its header is put right at the end. Closing reads all of it back, which is the CHECKING on the screen, and a file that does not come back as it went in is taken away and writes stop until a restart. STOP or a full card takes the unfinished file away. Stems finished before that stay. After a refused write the card takes no more writes, so part of the file can stay, and a power cut can leave part of one too: a computer can delete it, and it costs nothing else.

The samples a project uses are loaded first. Any that do not fit or will not load are missing from the render, and the message at the end says how many.

## On the computer

The web version makes the same files, as downloads: stems arrive as one zip. The librarian page can also ask a Drumboy for a song's MIDI file over the cable (`docs/PRESET_SYSEX.md`, command `08`), and saves it to the computer.
