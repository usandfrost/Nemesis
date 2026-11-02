#ifndef __GFX_TRACKERLAYOUT_H
#define __GFX_TRACKERLAYOUT_H

// Where the tracker's cells are, and where a popover goes. Rows are 20 px on a 20 px grid because the board sends the screen in tiles 32 x 20: a row on the grid is one row of tiles, so the
// playhead moving costs two rows of them, not three.
namespace gfx {

constexpr int kTrackX = 44, kTrackColW = 90, kTrackY = 120, kTrackRowH = 20;
constexpr int kPopW = 292, kPopH = 184, kPopGap = 8;

struct Box { int x, y, w, h; };

inline Box trackerCell(int lane, int row) { return {kTrackX + lane * kTrackColW, kTrackY + row * kTrackRowH, kTrackColW, kTrackRowH}; }   // `row`: on the page, 0..15

// Beside the cell, on the side it fits: it never covers the cell being edited.
inline Box popoverBox(const Box& cell) {
    constexpr int top = 90, bottom = 450, right = 952;
    const int x = cell.x + cell.w + kPopGap + kPopW <= right ? cell.x + cell.w + kPopGap : cell.x - kPopGap - kPopW;
    int y = cell.y - 8;
    if (y + kPopH > bottom) y = bottom - kPopH;
    if (y < top) y = top;
    return {x, y, kPopW, kPopH};
}

}  // namespace gfx

#endif
