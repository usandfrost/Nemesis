#include "audio/SongExport.h"

#include <math.h>
#include <string.h>

#include "app/MidiExport.h"

namespace audio {

using namespace app;

namespace {

constexpr int kBlock = 32;
constexpr uint32_t kRate = 44100;
constexpr uint32_t kTailMost = 12 * kRate, kTailQuiet = 2 * kRate;   // effects ring on after the last step: until this long has been silent, as a delay's gap can be 1.5 s, or this long at most
constexpr uint32_t kDrainMost = 6 * kRate;                          // what was sounding before a pass dies away: this long at most
constexpr float kSilence = 1.0f / 16384.0f;

// What a project's name can be as a folder on a card
void folderOf(const Project& pj, Str<48>& out) {
    out.add("Export/");
    const int at = out.size();
    for (const char* p = pj.name; *p; ++p) {
        const char c = *p;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
        out.ch(ok ? c : '_');
    }
    while (out.n > at && out.s[out.n - 1] == ' ') out.s[--out.n] = 0;   // (FAT will have no name that ends in a space)
    if (out.size() == at) out.add("PROJECT");
}

void wavHeader(uint8_t* h, uint32_t frames) {
    const uint32_t bytes = frames * 4;
    const auto put32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; ++i) h[o + i] = (uint8_t)(v >> (8 * i)); };
    const auto put16 = [&](int o, uint32_t v) { h[o] = (uint8_t)v; h[o + 1] = (uint8_t)(v >> 8); };
    memcpy(h, "RIFF", 4); put32(4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8); put32(16, 16); put16(20, 1); put16(22, 2);
    put32(24, kRate); put32(28, kRate * 4); put16(32, 4); put16(34, 16); memcpy(h + 36, "data", 4); put32(40, bytes);
}

struct FileSink : ByteSink {
    ExportFiles& f;
    explicit FileSink(ExportFiles& x) : f(x) {}
    bool write(const void* d, size_t n) override { return f.write(d, n); }
};

uint32_t songLength(const Project& pj, int song) {
    const Song& so = pj.songs[song];
    double steps = 0;
    for (int r = 0; r < so.nRows; ++r) {
        int longest = 1;
        for (const Lane& l : pj.patterns[so.rows[r].pat].lanes) if (l.n > longest) longest = l.n;
        steps += (double)so.rows[r].rep * longest;
    }
    return (uint32_t)(steps * 60.0 / pj.bpm / 4.0 * kRate);
}

}  // namespace

bool SongExport::lanePlays(const Project& pj, int song, int lane) {
    const Song& so = pj.songs[song];
    for (int r = 0; r < so.nRows; ++r) {
        const Lane& l = pj.patterns[so.rows[r].pat].lanes[lane];
        if (l.mute) continue;
        if (isSampler(pj, l.part) && l.smpParam(PG_SLC, SLC_MODE) == SLICE_SYNC) return true;
        for (int i = 0; i < l.n; ++i) if (trig(l, i, false)) return true;
    }
    return false;
}

bool SongExport::start(int kind, int song) {
    if (song < 0 || song >= kSongs || pj_.songs[song].nRows == 0) return false;
    kind_ = kind;
    song_ = song;
    stem_ = 0;
    stems_ = 0;
    cancel_ = false;
    open_ = false;
    result_ = EXPORT_RUNNING;
    why_ = nullptr;
    for (int l = 0; l < kLanes; ++l) if (lanePlays(pj_, song, l)) lanes_[stems_++] = (uint8_t)l;
    if (stems_ == 0) { why_ = "NOTHING PLAYS IN THIS SONG"; return false; }
    if (kind != EXPORT_STEMS) stems_ = 1;
    if (kind != EXPORT_MIDI) {
        savedLoop_ = pj_.songs[song].loop;
        savedPat_ = tp_.pat;
        savedSong_ = tp_.songIdx;
        savedRow_ = tp_.playRow;
        savedRep_ = tp_.repLeft;
        savedNext_ = tp_.nextPat;
        savedDegrade_ = inst_.autoDegrade();
        pj_.songs[song].loop = 0;
        tp_.fill = 0;
        inst_.setAutoDegrade(false);
        inst_.setRendering(true);
    }
    songFrames_ = songLength(pj_, song);
    if (nextFile()) return true;
    finish(EXPORT_FAILED);
    return false;
}

// First what was sounding dies away, in step(): it must not ring into the file
void SongExport::beginPass() {
    tp_.playing = 0;
    inst_.stopAllSound();
    drain_ = true;
    drained_ = 0;
    tail_ = false;
    quiet_ = 0;
    tailFrames_ = 0;
    frames_ = 0;
    have_ = 0;
}

void SongExport::startSong() {
    drain_ = false;
    inst_.clearFx();
    inst_.setOnlyLane(lane_);
    inst_.reseed();
    const Song& so = pj_.songs[song_];
    tp_.songIdx = (uint8_t)song_;
    tp_.pat = so.rows[0].pat;
    tp_.nextPat = -1;
    tp_.jumpTo = -1;
    tp_.playRow = 0;
    tp_.repLeft = so.rows[0].rep;
    tp_.pstep = 0;
    tp_.gstep = -1;
    tp_.songPlaying = 1;
    tp_.playGen = tp_.playGen + 1;
    tp_.playing = 1;
}

void SongExport::endPass() {
    tp_.playing = 0;
    tp_.songPlaying = 0;
    inst_.setOnlyLane(-1);
}

