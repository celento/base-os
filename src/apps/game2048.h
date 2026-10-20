#ifndef GAME2048_H
#define GAME2048_H

#define G2048_CELL 78
#define G2048_GAP  10
#define G2048_HEAD 56
#define G2048_W (4 * G2048_CELL + 5 * G2048_GAP)
#define G2048_H (G2048_HEAD + 4 * G2048_CELL + 5 * G2048_GAP)

void g2048_new(unsigned seed);
void g2048_draw(int bx, int by);
int g2048_key(int sc);

#endif
