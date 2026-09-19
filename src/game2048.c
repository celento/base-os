#include "app.h"
#include "game2048.h"

static int board[4][4];
static int score;
static int best;
static int over;
static unsigned rnd;
static int started;

static unsigned rand_next(void) {
    rnd = rnd * 1103515245u + 12345u;
    return (rnd >> 16) & 0x7FFF;
}

static void spawn(void) {
    int free_n = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (!board[r][c])
                free_n++;
    if (!free_n)
        return;
    int pick = (int)(rand_next() % (unsigned)free_n);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            if (board[r][c])
                continue;
            if (pick-- == 0) {
                board[r][c] = (rand_next() % 10 == 0) ? 4 : 2;
                return;
            }
        }
}

void g2048_new(unsigned seed) {
    rnd = seed | 1;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            board[r][c] = 0;
    score = 0;
    over = 0;
    started = 1;
    spawn();
    spawn();
}

static int slide_line(int *v) {
    int out[4] = { 0, 0, 0, 0 };
    int n = 0;
    int moved = 0;
    for (int i = 0; i < 4; i++)
        if (v[i])
            out[n++] = v[i];
    for (int i = 0; i + 1 < n; i++) {
        if (out[i] == out[i + 1]) {
            out[i] *= 2;
            score += out[i];
            for (int j = i + 1; j + 1 < 4; j++)
                out[j] = out[j + 1];
            out[3] = 0;
            n--;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (v[i] != out[i])
            moved = 1;
        v[i] = out[i];
    }
    return moved;
}

static int can_move(void) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            if (!board[r][c])
                return 1;
            if (c < 3 && board[r][c] == board[r][c + 1])
                return 1;
            if (r < 3 && board[r][c] == board[r + 1][c])
                return 1;
        }
    return 0;
}

int g2048_key(int sc) {
    if (!started)
        g2048_new(7);
    if (sc == KEY_SPACE || sc == KEY_ENTER) {
        if (over) {
            g2048_new(rnd);
            return 1;
        }
        return 0;
    }
    if (over)
        return 0;
    int moved = 0;
    int line[4];
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) {
            if (sc == KEY_LEFT) line[k] = board[i][k];
            else if (sc == KEY_RIGHT) line[k] = board[i][3 - k];
            else if (sc == KEY_UP) line[k] = board[k][i];
            else if (sc == KEY_DOWN) line[k] = board[3 - k][i];
            else return 0;
        }
        if (slide_line(line))
            moved = 1;
        for (int k = 0; k < 4; k++) {
            if (sc == KEY_LEFT) board[i][k] = line[k];
            else if (sc == KEY_RIGHT) board[i][3 - k] = line[k];
            else if (sc == KEY_UP) board[k][i] = line[k];
            else board[3 - k][i] = line[k];
        }
    }
    if (moved) {
        spawn();
        if (score > best)
            best = score;
        if (!can_move())
            over = 1;
    }
    return moved;
}

static void tile_style(int v, uint8_t *fill, uint8_t *ink) {
    switch (v) {
    case 2: *fill = gfx_rgb(0xEE, 0xE4, 0xDA); *ink = gfx_rgb(0x77, 0x6E, 0x65); break;
    case 4: *fill = gfx_rgb(0xED, 0xE0, 0xC8); *ink = gfx_rgb(0x77, 0x6E, 0x65); break;
    case 8: *fill = gfx_rgb(0xF2, 0xB1, 0x79); *ink = COLOR_WHITE; break;
    case 16: *fill = gfx_rgb(0xF5, 0x95, 0x63); *ink = COLOR_WHITE; break;
    case 32: *fill = gfx_rgb(0xF6, 0x7C, 0x5F); *ink = COLOR_WHITE; break;
    case 64: *fill = gfx_rgb(0xF6, 0x5E, 0x3B); *ink = COLOR_WHITE; break;
    case 128: *fill = gfx_rgb(0xED, 0xCF, 0x72); *ink = COLOR_WHITE; break;
    case 256: *fill = gfx_rgb(0xED, 0xCC, 0x61); *ink = COLOR_WHITE; break;
    case 512: *fill = gfx_rgb(0xED, 0xC8, 0x50); *ink = COLOR_WHITE; break;
    case 1024: *fill = gfx_rgb(0xED, 0xC5, 0x3F); *ink = COLOR_WHITE; break;
    default: *fill = gfx_rgb(0xED, 0xC2, 0x2E); *ink = COLOR_WHITE; break;
    }
}

