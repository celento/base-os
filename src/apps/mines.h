#ifndef MINES_APP_H
#define MINES_APP_H
#include "apps/pane_chrome.h"

#define MINES_COLS 10
#define MINES_ROWS 10
#define MINES_CELL 30
#define MINES_PAD  16
/* Client size of the default window. mines_client_h is the height in the
 * active style; MINES_H is the height in the taller (Modern) style. */
#define MINES_W (MINES_COLS * MINES_CELL + MINES_PAD * 2)
#define MINES_H (43 + MINES_PAD * 2 + MINES_ROWS * MINES_CELL + 28)

void mines_new(unsigned seed);
int mines_client_h(void);
/* bx, by, bw, bh: the client rectangle. hot registers hover targets (0 for
 * none). */
void mines_draw(int bx, int by, int bw, int bh, PaneHot hot);
/* flag: mark instead of reveal. Returns nonzero when the view changed. */
int mines_click(int bx, int by, int bw, int bh, int mx, int my, int flag);

#endif
