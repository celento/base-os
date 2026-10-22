#ifndef CALENDAR_H
#define CALENDAR_H
#include "apps/pane_chrome.h"

/* Client area of the default window, inside the frame and below the title
 * (and the Classic menu row). It fits an 800x600 desktop in both styles. */
#define CAL_W 620
#define CAL_H 460
#define CAL_MOD_CTRL 1
#define CAL_MOD_SHIFT 2
#define CAL_MOD_ALT 4
#define CAL_CHANGED 1
#define CAL_CLOSE 2

/* Opening never discards accepted appointments or the persisted entry draft. */
void cal_reset(void);
/* bx, by, bw, bh: the client rectangle. hot registers hover targets (0 for
 * none). */
void cal_draw(int bx, int by, int bw, int bh, PaneHot hot);
int cal_click(int bx, int by, int bw, int bh, int mx, int my);
int cal_key(int sc, char ch, int modifiers);
int cal_new(void);

#endif