void g2048_draw(int bx, int by) {
    if (!started)
        g2048_new(7);
    draw_rect(bx, by, G2048_W, G2048_H, gfx_rgb(0xFA, 0xF8, 0xEF));
    char buf[16];
    draw_logo_string("2048", bx + G2048_GAP, by + 6, gfx_rgb(0x77, 0x6E, 0x65));
    int pill_w = 92;
    int px = bx + G2048_W - G2048_GAP - pill_w;
    draw_round_rect(px, by + 8, pill_w, 40, 5, gfx_rgb(0xBB, 0xAD, 0xA0));
    draw_string("SCORE", px + (pill_w - ui_string_w("SCORE")) / 2, by + 8, gfx_rgb(0xEE, 0xE4, 0xDA));
    fmt_uint(buf, (unsigned)score);
    draw_string_bold(buf, px + (pill_w - uib_string_w(buf)) / 2, by + 8 + 18, COLOR_WHITE);
    px -= pill_w + 8;
    draw_round_rect(px, by + 8, pill_w, 40, 5, gfx_rgb(0xBB, 0xAD, 0xA0));
    draw_string("BEST", px + (pill_w - ui_string_w("BEST")) / 2, by + 8, gfx_rgb(0xEE, 0xE4, 0xDA));
    fmt_uint(buf, (unsigned)best);
    draw_string_bold(buf, px + (pill_w - uib_string_w(buf)) / 2, by + 8 + 18, COLOR_WHITE);

    int gx = bx;
    int gy = by + G2048_HEAD;
    draw_round_rect(gx, gy, G2048_W, G2048_H - G2048_HEAD, 8, gfx_rgb(0xBB, 0xAD, 0xA0));
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int x = gx + G2048_GAP + c * (G2048_CELL + G2048_GAP);
            int y = gy + G2048_GAP + r * (G2048_CELL + G2048_GAP);
            int v = board[r][c];
            if (!v) {
                draw_round_rect(x, y, G2048_CELL, G2048_CELL, 6, gfx_rgb(0xCD, 0xC1, 0xB4));
                continue;
            }
            uint8_t fill, ink;
            tile_style(v, &fill, &ink);
            draw_round_rect(x, y, G2048_CELL, G2048_CELL, 6, fill);
            fmt_uint(buf, (unsigned)v);
            if (v < 1000) {
                int tw = logo_string_w(buf);
                draw_logo_string(buf, x + (G2048_CELL - tw) / 2, y + (G2048_CELL - LOGO_FONT_H) / 2, ink);
            } else {
                int tw = uib_string_w(buf);
                draw_string_bold(buf, x + (G2048_CELL - tw) / 2, y + (G2048_CELL - UI_FONT_H) / 2, ink);
            }
        }
    }
    if (over) {
        shade_rect(gx, gy, G2048_W, G2048_H - G2048_HEAD, 1);
        int pw = 220, ph = 70;
        int ox = bx + (G2048_W - pw) / 2;
        int oy = gy + (G2048_H - G2048_HEAD - ph) / 2;
        draw_round_rect(ox, oy, pw, ph, 8, COLOR_WHITE);
        draw_string_bold("Game over", ox + (pw - uib_string_w("Game over")) / 2, oy + 12, gfx_rgb(0x77, 0x6E, 0x65));
        draw_string("Space for a new game", ox + (pw - ui_string_w("Space for a new game")) / 2, oy + 36, app_text_dim);
    }
}
