#ifndef __GFX_CANVAS_H
#define __GFX_CANVAS_H

#include <stddef.h>
#include <stdint.h>

#include "gfx/Font.h"

// A frame is recorded as a display list, then rasterised strip by strip into a small RGB565 buffer. The board pushes each strip's
// changed tiles to the panel (the display has its own frame memory; there is no room for a full frame in fast RAM, and the bit-banged
// bus is the slow part). The host rasterises the same list into a full frame for PNGs. Colours are 0xRRGGBB; alpha is 0..255.
namespace gfx {

constexpr int kWidth = 960, kHeight = 540;

// rounded, not truncated: truncating drops red and blue (5 bits) more than green (6 bits) and tints dark greys green
inline uint16_t rgb565(uint32_t c) {
    const uint32_t r = (((c >> 16) & 255) * 31 + 127) / 255, g = (((c >> 8) & 255) * 63 + 127) / 255, b = ((c & 255) * 31 + 127) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
// a over b, alpha 0..255 (CSS color-mix / a translucent fill, done once on the colour values)
inline uint32_t mix(uint32_t a, uint32_t b, int alpha) {
    auto ch = [&](int s) { const int x = (a >> s) & 255, y = (b >> s) & 255; return (uint32_t)((x * alpha + y * (255 - alpha) + 127) / 255) << s; };
    return ch(16) | ch(8) | ch(0);
}

enum class Align : uint8_t { Left, Center, Right };

class Canvas {
   public:
    // The arena holds the frame's commands; it is aligned here (the M7 faults on unaligned 64-bit stores into a command).
    Canvas(uint8_t* arena, size_t size);

    void begin(uint32_t background);   // a new frame
    // ---- recording
    void fill(int x, int y, int w, int h, uint32_t c, int alpha = 255, int radius = 0);
    void stroke(int x, int y, int w, int h, int width, uint32_t c, bool dashed = false, int alpha = 255, int radius = 0);
    // Text with its LINE BOX's top at y (CSS: the line is ascent + descent tall). `spacing16`: CSS letter-spacing in 1/16 px.
    // `clipW` > 0 cuts the text at that width with an ellipsis (CSS text-overflow: ellipsis). Returns the advance in px.
    int text(const Font& f, int x, int y, const char* s, uint32_t c, Align a = Align::Left, int spacing16 = 0, int clipW = 0, int alpha = 255);
    // A stroked polyline given as one y per column from x0 (at most kWidth + 1 columns), optionally filled down to `fillTo`.
    void curve(int x0, const int16_t* ys, int n, int thickness, uint32_t c, int fillTo = -1, uint32_t fillColor = 0, int fillAlpha = 0);
    // Vertical 1-px bars, one per column from x0: rows [lo, hi) in colour a (pick 0) or b (pick 1). A waveform in one command.
    void columns(int x0, const int16_t* lo, const int16_t* hi, const uint8_t* pick, int n, uint32_t a, uint32_t b);
    // CSS repeating-linear-gradient(135deg, transparent 0 9px, colour 9px 18px): the hatch of a read-only cell
    void hatch(int x, int y, int w, int h, uint32_t c, int alpha);
    // Clips nest: a pushed clip is the intersection with the current one; pop restores the one before.
    void pushClip(int x, int y, int w, int h);
    void popClip();
    static int measure(const Font& f, const char* s, int spacing16 = 0);   // px, rounded up

    // ---- rasterising
    void raster(uint16_t* strip, int y0, int rows) const;   // rows [y0, y0+rows), full width, kWidth stride
    bool overflowed() const { return overflow_; }
    size_t used() const { return len_; }

   private:
    enum class Op : uint8_t { Nop, Fill, Stroke, Text, Curve, Clip, Hatch, Columns };
    struct Cmd {
        Op op;
        uint8_t alpha, flag, radius;   // flag: dashed
        int16_t x, y, w, h;            // bounds (curve and columns: x0, top, n, height)
        uint16_t c, c2;                // colours, RGB565
        int16_t a1, a2;                // stroke width / text spacing16 / curve thickness; curve fillTo
        uint32_t size;                 // bytes of this command and its payload (the payload follows the command)
        const Font* font;              // text
    };
    static const uint8_t* payloadOf(const Cmd& c) { return reinterpret_cast<const uint8_t*>(&c) + ((sizeof(Cmd) + 7) & ~(size_t)7); }
    void* alloc(size_t n);
    Cmd* push(Op op);
    void* payload(Cmd* c, size_t n);
    void rasterText(const Cmd& c, uint16_t* strip, int base, int y0, int y1, int cx0, int cx1) const;

    uint8_t* arena_;
    size_t cap_, len_ = 0;
    bool overflow_ = false;
    uint16_t bg_ = 0;
    int16_t clips_[8][4];
    int clipDepth_ = 0;
};

}  // namespace gfx

#endif
