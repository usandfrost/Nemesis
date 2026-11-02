# Preset SysEx: add and remove presets over MIDI

**Status:** the librarian page, the simulated device and the firmware (`firmware/src/midi/PresetLink.cpp`) all speak this protocol, and the website's client has been run against the firmware code on a computer (`web/test/link.mjs`). It has not yet been run over a real MIDI cable.

## The connection

- **Transport:** the physical MIDI sockets only. In is USART1, out is USART6, both 31250 baud (`MidiIn.h`, `MidiOut.h`). There is no USB MIDI: the firmware has no USB code, and the
  hardware's USB-C socket is for power.
- **So a browser needs:** a USB-MIDI interface wired to both sockets, in and out, and a browser with Web MIDI that is allowed to use SysEx (`requestMIDIAccess({ sysex: true })`, on https or
  localhost). Chrome and Edge have it. Firefox asks for a permission add-on. Safari does not have it.

## A message

One SysEx message, every byte between the first and last below 128:

    F0  7D  4E 50  01  CMD  TAG  <payload>  SUM  F7

| Field | Meaning |
|---|---|
| `7D` | The MIDI maker number for non-commercial use. It is meant for experiments, not for released products: look at this again before the firmware side is released |
| `4E 50` | "NP", this protocol |
| `01` | The protocol's version |
| `CMD` | The command, below |
| `TAG` | 1 to 127. The sender of a request picks it, and every reply to that request repeats it. A reply with another tag is late and is ignored. A damaged message cannot be trusted for its tag, so it fails the request under way. |
| `payload` | The command's fields, in 7-bit coding. Numbers are little endian, as in a preset file. |
| `SUM` | The low seven bits of the sum of every byte from the version to the end of the payload |

**7-bit coding.** Each group of up to seven bytes is led by a byte that holds their top bits, bit 0 for the group's first byte. Seven bytes become eight.

**A message's size.** A chunk carries at most 154 bytes, so the largest message is 188 bytes, about 60 ms on the cable. MIDI real-time bytes (`F8` and up) may fall inside a message and are
ignored, as the parser already does.

## Commands

From the computer:

| CMD | Name | Payload |
|---|---|---|
| `01` | IDENTIFY | none |
| `02` | LIST | none |
| `03` | DOWNLOAD | id u16 |
| `04` | UPLOAD_BEGIN | id u16, size u16, CRC-32 u32, flags u8 (bit 0: replace a preset with this id) |
| `05` | UPLOAD_COMMIT | none |
| `06` | DELETE | id u16, CRC-32 u32 of the preset's 22-byte name field |
| `07` | ABORT | none. No reply. |
| `08` | SONG_MIDI | song u8 (0..7) |
| `43` | DATA_CHUNK | index u16, then the data |

From the device:

| CMD | Name | Payload |
|---|---|---|
| `41` | IDENTITY | protocol u8, firmware major u8, minor u8, flags u8 (bit 0: simulated), presets u16 (files on the card), capacity u16 (the same count at most), name[8] |
| `42` | DATA_BEGIN | kind u8 (1 list, 2 preset, 3 song), size u32, CRC-32 u32, chunks u16 |
| `43` | DATA_CHUNK | index u16, then the data |
| `44` | DATA_END | chunks u16 |
| `45` | ACK | index u16 (`FFFF` answers UPLOAD_BEGIN) |
| `46` | DONE | id u16, presets u16 (files on the card, after the change) |
| `7F` | ERROR | command u8, code u8, detail u16 |

## Transfers

Data moves in chunks of at most 154 bytes, numbered from 0 and in order. Every transfer is checked as a whole with CRC-32 (zlib's, the one a preset file uses), on top of each message's `SUM`.

**From the device** (LIST, DOWNLOAD, SONG_MIDI): `DATA_BEGIN`, the chunks, `DATA_END`. The computer sends no ACKs and checks the size, the order and the CRC-32 at the end. If a check fails, it drops
everything it received and says so. A preset arrives as its 148-byte `.PRS` file, in one chunk. A song arrives as a Standard MIDI File (`docs/EXPORT.md`), at most 256 KB. The device works it out first, so DATA_BEGIN can take as long as a DONE. A song with no rows, or over 256 KB, gets ERROR 16. A list arrives as 29-byte entries:

    id u16, category u8, voices u8, bank[2], flags u8 (bit 0: built in), name[22]

**To the device** (upload): each step waits for the device's answer, so the device sets the pace.

    UPLOAD_BEGIN      -> ACK FFFF, or ERROR
    DATA_CHUNK 0      -> ACK 0
    ...
    UPLOAD_COMMIT     -> DONE, or ERROR

The device keeps the data in memory, and nothing reaches the card before `UPLOAD_COMMIT`. There it checks the size, the CRC-32 and the file itself (`parsePresetFile`), and that the id inside the file
is the one announced. An upload that is silent for 2 s is dropped, with ERROR 11. While one is open, other requests get ERROR 10, except IDENTIFY.

**Delete** names the preset twice, by id and by the CRC-32 of its name field. A list that has gone stale, or a stray message, then removes nothing (ERROR 15).

**The computer's timeouts:** 1 s for an answer, 30 s for one that has to reach the card (DONE): the first write after a start reads the whole card through first (`docs/STORAGE.md`). A transfer that fails is not resumed. The person is told, and starts it again.

## Errors

| Code | Name | Meaning |
|---|---|---|
| 1 | BAD_MESSAGE | `SUM` is wrong or the message is cut off |
| 2 | UNKNOWN_COMMAND | |
| 3 | BAD_VERSION | The message has another protocol version |
| 4 | NOT_FOUND | No preset with that id. Detail: the id. |
| 5 | BUILT_IN | The three built-in presets (ids 0 to 2) are listed, and cannot be downloaded, replaced or deleted |
| 6 | BAD_PRESET | The file does not pass `parsePresetFile`, or its id is not the one announced |
| 7 | CRC_MISMATCH | The whole transfer does not match its CRC-32 |
| 8 | NO_CARD | |
| 9 | LIBRARY_FULL | 1024 files on the card (`kPresetCap`). The built-in three do not count. |
| 10 | BUSY | An upload is open, or the firmware cannot touch the card now |
| 11 | TIMEOUT | An upload went silent |
| 12 | SEQUENCE | A chunk came out of order, or too much data |
| 13 | ID_IN_USE | Upload without the replace flag, and that id is taken |
| 14 | WRITE_FAILED | The card write failed |
| 15 | NAME_MISMATCH | DELETE: the name's CRC-32 is not this preset's |
| 16 | NO_SONG | SONG_MIDI: the song has no rows, is too long to send, or an export is running |

## Timing

The cable carries 3125 bytes a second. One preset takes about 80 ms each way, before the card's time. A list of 200 presets is 5800 bytes in 38 chunks, about 2.3 s.

## The firmware side

`midi/SysexIn.h` collects messages in the UART interrupt, in two slots, and the main loop hands them to `midi/PresetLink`. No card access happens in an interrupt or the audio path. Replies go out through `midi::sendLong`: they wait for the note message in progress, and notes wait behind a reply, so a reply never has a note in the middle of it. Writes go through the preset library and `Card::beginWrite`, so a refused card gets WRITE_FAILED. A transfer is allowed while a pattern plays: the sound runs on its own interrupt.

The page asks for a confirmation before an upload or a delete. That lives in the page, so a firmware side must not assume it: `DELETE` and `UPLOAD_COMMIT` take effect as soon as they are valid.
