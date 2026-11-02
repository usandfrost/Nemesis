#ifndef __GFX_SCREEN_H
#define __GFX_SCREEN_H

#include "app/Ui.h"
#include "gfx/Canvas.h"

// Draws the panel's screen from the UI state: the mockup's render(), cellsHtml(), mainHtml() and drawViz(), with the mockup's geometry
// (measured in Chrome from docs/mockup/drumboy-panel.html), colours and fonts. Records one frame into the canvas; main loop only.
namespace gfx {

void drawScreen(Canvas& cv, app::Ui& ui);

}  // namespace gfx

#endif
