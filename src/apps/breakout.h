#ifndef BREAKOUT_H
#define BREAKOUT_H

#define BO_W 480
#define BO_H 400

void bo_new(void);
void bo_tick(void);
void bo_draw(int bx, int by);
int bo_key(int sc);
void bo_mouse(int bx, int mx);
void bo_click(void);

#endif
