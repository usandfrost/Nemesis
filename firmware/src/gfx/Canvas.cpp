#include "gfx/Canvas.h"

#include <string.h>

namespace gfx {

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int clampAlpha(int a) { return a < 0 ? 0 : a > 255 ? 255 : a; }
static inline int floorDiv16(int v) { return v >= 0 ? v / 16 : -((-v + 15) / 16); }

// a over the pixel, in the RGB565 domain: the channels spread into one word (0x07E0F81F), one multiply, alpha in 32 steps
static inline void blend(uint16_t& px, uint16_t c, int a) {
    if (a <= 0) return;
    if (a >= 255) { px = c; return; }
    const uint32_t a5 = (uint32_t)(a + 4) >> 3;
    const uint32_t fg = (c | ((uint32_t)c << 16)) & 0x07E0F81Fu, bg = (px | ((uint32_t)px << 16)) & 0x07E0F81Fu;
    const uint32_t r = ((((fg - bg) * a5) >> 5) + bg) & 0x07E0F81Fu;
    px = (uint16_t)(r | (r >> 16));
}

// coverage 0..256 of pixel (px, py) inside the rounded rectangle [0,w) x [0,h) with corner radius r (r <= 0: square)
static inline int roundCover(int px, int py, int w, int h, int r) {
    if (px < 0 || py < 0 || px >= w || py >= h) return 0;
    if (r <= 0) return 256;
    // in half pixels: the pixel centre against the corner circle's centre
    const int fx = 2 * px + 1, fy = 2 * py + 1, rr = 2 * r;
    const int cx = fx < rr ? rr : (fx > 2 * w - rr ? 2 * w - rr : fx), cy = fy < rr ? rr : (fy > 2 * h - rr ? 2 * h - rr : fy);
    const int dx = fx - cx, dy = fy - cy, d2 = dx * dx + dy * dy;
    const int in = (rr - 1) * (rr - 1), out = (rr + 1) * (rr + 1);
    if (d2 <= in) return 256;
    if (d2 >= out) return 0;
    return 256 - (d2 - in) * 256 / (out - in);   // linear over the pixel straddling the edge
}

Canvas::Canvas(uint8_t* arena, size_t size) {
    const uintptr_t a = (uintptr_t)arena, aligned = (a + 7) & ~(uintptr_t)7;
    arena_ = reinterpret_cast<uint8_t*>(aligned);
    cap_ = size > aligned - a ? size - (aligned - a) : 0;
}

void Canvas::begin(uint32_t background) {
    len_ = 0;
    overflow_ = false;
    bg_ = rgb565(background);
    clipDepth_ = 0;
    clips_[0][0] = 0; clips_[0][1] = 0; clips_[0][2] = kWidth; clips_[0][3] = kHeight;
}

void* Canvas::alloc(size_t n) {
    n = (n + 7) & ~(size_t)7;
    // latched: after one failure nothing more is recorded, so a frame never shows things drawn over a piece that is missing
    if (overflow_ || len_ + n > cap_) { overflow_ = true; return nullptr; }
    void* p = arena_ + len_;
    len_ += n;
    return p;
}

Canvas::Cmd* Canvas::push(Op op) {
    Cmd* c = static_cast<Cmd*>(alloc(sizeof(Cmd)));
    if (!c) return nullptr;
    memset(c, 0, sizeof(Cmd));
    c->op = op;
    c->size = (uint32_t)((sizeof(Cmd) + 7) & ~(size_t)7);
    return c;
}

void* Canvas::payload(Cmd* c, size_t n) {
    void* p = alloc(n);
    if (!p) { c->op = Op::Nop; return nullptr; }
    c->size += (uint32_t)((n + 7) & ~(size_t)7);
    return p;
}

static int clampRadius(int r, int w, int h) {
    r = imin(r, imin(w, h) / 2);   // CSS: a radius never exceeds half the box
    return imax(0, imin(r, 255));
}

void Canvas::fill(int x, int y, int w, int h, uint32_t col, int alpha, int radius) {
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    Cmd* c = push(Op::Fill);
    if (!c) return;
    c->x = (int16_t)x; c->y = (int16_t)y; c->w = (int16_t)w; c->h = (int16_t)h;
    c->c = rgb565(col);
    c->alpha = (uint8_t)clampAlpha(alpha);
    c->radius = (uint8_t)clampRadius(radius, w, h);
}

void Canvas::stroke(int x, int y, int w, int h, int width, uint32_t col, bool dashed, int alpha, int radius) {
    if (w <= 0 || h <= 0 || width <= 0 || alpha <= 0) return;
    Cmd* c = push(Op::Stroke);
    if (!c) return;
    c->x = (int16_t)x; c->y = (int16_t)y; c->w = (int16_t)w; c->h = (int16_t)h;
    c->c = rgb565(col);
    c->alpha = (uint8_t)clampAlpha(alpha);
    c->flag = dashed;
    c->a1 = (int16_t)imin(width, imin(w, h));
    c->radius = (uint8_t)clampRadius(radius, w, h);
}

int Canvas::measure(const Font& f, const char* s, int spacing16) {
    int w16 = 0;
    while (*s) {
        const Glyph* g = f.find(utf8Next(s));
        if (g) w16 += g->adv16 + spacing16;
    }
    return (w16 + 15) / 16;
}

int Canvas::text(const Font& f, int x, int y, const char* s, uint32_t col, Align a, int spacing16, int clipW, int alpha) {
    if (!s || !*s || alpha <= 0) return 0;
    size_t n = strlen(s);
    if (n > 1024) n = 1024;   // a screen line is far shorter; keeps a command's size bounded
    int w = measure(f, s, spacing16);
    Cmd* c = push(Op::Text);
    if (!c) return w;
    char* copy = static_cast<char*>(payload(c, n + 4));
    if (!copy) return w;
    memcpy(copy, s, n);
    copy[n] = 0;
    if (clipW > 0 && w > clipW) {
        // cut on a character boundary so the text plus an ellipsis fits
        static const char kEll[] = "\xE2\x80\xA6";
        const int ell = measure(f, kEll, spacing16);
        int w16 = 0;
        const char* p = copy;
        const char* cut = copy;
        while (*p) {
            const char* q = p;
            const Glyph* g = f.find(utf8Next(q));
            const int adv = g ? g->adv16 + spacing16 : 0;
            if ((w16 + adv + 15) / 16 + ell > clipW) break;
            w16 += adv;
            p = q;
            cut = p;
        }
        n = (size_t)(cut - copy);
        memcpy(copy + n, kEll, 4);
        w = (w16 + 15) / 16 + ell;
    }
    c->x = (int16_t)(a == Align::Center ? x - w / 2 : a == Align::Right ? x - w : x);
    c->y = (int16_t)y;
    c->w = (int16_t)w;
    c->h = (int16_t)f.lineHeight();
    c->c = rgb565(col);
    c->alpha = (uint8_t)clampAlpha(alpha);
    c->a1 = (int16_t)spacing16;
    c->font = &f;
    return w;
}

void Canvas::curve(int x0, const int16_t* ys, int n, int thickness, uint32_t col, int fillTo, uint32_t fillColor, int fillAlpha) {
    if (n <= 0) return;
    if (n > kWidth + 1) n = kWidth + 1;
    int top = ys[0], bot = ys[0];
    for (int i = 1; i < n; ++i) { top = imin(top, ys[i]); bot = imax(bot, ys[i]); }
    Cmd* c = push(Op::Curve);
    if (!c) return;
    int16_t* copy = static_cast<int16_t*>(payload(c, sizeof(int16_t) * (size_t)n));
    if (!copy) return;
    memcpy(copy, ys, sizeof(int16_t) * (size_t)n);
    c->x = (int16_t)x0;
    c->y = (int16_t)(top - thickness);
    c->w = (int16_t)n;
    c->h = (int16_t)((fillTo >= 0 ? imax(bot, fillTo) : bot) + thickness - c->y + 1);
    c->c = rgb565(col);
    c->c2 = rgb565(fillColor);
    c->alpha = 255;
    c->radius = (uint8_t)clampAlpha(fillAlpha);
    c->a1 = (int16_t)imax(1, thickness);
    c->a2 = (int16_t)fillTo;
}

void Canvas::columns(int x0, const int16_t* lo, const int16_t* hi, const uint8_t* pick, int n, uint32_t a, uint32_t b) {
    if (n <= 0) return;
    if (n > kWidth) n = kWidth;
    int top = lo[0], bot = hi[0];
    for (int i = 1; i < n; ++i) { top = imin(top, lo[i]); bot = imax(bot, hi[i]); }
    Cmd* c = push(Op::Columns);
    if (!c) return;
    int16_t* p = static_cast<int16_t*>(payload(c, sizeof(int16_t) * 3 * (size_t)n));
    if (!p) return;
    for (int i = 0; i < n; ++i) { p[3 * i] = lo[i]; p[3 * i + 1] = hi[i]; p[3 * i + 2] = pick[i]; }
    c->x = (int16_t)x0;
    c->y = (int16_t)top;
    c->w = (int16_t)n;
    c->h = (int16_t)(bot - top);
    c->c = rgb565(a);
    c->c2 = rgb565(b);
}

void Canvas::hatch(int x, int y, int w, int h, uint32_t col, int alpha) {
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    Cmd* c = push(Op::Hatch);
    if (!c) return;
    c->x = (int16_t)x; c->y = (int16_t)y; c->w = (int16_t)w; c->h = (int16_t)h;
    c->c = rgb565(col);
    c->alpha = (uint8_t)clampAlpha(alpha);
}

void Canvas::pushClip(int x, int y, int w, int h) {
    const int16_t* cur = clips_[clipDepth_];
    const int x0 = imax(x, cur[0]), y0 = imax(y, cur[1]);
    int x1 = imin(x + w, cur[0] + cur[2]), y1 = imin(y + h, cur[1] + cur[3]);
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    if (clipDepth_ < 7) ++clipDepth_;
    int16_t* nc = clips_[clipDepth_];
    nc[0] = (int16_t)x0; nc[1] = (int16_t)y0; nc[2] = (int16_t)(x1 - x0); nc[3] = (int16_t)(y1 - y0);
    Cmd* c = push(Op::Clip);
    if (!c) return;
    c->x = nc[0]; c->y = nc[1]; c->w = nc[2]; c->h = nc[3];
}

void Canvas::popClip() {
    if (clipDepth_ > 0) --clipDepth_;
    const int16_t* cur = clips_[clipDepth_];
    Cmd* c = push(Op::Clip);
    if (!c) return;
    c->x = cur[0]; c->y = cur[1]; c->w = cur[2]; c->h = cur[3];
}

// ---------------------------------------------------------------------------------------------------------------------------------

void Canvas::rasterText(const Cmd& c, uint16_t* strip, int base, int y0, int y1, int cx0, int cx1) const {
    const Font& f = *c.font;
    const char* s = reinterpret_cast<const char*>(payloadOf(c));
    int pen16 = c.x * 16;
    while (*s) {
        const Glyph* g = f.find(utf8Next(s));
        if (!g) continue;
        const int gx = floorDiv16(pen16 + 8) + g->dx, gy = c.y + g->top;
        pen16 += g->adv16 + c.a1;
        if (!g->w || gy >= y1 || gy + g->h <= y0 || gx >= cx1 || gx + g->w <= cx0) continue;
        const uint8_t* bits = f.bits + g->off;
        const int ra = imax(y0, gy), rb = imin(y1, gy + g->h);
        const int xa = imax(cx0, gx), xb = imin(cx1, gx + g->w);
        for (int y = ra; y < rb; ++y) {
            uint16_t* row = strip + (y - base) * kWidth;
            const int r0 = (y - gy) * g->w;
            for (int x = xa; x < xb; ++x) {
                const int i = r0 + (x - gx);
                const int cov = (bits[i >> 1] >> ((i & 1) * 4)) & 15;
                if (cov) blend(row[x], c.c, c.alpha == 255 ? cov * 17 : cov * 17 * c.alpha / 255);
            }
        }
    }
}

void Canvas::raster(uint16_t* strip, int y0, int rows) const {
    const int y1 = y0 + rows;
    for (int i = 0; i < rows * kWidth; ++i) strip[i] = bg_;
    int cx0 = 0, cy0 = 0, cx1 = kWidth, cy1 = kHeight;
    size_t pos = 0;
    while (pos < len_) {
        const Cmd& c = *reinterpret_cast<const Cmd*>(arena_ + pos);
        if (c.size == 0) break;   // cannot happen; never loop forever
        pos += c.size;
        if (c.op == Op::Nop) continue;
        if (c.op == Op::Clip) {
            cx0 = imax(0, c.x); cy0 = imax(0, c.y); cx1 = imin(kWidth, c.x + c.w); cy1 = imin(kHeight, c.y + c.h);
            continue;
        }
        const int extra = c.op == Op::Text ? 4 : 0;   // glyph ink may reach a little past the line box
        const int ya = imax(imax(y0, cy0), c.y - extra), yb = imin(imin(y1, cy1), c.y + c.h + extra);
        if (ya >= yb) continue;
        switch (c.op) {
            case Op::Fill: {
                const int xa = imax(cx0, c.x), xb = imin(cx1, c.x + c.w);
                if (xa >= xb) break;
                const int r = c.radius;
                for (int y = ya; y < yb; ++y) {
                    uint16_t* row = strip + (y - y0) * kWidth;
                    const int ry = y - c.y;
                    const bool corner = r > 0 && (ry < r || ry >= c.h - r);
                    if (!corner && c.alpha == 255) {
                        for (int x = xa; x < xb; ++x) row[x] = c.c;
                        continue;
                    }
                    for (int x = xa; x < xb; ++x) {
                        int a = c.alpha;
                        if (corner) {
                            const int rx = x - c.x;
                            if (rx < r || rx >= c.w - r) a = a * roundCover(rx, ry, c.w, c.h, r) >> 8;
                        }
                        blend(row[x], c.c, a);
                    }
                }
                break;
            }
            case Op::Stroke: {
                const int wd = c.a1, r = c.radius;
                const int dash = wd * 3;
                // dashed: each side gets an odd number of segments, so every corner sits on a dash (as Chrome draws it)
                const int segX = imax(1, c.w / dash) | 1, segY = imax(1, c.h / dash) | 1;
                const int xa = imax(cx0, c.x), xb = imin(cx1, c.x + c.w);
                const int innerR = r > wd ? r - wd : 0;
                for (int y = ya; y < yb; ++y) {
                    const int ry = y - c.y;
                    const bool band = ry < wd || ry >= c.h - wd;
                    uint16_t* row = strip + (y - y0) * kWidth;
                    for (int x = xa; x < xb; ++x) {
                        const int rx = x - c.x;
                        const bool side = rx < wd || rx >= c.w - wd;
                        int cov = 256;
                        const bool nearCorner = r && (ry < r || ry >= c.h - r) && (rx < r || rx >= c.w - r);
                        if (nearCorner) {
                            cov = roundCover(rx, ry, c.w, c.h, r) - roundCover(rx - wd, ry - wd, c.w - 2 * wd, c.h - 2 * wd, innerR);
                        } else if (!band && !side) {
                            x = imax(x, c.x + c.w - wd - 1);   // jump over the inside
                            continue;
                        }
                        if (cov <= 0) continue;
                        if (c.flag) {
                            const bool on = band ? ((rx * segX / c.w) & 1) == 0 : ((ry * segY / c.h) & 1) == 0;
                            if (!on) continue;
                        }
                        blend(row[x], c.c, c.alpha * cov >> 8);
                    }
                }
                break;
            }
            case Op::Text: rasterText(c, strip, y0, ya, yb, cx0, cx1); break;
            case Op::Curve: {
                const int16_t* ys = reinterpret_cast<const int16_t*>(payloadOf(c));
                const int t = c.a1, half = (t - 1) / 2, n = c.w;
                for (int i = 0; i < n; ++i) {
                    const int x = c.x + i;
                    if (x < cx0 || x >= cx1) continue;
                    if (c.a2 >= 0 && c.radius) {
                        for (int y = imax(ya, ys[i]); y < imin(yb, c.a2); ++y) blend(strip[(y - y0) * kWidth + x], c.c2, c.radius);
                    }
                    // a stroked polyline: the segments touching columns i-half .. i+half, each covering its two end values
                    int lo = ys[i], hi = ys[i];
                    for (int k = i - half; k <= i + half; ++k) {
                        if (k < 0 || k >= n) continue;
                        const int a = ys[k], b = k > 0 ? ys[k - 1] : ys[k], e = k + 1 < n ? ys[k + 1] : ys[k];
                        lo = imin(lo, imin(a, (a + b) / 2));
                        hi = imax(hi, imax(a, (a + b) / 2));
                        if (k == i) { lo = imin(lo, (a + e) / 2); hi = imax(hi, (a + e) / 2); }
                    }
                    for (int y = imax(ya, lo - half); y < imin(yb, hi + half + 1); ++y) strip[(y - y0) * kWidth + x] = c.c;
                }
                break;
            }
            case Op::Columns: {
                const int16_t* p = reinterpret_cast<const int16_t*>(payloadOf(c));
                for (int i = 0; i < c.w; ++i) {
                    const int x = c.x + i;
                    if (x < cx0 || x >= cx1) continue;
                    const uint16_t col = p[3 * i + 2] ? c.c2 : c.c;
                    for (int y = imax(ya, p[3 * i]); y < imin(yb, p[3 * i + 1]); ++y) strip[(y - y0) * kWidth + x] = col;
                }
                break;
            }
            case Op::Hatch: {
                // the distance along the 135deg gradient is (x + y) / sqrt 2: in 1/256 px, 181 per pixel; a band of 9 px every 18
                const int xa = imax(cx0, c.x), xb = imin(cx1, c.x + c.w);
                for (int y = ya; y < yb; ++y) {
                    uint16_t* row = strip + (y - y0) * kWidth;
                    int d = (((y - c.y) + (xa - c.x)) * 181) % (18 * 256);
                    for (int x = xa; x < xb; ++x) {
                        if (d >= 9 * 256) blend(row[x], c.c, c.alpha);
                        d += 181;
                        if (d >= 18 * 256) d -= 18 * 256;
                    }
                }
                break;
            }
            case Op::Clip: case Op::Nop: break;
        }
    }
}

}  // namespace gfx
