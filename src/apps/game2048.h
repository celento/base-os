#ifndef GAME2048_H
#define GAME2048_H
#include "apps/pane_chrome.h"

#define G2048_CELL 78
#define G2048_GAP  10
#define G2048_PAD  16
#define G2048_BOARD (4 * G2048_CELL + 5 * G2048_GAP)
/* Client size of the default window. g2048_client_h is the height in the
 * active style; G2048_H is the height in the taller (Modern) style. */
#define G2048_W (G2048_BOARD + 2 * G2048_PAD)
#define G2048_H (43 + 2 * G2048_PAD + G2048_BOARD + 28)

void g2048_new(unsigned seed);
int g2048_client_h(void);
/* bx, by, bw, bh: the client rectangle. hot registers hover targets (0 for
 * none). */
void g2048_draw(int bx, int by, int bw, int bh, PaneHot hot);
int g2048_click(int bx, int by, int bw, int bh, int mx, int my);
int g2048_key(int sc);

#endif
