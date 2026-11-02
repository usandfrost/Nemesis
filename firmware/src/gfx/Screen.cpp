#include "gfx/Screen.h"

#include <math.h>

#include "app/MidiMap.h"
#include "app/WaveLib.h"
#include "gfx/TrackerLayout.h"
#include "gfx/fonts/Fonts.h"

namespace gfx {

using namespace app;
namespace F = fonts;

// ---- the mockup's colours (CSS :root and the rules that use them)
constexpr uint32_t kScreenBg = 0x000000, kInk = 0xe8eef2, kDim = 0x6c7882, kLine = 0x20282e, kLock = 0xd7ff3c, kWarn = 0xff5d5d;
constexpr uint32_t kGood = 0x6df08c, kAmber = 0xffb02e, kDark = 0x001014, kRoVal = 0x9aa7b0, kMeterBg = 0x14191d, kStepOff = 0x11171b;
constexpr uint32_t kStepB4 = 0x151c21, kCellText = 0xcfd6da, kOffText = 0x3d474e, kTabLine = 0x2c353b, kListLine = 0x0d1215, kWhite = 0xffffff;
// letter-spacing in 1/16 px: .02em, .04em, .06em, .1em of 16 px; -.02em of 25 px
constexpr int kLs02 = 5, kLs04 = 10, kLs06 = 15, kLs10 = 26, kLsVal = -8;
constexpr int kOpacity28 = 71, kOpacity60 = 153, kOpacity62 = 158, kOpacity70 = 179, kOpacity75 = 191, kOpacity85 = 217;
constexpr int kMainTop = 87, kMainH = 366;

static uint32_t modeAccent(Mode m) {
    switch (m) {
        case Mode::Play: return 0x19d3e6;
        case Mode::Step: return 0xffb02e;
        case Mode::Mixer: return 0x6df08c;
        case Mode::Browse: return 0x4f8cff;
        case Mode::Save: return 0x4f8cff;
        case Mode::Record: return 0xff5a5a;
        case Mode::ProjectM: return 0xc9d3d9;
        case Mode::Song: return 0x9fd4ff;
        case Mode::Fx: return 0xb48cff;
        default: return 0x19d3e6;
    }
}
static uint32_t accent(Ui& ui) { return ui.mode() == Mode::Sound ? pageDef(ui.page()).acc : modeAccent(ui.mode()); }

template <int N>
static void upper(Str<N>& s) {
    for (int i = 0; i < s.n; ++i) if (s.s[i] >= 'a' && s.s[i] <= 'z') s.s[i] = (char)(s.s[i] - 32);
}
static int jr(double v) { return (int)floor(v + 0.5); }

// A run of text that wraps at spaces inside `width` (CSS normal wrapping), as segments with their own font and colour.
struct Run {
    const Font* f;
    const char* s;
    uint32_t c;
    int ls;
};
static int drawRuns(Canvas& cv, int x, int y, int width, int lineH, const Run* runs, int n) {
    int px = x, py = y;
    for (int r = 0; r < n; ++r) {
        const char* p = runs[r].s;
        while (*p) {
            // the next word, with the spaces in front of it
            const char* q = p;
            while (*q == ' ') ++q;
            while (*q && *q != ' ') ++q;
            char word[64];
            int len = (int)(q - p) < 63 ? (int)(q - p) : 63;
            while (len > 0 && len < (int)(q - p) && ((uint8_t)p[len] & 0xC0) == 0x80) --len;   // never split a UTF-8 sequence
            q = p + len;
            for (int i = 0; i < len; ++i) word[i] = p[i];
            word[len] = 0;
            int w = Canvas::measure(*runs[r].f, word, runs[r].ls);
            if (px + w > x + width && px > x) {   // wrap: drop the leading spaces on the new line
                py += lineH;
                px = x;
                const char* t = word;
                while (*t == ' ') ++t;
                w = Canvas::measure(*runs[r].f, t, runs[r].ls);
                cv.text(*runs[r].f, px, py, t, runs[r].c, Align::Left, runs[r].ls);
            } else {
                cv.text(*runs[r].f, px, py, word, runs[r].c, Align::Left, runs[r].ls);
            }
            px += w;
            p = q;
        }
    }
    return py + lineH;
}

// ------------------------------------------------------------------------------------------------------------------------------------
// the frame: corners and the eight cells

static void corner(Canvas& cv, int x, int y0, const char* b, uint32_t bc, const char* span, int bls = kLs02) {
    cv.text(F::kDisplay700_16, x + 40, y0 + 19, b, bc, Align::Center, bls);
    cv.text(F::kDisplay600_16, x + 40, y0 + 44, span, kDim, Align::Center, kLs02);
}

static void drawCell(Canvas& cv, const Cell& c, int i, uint32_t acc) {
    const int x = 80 + 200 * (i % 4), y0 = i < 4 ? 0 : 455;
    cv.fill(x, y0, 1, 85, kLine);
    if (c.ro) cv.hatch(x + 1, y0, 199, 85, kWhite, 12);   // #ffffff07 is below one RGB565 step; 12 is the faintest that shows
    const uint32_t col = c.col ? c.col : acc;
    const int a = c.dis || c.idle ? kOpacity28 : 255;
    Str<20> lab = c.label;
    upper(lab);
    const int labW = 159 - Canvas::measure(F::kDisplay600_16, c.tag.c_str(), kLs06);   // (a long folder name is cut before it reaches the tag)
    cv.text(F::kDisplay600_16, x + 17, y0 + 14, lab.c_str(), c.ro ? kDim : col, Align::Left, kLs06, labW > 0 ? labW : 1, a);
    // the tag: the knob's name, or LOCK, INFO, SHIFT, a cost badge
    if (c.ro) {
        const int w = Canvas::measure(F::kDisplay600_16, c.tag.c_str(), kLs06);
        cv.stroke(x + 184 - w - 12, y0 + 13, w + 12, 23, 1, 0x3a444b);
        cv.text(F::kDisplay600_16, x + 184 - 6, y0 + 14, c.tag.c_str(), kDim, Align::Right, kLs06);
    } else if (c.lock) {
        cv.text(F::kDisplay700_16, x + 184, y0 + 14, c.tag.c_str(), kLock, Align::Right, kLs06, 0, a);
    } else if (c.costTag) {
        cv.text(F::kDisplay700_16, x + 184, y0 + 14, c.tag.c_str(), kAmber, Align::Right, kLs06, 0, a);
    } else {
        cv.text(F::kDisplay600_16, x + 184, y0 + 14, c.tag.c_str(), kDim, Align::Right, kLs06, 0, a);
    }
    const uint32_t vc = c.warn ? kWarn : c.lock ? kLock : c.pend ? kAmber : c.ro ? kRoVal : kInk;
    cv.text(F::kMono500_25, x + 17, y0 + 40, c.value.c_str(), vc, Align::Left, kLsVal, 167, a);
    if (!c.dest.empty()) {
        Text sp = c.value;
        sp.ch(' ');
        const int dx = Canvas::measure(F::kMono500_25, sp.c_str(), kLsVal) + 4;
        cv.text(F::kMono500_25, x + 17 + dx, y0 + 40, c.dest.c_str(), acc, Align::Left, kLsVal, 167 - dx > 0 ? 167 - dx : 1, a);
    }
    if (c.bar >= 0) {
        const float b = c.bar < 0 ? 0 : c.bar > 1 ? 1 : c.bar;
        cv.fill(x + 17, y0 + 71, 167, 4, kMeterBg, 255, 2);
        cv.fill(x + 17, y0 + 71, jr(b * 167), 4, col, a, 2);
    }
}

static void drawFrame(Canvas& cv, Ui& ui, uint32_t acc) {
    const bool locking = ui.lockOpen();
    Transport& tp = ui.transport();
    // top bar, bottom bar
    if (locking) cv.fill(0, 84, kWidth, 2, kLock);
    else cv.fill(0, 85, kWidth, 1, kLine);
    cv.fill(0, 454, kWidth, 1, kLine);
    Cell cells[8];
    ui.cells(cells);
    for (int i = 0; i < 8; ++i) drawCell(cv, cells[i], i, acc);
    // top left: the mode (and how to go back)
    Str<24> tag, back;
    if (locking) tag.add("LOCK");
    else if (ui.tracker()) tag.add("TRACKER");
    else if (ui.mode() == Mode::Sound) {   // the corner is 80 px wide: the two filters and the unison page get short names (a cut-off 'FILTER 2' read as 'FILTER')
        const int pgi = ui.page();
        tag.add(pgi == PG_FLT1 ? "FLT 1" : pgi == PG_FLT2 ? "FLT 2" : pgi == PG_SUB ? "SUB/UNI" : pageDef(pgi).title);
    }
    else {
        static const char* const kModes[] = {"PLAY", "STEP", "SOUND", "MIXER", "BROWSE", "PROJECT", "SONG", "FX", "SAVE", "REC"};
        tag.add(kModes[(int)ui.mode()]);
    }
    if (tag.n > 7) { tag.s[7] = 0; tag.n = 7; }
    if (locking) back.ch('L').num(ui.lockLane() + 1).add(" S").num(ui.lockIdx() + 1);
    else back.add(ui.mode() != Mode::Play ? "L BACK" : "HOME");
    corner(cv, 0, 0, tag.c_str(), locking ? kLock : acc, back.c_str());
    // top right: transport and tempo, and a red D-level badge while the CPU gives things up
    Str<24> tr, bpm;
    tr.add(tp.playing ? (tp.songPlaying ? "\xE2\x96\xB6 SONG" : "\xE2\x96\xB6 PLAY") : "\xE2\x96\xA0 STOP");
    const bool tempoKnob = ui.mode() == Mode::Play || (ui.mode() == Mode::ProjectM && ui.projSub() == 0);   // T1 / T3 show it: the corner says it where no knob does
    if (!tempoKnob) bpm.num(ui.project().bpm).add(" BPM");
    corner(cv, 880, 0, tr.c_str(), tp.playing ? kGood : locking ? kLock : kDim, bpm.c_str(), 0);
    const int lvl = ui.degradeLevel();
    if (lvl > 0) {
        Str<8> d;
        d.ch('D').num(lvl);
        const int w = Canvas::measure(F::kDisplay700_16, d.c_str());
        cv.fill(954 - w - 8, 6, w + 8, 23, kWarn, 255, 3);
        cv.text(F::kDisplay700_16, 954 - 4, 7, d.c_str(), 0x1a0000, Align::Right);
    }
    // bottom left: the pattern (always); bottom right: the part
    Str<8> p, next;
    p.ch('P').num(tp.pat + 1);
    if (tp.nextPat >= 0) next.add("\xE2\x96\xB8P").num(tp.nextPat + 1);
    else next.add("PAT");
    corner(cv, 0, 455, p.c_str(), locking ? kLock : acc, next.c_str());
    corner(cv, 880, 455, kPartName[ui.part()], kPartHex[ui.part()], "PART");
}

// ------------------------------------------------------------------------------------------------------------------------------------
// PLAY: the ten lanes

static void drawPlay(Canvas& cv, Ui& ui, uint32_t acc) {
    const Pattern& pat = ui.pattern();
    const Project& pj = ui.project();
    const Transport& tp = ui.transport();
    const SampleLib& lib = ui.samples();
    for (int li = 0; li < kLanes; ++li) {
        const Lane& ln = pat.lanes[li];
        const uint32_t lc = kPartHex[ln.part];
        const bool sel = li == ui.sel();
        const int top = jr(kMainTop + 36.6 * li), bot = jr(kMainTop + 36.6 * (li + 1)) - 1;
        const int h = bot - top;
        uint32_t rowBg = kScreenBg;
        if (sel) {
            rowBg = mix(lc, 0x070a0c, 41);
            const int left = ui.flashLeftMs(li);
            if (left > 0) rowBg = mix(mix(lc, 0x000000, 179), rowBg, left * 255 / 900);   // the row that just changed part flashes
            cv.fill(0, top, kWidth, h, rowBg);
            cv.fill(0, top, 5, h, lc);
            cv.fill(5, top, kWidth - 5, 1, lc, 179);
            cv.fill(5, bot - 1, kWidth - 5, 1, lc, 179);
        }
        cv.fill(0, bot, kWidth, 1, kLine);
        const int ty = top + 8;
        // lane number (with a small triangle when selected), part badge, M / A, root note or sample
        Str<4> no;
        no.num(li + 1);
        if (sel) {
            const int w = Canvas::measure(F::kMono600_16, no.c_str());
            cv.text(F::kMono600_16, 32, ty, no.c_str(), kWhite, Align::Right);
            cv.text(F::kDisplay600_16, 32 - w - 4, ty - 1, "\xE2\x96\xB8", lc, Align::Right);
        } else {
            cv.text(F::kMono400_16, 32, ty, no.c_str(), kDim, Align::Right, 0, 0, kOpacity70);
        }
        cv.fill(40, top + 6, 36, 23, lc, 255, 3);
        cv.text(F::kDisplay700_16, 58, top + 7, kPartName[ln.part], kDark, Align::Center);
        if (ln.mute) cv.text(F::kMono600_16, 95, ty, "M", kWarn, Align::Center);
        else if (ln.arp.mode) cv.text(F::kMono600_16, 95, ty, "A", acc, Align::Center);
        Text nt;
        if (isSampler(pj, ln.part)) nt.add(lib.count ? lib.items[clampi(ln.sample(), 0, lib.count - 1)].name : "?");
        else noteName(nt, ln.root);
        cv.text(F::kMono400_16, 106, ty, nt.c_str(), sel ? kWhite : kRoVal, Align::Left, 0, 120, sel ? 255 : kOpacity70);
        // the steps
        const int n = ln.n;
        const float cw = (714.0f - 3.0f * (n - 1)) / n;
        const int ph = tp.playing && tp.shownStep >= 0 ? tp.shownStep % n : -1;
        const int ga = sel ? 255 : kOpacity62;   // unselected lanes: the whole step row at .62
        const bool smp = isSampler(pj, ln.part);
        for (int i = 0; i < n; ++i) {
            const int x = jr(232 + i * (cw + 3.0f)), x2 = jr(232 + i * (cw + 3.0f) + cw), w = x2 - x;
            const int y = top + 8, sh = 20;
            const bool b4 = i % 4 == 0, eu = baseHit(ln, i, tp.fill), on = trig(ln, i, tp.fill), ov = ln.st[i].ov;
            const uint32_t idle = sel ? (b4 ? mix(lc, kStepB4, 71) : mix(lc, kStepOff, 51)) : (b4 ? kStepB4 : kStepOff);
            if (on && ov) {
                cv.fill(x, y, w, sh, ln.mute ? 0x3a444b : lc, ga, 2);   // a step tapped on over the rhythm: filled, with a white underline (the playhead is an outline)
                cv.fill(x, y + sh - 3, w, 3, kWhite, ga);
            } else if (on) {
                cv.fill(x, y, w, sh, ln.mute ? 0x3a444b : lc, ga, 2);
            } else if (ov && eu) {
                // a Euclid hit tapped off: crossed
                cv.fill(x, y, w, sh, 0x0b0f12, ga, 2);
                int16_t ys[128];
                const int m = w < 128 ? w : 128;
                for (int k = 0; k < m; ++k) ys[k] = (int16_t)(y + sh / 2 + (w / 2 - k));
                cv.pushClip(x, y, w, sh);
                cv.curve(x, ys, m, 5, mix(lc, 0x0b0f12, 140 * ga / 255));
                cv.popClip();
            } else {
                cv.fill(x, y, w, sh, idle, ga, 2);
            }
            if (ln.st[i].nLocks) cv.fill(x + w / 4, y + 3, w / 2, 3, kLock, ga, 2);
            if (ln.st[i].chord) cv.fill(x, y + sh - 4, w, 4, kWhite, 204 * ga / 255);
            if (smp && ln.st[i].hasLock(lockKey(PG_SMP, 0))) cv.fill(x + w / 2 - 3, y + sh / 2 - 3, 6, 6, kWhite, ga, 3);
            if (i == ph) cv.stroke(x, y - 4, w, sh + 8, 1, kWhite, false, ga, 3);
        }
    }
}

// ------------------------------------------------------------------------------------------------------------------------------------
// STEP: one lane, 8 steps per page, five rows

static void drawTabs(Canvas& cv, int right, int y, const char* const* names, int n, int on, uint32_t acc) {
    int w[8], total = 0;
    for (int i = 0; i < n; ++i) { w[i] = Canvas::measure(F::kDisplay600_16, names[i], kLs10) + 22; total += w[i] + (i ? 4 : 0); }
    int x = right - total;
    for (int i = 0; i < n; ++i) {
        if (i == on) cv.fill(x, y, w[i], 29, acc, 255, 4);
        else cv.stroke(x, y, w[i], 29, 1, kTabLine, false, 255, 4);
        cv.text(F::kDisplay600_16, x + 11, y + 4, names[i], i == on ? kDark : kDim, Align::Left, kLs10);
        x += w[i] + 4;
    }
}
static int tabsWidth(const char* const* names, int n) {
    int t = 0;
    for (int i = 0; i < n; ++i) t += Canvas::measure(F::kDisplay600_16, names[i], kLs10) + 22 + (i ? 4 : 0);
    return t;
}

// one cell's contents: a column of items (CSS: flex column, centred, gap 7, line-height 1)
struct Item {
    enum Kind : uint8_t { Big, Small, Note, Badges, Bar } kind;
    Text text;           // Big / Small / Note; Badges: up to three badges separated by '\n'
    float bar = 0;
    bool lockBadge = false;   // the first badge is a lock count
};
static int itemH(const Item& it) {
    switch (it.kind) {
        case Item::Big: return 22;
        case Item::Small: case Item::Note: return 16;
        case Item::Badges: return 21;
        case Item::Bar: return 4;
    }
    return 0;
}

static void drawStep(Canvas& cv, Ui& ui, uint32_t acc) {
    const Lane& ln = ui.lane();
    const Project& pj = ui.project();
    const Transport& tp = ui.transport();
    const SampleLib& lib = ui.samples();
    const uint32_t lc = kPartHex[ln.part];
    const bool smp = isSampler(pj, ln.part);
    const int pages = (ln.n + 7) / 8, s0 = ui.stepPage() * 8, ph = tp.playing && tp.shownStep >= 0 ? tp.shownStep % ln.n : -1;
    const Step& cur = ln.st[ui.cursor()];
    // header
    Str<16> lane;
    lane.add("LANE ").num(ui.sel() + 1);
    int x = 24 + cv.text(F::kDisplay700_20, 24, 101, lane.c_str(), acc, Align::Left, 13) + 14;
    Str<16> part;
    part.add("PART ").add(kPartName[ln.part]);
    x += cv.text(F::kDisplay600_16, x, 105, part.c_str(), kDim, Align::Left, kLs04) + 14;
    Text t;
    if (smp) { if (ui.stepSub() == 1) t.add("EDITING STEP ").num(ui.cursor() + 1); }
    else if (ln.arp.mode) t.add("ARP ").add(kArpModes[ln.arp.mode]).add(" x").num(kArpRates[ln.arp.rate]).add(" OCT").num(ln.arp.oct);
    else t.add("ARP OFF");
    x += cv.text(F::kDisplay600_16, x, 105, t.c_str(), kDim, Align::Left, kLs04) + 14;
    static const char* const kTabsSynth[] = {"NOTES", "CHORD + ARP", "TRACKER"};
    static const char* const kTabsSmp[] = {"NOTES", "SAMPLE PER STEP", "TRACKER"};
    const char* const* tabs = smp ? kTabsSmp : kTabsSynth;
    Str<16> pg;
    pg.add("PAGE ").num(ui.stepPage() + 1).ch('/').num(pages);
    const int pageX = 936 - tabsWidth(tabs, 3) - 14;
    if (!smp) {
        const int need = ln.arp.mode ? 1 : kChords[cur.chord].count, have = pj.parts[ln.part].voices;
        if (need > have) {
            Text w;
            w.add("CHORD ").num(need).add(" NOTES > ").num(have).add(" VOICES");
            x += cv.text(F::kDisplay700_16, x, 105, w.c_str(), kWarn, Align::Left, kLs04) + 14;
        }
        if (pj.scale > 0) {
            Text k;
            k.add("KEY ").add(kNoteNames[pj.key]).ch(' ').add(kScales[pj.scale].name);
            if (x + Canvas::measure(F::kDisplay600_16, k.c_str(), kLs04) + 14 <= pageX - Canvas::measure(F::kDisplay600_16, pg.c_str(), kLs04))   // (with a warning there may be no room left: PROJECT has it)
                cv.text(F::kDisplay600_16, x, 105, k.c_str(), kDim, Align::Left, kLs04);
        }
    }
    drawTabs(cv, 936, 101, tabs, 3, ui.stepSub(), acc);
    cv.text(F::kDisplay600_16, pageX, 105, pg.c_str(), kDim, Align::Right, kLs04);

    // the rows
    enum Row : uint8_t { RStep, RNote, RVel, RGate, RProb, RSample, RChord, RInv, RVoicing };
    Row rows[5];
    if (ui.stepSub() == 1 && smp) { Row r[5] = {RStep, RSample, RNote, RVel, RProb}; for (int i = 0; i < 5; ++i) rows[i] = r[i]; }
    else if (ui.stepSub() == 1) { Row r[5] = {RStep, RNote, RChord, RInv, RVoicing}; for (int i = 0; i < 5; ++i) rows[i] = r[i]; }
    else { Row r[5] = {RStep, RNote, RVel, RGate, RProb}; for (int i = 0; i < 5; ++i) rows[i] = r[i]; }
    static const char* const kRowNames[] = {"STEP", "NOTE", "VEL", "GATE", "PROB", "SAMPLE", "CHORD", "INV", "VOICING"};
    int ry = 138;
    for (int r = 0; r < 5; ++r) {
        bool tall = false;
        for (int c = 0; c < 8; ++c) {
            const int i = s0 + c;
            if (i < ln.n && trig(ln, i, tp.fill) && !(!ln.st[i].chord && (rows[r] == RInv || rows[r] == RVoicing))) tall = true;
        }
        const int rowH = tall ? 56 : 48;
        cv.text(F::kDisplay600_16, 24, ry + (rowH - 21) / 2, rows[r] == RNote && smp ? "PITCH" : kRowNames[rows[r]], kDim, Align::Left, kLs02);
        for (int c = 0; c < 8; ++c) {
            const int i = s0 + c, cx = 110 + c * 104;
            const bool exists = i < ln.n;
            const Step& st = ln.st[exists ? i : 0];
            const bool on0 = exists && trig(ln, i, tp.fill), ov0 = exists && st.ov;
            const bool na = exists && !st.chord && (rows[r] == RInv || rows[r] == RVoicing);
            const bool sm = rows[r] == RSample || rows[r] == RChord || rows[r] == RVoicing;
            const int smpOwn = exists ? st.lockValue(lockKey(PG_SMP, 0), -1) : -1;
            const bool own = rows[r] == RSample && smpOwn >= 0;
            const int h = on0 && !na ? 56 : 48, y = ry + (rowH - h) / 2;
            uint32_t bg = na ? 0x0b0f12 : on0 ? lc : kStepOff;
            uint32_t fg = na ? kOffText : !exists ? kOffText : on0 ? kDark : kCellText;
            if (own && !on0) fg = kWhite;
            cv.fill(cx, y, 98, h, bg, 255, 4);
            // one box-shadow paints (CSS: the last matching rule): own sample, else the playhead underline, else tapped-on
            if (own) cv.stroke(cx, y, 98, h, 2, on0 ? kWhite : lc, false, on0 ? 170 : 255, 4);
            else if (exists && i == ph) cv.fill(cx, y + h - 5, 98, 5, kWhite, 255, 4);
            else if (!na && ov0) cv.stroke(cx, y, 98, h, 3, kWhite, false, 119, 4);
            // the items
            Item it[3];
            int ni = 0;
            const Font* big = on0 && !na ? &F::kMono600_22 : &F::kMono400_22;
            const Font* small = on0 && !na ? &F::kMono600_16 : &F::kMono400_16;
            switch (rows[r]) {
                case RStep:
                    it[ni].kind = Item::Big;
                    if (exists) it[ni].text.num(i + 1); else it[ni].text.add("\xC2\xB7");
                    ++ni;
                    if (exists && (st.nLocks || st.chord || (smp && smpOwn >= 0))) {
                        it[ni].kind = Item::Badges;
                        if (st.nLocks) { it[ni].text.add("LK").num(st.nLocks); it[ni].lockBadge = true; }
                        if (st.chord) { if (!it[ni].text.empty()) it[ni].text.ch('\n'); it[ni].text.add(kChords[st.chord].name); }
                        if (smp && smpOwn >= 0 && smpOwn < lib.count) {
                            if (!it[ni].text.empty()) it[ni].text.ch('\n');
                            Str<24> w;
                            for (const char* p = lib.items[smpOwn].name; *p && *p != ' '; ++p) w.ch(*p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p);
                            it[ni].text.add(w.c_str());
                        }
                        ++ni;
                    }
                    break;
                case RNote:
                    if (!exists) break;
                    it[ni].kind = Item::Big; it[ni].text.sgn(st.note); ++ni;
                    if (!smp) { it[ni].kind = Item::Note; noteName(it[ni].text, scaleNote(ln.root + st.note, pj.key, pj.scale)); ++ni; }
                    break;
                case RVel:
                    if (!exists) break;
                    it[ni].kind = Item::Big; it[ni].text.num(st.vel); ++ni;
                    it[ni].kind = Item::Bar; it[ni].bar = st.vel / 127.0f; ++ni;
                    break;
                case RGate:
                    if (!exists) break;
                    it[ni].kind = Item::Big; it[ni].text.fixed(st.gate / 32.0, 2); ++ni;
                    it[ni].kind = Item::Bar; it[ni].bar = st.gate / 127.0f; ++ni;
                    break;
                case RProb:
                    if (!exists) break;
                    it[ni].kind = Item::Big; it[ni].text.num(st.prob); ++ni;
                    break;
                case RSample:
                    if (!exists) break;
                    it[ni].kind = Item::Small;
                    it[ni].text.add(own && smpOwn < lib.count ? lib.items[smpOwn].name : "lane");
                    ++ni;
                    break;
                case RChord:
                    if (!exists) break;
                    it[ni].kind = Item::Small; it[ni].text.add(st.chord ? kChords[st.chord].name : "-"); ++ni;
                    break;
                case RInv:
                    if (na) { it[ni].kind = Item::Big; it[ni].text.add("\xE2\x80\x93"); ++ni; break; }
                    if (!exists) break;
                    it[ni].kind = Item::Big; it[ni].text.num(st.inv); ++ni;
                    break;
                case RVoicing:
                    if (na) { it[ni].kind = Item::Big; it[ni].text.add("\xE2\x80\x93"); ++ni; break; }
                    if (!exists) break;
                    it[ni].kind = Item::Small;
                    it[ni].text.add(st.spread ? "OPEN" : "CLOSE");
                    if (st.strum && !ln.arp.mode) it[ni].text.ch(' ').num(st.strum).add("ms");
                    ++ni;
                    break;
            }
            (void)sm;
            // a small text that does not fit wraps at its space (the sample names): two lines
            int lines[3] = {1, 1, 1};
            for (int k = 0; k < ni; ++k)
                if (it[k].kind == Item::Small && Canvas::measure(*small, it[k].text.c_str()) > 98) lines[k] = 2;
            int total = 0;
            for (int k = 0; k < ni; ++k) total += itemH(it[k]) * lines[k] + (k ? 7 : 0);
            int iy = y + (h - total) / 2;
            for (int k = 0; k < ni; ++k) {
                const Item& m = it[k];
                const int ccx = cx + 49;
                switch (m.kind) {
                    case Item::Big: cv.text(*big, ccx, iy - (big->lineHeight() - 22) / 2, m.text.c_str(), fg, Align::Center); break;
                    case Item::Note:
                        cv.text(F::kMono600_16, ccx, iy - (F::kMono600_16.lineHeight() - 16) / 2, m.text.c_str(), fg, Align::Center, 0, 0, kOpacity75);
                        break;
                    case Item::Small: {
                        const int oy = iy - (small->lineHeight() - 16) / 2;
                        if (lines[k] == 2) {
                            Text a, b;
                            const char* sp = m.text.c_str();
                            while (*sp && *sp != ' ') a.ch(*sp++);
                            if (*sp) ++sp;
                            b.add(sp);
                            cv.text(*small, ccx, oy, a.c_str(), fg, Align::Center);
                            cv.text(*small, ccx, oy + 16, b.c_str(), fg, Align::Center);
                        } else {
                            cv.text(*small, ccx, oy, m.text.c_str(), fg, Align::Center);
                        }
                        break;
                    }
                    case Item::Badges: {
                        // badges side by side, gap 8; a lock count is lime (on a lit step: a lime chip)
                        const char* parts[3];
                        char buf[3][24];
                        int np = 0;
                        const char* p = m.text.c_str();
                        while (*p && np < 3) {
                            int l = 0;
                            while (*p && *p != '\n' && l < 23) buf[np][l++] = *p++;
                            buf[np][l] = 0;
                            parts[np] = buf[np];
                            ++np;
                            if (*p == '\n') ++p;
                        }
                        int bw[3], tw = 0;
                        for (int q = 0; q < np; ++q) { bw[q] = Canvas::measure(F::kDisplay700_16, parts[q]) + (q == 0 && m.lockBadge && on0 ? 6 : 0); tw += bw[q] + (q ? 8 : 0); }
                        int bx = ccx - tw / 2;
                        for (int q = 0; q < np; ++q) {
                            if (q == 0 && m.lockBadge) {
                                if (on0) {
                                    cv.fill(bx, iy, bw[q], 21, kLock, 255, 2);
                                    cv.text(F::kDisplay700_16, bx + 3, iy, parts[q], 0x1a2000);
                                } else {
                                    cv.text(F::kDisplay700_16, bx, iy, parts[q], kLock);
                                }
                            } else {
                                cv.text(F::kDisplay700_16, bx, iy, parts[q], fg, Align::Left, 0, 0, kOpacity85);
                            }
                            bx += bw[q] + 8;
                        }
                        break;
                    }
                    case Item::Bar: {
                        cv.fill(cx + 10, iy, 78, 4, 0x000000, 102, 2);
                        cv.fill(cx + 10, iy, jr(78 * (m.bar > 1 ? 1 : m.bar)), 4, fg, kOpacity75);
                        break;
                    }
                }
                iy += itemH(m) * lines[k] + 7;
            }
            // outlines on top: the cursor (white, solid) and an open lock (lime, dashed)
            if (exists && i == ui.cursor()) cv.stroke(cx - 4, y - 4, 106, h + 8, 3, kWhite, false, 255, 8);
            if (exists && ui.lockOpen() && ui.lockLane() == ui.sel() && ui.lockIdx() == i) cv.stroke(cx - 5, y - 5, 108, h + 10, 3, kLock, true, 255, 9);
        }
        ry += rowH + 6;
    }
    // the whole lane under the grid, the page on screen marked: only when the rows leave room for it (the CSS flex column shrinks
    // it to nothing otherwise)
    const int stripY = ry - 6 + 8 + 4, room = kMainTop + kMainH - 14 - stripY;
    if (room >= 14) {
        const float sw = (912.0f - 2.0f * (ln.n - 1)) / ln.n;
        for (int i = 0; i < ln.n; ++i) {
            const int x = jr(24 + i * (sw + 2)), w = jr(24 + i * (sw + 2) + sw) - x;
            const bool on = trig(ln, i, tp.fill), pg = i >= s0 && i < s0 + 8;
            cv.fill(x, stripY, w, 14, i == ph ? kWhite : on ? lc : 0x12181c, on && i != ph ? 204 : 255, 1);
            if (pg) cv.fill(x, stripY - 3, w, 3, acc);
        }
    }
}

// ------------------------------------------------------------------------------------------------------------------------------------
// TRACKER: STEP's third view. A row is a step, a column a lane, and a page 16 rows that stay put: only the playhead moves (docs/TRACKER.md)

struct PopRow {
    Canvas& cv;
    int x, y;
    void line(int k, const char* label, const char* value, uint32_t col, const char* tag) const {
        const int ry = y + 30 + k * 24;
        cv.text(F::kDisplay600_16, x + 12, ry, label, kDim, Align::Left, kLs04);
        cv.text(F::kMono400_17, x + 84, ry, value, col, Align::Left, 0, kPopW - 96 - (tag ? 28 : 0));
        if (tag) cv.text(F::kDisplay600_16, x + kPopW - 12, ry, tag, kDim, Align::Right, kLs04);
    }
};

// What the knobs and the sequencer have: nothing is worked out twice. The notes come from stepPitches().
static void drawPopover(Canvas& cv, Ui& ui, uint32_t acc, const Box& b) {
    const Lane& ln = ui.lane();
    const Project& pj = ui.project();
    Step st = ln.st[ui.cursor()];
    const bool on = trig(ln, ui.cursor(), false), smp = isSampler(pj, ln.part);
    st.note = (int8_t)ui.trackerNote();
    cv.fill(b.x, b.y, b.w, b.h, 0x0b0f12, 255, 6);
    cv.stroke(b.x, b.y, b.w, b.h, 2, acc, false, 255, 6);
    Text head;
    head.add("STEP ").num(ui.cursor() + 1).add(" \xC2\xB7 LANE ").num(ui.sel() + 1);
    cv.text(F::kDisplay700_16, b.x + 12, b.y + 6, head.c_str(), acc, Align::Left, kLs04);
    Text side;
    if (!on) side.add("EMPTY");
    else if (ui.trackerRow() >= ln.n) side.add("REPEAT");
    if (st.nLocks) { if (!side.empty()) side.ch(' '); side.add("LK").num(st.nLocks); }
    cv.text(F::kDisplay700_16, b.x + b.w - 12, b.y + 6, side.c_str(), st.nLocks ? kLock : kDim, Align::Right, kLs04);
    const PopRow pr{cv, b.x, b.y};
    Text foot;
    uint32_t footCol = kDim;
    if (ui.trackerStop()) { foot.add("STOPPED: THE LIMIT IS ").sgn(ui.trackerStop() * 24); footCol = kWarn; }
    Text v;
    if (smp) {
        Text name, folder;
        bool pending = false;
        ui.trackerSample(name, folder, pending);
        const int slice = st.lockValue(lockKey(PG_SLC, SLC_SLICE), 0);
        pr.line(0, "SAMPLE", name.c_str(), pending ? kAmber : kInk, "B3");
        pr.line(1, "FOLDER", folder.c_str(), kRoVal, nullptr);
        v.sgn(st.note).add(" st");
        pr.line(2, "PITCH", v.c_str(), kInk, "B1");
        v.clear();
        v.sgn(st.note / 12);
        pr.line(3, "OCTAVE", v.c_str(), kInk, "B2");
        v.clear();
        if (ln.smpParam(PG_SLC, SLC_MODE) == SLICE_OFF) v.add("-");
        else if (slice > 0) v.num(slice);
        else v.add("AUTO");
        pr.line(4, "SLICE", v.c_str(), slice > 0 ? kLock : kRoVal, "B4");
        if (foot.empty()) { if (st.chord) foot.add("PLAYS A CHORD: ").add(kChords[st.chord].name); else foot.add("SHIFT + B3: ANOTHER FOLDER"); }
    } else {
        int notes[kMaxChordNotes];
        const int n = stepPitches(pj, ln, st, notes);
        const int played = clampi(scaleNote(ln.root + st.note, pj.key, pj.scale), 0, 127);
        trackerNoteName(v, played);
        v.add("  ").sgn(st.note);
        pr.line(0, "NOTE", v.c_str(), kInk, "B1");
        v.clear();
        v.num(floorDiv(played, 12) - 1);
        pr.line(1, "OCTAVE", v.c_str(), kInk, "B2");
        pr.line(2, "CHORD", kChords[st.chord].name, kInk, "B3");
        v.clear();
        if (st.chord) v.num(st.inv); else v.add("-");
        pr.line(3, "INVERT", v.c_str(), st.chord ? kInk : kRoVal, "B4");
        v.clear();
        for (int i = 0; i < n; ++i) { if (i) v.ch(' '); noteName(v, notes[i]); }
        pr.line(4, "NOTES", v.c_str(), acc, nullptr);
        const int need = ln.arp.mode ? 1 : n, have = pj.parts[ln.part].voices;
        if (foot.empty() && need > have) { foot.add("CHORD ").num(need).add(" NOTES > ").num(have).add(" VOICES"); footCol = kWarn; }
        if (foot.empty() && ln.arp.mode) foot.add("ARP ").add(kArpModes[ln.arp.mode]).add(" x").num(kArpRates[ln.arp.rate]).add(" OCT").num(ln.arp.oct);
        if (foot.empty() && ui.inScale()) foot.add("STAYS IN ").add(kNoteNames[pj.key]).ch(' ').add(kScales[pj.scale].name);
    }
    cv.text(F::kDisplay600_16, b.x + 12, b.y + b.h - 28, foot.c_str(), footCol, Align::Left, kLs02, b.w - 24);
}

static void drawTracker(Canvas& cv, Ui& ui, uint32_t acc) {
    const Pattern& pat = ui.pattern();
    const Project& pj = ui.project();
    const Transport& tp = ui.transport();
    const SampleLib& lib = ui.samples();
    const int rows = ui.trackerRows(), r0 = ui.trackerRow() / kTrackerRows * kTrackerRows, shown = rows - r0 < kTrackerRows ? rows - r0 : kTrackerRows;
    const int ph = tp.playing && tp.shownStep >= 0 && rows > 0 ? tp.shownStep % rows : -1;
    const int ty = (kTrackRowH - F::kMono400_16.lineHeight()) / 2;
    // the lanes' heads: as on PLAY, number, part, root or sample
    for (int li = 0; li < kLanes; ++li) {
        const Lane& ln = pat.lanes[li];
        const uint32_t lc = kPartHex[ln.part];
        const bool sel = li == ui.sel();
        const int x = kTrackX + li * kTrackColW;
        Str<4> no;
        no.num(li + 1);
        cv.text(sel ? F::kMono600_16 : F::kMono400_16, x + 4, 92, no.c_str(), sel ? kWhite : kDim);
        const int bx = x + (li == 9 ? 26 : 16);
        cv.fill(bx, 93, 20, 20, lc, ln.mute ? 90 : 255, 3);
        cv.text(F::kDisplay700_16, bx + 10, 93, kPartName[ln.part], kDark, Align::Center);
        Text nt;
        if (ln.mute) nt.ch('M');
        else if (isSampler(pj, ln.part)) nt.add(lib.count ? lib.items[clampi(ln.sample(), 0, lib.count - 1)].name : "?");
        else noteName(nt, ln.root);
        cv.text(F::kMono400_16, bx + 24, 92, nt.c_str(), ln.mute ? kWarn : sel ? kWhite : kRoVal, Align::Left, 0, x + kTrackColW - bx - 26);
        if (sel) cv.fill(x + 2, 116, kTrackColW - 4, 2, acc);
    }
    if (rows > kTrackerRows) {
        Str<8> pg;
        pg.num(r0 / kTrackerRows + 1).ch('/').num((rows + kTrackerRows - 1) / kTrackerRows);
        cv.text(F::kMono400_16, kTrackX - 6, 92, pg.c_str(), kDim, Align::Right);
    }
    // the bar's beats, and the playhead: a row on the page, the only thing that moves while it plays
    for (int r = 0; r < shown; r += 4) cv.fill(0, kTrackY + r * kTrackRowH, kWidth, kTrackRowH, kStepB4);
    if (ph >= r0 && ph < r0 + kTrackerRows) cv.fill(0, kTrackY + (ph - r0) * kTrackRowH, kWidth, kTrackRowH, 0x2c353b);
    for (int r = 0; r < shown; ++r) {
        const int row = r0 + r;
        const int y = kTrackY + r * kTrackRowH;
        Str<4> no;
        no.num(row + 1);
        cv.text(F::kMono400_16, kTrackX - 6, y + ty, no.c_str(), row == ph ? kWhite : r % 4 == 0 ? kRoVal : kDim, Align::Right);
        for (int li = 0; li < kLanes; ++li) {
            const Lane& ln = pat.lanes[li];
            if (!ln.n) continue;
            const int i = row % ln.n, x = kTrackX + li * kTrackColW;
            const Step& st = ln.st[i];
            const bool rep = row >= ln.n, smp = isSampler(pj, ln.part), on = trig(ln, i, tp.fill);
            const int a = rep ? 100 : ln.mute ? kOpacity60 : 255;
            if (!on) {
                cv.text(F::kMono400_16, x + 6, y + ty, "---  --", kOffText, Align::Left, 0, 0, rep ? 150 : 255);
                continue;
            }
            Str<8> note;
            if (smp) { note.ch(st.note < 0 ? '-' : '+'); note.pad(st.note < 0 ? -st.note : st.note, 2); }
            else trackerNoteName(note, clampi(scaleNote(ln.root + st.note, pj.key, pj.scale), 0, 127));
            cv.text(F::kMono400_16, x + 6, y + ty, note.c_str(), mix(kPartHex[ln.part], kWhite, 200), Align::Left, 0, 0, a);
            Str<4> vel;
            vel.num(st.vel);
            cv.text(F::kMono400_16, x + 74, y + ty, vel.c_str(), kCellText, Align::Right, 0, 0, a);
            // the marks: locks in the lock's colour, as PLAY has them; under them a chord, or a sample or slice of the step's own
            if (st.nLocks) cv.fill(x + 78, y + 4, 6, 5, kLock, a, 1);
            const bool own = smp ? st.hasLock(lockKey(PG_SMP, 0)) || st.hasLock(lockKey(PG_SLC, SLC_SLICE)) : st.chord != 0;
            if (own) cv.fill(x + 78, y + 11, 6, 5, kWhite, a, 1);
        }
    }
    // outlines on top, as in STEP: the cursor (white, solid) and an open lock (lime, dashed)
    const Box cur = trackerCell(ui.sel(), ui.trackerRow() - r0);
    cv.stroke(cur.x, cur.y, cur.w, cur.h, 2, kWhite, false, 255, 3);
    if (ui.lockOpen()) {
        const Lane& ln = pat.lanes[ui.lockLane()];
        for (int r = 0; r < shown; ++r)
            if (ln.n && (r0 + r) % ln.n == ui.lockIdx()) { const Box b = trackerCell(ui.lockLane(), r); cv.stroke(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 2, kLock, true, 255, 3); }
    }
    // two pages: which one this is, and where the playhead is when it is on the other
    if (rows > kTrackerRows)
        for (int i = 0; i < rows; ++i) {
            const int x = kTrackX + i * 900 / rows, w = kTrackX + (i + 1) * 900 / rows - x - 1;
            cv.fill(x, 444, w, 5, i == ph ? kWhite : i >= r0 && i < r0 + kTrackerRows ? mix(acc, 0x000000, 150) : kStepB4);
        }
    if (ui.popover()) drawPopover(cv, ui, acc, popoverBox(cur));
}

// ------------------------------------------------------------------------------------------------------------------------------------
// SOUND: the page's picture (the mockup's drawViz)

static int textTopFromBaseline(const Font& f, int baseline) { return baseline - f.ascent; }

static void caption(Canvas& cv, const char* s) {   // top right: the top left is the PART label
    cv.text(F::kDisplay600_16, kWidth - 24, kMainTop + textTopFromBaseline(F::kDisplay600_16, 28), s, kDim, Align::Right);
}
static void footer(Canvas& cv, const char* s) {
    cv.text(F::kDisplay600_16, 24, kMainTop + textTopFromBaseline(F::kDisplay600_16, kMainH - 12), s, kDim);
}

static double waveAt(int w, double ph, int amt, int shp) {
    double d = 0.5 + (amt / 127.0) * (shp / 127.0 - 0.5);
    d = d < 0.0001 ? 0.0001 : d > 0.9999 ? 0.9999 : d;
    const double wp = w == 0 ? (ph < d ? 0.5 * ph / d : 0.5 + 0.5 * (ph - d) / (1 - d)) : ph;
    if (w == 0) return sin(2 * M_PI * wp);
    if (w == 1) return 1 - 4 * fabs(ph - 0.5);
    if (w == 2) return 2 * ph - 1;
    return ph < 0.5 ? 1 : -1;
}

// The sample with its START and END, or (`sl`) with the slices it is cut into, the selected one lit when they are being edited.
static void drawSample(Canvas& cv, Ui& ui, uint32_t acc, const int16_t* pg, const SliceMap* sl = nullptr, bool showLoop = false) {
    const SampleLib& lib = ui.samples();
    if (!lib.count) return;
    const SampleItem& smp = lib.items[clampi(pg[0], 0, lib.count - 1)];
    if (!smp.data || !smp.length) { footer(cv, (Text().add(smp.name).add("   (not loaded)")).c_str()); return; }
    const int W = kWidth, H = kMainH, x0 = 30, w = W - 60, mid = H / 2 + 8;
    const double amp = 125;
    double s0 = pg[1] / 127.0, s1 = (pg[2] / 127.0 > s0 + 0.004 ? pg[2] / 127.0 : s0 + 0.004);
    const int selSlice = sl ? clampi(ui.sliceSel(), 0, sl->count - 1) : 0;
    if (sl) {   // the whole sample, or the one slice
        s0 = 0.0;
        s1 = 1.0;
        if (ui.sliceEdit() && sl->frames) {
            s0 = (double)sl->s[selSlice].start / sl->frames;
            s1 = selSlice + 1 < sl->count ? (double)sl->s[selSlice + 1].start / sl->frames : 1.0;
        }
    }
    uint32_t loopS = 0, loopE = 0;
    double loopX = 0, loopLap = 0;
    const bool haveLoop = showLoop && pg[3] == 2 && ui.shownLoop(loopS, loopE, loopX, loopLap, ui.shownValue(PG_LOOP, LP_XFADE), pg[0]) && loopE > loopS && smp.length;
    if (haveLoop) { s0 = (double)loopS / smp.length; s1 = (double)loopE / smp.length; }
    const bool loop = !sl && pg[3] == 2, rev = !sl && pg[6] == 1;
    const int T = kMainTop;
    if (loop) cv.fill(jr(x0 + s0 * w), T + 20, jr((s1 - s0) * w), H - 40, acc, 0x22);
    const uint32_t n = smp.length;
    static int16_t lo16[kWidth], hi16[kWidth];
    static uint8_t pick[kWidth];
    for (int x = 0; x < w; ++x) {
        const uint32_t a = (uint32_t)((double)x / w * n), b0 = (uint32_t)((double)(x + 1) / w * n), b = b0 > a + 1 ? b0 : a + 1;
        const uint32_t stride = (b - a) / 12 > 1 ? (b - a) / 12 : 1;
        double lo = 0, hi = 0;
        for (uint32_t i = a; i < b && i < n; i += stride) {
            const double v = smp.data[i] / 32767.0;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        const double f = (double)x / w;
        const int hh = (int)((hi - lo) * amp);
        lo16[x] = (int16_t)(T + jr(mid - hi * amp));
        hi16[x] = (int16_t)(lo16[x] + (hh > 1 ? hh : 1));
        pick[x] = f >= s0 && f <= s1 ? 0 : 1;   // dim outside the playing region
    }
    cv.columns(x0, lo16, hi16, pick, w, acc, 0x2b343a);
    if (sl && sl->frames) {
        for (int k = 0; k < sl->count; ++k) {
            const int x = jr(x0 + (double)sl->s[k].start / sl->frames * w);
            const bool on = ui.sliceEdit() && k == selSlice;
            cv.fill(x, T + 22, on ? 2 : 1, H - 44, sl->s[k].flags & SLICE_MUTED ? 0x4a5359 : 0xffb13d, on ? 255 : 170);
            if (on || sl->count <= 16) {
                Str<8> n;
                n.num(k + 1);
                cv.text(F::kDisplay600_16, x + 3, T + 24, n.c_str(), on ? kWhite : kDim);
            }
        }
        Text t;
        t.add(smp.name).add("   ").num(sl->count).add(" SLICES");
        const int beats = kSliceBeats[clampi(ui.shownValue(PG_SLC, SLC_BEATS), 0, kSliceBeatChoices - 1)];
        const float secs = lib.seconds(clampi(pg[0], 0, lib.count - 1));
        if (secs > 0) t.add("   ").num(beats).add(beats == 1 ? " BEAT = " : " BEATS = ").fixed(beats * 60.0 / secs, 1).add(" BPM");
        footer(cv, t.c_str());
        return;
    }
    if (haveLoop && loopX > 0) {   // where the end melts into the start
        const int fw = jr(loopX / smp.length * w);
        cv.fill(jr(x0 + s1 * w) - fw, T + 20, fw, H - 40, 0x6fd3ff, 0x38);
        cv.fill(jr(x0 + s0 * w), T + 20, fw, H - 40, 0x6fd3ff, 0x38);
    }
    const double marks[2] = {s0, s1};
    const char* names[2] = {"START", "END"};
    for (int k = 0; k < 2; ++k) {
        const int x = jr(x0 + marks[k] * w);
        cv.fill(x - 1, T + 22, 2, H - 44, 0xff7e5f);
        cv.text(F::kDisplay600_16, marks[k] > 0.9 ? x - 44 : x + 6, T + textTopFromBaseline(F::kDisplay600_16, 62), names[k], kCellText);
    }
    Text t;
    static const char* const kModes[] = {"ONE-SHOT", "GATED", "LOOP"};
    t.add(smp.name).add("   ").fixed(lib.seconds(clampi(pg[0], 0, lib.count - 1)), 2).add(" s   ").add(kModes[clampi(pg[3], 0, 2)]);
    if (rev) t.add("   REVERSE (plays END to START)");
    if (haveLoop) t.add("   LOOP ").fixed(loopLap / (smp.rate ? smp.rate : 44100), 3).add(" s");
    if (showLoop && pg[3] != 2) t.add("   SET PLAY TO LOOP");
    footer(cv, t.c_str());
}

static void drawSound(Canvas& cv, Ui& ui, uint32_t acc) {
    const Project& pj = ui.project();
    const int W = kWidth, H = kMainH, T = kMainTop;
    const int page = ui.page();
    int16_t pg[8];
    for (int i = 0; i < 8; ++i) pg[i] = (int16_t)ui.shownValue(page, i);   // an open step lock shows in the picture too
    // grid
    for (int i = 1; i < 6; ++i) cv.fill(0, T + H * i / 6, W, 1, 0x171e23);
    const int mid = H / 2 + 10;
    const double amp = 120;
    int16_t ys[kWidth + 1];
    auto path = [&](auto fn, int thickness = 3) {
        for (int x = 0; x <= W; ++x) ys[x] = (int16_t)(T + jr(fn((double)x / W)));
        cv.curve(0, ys, kWidth + 1, thickness, acc);
    };
    Text t;
    if (page == PG_SMP) {
        drawSample(cv, ui, acc, pg);
    } else if (page == PG_LOOP) {
        int16_t sp[8];
        for (int i = 0; i < 8; ++i) sp[i] = (int16_t)ui.shownValue(PG_SMP, i);
        drawSample(cv, ui, acc, sp, nullptr, true);
    } else if (page == PG_SLC) {
        int16_t sp[8];
        for (int i = 0; i < 8; ++i) sp[i] = (int16_t)ui.shownValue(PG_SMP, i);
        const SliceMap* sl = ui.shownSlices();
        if (sl && sl->sample != sp[0]) sl = nullptr;   // a step lock shows another sample, not this one's cuts
        if (sl) drawSample(cv, ui, acc, sp, sl);
        else if (pg[SLC_MODE] == SLICE_OFF) footer(cv, "SLICING IS OFF: TURN MODE TO STEP OR SYNC");
        else footer(cv, "CUTTING THE SAMPLE");
    } else if (page == PG_OSC1 || page == PG_OSC2) {
        const int wv = pg[0], am = pg[2], shp = pg[1];
        const bool bank = waveIsBank(wv);
        const int shape = bank ? waveBankNearest(waveBankOf(wv)) : wv;   // a bank is drawn as the basic wave it is nearest to
        path([&](double tt) { return mid - amp * waveAt(shape, fmod(tt * 3, 1.0), bank ? 0 : am, bank ? 0 : shp); });
        formatWave(t, wv);
        if (bank) t.add("  ").add(waveCategoryName(waveBankCategory(waveBankOf(wv)))).add("  POSITION ").num(jr(shp / 1.27)).ch('%');
        else if (wv == 0) t.add("  PHASE-DISTORTED  ").num(jr(am / 127.0 * 100)).ch('%');
        footer(cv, t.c_str());
    } else if (page == PG_FLT1 || page == PG_FLT2 || page == PG_SFLT) {
        const double q = 0.5 + pg[2] / 127.0 * 16, fc = cutHz(pg[1]);
        const int ty = pg[0];
        auto mag = [&](double hz) {
            const double w0 = 2 * M_PI * fc / 48000, w = 2 * M_PI * hz / 48000, alpha = sin(w0) / (2 * q), cw = cos(w0);
            double b0, b1, b2;
            const double a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
            if (ty == 2) { b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; }
            else if (ty == 1) { b0 = alpha; b1 = 0; b2 = -alpha; }
            else if (ty == 3 && page != PG_SFLT) { b0 = 1; b1 = -2 * cw; b2 = 1; }
            else { b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; }
            const double cw1 = cos(w), sw1 = sin(w), cw2 = cos(2 * w), sw2 = sin(2 * w);
            const double nr = b0 + b1 * cw1 + b2 * cw2, ni = -(b1 * sw1 + b2 * sw2), dr = a0 + a1 * cw1 + a2 * cw2, di = -(a1 * sw1 + a2 * sw2);
            return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
        };
        for (int x = 0; x < kWidth; ++x) {
            const double hz = 20 * pow(1000, (double)x / W), m = mag(hz), db = 20 * log10(m > 1e-4 ? m : 1e-4);
            double y = H - 70 - (db + 40) * 4;
            y = y < 24 ? 24 : y > H - 20 ? H - 20 : y;
            ys[x] = (int16_t)(T + jr(y));
        }
        const bool f2off = page == PG_FLT2 && pg[5] == 0;   // filter 2 is off: its curve is drawn, but as what it would do
        cv.curve(0, ys, kWidth, 3, f2off ? kDim : acc, T + H, f2off ? kDim : acc, 0x26);
        static const int kHz[3] = {100, 1000, 10000};
        static const char* const kHzNames[3] = {"100", "1k", "10k"};
        for (int k = 0; k < 3; ++k)
            cv.text(F::kDisplay600_16, jr(log(kHz[k] / 20.0) / log(1000.0) * W) + 4, T + textTopFromBaseline(F::kDisplay600_16, H - 6), kHzNames[k], kDim);
        t.add(page == PG_SFLT ? (ty == 1 ? "BP" : ty == 2 ? "HP" : "LP") : kModels[ty]).add("  ").num(jr(fc)).add(" Hz");
        if (f2off) t.add("  OFF");
        caption(cv, t.c_str());
    } else if (page == PG_AMP || page == PG_SAMP) {
        const double A = envMs(pg[0]), D = envMs(pg[1]), Su = pg[2] / 127.0, R = envMs(pg[3]), tot = A + D + 400 + R, sc = (W - 60) / tot;
        // a segment is drawn at least 10 px wide (a 2 ms attack on a scale of a second is a vertical jag, not a shape); the sustain plateau gives the room back
        auto seg = [&](double ms) { const double w = ms * sc; return w < 10 ? 10.0 : w; };
        const double wA = seg(A), wD = seg(D), wR = seg(R), wS = (W - 60) - wA - wD - wR > 60 ? (W - 60) - wA - wD - wR : 60;
        const double px[5] = {30, 30 + wA, 30 + wA + wD, 30 + wA + wD + wS, 30 + wA + wD + wS + wR};
        const double py[5] = {(double)H - 40, 50, 50 + (1 - Su) * (H - 90), 50 + (1 - Su) * (H - 90), (double)H - 40};
        const int xa = 30, xb = jr(px[4]);
        int n = 0;
        for (int x = xa; x <= xb && n < kWidth; ++x, ++n) {
            int s = 0;
            while (s < 3 && x > px[s + 1]) ++s;
            const double f = px[s + 1] > px[s] ? (x - px[s]) / (px[s + 1] - px[s]) : 1.0;
            ys[n] = (int16_t)(T + jr(py[s] + (py[s + 1] - py[s]) * (f > 1 ? 1 : f)));
        }
        for (int k = 1; k < 4; ++k) {   // a corner closer than a column to the next one would be skipped: put it on its column
            const int col = jr(px[k]) - xa;
            if (col > 0 && col < n) ys[col] = (int16_t)(T + jr(py[k]));
        }
        cv.curve(xa, ys, n, 3, acc);
        t.add("A ").num(jr(A)).add(" ms   D ").num(jr(D)).add(" ms   S ").num(jr(Su * 100)).add("%   R ").num(jr(R)).add(" ms");
        caption(cv, t.c_str());
    } else if (page == PG_GRIT) {
        const int bits = pg[0];
        const double lv = pow(2, bits - 1);
        path([&](double tt) { const double s = sin(2 * M_PI * tt * 2); return mid - amp * (floor(s * lv + 0.5) / lv); });
        t.num(bits).add(" BIT  ").add(pg[1] > 0 ? "COMPANDED" : "LINEAR");
        caption(cv, t.c_str());
    } else if (page == PG_MOD) {
        const int sh = pg[3];
        const double d = pg[1] / 127.0;
        path([&](double tt) { return mid - amp * d * waveAt(sh, fmod(tt * (1 + pg[0] / 40.0), 1.0), 0, 0); });
        t.add("LFO ").add(kWaves[sh]).add(" > ").add(pageDef(PG_MOD).p[2].list[pg[2]]).add("   depth ").num(jr(d * 100)).ch('%');
        caption(cv, t.c_str());
    } else if (page == PG_SUB) {
        const int vals[5] = {pg[0], pg[2], pg[3], pg[6], pg[7]};
        static const char* const kNames[5] = {"SUB", "NOISE", "RING", "BLEND", "WIDTH"};
        for (int i = 0; i < 5; ++i) {
            const int x = 90 + i * 170, h = jr(vals[i] / 127.0 * 230);
            cv.fill(x, T + H - 50 - h, 80, h, acc, 0x55);
            cv.text(F::kDisplay600_16, x + 20, T + textTopFromBaseline(F::kDisplay600_16, H - 28), kNames[i], kDim);
        }
        t.add("UNISON ").num(pg[4]).add(pg[4] > 1 ? " VOICES" : " VOICE");
        caption(cv, t.c_str());
    } else {
        static const char* const kLines[] = {"Master level, pitch-bend range, MIDI channel, fine tune,", "clock source and preview volume live here.", "",
                                             "Tempo, swing and pattern are on the PLAY view."};
        for (int i = 0; i < 4; ++i) cv.text(F::kDisplay600_18, 40, T + textTopFromBaseline(F::kDisplay600_18, 80 + i * 34), kLines[i], kDim);
    }
    // the label over the picture: PART A · preset (or SAMPLER · LANE n)
    int x = 24 + cv.text(F::kDisplay600_16, 24, 95, "PART ", kDim, Align::Left, kLs06);
    x += cv.text(F::kDisplay700_16, x, 95, kPartName[ui.part()], acc, Align::Left, kLs06);
    Text v;
    v.add(" \xC2\xB7 ");
    if (isSampler(pj, ui.part())) { v.add("SAMPLER"); if (ui.partHasLane(ui.part())) v.add(" \xC2\xB7 LANE ").num(ui.curLaneIndex() + 1); }
    else if (pj.parts[ui.part()].preset >= 0) v.add(presetName(pj.parts[ui.part()].preset));
    upper(v);
    cv.text(F::kDisplay600_16, x, 95, v.c_str(), kDim, Align::Left, kLs06);
}

// ------------------------------------------------------------------------------------------------------------------------------------
// MIXER

static void drawMixer(Canvas& cv, Ui& ui, uint32_t acc) {
    Project& pj = ui.project();
    static const char* const kTabs[] = {"LEVELS", "VOICES", "PRIORITY", "CPU"};
    const Ladder& lad = ui.ladder();
    const int lvl = ui.degradeLevel();
    static const char* const kPrioLow[] = {"low", "mid", "high", "keep"};
    if (ui.mixSub() < 3) {
        constexpr int kColW = kWidth / kParts, kInW = kColW - 20;
        for (int i = 0; i < kParts; ++i) {
            const Part& p = pj.parts[i];
            const bool idle = isIdlePart(pj, i);
            const uint32_t pc = kPartHex[i];
            const int x0 = i * kColW, x = x0 + 11;
            if (i) cv.fill(x0, kMainTop, 1, kMainH, kLine);
            if (i == ui.part()) cv.fill(x0 + 1, kMainTop, kColW - 1, 4, pc);
            const int hw = cv.text(F::kDisplay700_22, x, 135, kPartName[i], pc, Align::Left, 35);
            cv.text(F::kDisplay700_16, x + hw + 6, 140, p.type == PT_SAMPLER ? "SAMPLER" : "SYNTH", kDim, Align::Left, kLs04);
            cv.text(F::kMono400_16, x, 173, idle ? "no lanes" : p.type == PT_SAMPLER ? "samples" : (p.preset >= 0 ? presetName(p.preset) : ""), kDim, Align::Left, 0, kInW);
            cv.fill(x, 208, 22, 145, kStepOff, 255, 3);
            const int fh = jr(p.level / 127.0 * 145);
            cv.fill(x, 353 - fh, 22, fh, pc, 255, 3);
            {   // the level as a number beside the fader, the priority under it
                Str<8> n;
                n.num(p.level);
                cv.text(F::kMono600_16, x + 32, 208, n.c_str(), kInk);
                cv.text(F::kMono400_16, x + 32, 232, kPrioLow[p.priority], ui.mixSub() == 2 ? kInk : kDim);
            }
            const int act = ui.services().activeVoices(i);
            const int slots = p.type == PT_SAMPLER ? 8 : 5, vw = kInW / slots;
            for (int v = 0; v < slots; ++v) {
                const int vx = x + v * vw;
                if (v < p.voices) cv.fill(vx, 365, vw - 3, 16, v < act ? kWhite : pc, 255, 3);
                else cv.fill(vx, 365, vw - 3, 16, kMeterBg, 255, 3);
            }
            // "2 x 1.00 = 2.00 · lanes 1 2 3", and the degraded figures in red when the CPU gives things up
            const float unit = cost::voiceUnit(partDesc(pj, i), kFwCost), pc2 = partCost(pj, i);
            Text a, b;
            a.num(p.voices).add("x").fixed(unit, 2);
            if (pj.fx.amp[i][0] > 0) a.ch('+').fixed(cost::ampCost((uint8_t)pj.fx.amp[i][0]), 2);
            a.add(" =").fixed(pc2, 2);
            if (lvl > 0) {
                const cost::PartDesc e = degradedPart(pj, i, lad, lvl);
                const float ec = cost::partCost(e, kFwCost);
                if (fabsf(ec - pc2) > 0.004f) b.add(" \xE2\x86\x92").fixed(ec, 2);
            }
            Text c;
            for (int l = 0; l < kLanes; ++l) if (ui.pattern().lanes[l].part == i) { if (c.empty()) c.add(" lanes"); c.ch(' ').num(l + 1); }
            Run runs[3] = {{&F::kMono400_16, a.c_str(), kDim, 0}, {&F::kMono600_16, b.c_str(), kWarn, 0}, {&F::kMono400_16, c.c_str(), kDim, 0}};
            drawRuns(cv, x, 392, kInW, 19, runs, 3);
        }
        drawTabs(cv, 944, 97, kTabs, 4, ui.mixSub(), acc);   // over the column lines (z-index 2 in the mockup)
        return;
    }
    drawTabs(cv, 944, 97, kTabs, 4, ui.mixSub(), acc);
    // the last view: what the CPU budget is spent on, and what goes first
    const float total = voiceTotal(pj);
    int y = 139;
    auto label = [&](const char* s) { cv.text(F::kDisplay600_16, 32, y + 1, s, kDim, Align::Left, kLs04); };
    Text t;
    // "5 of 5", "4.5 of 39" (the mockup trims trailing zeros)
    Text tot;
    tot.fixed(total, 2);
    while (tot.n > 0 && tot.s[tot.n - 1] == '0') tot.s[--tot.n] = 0;
    if (tot.n > 0 && tot.s[tot.n - 1] == '.') tot.s[--tot.n] = 0;
    t.add(tot.c_str()).add(" of ").num(kVoiceBudget);
    if (total > kVoiceBudget) t.add(" \xC2\xB7 OVER BUDGET");
    if (lvl > 0) t.add(" \xC2\xB7 running at ").fixed(effTotal(pj, lad, lvl), 2).add(" (degrade ").num(lvl).ch(')');
    label("WILL IT FIT");   // the answer first (a sentence and a bar of the voice budget), the numbers behind it
    {
        const bool over = total > kVoiceBudget;
        Text a;
        if (lvl > 0) a.add("THE CPU IS GIVING UP ").num(lvl).add(lvl == 1 ? " THING" : " THINGS").add(" RIGHT NOW");
        else if (over) a.add("OVER BUDGET: THE CPU WILL RELEASE NOTES WHEN IT RUNS OUT");
        else a.add("YES: NO NOTES WILL BE DROPPED");
        const int aw = cv.text(F::kDisplay700_16, 224, y, a.c_str(), lvl > 0 || over ? kWarn : kGood, Align::Left, kLs04);
        const float fill = total / (float)kVoiceBudget;
        cv.fill(224 + aw + 24, y + 6, 160, 8, kMeterBg, 255, 4);
        cv.fill(224 + aw + 24, y + 6, jr((fill > 1 ? 1 : fill) * 160), 8, over ? kWarn : kGood, 255, 4);
    }
    y += 30;
    label("VOICE COST");
    cv.text(F::kMono400_17, 224, y, t.c_str(), kInk);
    y += 30;
    label("EFFECTS");   // what the effects add to that: the shared ones (sends in use) and each part's amp
    {
        const cost::PartDesc fd = fxDesc(pj);
        Text fe;
        if (!fd.fx) fe.add("shared: none");
        else {
            fe.add("shared:");
            if (fd.fx & cost::kFxDelay) fe.add(" delay");
            if (fd.fx & cost::kFxReverb) fe.add(" reverb");
            if (fd.fx & cost::kFxChorus) fe.add(" chorus");
            if (fd.fx & cost::kFxComp) fe.add(" comp");
            fe.add(" +").fixed(fxCostOf(pj), 2);
        }
        for (int i = 0; i < kParts; ++i)
            if (pj.fx.amp[i][0] > 0) fe.add(" \xC2\xB7 amp ").add(kPartName[i]).add(" +").fixed(cost::ampCost((uint8_t)pj.fx.amp[i][0]), 2);
        cv.text(F::kMono400_17, 224, y, fe.c_str(), kInk, Align::Left, 0, 704);
    }
    y += 30;
#if !DBOY_COST_LEAN
    label("HEAVY FILTERS");
    Text hv;
    for (int i = 0; i < kParts; ++i)
        if (!isSampler(pj, i)) hv.add(kPartName[i]).ch(' ').num(cost::heavyCount(descOf(pj, i))).ch(' ');
    const int hx = cv.text(F::kMono400_17, 224, y, hv.c_str(), kInk);
    cv.text(F::kDisplay600_16, 224 + hx, y + 1, "(RULE: AT MOST 1 PER LANE)", kDim, Align::Left, kLs04);
    y += 30;
#endif
    label("IF THE CPU RUNS OUT");
    const int shown = lad.count < 6 ? lad.count : 6;
    if (!shown) cv.text(F::kMono400_16, 224, y, "nothing to give up", kDim);
    for (int i = 0; i < shown; ++i) {
        Text s;
        if (i < lvl) s.add("\xE2\x96\xB6 ");
        s.num(i + 1).add(". ");
        describeStep(s, lad.steps[i]);
        cv.text(F::kMono400_16, 224 + (i % 2) * 366, y + (i / 2) * 22, s.c_str(), i < lvl ? kWarn : kDim, Align::Left, 0, 338);
    }
    y += (shown > 0 ? ((shown + 1) / 2) * 22 : 22) + 8;
    label("PARTS");
    Text ps;
    for (int i = 0; i < kParts; ++i) {
        if (isIdlePart(pj, i)) continue;
        if (!ps.empty()) ps.ch(' ');
        ps.add(kPartName[i]).add(" x").num(pj.parts[i].voices).ch(' ').add(kPrioLow[pj.parts[i].priority]);
    }
    cv.text(F::kMono400_16, 224, y, ps.c_str(), kInk, Align::Left, 0, 704);
    y += 30;
    const Run note[1] = {{&F::kMono400_16, "Near the CPU limit the instrument gives these up in order and takes them back later; the patch is never edited.", kDim, 0}};
    drawRuns(cv, 32, y + 1, 860, 23, note, 1);
}

// ------------------------------------------------------------------------------------------------------------------------------------
// lists: BROWSE, SONG, the PROJECT card

struct ListRow {
    const char* c;        // the left column (category, row number)
    const char* name;
    const char* small;    // after the name, dimmer
    const char* tag;      // right
    bool sel, play;
};
static void drawListRow(Canvas& cv, int y, int h, const ListRow& r, uint32_t acc, const Font& nameF, const Font& nameSel, int nameDy, int smallDy) {
    if (r.sel) cv.fill(0, y, kWidth, h - 1, acc);
    cv.fill(0, y + h - 1, kWidth, 1, kListLine);
    if (r.play) cv.fill(0, y, 4, h - 1, kGood);
    const uint32_t fg = r.sel ? kDark : kInk;
    const int cy = y + (h - 1 - 21) / 2;
    Str<24> c;
    c.add(r.c);
    upper(c);
    cv.text(F::kDisplay600_16, 28, cy, c.c_str(), fg, Align::Left, kLs04, 86, kOpacity70);
    const Font& nf = r.sel ? nameSel : nameF;
    const int w = cv.text(nf, 118, y + nameDy, r.name, fg, Align::Left, 0, 680);
    if (r.small && *r.small) cv.text(r.sel ? F::kMono600_16 : F::kMono400_16, 118 + w + 10, y + smallDy, r.small, fg, Align::Left, 0, 0, kOpacity60);
    if (r.tag && *r.tag) cv.text(F::kDisplay600_16, 932, cy, r.tag, fg, Align::Right, kLs02);
}

static void drawBrowse(Canvas& cv, Ui& ui, uint32_t acc) {
    const int n = ui.browseCount(), vis = 8;
    const bool sm = ui.isSamplerPart(ui.part());
    const int start = clampi(ui.browseIdx() - 4, 0, n - vis > 0 ? n - vis : 0);
    const Project& pj = ui.project();
    const SampleLib& lib = ui.samples();
    for (int j = 0; j < vis && start + j < n; ++j) {
        const int it = ui.browseItem(start + j);
        Text tag, small;
        ListRow r{};
        if (sm) {
            for (int k = 0; k < kLanes; ++k)
                if (isSampler(pj, ui.pattern().lanes[k].part) && ui.pattern().lanes[k].sample() == it) { if (!tag.empty()) tag.ch(' '); tag.ch('L').num(k + 1); }
            if (lib.items[it].length) small.fixed(lib.seconds(it), 2).add(" s");   // known once the sample has been loaded
            r.c = lib.cats[lib.items[it].cat];
            r.name = lib.items[it].name;
        } else {
            for (int k = 0; k < kParts; ++k)
                if (ui.browseLoaded(k) == it && !isSampler(pj, k)) { if (!tag.empty()) tag.ch(' '); tag.add("PART ").add(kPartName[k]); }
            r.c = kPresetCats[presetCat(it)];
            r.name = presetName(it);
        }
        r.small = small.c_str();
        r.tag = tag.c_str();
        r.sel = start + j == ui.browseIdx();
        drawListRow(cv, 95 + j * 40, 40, r, acc, F::kMono400_20, F::kMono600_20, 7, 11);
    }
}

static void drawSong(Canvas& cv, Ui& ui, uint32_t acc) {
    const Song& so = ui.song();
    const Transport& tp = ui.transport();
    Str<16> h;
    h.add("SONG ").num(tp.songIdx + 1);
    int x = 28 + cv.text(F::kDisplay700_20, 28, 99, h.c_str(), acc, Align::Left, 13) + 18;
    const double bars = ui.songBars(so), sec = bars * 16 * ui.stepDur();
    Text t;
    t.num(so.nRows).add(so.nRows == 1 ? " ROW" : " ROWS").add(" \xC2\xB7 ");
    {   // bars as JavaScript prints them
        long q = (long)(bars * 16 + 0.5);
        t.num(q / 16);
        if (q % 16) { Str<8> f; f.pad((q % 16) * 625, 4); int len = 4; while (len && f.s[len - 1] == '0') --len; f.s[len] = 0; t.ch('.').add(f.c_str()); }
    }
    t.add(" BARS \xC2\xB7 ").num((long)(sec / 60)).ch(':').pad(jr(sec - 60.0 * (long)(sec / 60)), 2).add(" \xC2\xB7 END ").add(so.loop ? "LOOP" : "STOP");
    cv.text(F::kDisplay600_16, x, 103, t.c_str(), kDim, Align::Left, kLs04);
    if (tp.songPlaying && so.nRows) {
        Text now;
        const int pr = tp.playRow < so.nRows ? tp.playRow : so.nRows - 1;
        now.add("\xE2\x96\xB6 ROW ").num(pr + 1).add(" \xC2\xB7 ").num(so.rows[pr].rep - tp.repLeft + 1).ch('/').num(so.rows[pr].rep);
        cv.text(F::kDisplay600_16, 932, 103, now.c_str(), kGood, Align::Right, kLs04);
    }
    if (!so.nRows) {
        const Run runs[3] = {{&F::kMono400_16, "An empty song. Press ", kDim, 0}, {&F::kMono600_16, "\xE2\x97\x8F", kDim, 0},
                             {&F::kMono400_16, " to add a row: the pattern on screen, played once.", kDim, 0}};
        drawRuns(cv, 32, 152, 860, 23, runs, 3);
        return;
    }
    const int vis = 7, start = clampi(ui.songRow() - 3, 0, so.nRows - vis > 0 ? so.nRows - vis : 0);
    for (int j = 0; j < vis && start + j < so.nRows; ++j) {
        const int i = start + j;
        const SongRow& r = so.rows[i];
        const bool on = tp.songPlaying && i == tp.playRow;
        Str<12> c, name, small, tag;
        if (on) c.add("\xE2\x96\xB6 ");
        c.num(i + 1);
        name.ch('P').num(r.pat + 1).ch(' ');
        small.ch('x').num(r.rep);
        const double b = (double)r.rep * patLen(ui.project().patterns[r.pat]) / 16.0;
        {
            long q = (long)(b * 16 + 0.5);
            tag.num(q / 16);
            if (q % 16) { Str<8> f; f.pad((q % 16) * 625, 4); int len = 4; while (len && f.s[len - 1] == '0') --len; f.s[len] = 0; tag.ch('.').add(f.c_str()); }
            tag.add(q == 16 ? " bar" : " bars");
        }
        ListRow row{c.c_str(), name.c_str(), small.c_str(), tag.c_str(), i == ui.songRow(), on};
        drawListRow(cv, 137 + j * 40, 40, row, acc, F::kMono400_20, F::kMono600_20, 7, 11);
    }
}

// PROJECT view 2: each lane's MIDI channel and note
static void drawMidi(Canvas& cv, Ui& ui, uint32_t acc) {
    const Project& pj = ui.project();
    const SampleLib& lib = ui.samples();
    // column titles
    const int y0 = 137, rowH = 31;
    cv.text(F::kDisplay600_16, 28, 108, "LANE", kDim, Align::Left, kLs04);
    cv.text(F::kDisplay600_16, 120, 108, "PLAYS", kDim, Align::Left, kLs04);
    cv.text(F::kDisplay600_16, 420, 108, "CHANNEL", kDim, Align::Left, kLs04);
    cv.text(F::kDisplay600_16, 560, 108, "NOTE", kDim, Align::Left, kLs04);
    for (int i = 0; i < kLanes; ++i) {
        const Lane& l = ui.pattern().lanes[i];
        const LaneMidi& m = pj.midi[i];
        const int y = y0 + i * rowH;
        const bool sel = i == ui.midiLane(), smp = isSampler(pj, l.part);
        const uint32_t fg = sel ? kDark : kInk;
        if (sel) cv.fill(0, y, kWidth, rowH - 1, acc);
        cv.fill(0, y + rowH - 1, kWidth, 1, kListLine);
        const int ty = y + (rowH - 1 - 22) / 2;
        Str<4> no;
        no.num(i + 1);
        cv.text(F::kMono600_17, 46, ty, no.c_str(), fg, Align::Right);
        cv.fill(60, y + 4, 30, rowH - 9, kPartHex[l.part], 255, 3);
        cv.text(F::kDisplay700_16, 75, y + 4, kPartName[l.part], kDark, Align::Center);
        Text what;
        if (smp) what.add(lib.count ? lib.items[clampi(l.sample(), 0, lib.count - 1)].name : "?");
        else { what.add("synth, root "); noteName(what, l.root); }
        cv.text(F::kMono400_17, 120, ty, what.c_str(), fg, Align::Left, 0, 290);
        Text ch;
        if (m.ch) ch.add("CH ").num(m.ch); else ch.add("OFF");
        cv.text(sel ? F::kMono600_17 : F::kMono400_17, 420, ty, ch.c_str(), m.ch ? fg : (sel ? kDark : kDim));
        if (!m.ch) continue;
        const int shared = lanesOnChannel(pj, m.ch);
        Text note, hint;
        if (shared > 1 || smp) { note.num(m.note).ch(' '); noteName(note, m.note); }
        else note.add("all");
        cv.text(sel ? F::kMono600_17 : F::kMono400_17, 560, ty, note.c_str(), fg);
        int same = -1;   // two lanes on one channel with one note: MIDI in only reaches the first
        if (shared > 1)
            for (int k = 0; k < kLanes && same < 0; ++k) if (k != i && pj.midi[k].ch == m.ch && pj.midi[k].note == m.note) same = k;
        if (same >= 0) {
            hint.add("same note as lane ").num(same + 1);
        } else if (shared > 1) {
            hint.add("shares with");
            for (int k = 0; k < kLanes; ++k) if (k != i && pj.midi[k].ch == m.ch) hint.ch(' ').num(k + 1);
        } else {
            hint.add(smp ? "notes transpose the sample" : "plays every note");
        }
        cv.text(F::kMono400_16, 690, ty + 1, hint.c_str(), same >= 0 ? (sel ? kDark : kWarn) : (sel ? kDark : kDim), Align::Left, 0, 250);
    }
}

// PROJECT view 3: a song to files
static void drawExport(Canvas& cv, Ui& ui, uint32_t acc) {
    const Project& pj = ui.project();
    const Song& so = pj.songs[ui.exportSongIndex()];
    Text t;
    t.add("SONG ").num(ui.exportSongIndex() + 1);
    cv.text(F::kDisplay700_22, 32, 150, t.c_str(), acc);
    t.clear();
    if (so.nRows) t.num(so.nRows).add(so.nRows == 1 ? " row \xC2\xB7 " : " rows \xC2\xB7 ").num((int)ui.songBars(so)).add(" bars \xC2\xB7 ").num(pj.bpm).add(" BPM");
    else t.add("no rows yet: SONG adds them");
    cv.text(F::kMono400_17, 224, 153, t.c_str(), kInk);
    static const char* const kAbout[kExportKinds] = {"The whole song, stereo, 16-bit, 44.1 kHz, effects and all.", "A WAV for each lane that plays. They keep their effects, and line up from the start.",
                                                     "A MIDI file: a track for each lane, with the notes as they play."};
    cv.text(F::kMono400_17, 32, 198, kAbout[ui.exportKind()], kInk);
    cv.text(F::kMono400_17, 32, 226, ui.exportPlace(), kDim);
    if (ui.exporting()) {
        const ExportStatus& e = ui.exportStatus();
        const int w = 896;
        cv.fill(32, 300, w, 22, 0x1f2a30, 255, 3);
        cv.fill(32, 300, w * e.percent / 100, 22, acc, 255, 3);
        t.clear();
        if (e.kind == EXPORT_STEMS) t.add("STEM ").num(e.stem).ch('/').num(e.stems).add(" \xC2\xB7 ");
        t.add(e.kind == EXPORT_MIDI ? "WRITING" : "RENDERING").add(" \xC2\xB7 ").num(e.percent).add("%");
        cv.text(F::kDisplay700_22, 32, 335, t.c_str(), kInk);
        cv.text(F::kMono400_17, 32, 372, "No sound while it works. STOP gives up.", kDim);
        return;
    }
    cv.text(F::kMono600_17, 32, 300, ui.exportKind() == EXPORT_MIDI ? "OK starts. What plays plays on." : "OK starts. No sound while it works, and STOP gives up.", kInk);
    if (ui.exportNote()[0]) cv.text(F::kMono400_17, 32, 340, ui.exportNote(), acc);
}

static void drawProject(Canvas& cv, Ui& ui, uint32_t acc) {
    static const char* const kTabs[] = {"KEY + CARD", "MIDI", "EXPORT"};
    drawTabs(cv, 944, 97, kTabs, 3, ui.projSub(), acc);
    if (ui.projSub() == 1) { drawMidi(cv, ui, acc); return; }
    if (ui.projSub() == 2) { drawExport(cv, ui, acc); return; }
    const Project& pj = ui.project();
    const bool scl = pj.scale > 0;
    cv.text(F::kDisplay600_16, 32, 102, "KEY AND SCALE", kDim, Align::Left, kLs04);
    Text t;
    if (scl) {
        Str<16> sname;
        sname.add(kScales[pj.scale].name);
        for (int i = 0; i < sname.n; ++i) if (sname.s[i] >= 'A' && sname.s[i] <= 'Z') sname.s[i] = (char)(sname.s[i] + 32);
        t.add(kNoteNames[pj.key]).ch(' ').add(sname.c_str()).add(" \xC2\xB7 synth notes snap to it; drum lanes do not");
    } else {
        t.add("off \xC2\xB7 notes play as written (T1 key, T2 scale)");
    }
    cv.text(F::kMono400_17, 224, 101, t.c_str(), kInk);
    // the twelve notes: in the scale lit, the root amber
    for (int i = 0; i < 12; ++i) {
        const int rel = mod(i - pj.key, 12);
        const bool in = scl ? scaleHas(pj.scale, rel) : true, root = scl && rel == 0, blk = kNoteNames[i][1] == '#';
        const int x = 224 + i * 52;
        cv.fill(x, 131, 48, 29, root ? kAmber : in ? 0x1f2a30 : blk ? 0x070a0c : 0x0d1215, 255, 3);
        cv.text(F::kDisplay600_16, x + 24, 135, kNoteNames[i], root ? kDark : in ? 0xe8eef1 : 0x4a555c, Align::Center);
    }
    cv.text(F::kDisplay600_16, 32, 169, "PROJECTS", kDim, Align::Left, kLs04);
    const Run runs[7] = {{&F::kMono400_17, "B1 picks a slot \xC2\xB7 ", kInk, 0}, {&F::kMono600_17, "OK", kInk, 0}, {&F::kMono400_17, " loads \xC2\xB7 ", kInk, 0},
                         {&F::kMono600_17, "SHIFT + OK", kInk, 0}, {&F::kMono400_17, " saves (patterns, songs, sounds, key, tempo) \xC2\xB7 ", kInk, 0},
                         {&F::kMono600_17, "\xE2\x97\x8B", kInk, 0}, {&F::kMono400_17, " deletes", kInk, 0}};
    drawRuns(cv, 224, 168, 704, 22, runs, 7);
    // the list: the factory's projects, then the card's slots, as many rows as fit, with the cursor kept off the edge
    const int total = kFactoryProjects + kCardSlots, rows = 8, cursor = ui.slot() + kFactoryProjects;
    int top = cursor - rows / 2;
    top = top < 0 ? 0 : top > total - rows ? total - rows : top;
    for (int row = 0; row < rows; ++row) {
        const int i = top + row - kFactoryProjects;
        const int y = 226 + row * 28;
        if (i < 0) {
            Str<4> f;
            f.ch('F').num(i + kFactoryProjects + 1);
            ListRow r{f.c_str(), factoryProjectName(i + kFactoryProjects), "factory", ui.loadedSlot() == i ? "LOADED" : "", i == ui.slot(), false};
            drawListRow(cv, y, 28, r, acc, F::kMono400_17, F::kMono600_17, 3, 4);
            continue;
        }
        SlotInfo si;
        const bool has = ui.slotInfo(i, si);
        Str<4> c;
        c.num(i + 1);
        Text small;
        if (ui.deleteAsked() == i) {
            small.add("\xE2\x97\x8B again deletes this project");
        } else if (has) {
            small.num(si.usedPatterns).add(si.usedPatterns == 1 ? " pattern" : " patterns").add(" \xC2\xB7 ").num(si.songs).add(si.songs == 1 ? " song" : " songs")
                .add(" \xC2\xB7 ").num(si.bpm).add(" BPM");
        } else {
            small.add("empty");
        }
        ListRow r{c.c_str(), has ? si.name : "", small.c_str(), ui.loadedSlot() == i ? "LOADED" : "", i == ui.slot(), false};
        drawListRow(cv, y, 28, r, acc, F::kMono400_17, F::kMono600_17, 3, 4);
    }
}

// ------------------------------------------------------------------------------------------------------------------------------------

// ------------------------------------------------------------------------------------------------------------------------------------
// FX: one small graph per view of what the knobs do (the delay's taps, the reverb's decay, the compressor's curve, the amp's tone)

static void drawFx(Canvas& cv, Ui& ui, uint32_t acc) {
    static const char* const kTabs[FX_VIEWS] = {"DELAY", "REVERB", "CHORUS", "COMP", "AMP SIM", "SENDS"};
    const Project& pj = ui.project();
    const int view = ui.fxView(), part = ui.part();
    const int16_t* v = fxValues(pj.fx, view, part);
    if (ui.lockOpen()) {   // a step's lock: only the part's own SENDS and AMP, held for the step
        static const char* const kLockTabs[2] = {"AMP SIM", "SENDS"};
        drawTabs(cv, 944, 97, kLockTabs, 2, view == FX_AMP ? 0 : 1, kLock);
        Str<64> h;
        h.add("STEP ").num(ui.lockIdx() + 1).add(" LOCK \xC2\xB7 PART ").add(kPartName[part]);
        cv.text(F::kDisplay700_22, 32, 99, h.c_str(), kLock, Align::Left, 35);
    } else {
        drawTabs(cv, 944, 97, kTabs, FX_VIEWS, view, acc);
    }
    if (!ui.lockOpen() && (view == FX_AMP || view == FX_SENDS)) {
        Str<24> h;
        h.add("PART ").add(kPartName[part]);
        cv.text(F::kDisplay700_22, 32, 99, h.c_str(), kPartHex[part], Align::Left, 35);
    }
    auto real = [&](int i) { return fxReal(fxDef(view, i), v[i]); };
    const int gx = 32, gy = 148, gw = 896, gh = 290;
    cv.fill(gx, gy, gw, gh, 0x0b0f12, 255, 6);
    cv.stroke(gx, gy, gw, gh, 1, kLine, false, 255, 6);
    Text a, b;
    int16_t ys[kWidth + 1];
    switch (view) {
        case FX_DELAY: {
            const double ms = delayMs(v[0], v[1], pj.bpm), fb = real(2) / 100.0;
            const int base = gy + gh - 28, hmax = gh - 90, sp = 96;
            cv.fill(gx + 20, base, gw - 40, 1, kLine);
            for (int k = 0; k <= 8; ++k) {
                const double lvl = k == 0 ? 1.0 : pow(fb, k) * 0.9;
                const int hh = jr(hmax * lvl), x = gx + 52 + k * sp;
                if (hh < 2) break;
                const bool right = (k & 1) != 0 && real(4) > 20;   // the spread moves every other tap to the other side
                cv.fill(x - 8, base - hh, 16, hh, k == 0 ? kInk : acc, k == 0 ? 255 : (right ? 150 : 230), 3);
            }
            a.add("TIME ").fixed(ms, 0).add(" ms");
            if (v[0] > 0) a.add("  (").add(fxDef(FX_DELAY, 0).list[clampi(v[0], 0, kDelayModes - 1)]).add(" at ").num(pj.bpm).add(" BPM)");
            b.add("FEEDBACK ").num(jr(real(2))).add("%  TONE ");
            formatFx(b, fxDef(FX_DELAY, 3), v[3]);
            break;
        }
        case FX_REVERB: {
            const double dec = real(1), pd = real(3);
            const double win = dec * 1.25 + pd / 1000.0;
            const int top = gy + 76, bot = gy + gh - 28;
            cv.fill(gx + 20, bot, gw - 40, 1, kLine);
            const int px = gx + 40, pw = gw - 80;
            for (int i = 0; i < pw; ++i) {
                const double t = win * i / pw - pd / 1000.0;
                double db = t <= 0 ? 0.0 : -60.0 * t / dec;   // linear in dB: a reverb's decay
                if (db < -60) db = -60;
                ys[i] = (int16_t)jr(top + (bot - top) * (-db / 60.0));
            }
            cv.curve(px, ys, pw, 4, acc, bot, acc, 0x30);
            const int n = 5 + jr(real(0) / 25);   // the size: the early reflections
            for (int k = 0; k < n; ++k) {
                const int x = px + jr(pw * (pd / 1000.0 + 0.012 + 0.018 * k * (0.4 + real(0) / 100.0)) / win), hh = jr((bot - top) * 0.6 * pow(0.78, k));
                cv.fill(x, bot - hh, 3, hh, kInk, 150);
            }
            a.add("DECAY ").fixed(dec, dec < 10 ? 1 : 0).add(" s  PRE-DELAY ").num(jr(pd)).add(" ms");
            b.add("SIZE ").num(jr(real(0))).add("%  DAMP ");
            formatFx(b, fxDef(FX_REVERB, 2), v[2]);
            break;
        }
        case FX_CHORUS: {
            const double rate = real(0), depth = real(1) / 100.0;
            const int mid = gy + gh / 2 + 18, amp = jr((gh / 2 - 62) * depth);
            cv.fill(gx + 20, mid, gw - 40, 1, kLine);
            const int pw = gw - 40;
            for (int i = 0; i < pw; ++i) ys[i] = (int16_t)jr(mid - amp * sin(6.283185307 * rate * 4.0 * i / pw));
            cv.curve(gx + 20, ys, pw, 4, acc);
            a.add("RATE ");
            formatFx(a, fxDef(FX_CHORUS, 0), v[0]);
            a.add("  DEPTH ").num(jr(real(1))).add("%  DELAY ").fixed(real(2), 0).add(" ms");
            b.add("FEEDBACK ").num(jr(real(3))).add("%  WIDTH ").num(jr(real(4))).add("%");
            break;
        }
        case FX_COMP: {
            const int sz = 270, sx = gx + (gw - sz) / 2, sy = gy + 12;
            const double thr = real(1), ratio = compRatio(v[2]), mk = real(5);
            for (int g = 0; g <= 5; ++g) {   // a grid every 12 dB
                cv.fill(sx + g * sz / 5, sy, 1, sz, kLine);
                cv.fill(sx, sy + g * sz / 5, sz, 1, kLine);
            }
            // input -60..0 dB across, output -60..+12 dB up (the diagonal is no compression)
            auto yOf = [&](double db) { return jr(sy + sz - (db + 60.0) / 72.0 * sz); };
            for (int i = 0; i < sz; ++i) {
                const double in = -60.0 + 60.0 * i / (sz - 1);
                const double out = (in < thr ? in : thr + (in - thr) / ratio) + mk;
                const int y = yOf(out);
                ys[i] = (int16_t)(y < sy - 4 ? sy - 4 : y > sy + sz + 4 ? sy + sz + 4 : y);
            }
            for (int i = 0; i < sz; i += 6) cv.fill(sx + i, yOf(-60.0 + 60.0 * i / (sz - 1)), 3, 2, kDim, 120);
            cv.curve(sx, ys, sz, 4, v[0] ? acc : kDim);
            cv.fill(sx + jr((thr + 60.0) / 60.0 * sz), sy, 1, sz, kWarn, 160);
            a.add("THRESH ");
            formatFx(a, fxDef(FX_COMP, 1), v[1]);
            a.add("  RATIO ").add(fxDef(FX_COMP, 2).list[clampi(v[2], 0, 8)]);
            b.add(v[0] ? "ON" : "OFF").add("  MAKEUP ");
            formatFx(b, fxDef(FX_COMP, 5), v[5]);
            break;
        }
        case FX_AMP: {
            const int mid = gy + gh / 2 + 10, pw = gw - 40, model = v[0], cab = v[6];
            cv.fill(gx + 20, mid, pw, 1, kLine);
            const double gB = real(2), gM = real(3), gT = real(4), pres = real(5) / 100.0 * 6.0;
            const double cabLp = cab == 0 ? 0.0 : cab == 1 ? 6500.0 : cab == 2 ? 5500.0 : 4500.0;
            for (int i = 0; i < pw; ++i) {
                const double f = 40.0 * pow(16000.0 / 40.0, (double)i / (pw - 1));
                const double lo = 1.0 / (1.0 + pow(f / 140.0, 2.0)), hi = 1.0 - 1.0 / (1.0 + pow(f / 2800.0, 2.0));
                const double md = exp(-pow(log(f / 800.0) / log(2.0), 2.0) / 2.0);
                double db = gB * lo + gM * md + (gT + pres * 0.5) * hi;
                if (cabLp > 0) db -= 10.0 * log10(1.0 + pow(f / cabLp, 4.0)) + 10.0 * log10(1.0 + pow(80.0 / f, 4.0));
                db = db < -17 ? -17 : db > 17 ? 17 : db;   // the graph box: 7 px per dB around its middle line
                ys[i] = (int16_t)jr(mid - db * 7.0);
            }
            cv.curve(gx + 20, ys, pw, 4, model ? kPartHex[part] : kDim);
            a.add("MODEL ").add(fxDef(FX_AMP, 0).list[clampi(model, 0, 4)]).add("  DRIVE ").num(jr(real(1))).add("%  CAB ").add(fxDef(FX_AMP, 6).list[clampi(cab, 0, 3)]);
            b.add(model ? "an insert on the part, before the sends" : "OFF: the part is not changed");
            break;
        }
        default: {   // SENDS: every part's three sends, the part on screen lit
            static const char* const kSendName[3] = {"DELAY", "REVERB", "CHORUS"};
            constexpr int kRow = (290 - 62) / kParts;
            for (int p = 0; p < kParts; ++p) {
                const int y = gy + 60 + p * kRow;
                if (p == part) cv.fill(gx + 8, y - 2, gw - 16, kRow - 1, kPartHex[p], 24, 5);
                cv.text(F::kDisplay700_16, gx + 28, y + 5, kPartName[p], kPartHex[p]);
                for (int s = 0; s < kSendsUsed; ++s) {
                    const int x = gx + 70 + s * 276, val = pj.fx.send[p][s];
                    Text t;
                    t.add(kSendName[s]).ch(' ').num(jr(val / 127.0 * 100)).ch('%');
                    cv.text(F::kMono400_16, x, y + 5, t.c_str(), p == part ? kInk : kDim, Align::Left, 0, 120);
                    cv.fill(x + 126, y + 10, 124, 10, kStepOff, 255, 3);
                    if (val) cv.fill(x + 126, y + 10, jr(124 * val / 127.0), 10, kPartHex[p], 255, 3);
                }
            }
            a.add("a send is how much of the part goes to the effect");
            break;
        }
    }
    if (!a.empty()) cv.text(F::kMono400_17, gx + 20, gy + 14, a.c_str(), kInk, Align::Left, 0, gw - 40);
    if (!b.empty()) cv.text(F::kMono400_16, gx + 20, gy + 40, b.c_str(), kDim, Align::Left, 0, gw - 40);
}

// ------------------------------------------------------------------------------------------------------------------------------------
// REC: the input's level with the threshold on it, and the take as it grows

static void drawRecord(Canvas& cv, Ui& ui, uint32_t acc) {
    const RecStatus& r = ui.recording();
    const RecSettings& s = ui.recSettings();
    static const char* const kState[4] = {"READY", "WAITING FOR SOUND", "RECORDING", "TAKE DONE"};
    cv.text(F::kDisplay700_22, 32, 99, kState[r.state > 3 ? 0 : r.state], r.state == REC_TAKING ? kWarn : acc, Align::Left, 35);
    {
        Text t;
        if (r.state == REC_TAKING || r.state == REC_DONE) t.fixed((double)r.frames / r.rate, 2).add(" s");
        cv.text(F::kMono600_22, kWidth - 32, 99, t.c_str(), kInk, Align::Right);
    }
    // the meter: -60 dB to 0 across the screen
    const int mx = 32, mw = kWidth - 64, my = 142;
    auto toX = [&](float db) { return mx + jr((db + 60.0f) / 60.0f * mw); };
    const float level = ui.recLevel(), db = level > 0.001f ? 20.0f * log10f(level) : -60.0f;
    cv.fill(mx, my, mw, 18, kStepOff, 255, 3);
    const int fw = toX(db) - mx;
    if (fw > 0) cv.fill(mx, my, fw, 18, db > -3.0f ? kWarn : kGood, 255, 3);
    if (s.autoCapture || s.stop) {
        const int tx = toX((float)s.thresholdDb);
        cv.fill(tx - 1, my - 6, 3, 30, kWhite);
    }
    // the take
    const int T = 180, H = 250, mid = T + H / 2;
    cv.fill(mx, mid, mw, 1, kLine);
    if ((r.state == REC_TAKING || r.state == REC_DONE) && r.data && r.frames > 1) {
        static int16_t lo[kWidth], hi[kWidth];
        static uint8_t pick[kWidth];
        const uint32_t span = r.frames > r.rate * 4 ? r.frames : r.rate * 4;   // four seconds, then the whole take
        for (int x = 0; x < mw; ++x) {
            const uint32_t a = (uint32_t)((uint64_t)x * span / mw), b = (uint32_t)((uint64_t)(x + 1) * span / mw);
            int mn = 0, mxv = 0;
            const uint32_t stride = (b - a) / 12 > 1 ? (b - a) / 12 : 1;
            for (uint32_t i = a; i < b && i < r.frames; i += stride) { const int v = r.data[i]; if (v < mn) mn = v; if (v > mxv) mxv = v; }
            lo[x] = (int16_t)(mid - mxv * (H / 2) / 32768);
            const int h = (mxv - mn) * (H / 2) / 32768;
            hi[x] = (int16_t)(lo[x] + (h > 1 ? h : 1));
            pick[x] = 0;
        }
        cv.columns(mx, lo, hi, pick, mw, acc, 0x2b343a);
    }
    if (r.state == REC_DONE) footer(cv, r.clipped ? "CLIPPED: LOWER THE GAIN AND GO AGAIN" : "OK KEEPS IT ON THIS LANE, \xE2\x97\x8B DROPS IT, REC GOES AGAIN");
    else if (r.state == REC_IDLE) footer(cv, s.autoCapture ? "REC ARMS: IT STARTS WHEN THE SOUND PASSES THE LINE" : "REC STARTS");
}

// ------------------------------------------------------------------------------------------------------------------------------------
// SAVE PRESET: the name with its cursor, and what will be written

static void drawSave(Canvas& cv, Ui& ui, uint32_t acc) {
    const Project& pj = ui.project();
    const int part = ui.part();
    cv.text(F::kDisplay700_22, 32, 99, "SAVE PRESET", acc, Align::Left, 35);
    const int gx = 32, gy = 148, gw = 896, gh = 290;
    cv.fill(gx, gy, gw, gh, 0x0b0f12, 255, 6);
    cv.stroke(gx, gy, gw, gh, 1, kLine, false, 255, 6);
    // the name: twenty places, the cursor one drawn as a block
    const char* name = ui.saveName();
    const int cw = Canvas::measure(F::kMono600_22, "M", 0) + 6, nx = gx + 36, ny = gy + 34;
    for (int i = 0; i < 20; ++i) {
        const int x = nx + i * cw;
        const bool cur = i == ui.saveCursor();
        if (cur) cv.fill(x - 2, ny - 6, cw, 36, acc, 255, 3);
        else cv.fill(x + 2, ny + 27, cw - 8, 2, kLine);
        char ch[2] = {name[i] ? name[i] : ' ', 0};
        if (ch[0] != ' ') cv.text(F::kMono600_22, x + 1, ny, ch, cur ? kDark : kInk, Align::Left, 0);
    }
    Text a, b, c;
    const int id = ui.saveTargetId();
    if (ui.saveReplaces()) a.add("REPLACES  ").add(presetName(ui.saveSource())).add("  (ID ").num(ui.saveSource()).add(")");
    else if (id >= 0) a.add("NEW PRESET  ID ").num(id);
    else a.add("NO ROOM FOR A NEW PRESET");
    b.add("CATEGORY ").add(kPresetCats[ui.saveCategory()]).add("   VOICES ").num(pj.parts[part].voices).add("   PART ").add(kPartName[part]);
    c.add("EFFECTS");
    bool any = false;
    static const char* const kSendName[3] = {"DELAY", "REVERB", "CHORUS"};
    for (int e = 0; e < 3; ++e)
        if (pj.fx.send[part][e] > 0) { c.ch(' ').add(kSendName[e]); any = true; }
    if (pj.fx.amp[part][0] != 0) { c.add(" AMP"); any = true; }
    if (!any) c.add(" NONE");
    cv.text(F::kMono400_20, gx + 36, gy + 112, a.c_str(), kInk, Align::Left, 0, gw - 72);
    cv.text(F::kMono400_17, gx + 36, gy + 150, b.c_str(), kDim, Align::Left, 0, gw - 72);
    cv.text(F::kMono400_17, gx + 36, gy + 178, c.c_str(), kDim, Align::Left, 0, gw - 72);
    cv.text(F::kMono400_16, gx + 36, gy + 236, "KNOB 1 CURSOR, KNOB 2 CHARACTER (KEYS AROUND OK TOO)   OK SAVES   L CANCELS", kDim, Align::Left, 0, gw - 72);
}

void drawScreen(Canvas& cv, Ui& ui) {
    ui.settle();   // before anything is drawn: the pattern may have switched since the last input
    cv.begin(kScreenBg);
    const uint32_t acc = accent(ui);
    cv.pushClip(0, kMainTop, kWidth, kMainH);
    switch (ui.mode()) {
        case Mode::Play: drawPlay(cv, ui, acc); break;
        case Mode::Step: if (ui.tracker()) drawTracker(cv, ui, acc); else drawStep(cv, ui, acc); break;
        case Mode::Sound: drawSound(cv, ui, acc); break;
        case Mode::Mixer: drawMixer(cv, ui, acc); break;
        case Mode::Browse: drawBrowse(cv, ui, acc); break;
        case Mode::Song: drawSong(cv, ui, acc); break;
        case Mode::ProjectM: drawProject(cv, ui, acc); break;
        case Mode::Fx: drawFx(cv, ui, acc); break;
        case Mode::Save: drawSave(cv, ui, acc); break;
        case Mode::Record: drawRecord(cv, ui, acc); break;
    }
    cv.popClip();
    drawFrame(cv, ui, acc);
    // the toast: centred, 100 px above the bottom
    if (const char* msg = ui.toast()) {
        Str<96> m;
        m.add(msg);
        upper(m);
        const int w = Canvas::measure(F::kDisplay700_16, m.c_str(), kLs10), bw = w + 32;
        int bx = 480 - bw / 2 > 4 ? 480 - bw / 2 : 4;
        // 100 px above the bottom, except where that sits on what is being worked on: STEP and the sound pages put it under the top bar (their data starts lower
        // or is a picture), PLAY on the half of the grid the selected lane is not in
        const Mode md = ui.mode();
        const bool mixTop = md == Mode::Mixer && bw <= 520;   // MIXER: top left, clear of its view tabs (its text runs to the bottom)
        const bool lowRow = ui.tracker() && ui.trackerRow() % kTrackerRows >= 8;   // the tracker: as PLAY, on the half the cursor is not in
        const int ty = ((md == Mode::Step && !ui.tracker()) || lowRow || md == Mode::Sound || mixTop || (md == Mode::Play && ui.sel() >= 5)) ? 96 : 403;
        if (mixTop) bx = 16;
        cv.fill(bx, ty, bw < kWidth - 8 ? bw : kWidth - 8, 37, acc, 255, 4);
        cv.text(F::kDisplay700_16, bx + 16, ty + 8, m.c_str(), kDark, Align::Left, kLs10, kWidth - 8 - 32);
    }
}

}  // namespace gfx
