#ifndef CALENDAR_H
#define CALENDAR_H

#define CAL_W 420
#define CAL_H 360

void cal_reset(void);
void cal_draw(int bx, int by, int bw, int bh);
int cal_click(int bx, int by, int bw, int mx, int my);
int cal_key(int sc);

#endif
