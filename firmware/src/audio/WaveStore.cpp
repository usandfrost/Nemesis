#include "audio/WaveStore.h"

#include <string.h>

namespace audio {

using nemesis::dsp::Real;
using nemesis::dsp::Wavetable;

size_t WaveStore::slotSamples() { return Wavetable::pyramidSamples(kFrames, kFrameLen, true); }

void WaveStore::init(Real* arena, int slots, int16_t* staging, SharedTables* pub, WaveFile* file) {
    arena_ = arena;
    slots_ = slots < 0 ? 0 : slots > kMaxSlots ? kMaxSlots : slots;
    staging_ = staging;
    pub_ = pub;
    file_ = file;
    for (int i = 0; i < slots_; ++i) {
        slot_[i] = Slot();
        slot_[i].wt.setStorage(slotBase(i), slotSamples());   // the first slot is the top of the arena: the shared memory fills from there
    }
}

bool WaveStore::resident(int bank) const {
    for (int i = 0; i < slots_; ++i) if (slot_[i].bank == bank && slot_[i].published) return true;
    return false;
}
int WaveStore::highSlot() const {
    int n = 0;
    for (int i = 0; i < slots_; ++i) if (slot_[i].bank >= 0) n = i + 1;
    return n;
}
int WaveStore::residentCount() const {
    int n = 0;
    for (int i = 0; i < slots_; ++i) n += slot_[i].bank >= 0 && slot_[i].published;
    return n;
}

// CRC-32 (the zlib polynomial) by nibbles: a 64-byte table instead of a bit loop. Run over small chunks in internal RAM (the payload itself is
// in uncached SDRAM, which a byte loop would crawl through).
static uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc) {
    static const uint32_t tab[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
                                     0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ tab[crc & 15];
        crc = (crc >> 4) ^ tab[crc & 15];
    }
    return ~crc;
}
static uint32_t rd16(const uint8_t* h, int o) { return h[o] | (h[o + 1] << 8); }
static uint32_t rd32(const uint8_t* h, int o) { return rd16(h, o) | (rd16(h, o + 2) << 16); }

// Reads bank's file into the slot: header, payload (checked against its CRC), then the table is adopted. False: no file, damaged, or not the
// geometry the arena is sized for.
bool WaveStore::load(int si, int bank) {
    Slot& s = slot_[si];
    if (!file_ || !file_->open(bank)) return false;
    bool ok = false;
    uint8_t h[64];
    const size_t cap = slotSamples();
    // a bank has 32 frames, or 16 (a few of the catalogue's): either fits the slot; the payload must be exactly what that geometry holds
    int frames = 0;
    size_t want = 0;
    if (file_->read(h, 64) && memcmp(h, "DBWP", 4) == 0 && rd16(h, 4) == 1 && (rd16(h, 6) & 1) && (int)rd16(h, 10) == kFrameLen) {
        frames = (int)rd16(h, 8);
        if (frames >= 1 && frames <= kFrames) want = Wavetable::pyramidSamples(frames, kFrameLen, true);
    }
    if (want != 0 && want <= cap && rd32(h, 20) == want * 2) {
        float scale;
        memcpy(&scale, h + 16, 4);
        // the header is not under the CRC: a scale that is not a small positive number would put garbage (or NaN) into the oscillators
        if (!(scale > 0.0f && scale < 0.1f)) { file_->close(); return false; }
        // the payload goes to the staging buffer, or (the board, Real = float) into the slot's own second half, where converting forward in place is safe
        int16_t* q = staging_ ? staging_ : reinterpret_cast<int16_t*>(slotBase(si)) + cap;   // (the slot's second half: cap floats = 2 x cap int16)
        if (!staging_ && sizeof(Real) != 4) { file_->close(); return false; }
        uint8_t chunk[2048];
        uint8_t* dst = reinterpret_cast<uint8_t*>(q);
        uint32_t crc = 0;
        size_t left = want * 2;
        bool got = true;
        while (left && got) {
            const size_t n = left < sizeof(chunk) ? left : sizeof(chunk);
            got = file_->read(chunk, n);
            if (got) {
                crc = crc32(chunk, n, crc);
                memcpy(dst, chunk, n);
                dst += n;
                left -= n;
            }
        }
        if (got && crc == rd32(h, 24)) {
            ok = s.wt.adoptPyramid(frames, kFrameLen, true, q, want, scale);
            bytesRead_ += (uint32_t)(want * 2 + 64);
        }
    }
    file_->close();
    return ok;
}

bool WaveStore::want(const uint8_t bits[(app::kWaveBanks + 7) / 8], uint32_t now) {
    refused_ = -1;
    bool settled = true;
    auto wanted = [&](int b) { return b >= 0 && b < app::kWaveBanks && (bits[b >> 3] >> (b & 7) & 1); };
    // 1. banks nobody asks for any more: un-publish (the audio interrupt re-binds those voices to the basic wave), free the slot a moment later
    for (int i = 0; i < slots_; ++i) {
        Slot& s = slot_[i];
        if (s.bank >= 0 && !wanted(s.bank)) {
            if (s.published) {
                pub_->publish(s.bank, nullptr);
                s.published = false;
                s.freeAt = now + kRetireMs;
            }
            if ((int32_t)(now - s.freeAt) >= 0) s.bank = -1;
            else settled = false;
        }
    }
    // 2. banks that are asked for and not in memory: into a free slot
    for (int b = 0; b < app::kWaveBanks; ++b) {
        if (!wanted(b) || !app::waveSet().present[b] || resident(b)) continue;
        // asked for again while its slot is still waiting out the retire time: the table is intact, it goes back on
        bool back = false;
        for (int i = 0; i < slots_; ++i) {
            if (slot_[i].bank == b && !slot_[i].published) { pub_->publish(b, &slot_[i].wt); slot_[i].published = true; back = true; break; }
        }
        if (back) continue;
        int free = -1;
        bool retiring = false;
        for (int i = 0; i < slots_; ++i) {
            if (slot_[i].bank < 0) { if (free < 0 && i < limit_) free = i; }
            else if (!slot_[i].published) retiring = true;
        }
        if (free < 0) {
            if (!retiring) refused_ = refused_ < 0 ? b : refused_;   // really out of slots; a slot that is just being let go is only a short wait
            settled = false;
            continue;
        }
        Slot& s = slot_[free];
        s.bank = b;
        s.published = false;
        if (load(free, b)) {
            pub_->publish(b, &s.wt);
            s.published = true;
        } else {
            s.bank = -1;   // no file, or a damaged one: the oscillator plays the basic wave it is nearest to
            app::waveSet().present[b] = false;
        }
    }
    return settled;
}

void neededBanks(const app::Project& pj, uint8_t bits[(app::kWaveBanks + 7) / 8]) {
    memset(bits, 0, (app::kWaveBanks + 7) / 8);
    for (int p = 0; p < app::kParts; ++p) {
        if (pj.parts[p].type != app::PT_CATHODE) continue;
        for (int pg : {app::PG_OSC1, app::PG_OSC2}) {
            const int w = pj.parts[p].pages[pg][0];
            if (w >= app::kBasicWaves && w <= app::kWaveMax) {
                const int b = w - app::kBasicWaves;
                bits[b >> 3] = (uint8_t)(bits[b >> 3] | (1u << (b & 7)));
            }
        }
    }
}

}  // namespace audio
