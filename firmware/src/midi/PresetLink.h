#ifndef __MIDI_PRESET_LINK_H
#define __MIDI_PRESET_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "app/PresetFile.h"

// The preset protocol's device side (docs/PRESET_SYSEX.md): the website's librarian lists, fetches, sends and deletes presets over the MIDI sockets. Main loop only:
// a whole message comes in from midi::SysexIn, and what goes out waits its turn on the socket, which step() checks. Nothing reaches the card before UPLOAD_COMMIT,
// and every write goes through the preset library and so through Card::beginWrite.
namespace midi {

class PresetLink {
   public:
    using Send = bool (*)(const uint8_t* bytes, int n);   // false: still sending the last one
    using Busy = void (*)(const char* message);           // long card work is coming: for the screen
    using SongMidi = const uint8_t* (*)(int song, uint32_t& size);   // a song as a MIDI file, kept until the next call; nullptr: there is none now
    PresetLink(Send send, Busy busy) : send_(send), busy_(busy) {}

    void receive(const uint8_t* msg, int n, uint32_t nowMs);   // F0 .. F7
    void step(uint32_t nowMs);                                 // once a main-loop pass
    bool changed();                                            // the library changed since this was last asked
    bool transferring() const { return upOpen_ || outKind_ != 0; }
    void setSongMidi(SongMidi f) { songMidi_ = f; }

   private:
    void handle(uint8_t cmd, uint8_t tag, const uint8_t* p, int n, uint32_t nowMs);
    void reply(uint8_t cmd, uint8_t tag, const uint8_t* payload, int n);
    void error(uint8_t forCmd, uint8_t tag, uint8_t code, uint16_t detail = 0);
    void beginOut(uint8_t kind, uint8_t tag, uint32_t size, uint32_t crc);
    int listBytes(uint32_t offset, uint8_t* out, int want) const;
    uint32_t listSize() const;
    uint32_t listCrc() const;
    bool nextOut();

    Send send_;
    Busy busy_;
    uint8_t out_[200];
    int outLen_ = 0;               // a message waiting for the socket
    // what is going out, chunk by chunk
    uint8_t outKind_ = 0, outTag_ = 0;
    SongMidi songMidi_ = nullptr;
    const uint8_t* outData_ = nullptr;
    uint32_t outSize_ = 0, outAt_ = 0;
    uint16_t outChunk_ = 0, outChunks_ = 0;
    uint8_t file_[app::kPresetBodySize];
    // what is coming in
    bool upOpen_ = false, upReplace_ = false;
    uint8_t upTag_ = 0, upData_[app::kPresetBodySize];
    uint16_t upId_ = 0, upNext_ = 0;
    uint32_t upSize_ = 0, upAt_ = 0, upCrc_ = 0, upLastMs_ = 0;
    bool changed_ = false;
};

}  // namespace midi

#endif
