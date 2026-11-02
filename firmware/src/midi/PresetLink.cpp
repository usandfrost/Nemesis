#include "midi/PresetLink.h"

#include <string.h>

#include "app/Params.h"

namespace midi {

using namespace app;

namespace {
constexpr uint8_t kVersion = 1;
enum : uint8_t { SONG_MIDI = 0x08, IDENTIFY = 0x01, LIST = 0x02, DOWNLOAD = 0x03, UPLOAD_BEGIN = 0x04, UPLOAD_COMMIT = 0x05, DELETE_ = 0x06, ABORT = 0x07, IDENTITY = 0x41, DATA_BEGIN = 0x42,
                 DATA_CHUNK = 0x43, DATA_END = 0x44, ACK = 0x45, DONE = 0x46, ERROR = 0x7F };
enum : uint8_t { E_BAD_MESSAGE = 1, E_UNKNOWN = 2, E_VERSION = 3, E_NOT_FOUND = 4, E_BUILT_IN = 5, E_BAD_PRESET = 6, E_CRC = 7, E_NO_CARD = 8, E_FULL = 9, E_BUSY = 10, E_TIMEOUT = 11,
                 E_SEQUENCE = 12, E_IN_USE = 13, E_WRITE = 14, E_NAME = 15, E_NO_SONG = 16 };
enum : uint8_t { KIND_LIST = 1, KIND_PRESET = 2, KIND_SONG = 3 };
constexpr int kChunk = 154, kEntry = 29, kNameField = kPresetNameMax + 2;
constexpr uint16_t kAckBegin = 0xFFFF;
constexpr uint32_t kStallMs = 2000;

// a preset list entry, for the list's i-th preset: id, category, voices, banks, flags (bit 0: built in), the 22 bytes of the name
void entry(int i, uint8_t out[kEntry]) {
    const int id = presetIdAt(i);
    memset(out, 0, kEntry);
    presetWr16(out, (uint32_t)id);
    out[2] = (uint8_t)presetCat(id);
    out[3] = (uint8_t)presetVoices(id);
    int banks[2];
    presetBanks(id, banks);
    out[4] = banks[0] < 0 ? 255 : (uint8_t)banks[0];
    out[5] = banks[1] < 0 ? 255 : (uint8_t)banks[1];
    out[6] = id < kBasePresets ? 1 : 0;
    strncpy(reinterpret_cast<char*>(out + 7), presetName(id), kNameField);
}

// eight bytes on the wire for every seven of data: the first holds their top bits, bit 0 for the group's first byte
int pack7(const uint8_t* d, int n, uint8_t* out) {
    int w = 0;
    for (int i = 0; i < n; i += 7) {
        const int c = n - i < 7 ? n - i : 7;
        uint8_t top = 0;
        for (int k = 0; k < c; ++k) top = (uint8_t)(top | ((d[i + k] >> 7) & 1) << k);
        out[w++] = top;
        for (int k = 0; k < c; ++k) out[w++] = d[i + k] & 0x7F;
    }
    return w;
}
int unpack7(const uint8_t* d, int n, uint8_t* out) {
    int w = 0;
    for (int i = 0; i < n; i += 8) {
        const uint8_t top = d[i];
        for (int k = 1; k < 8 && i + k < n; ++k) out[w++] = (uint8_t)(d[i + k] | ((top >> (k - 1)) & 1) << 7);
    }
    return w;
}
}  // namespace

bool PresetLink::changed() {
    const bool c = changed_;
    changed_ = false;
    return c;
}

void PresetLink::reply(uint8_t cmd, uint8_t tag, const uint8_t* payload, int n) {
    int w = 0;
    out_[w++] = 0xF0;
    out_[w++] = 0x7D;
    out_[w++] = 0x4E;
    out_[w++] = 0x50;
    const int body = w;
    out_[w++] = kVersion;
    out_[w++] = cmd;
    out_[w++] = tag & 0x7F;
    w += pack7(payload, n, out_ + w);
    uint32_t sum = 0;
    for (int i = body; i < w; ++i) sum += out_[i];
    out_[w++] = (uint8_t)(sum & 0x7F);
    out_[w++] = 0xF7;
    outLen_ = w;
    if (send_(out_, outLen_)) outLen_ = 0;
}

void PresetLink::error(uint8_t forCmd, uint8_t tag, uint8_t code, uint16_t detail) {
    const uint8_t p[4] = {forCmd, code, (uint8_t)detail, (uint8_t)(detail >> 8)};
    reply(ERROR, tag, p, 4);
}

uint32_t PresetLink::listSize() const { return (uint32_t)presetCount() * kEntry; }

uint32_t PresetLink::listCrc() const {
    uint32_t c = 0xFFFFFFFFu;
    uint8_t e[kEntry];
    for (int i = 0; i < presetCount(); ++i) {
        entry(i, e);
        c = presetCrcUpdate(c, e, kEntry);
    }
    return ~c;
}

int PresetLink::listBytes(uint32_t offset, uint8_t* out, int want) const {
    int w = 0;
    uint8_t e[kEntry];
    while (w < want && offset < listSize()) {
        entry((int)(offset / kEntry), e);
        const int at = (int)(offset % kEntry), take = kEntry - at < want - w ? kEntry - at : want - w;
        memcpy(out + w, e + at, (size_t)take);
        w += take;
        offset += (uint32_t)take;
    }
    return w;
}

void PresetLink::beginOut(uint8_t kind, uint8_t tag, uint32_t size, uint32_t crc) {
    outKind_ = kind;
    outTag_ = tag;
    outSize_ = size;
    outAt_ = 0;
    outChunk_ = 0;
    outChunks_ = (uint16_t)((size + kChunk - 1) / kChunk);
    uint8_t p[11];
    p[0] = kind;
    presetWr32(p + 1, size);
    presetWr32(p + 5, crc);
    presetWr16(p + 9, outChunks_);
    reply(DATA_BEGIN, tag, p, sizeof(p));
}

// The next message of what is going out, once the socket has taken the last. False: nothing more.
bool PresetLink::nextOut() {
    if (!outKind_) return false;
    if (outChunk_ >= outChunks_) {
        uint8_t p[2];
        presetWr16(p, outChunks_);
        const uint8_t tag = outTag_;
        outKind_ = 0;
        reply(DATA_END, tag, p, 2);
        return true;
    }
    uint8_t p[kChunk + 2];
    presetWr16(p, outChunk_);
    const uint32_t left = outSize_ - outAt_;
    const int n = left < (uint32_t)kChunk ? (int)left : kChunk;
    if (outKind_ == KIND_LIST) listBytes(outAt_, p + 2, n);
    else if (outKind_ == KIND_SONG) memcpy(p + 2, outData_ + outAt_, (size_t)n);
    else memcpy(p + 2, file_ + outAt_, (size_t)n);
    outAt_ += (uint32_t)n;
    ++outChunk_;
    reply(DATA_CHUNK, outTag_, p, n + 2);
    return true;
}

void PresetLink::step(uint32_t nowMs) {
    if (outLen_ && send_(out_, outLen_)) outLen_ = 0;
    if (!outLen_) nextOut();
    if (upOpen_ && nowMs - upLastMs_ > kStallMs) {
        upOpen_ = false;
        error(DATA_CHUNK, upTag_, E_TIMEOUT);
    }
}

void PresetLink::receive(const uint8_t* m, int n, uint32_t nowMs) {
    if (n < 9 || m[0] != 0xF0 || m[n - 1] != 0xF7) return;   // (SysexIn only hands over whole ones, with our head)
    const uint8_t cmd = m[5] & 0x7F, tag = m[6] & 0x7F;
    uint32_t sum = 0;
    bool clean = true;
    for (int i = 4; i < n - 2; ++i) { sum += m[i]; clean = clean && m[i] < 0x80; }
    if (!clean || (sum & 0x7F) != m[n - 2]) { error(cmd, tag, E_BAD_MESSAGE); return; }
    if (m[4] != kVersion) { error(cmd, tag, E_VERSION); return; }
    static uint8_t p[kChunk + 16];
    const int packed = n - 9;
    if (packed > (int)sizeof(p) * 8 / 7) { error(cmd, tag, E_BAD_MESSAGE); return; }
    const int len = unpack7(m + 7, packed, p);
    handle(cmd, tag, p, len, nowMs);
}

void PresetLink::handle(uint8_t cmd, uint8_t tag, const uint8_t* p, int n, uint32_t nowMs) {
    // what a device sends, heard back on a looped cable or from another Nemesis: never answered, or two of them would talk for ever
    if (cmd >= IDENTITY && cmd != DATA_CHUNK) return;
    // one thing at a time: there is one message's room for going out, and it may be in use. ABORT also stops a transfer that is going out.
    if (outKind_ || outLen_) {
        if (cmd == ABORT) outKind_ = 0;
        return;
    }
    if (upOpen_ && cmd != DATA_CHUNK && cmd != UPLOAD_COMMIT && cmd != ABORT && cmd != IDENTIFY) { error(cmd, tag, E_BUSY); return; }
    switch (cmd) {
        case IDENTIFY: {
            uint8_t r[16] = {kVersion, 0, 2, 0};   // the protocol, firmware 0.2 (Beta 2), not simulated
            presetWr16(r + 4, (uint32_t)presetFiles());
            presetWr16(r + 6, kPresetCap);
            memcpy(r + 8, "NEMESIS", 7);
            reply(IDENTITY, tag, r, sizeof(r));
            break;
        }
        case LIST:
            beginOut(KIND_LIST, tag, listSize(), listCrc());
            break;
        case SONG_MIDI: {
            if (n < 1) { error(cmd, tag, E_BAD_MESSAGE); break; }
            if (!songMidi_) { error(cmd, tag, E_UNKNOWN); break; }
            uint32_t size = 0;
            outData_ = songMidi_(p[0], size);
            if (!outData_) { error(cmd, tag, E_NO_SONG, p[0]); break; }
            beginOut(KIND_SONG, tag, size, presetCrc(outData_, size));
            break;
        }
        case DOWNLOAD: {
            if (n < 2) { error(cmd, tag, E_BAD_MESSAGE); break; }
            const int id = presetRd16(p);
            if (id < kBasePresets) { error(cmd, tag, E_BUILT_IN, (uint16_t)id); break; }
            if (!presetLibraryOpen()) { error(cmd, tag, E_NO_CARD); break; }
            if (!presetExists(id)) { error(cmd, tag, E_NOT_FOUND, (uint16_t)id); break; }
            if (!presetReadFile(id, file_)) { error(cmd, tag, E_NOT_FOUND, (uint16_t)id); break; }
            beginOut(KIND_PRESET, tag, kPresetBodySize, presetCrc(file_, kPresetBodySize));
            break;
        }
        case UPLOAD_BEGIN: {
            if (n < 9) { error(cmd, tag, E_BAD_MESSAGE); break; }
            const int id = presetRd16(p), size = presetRd16(p + 2);
            const uint32_t crc = presetRd32(p + 4);
            const bool replace = p[8] & 1;
            if (!presetLibraryOpen()) { error(cmd, tag, E_NO_CARD); break; }
            if (size != kPresetBodySize) { error(cmd, tag, E_BAD_PRESET); break; }
            if (id < kBasePresets) { error(cmd, tag, E_BUILT_IN, (uint16_t)id); break; }
            const bool have = presetExists(id);
            if (have && !replace) { error(cmd, tag, E_IN_USE, (uint16_t)id); break; }
            if (!have && presetFiles() >= kPresetCap) { error(cmd, tag, E_FULL); break; }
            upOpen_ = true;
            upReplace_ = replace;
            upTag_ = tag;
            upId_ = (uint16_t)id;
            upSize_ = (uint32_t)size;
            upCrc_ = crc;
            upAt_ = 0;
            upNext_ = 0;
            upLastMs_ = nowMs;
            uint8_t a[2];
            presetWr16(a, kAckBegin);
            reply(ACK, tag, a, 2);
            break;
        }
        case DATA_CHUNK: {
            if (!upOpen_ || tag != upTag_ || n < 2) { error(cmd, tag, E_SEQUENCE); break; }
            const int i = presetRd16(p), got = n - 2;
            if (i != upNext_ || upAt_ + (uint32_t)got > upSize_) { upOpen_ = false; error(cmd, tag, E_SEQUENCE, (uint16_t)i); break; }
            memcpy(upData_ + upAt_, p + 2, (size_t)got);
            upAt_ += (uint32_t)got;
            ++upNext_;
            upLastMs_ = nowMs;
            uint8_t a[2];
            presetWr16(a, (uint32_t)i);
            reply(ACK, tag, a, 2);
            break;
        }
        case UPLOAD_COMMIT: {
            if (!upOpen_ || tag != upTag_) { error(cmd, tag, E_SEQUENCE); break; }
            upOpen_ = false;
            if (upAt_ != upSize_) { error(cmd, tag, E_SEQUENCE); break; }
            if (presetCrc(upData_, upSize_) != upCrc_) { error(cmd, tag, E_CRC); break; }
            busy_("SAVING PRESET\xE2\x80\xA6");
            switch (presetPutFile(upData_, upSize_, upId_, upReplace_)) {
                case PresetPut::Ok: {
                    changed_ = true;
                    uint8_t r[4];
                    presetWr16(r, upId_);
                    presetWr16(r + 2, (uint32_t)presetFiles());
                    reply(DONE, tag, r, 4);
                    break;
                }
                case PresetPut::NoCard: error(cmd, tag, E_NO_CARD); break;
                case PresetPut::Full: error(cmd, tag, E_FULL); break;
                case PresetPut::BuiltIn: error(cmd, tag, E_BUILT_IN, upId_); break;
                case PresetPut::InUse: error(cmd, tag, E_IN_USE, upId_); break;
                case PresetPut::BadPreset: error(cmd, tag, E_BAD_PRESET); break;
                default: error(cmd, tag, E_WRITE); break;
            }
            break;
        }
        case DELETE_: {
            if (n < 6) { error(cmd, tag, E_BAD_MESSAGE); break; }
            const int id = presetRd16(p);
            busy_("REMOVING PRESET\xE2\x80\xA6");
            switch (presetRemove(id, presetRd32(p + 2))) {
                case PresetPut::Ok: {
                    changed_ = true;
                    uint8_t r[4];
                    presetWr16(r, (uint32_t)id);
                    presetWr16(r + 2, (uint32_t)presetFiles());
                    reply(DONE, tag, r, 4);
                    break;
                }
                case PresetPut::NoCard: error(cmd, tag, E_NO_CARD); break;
                case PresetPut::BuiltIn: error(cmd, tag, E_BUILT_IN, (uint16_t)id); break;
                case PresetPut::Missing: error(cmd, tag, E_NOT_FOUND, (uint16_t)id); break;
                case PresetPut::WrongName: error(cmd, tag, E_NAME, (uint16_t)id); break;
                default: error(cmd, tag, E_WRITE); break;
            }
            break;
        }
        case ABORT:
            upOpen_ = false;   // no answer: the other side has moved on
            break;
        default:
            error(cmd, tag, E_UNKNOWN);
    }
}

}  // namespace midi
