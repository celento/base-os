#ifndef CALENDAR_H
#define CALENDAR_H

#define CAL_W 620
#define CAL_H 474
#define CAL_MOD_CTRL 1
#define CAL_MOD_SHIFT 2
#define CAL_MOD_ALT 4
#define CAL_CHANGED 1
#define CAL_CLOSE 2

/* Opening never discards accepted appointments or the persisted entry draft. */
void cal_reset(void);
void cal_draw(int bx, int by, int bw, int bh);
int cal_click(int bx, int by, int bw, int mx, int my);
int cal_key(int sc, char ch, int modifiers);
int cal_new(void);

#endif
