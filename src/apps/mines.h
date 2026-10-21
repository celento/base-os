#ifndef MINES_APP_H
#define MINES_APP_H

#define MINES_COLS 10
#define MINES_ROWS 10
#define MINES_CELL 30
#define MINES_HEAD 44
#define MINES_PAD  12
#define MINES_W (MINES_COLS * MINES_CELL + MINES_PAD * 2)
#define MINES_H (MINES_HEAD + MINES_ROWS * MINES_CELL + MINES_PAD * 2)

void mines_new(unsigned seed);
void mines_draw(int bx, int by);
int mines_click(int bx, int by, int mx, int my, int flag);

#endif
