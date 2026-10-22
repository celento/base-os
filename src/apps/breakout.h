#ifndef BREAKOUT_H
#define BREAKOUT_H
#include "apps/pane_chrome.h"

/* Playfield size; the ball and paddle move in these coordinates. */
#define BO_W 480
#define BO_H 376
#define BO_MARGIN_X 12
#define BO_MARGIN_Y 8
/* Client size of the default window. bo_client_h is the height in the
 * active style; BO_CLIENT_H is the height in the taller (Modern) style. */
#define BO_CLIENT_W (BO_W + 2 * BO_MARGIN_X)
#define BO_CLIENT_H (43 + 2 * BO_MARGIN_Y + BO_H + 28)

void bo_new(void);
void bo_tick(void);
int bo_client_h(void);
/* bx, by, bw, bh: the client rectangle. hot registers hover targets (0 for
 * none). */
void bo_draw(int bx, int by, int bw, int bh, PaneHot hot);
int bo_key(int sc);
void bo_mouse(int bx, int by, int bw, int bh, int mx);
/* New game button, the result card's button, or a launch click on the
 * field. Returns nonzero when the view changed. */
int bo_click(int bx, int by, int bw, int bh, int mx, int my);

#endif
