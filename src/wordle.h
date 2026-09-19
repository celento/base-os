#ifndef WORDLE_H_INCLUDED
#define WORDLE_H_INCLUDED

#define WORDLE_W 440
#define WORDLE_WIN_H 520

/* wordle_key actions that are not plain letters. */
#define WORDLE_ACT_NONE   0
#define WORDLE_ACT_ENTER  1
#define WORDLE_ACT_DELETE 2

void wordle_new_game(unsigned int seed);

/* bx, by = window body origin (wx, wy + TITLE_H). */
void wordle_draw(int bx, int by);
int wordle_click(int bx, int by, int mx, int my);
int wordle_key(int action, char ch);

#endif