bool SongExport::nextFile() {
    Str<48> folder;
    folderOf(pj_, folder);
    Str<64> name;
    name.add("Song ").num(song_ + 1);
    lane_ = -1;
    if (kind_ == EXPORT_MIDI) {
        name.add(".mid");
        if (!files_.open(folder.c_str(), name.c_str())) return false;
        scratch_ = pj_;
        FileSink sink(files_);
        const bool wrote = writeSongMidi(scratch_, song_, &lib_, sink);
        if (!wrote && midiTooManyNotes()) why_ = "TOO MANY NOTES FOR A MIDI FILE";
        return files_.close(wrote) && wrote;
    }
    if (kind_ == EXPORT_STEMS) {
        lane_ = lanes_[stem_];
        name.add(" lane ");
        if (lane_ + 1 < 10) name.ch('0');
        name.num(lane_ + 1);
        const Lane& l = pj_.patterns[pj_.songs[song_].rows[laneFirstRow(pj_, song_, lane_)].pat].lanes[lane_];
        if (isSampler(pj_, l.part) && l.sample() >= 0 && l.sample() < lib_.count) {
            name.ch(' ');
            for (const char* p = lib_.items[l.sample()].name; *p; ++p) name.ch(strchr("/\\:?*\"<>|", *p) ? '_' : *p);
        } else {
            name.add(" part ").add(kPartName[l.part]);
        }
    }
    name.add(".wav");
    if (!files_.open(folder.c_str(), name.c_str())) return false;
    open_ = true;
    uint8_t h[44];
    wavHeader(h, 0);
    if (!files_.write(h, sizeof h)) return false;
    beginPass();
    return true;
}

bool SongExport::finishFile() {
    if (have_ && !files_.write(buffer_, (size_t)have_ * 4)) return false;
    have_ = 0;
    uint8_t h[44];
    wavHeader(h, frames_);
    if (!files_.rewrite(0, h, sizeof h)) return false;
    open_ = false;
    return files_.close(true);
}

void SongExport::finish(uint8_t result) {
    if (open_) { files_.close(false); open_ = false; }
    if (kind_ != EXPORT_MIDI) {
        endPass();
        pj_.songs[song_].loop = savedLoop_;
        tp_.pat = savedPat_;
        tp_.songIdx = savedSong_;
        tp_.playRow = savedRow_;
        tp_.repLeft = savedRep_;
        tp_.nextPat = savedNext_;
        tp_.jumpTo = -1;
        tp_.pstep = 0;
        tp_.gstep = -1;
        tp_.shownStep = -1;
        inst_.stopAllSound();
        inst_.clearFx();          // (what was left of a render given up must not be heard)
        inst_.setRendering(false);
        inst_.setAutoDegrade(savedDegrade_);
    }
    result_ = result;
}

bool SongExport::step(ExportStatus& s, int blocks) {
    s.kind = (uint8_t)kind_;
    s.stem = (uint8_t)(stem_ + 1);
    s.stems = (uint8_t)stems_;
    s.percent = 100;
    s.missing = (uint16_t)missing_;
    if (result_ != EXPORT_RUNNING) { s.result = result_; return false; }
    if (kind_ == EXPORT_MIDI) { finish(EXPORT_DONE); s.result = result_; return false; }   // (the file was made in one go: it takes a moment)
    if (cancel_) { finish(EXPORT_STOPPED); s.result = result_; return false; }

    for (int b = 0; b < blocks; ++b) {
        float l[kBlock], r[kBlock];
        inst_.beginBlock();
        inst_.render(l, r, kBlock);
        if (drain_) {
            int sounding = 0;
            for (int p = 0; p < kParts; ++p) sounding += inst_.activeVoices(p);
            drained_ += kBlock;
            if (sounding == 0 || drained_ >= kDrainMost) startSong();
            continue;
        }
        float peak = 0.0f;
        for (int i = 0; i < kBlock; ++i) {
            const float a = fabsf(l[i]), c = fabsf(r[i]);
            peak = a > peak ? a : peak;
            peak = c > peak ? c : peak;
            const float x = l[i] < -1.0f ? -1.0f : l[i] > 1.0f ? 1.0f : l[i], y = r[i] < -1.0f ? -1.0f : r[i] > 1.0f ? 1.0f : r[i];
            buffer_[2 * have_] = (int16_t)(x * 32767.0f);
            buffer_[2 * have_ + 1] = (int16_t)(y * 32767.0f);
            ++have_;
        }
        frames_ += kBlock;
        if (have_ + kBlock > kBufferFrames) {
            if (!files_.write(buffer_, (size_t)have_ * 4)) { finish(EXPORT_FAILED); s.result = result_; return false; }
            have_ = 0;
        }
        if (!tp_.playing && !tail_) tail_ = true;
        if (tail_) {
            tailFrames_ += kBlock;
            quiet_ = peak < kSilence ? quiet_ + kBlock : 0;
            if (quiet_ >= kTailQuiet || tailFrames_ >= kTailMost) {
                endPass();
                if (!finishFile()) { finish(EXPORT_FAILED); s.result = result_; return false; }
                if (++stem_ >= stems_) { finish(EXPORT_DONE); s.result = result_; return false; }
                if (!nextFile()) { finish(EXPORT_FAILED); s.result = result_; return false; }
                break;
            }
        }
    }
    const uint32_t each = songFrames_ ? songFrames_ : 1, f = drain_ ? 0 : tp_.playing ? frames_ : each;
    const uint32_t part = f >= each ? 100 : (uint32_t)((uint64_t)f * 100 / each);
    s.percent = (uint8_t)((stem_ * 100 + part) / stems_);
    s.stem = (uint8_t)(stem_ + 1);
    s.result = EXPORT_RUNNING;
    return true;
}

}  // namespace audio
