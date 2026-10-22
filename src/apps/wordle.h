#ifndef WORDLE_H_INCLUDED
#define WORDLE_H_INCLUDED
#include "apps/pane_chrome.h"

/* Client size of the default window. wordle_client_h is the height in the
 * active style; WORDLE_CLIENT_H is the height in the taller (Modern) style. */
#define WORDLE_W 440
#define WORDLE_BODY_H 383
#define WORDLE_CLIENT_H (43 + WORDLE_BODY_H + 28)

/* wordle_key actions that are not plain letters. */
#define WORDLE_ACT_NONE   0
#define WORDLE_ACT_ENTER  1
#define WORDLE_ACT_DELETE 2

void wordle_new_game(unsigned int seed);
int wordle_client_h(void);

/* bx, by, bw, bh: the client rectangle. hot registers hover targets (0 for
 * none). */
void wordle_draw(int bx, int by, int bw, int bh, PaneHot hot);
/* seed starts a new game from the New game buttons. Returns nonzero when
 * the view changed. */
int wordle_click(int bx, int by, int bw, int bh, int mx, int my, unsigned int seed);
int wordle_key(int action, char ch);

#endif
