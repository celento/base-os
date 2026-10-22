#ifndef MINES_APP_H
#define MINES_APP_H
#include "apps/pane_chrome.h"

#define MINES_COLS 10
#define MINES_ROWS 10
#define MINES_CELL 30
#define MINES_PAD  16
/* Client width of the default window; the height depends on the style. */
#define MINES_W (MINES_COLS * MINES_CELL + MINES_PAD * 2)

void mines_new(unsigned seed);
int mines_client_h(void);
/* bx, by, bw, bh: the client rectangle. hot registers hover targets (0 for
 * none). */
void mines_draw(int bx, int by, int bw, int bh, PaneHot hot);
/* flag: mark instead of reveal. Returns nonzero when the view changed. */
int mines_click(int bx, int by, int bw, int bh, int mx, int my, int flag);

#endif
