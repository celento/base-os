#include "app.h"
#include "mines.h"

#define NMINES 12

static uint8_t mine[MINES_ROWS][MINES_COLS];
static uint8_t open_[MINES_ROWS][MINES_COLS];
static uint8_t flag_[MINES_ROWS][MINES_COLS];
static int state;
static int opened;
static int flags;
static unsigned rnd;
static int started;

static unsigned rand_next(void) {
    rnd = rnd * 1103515245u + 12345u;
    return (rnd >> 16) & 0x7FFF;
}

void mines_new(unsigned seed) {
    rnd = seed | 1;
    for (int r = 0; r < MINES_ROWS; r++)
        for (int c = 0; c < MINES_COLS; c++)
            mine[r][c] = open_[r][c] = flag_[r][c] = 0;
    int placed = 0;
    while (placed < NMINES) {
        int r = (int)(rand_next() % MINES_ROWS);
        int c = (int)(rand_next() % MINES_COLS);
        if (!mine[r][c]) {
            mine[r][c] = 1;
            placed++;
        }
    }
    state = 0;
    opened = 0;
    flags = 0;
    started = 1;
}

static int count(int r, int c) {
    int n = 0;
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            int rr = r + dr, cc = c + dc;
            if (rr < 0 || cc < 0 || rr >= MINES_ROWS || cc >= MINES_COLS)
                continue;
            n += mine[rr][cc];
        }
    return n;
}

static void reveal(int r, int c) {
    if (r < 0 || c < 0 || r >= MINES_ROWS || c >= MINES_COLS)
        return;
    if (open_[r][c] || flag_[r][c])
        return;
    open_[r][c] = 1;
    opened++;
    if (count(r, c) == 0 && !mine[r][c]) {
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++)
                if (dr || dc)
                    reveal(r + dr, c + dc);
    }
}

static void grid_origin(int bx, int by, int *gx, int *gy) {
    *gx = bx + MINES_PAD;
    *gy = by + MINES_HEAD + MINES_PAD;
}

static void face_rect(int bx, int by, int *x, int *y) {
    *x = bx + MINES_W / 2 - 16;
    *y = by + 6;
}

void mines_draw(int bx, int by) {
    if (!started)
        mines_new(1);
    draw_rect(bx, by, MINES_W, MINES_H, app_chrome);
    static const uint8_t numrgb[9][3] = {
        {0, 0, 0}, {0x2F, 0x72, 0xD2}, {0x2E, 0x94, 0x56}, {0xD8, 0x3A, 0x3A},
        {0x24, 0x40, 0x7A}, {0x7A, 0x30, 0x48}, {0x21, 0x96, 0xA8}, {0x20, 0x20, 0x20}, {0x80, 0x80, 0x80}
    };

    char buf[8];
    int left = NMINES - flags;
    if (left < 0) left = 0;
    fmt_uint(buf, (unsigned)left);
    draw_round_rect(bx + MINES_PAD, by + 8, 56, 28, 5, gfx_gray(0x20));
    draw_string_bold(buf, bx + MINES_PAD + (56 - uib_string_w(buf)) / 2, by + 8 + (28 - UI_FONT_H) / 2,
                     gfx_rgb(0xF0, 0x50, 0x48));

    int fx, fy;
    face_rect(bx, by, &fx, &fy);
    uint8_t face = state == 1 ? gfx_rgb(0xE8, 0x5A, 0x50) : (state == 2 ? gfx_rgb(0x4C, 0xA8, 0x60) : gfx_rgb(0xF2, 0xC9, 0x4C));
    draw_round_rect(fx, fy, 32, 32, 16, gfx_darker(face, 70));
    draw_round_rect(fx + 1, fy + 1, 30, 30, 15, face);
    draw_rect(fx + 10, fy + 11, 3, 3, gfx_gray(0x20));
    draw_rect(fx + 19, fy + 11, 3, 3, gfx_gray(0x20));
    if (state == 1) {
        draw_hline(fx + 11, fy + 22, 10, gfx_gray(0x20));
        draw_hline(fx + 10, fy + 23, 12, gfx_gray(0x20));
    } else {
        draw_hline(fx + 10, fy + 20, 12, gfx_gray(0x20));
        draw_hline(fx + 11, fy + 21, 10, gfx_gray(0x20));
    }
    const char *status = state == 1 ? "Boom" : (state == 2 ? "Cleared" : "Minesweeper");
    draw_string(status, bx + MINES_W - MINES_PAD - ui_string_w(status), by + 8 + (28 - UI_FONT_H) / 2, app_text_dim);

    int gx, gy;
    grid_origin(bx, by, &gx, &gy);
    for (int r = 0; r < MINES_ROWS; r++) {
        for (int c = 0; c < MINES_COLS; c++) {
            int x = gx + c * MINES_CELL;
            int y = gy + r * MINES_CELL;
            int show = open_[r][c] || (state == 1 && mine[r][c]);
            if (show) {
                if (mine[r][c]) {
                    uint8_t mc = open_[r][c] ? gfx_rgb(0xE8, 0x40, 0x38) : gfx_gray(0x20);
                    draw_round_rect(x + 8, y + 8, MINES_CELL - 16, MINES_CELL - 16, (MINES_CELL - 16) / 2, mc);
                } else {
                    int n = count(r, c);
                    if (n) {
                        char s[2] = { (char)('0' + n), 0 };
                        draw_string_bold(s, x + (MINES_CELL - uib_string_w(s)) / 2, y + (MINES_CELL - UI_FONT_H) / 2,
                                         gfx_rgb(numrgb[n][0], numrgb[n][1], numrgb[n][2]));
                    }
                }
            } else {
                draw_round_rect(x + 1, y + 1, MINES_CELL - 2, MINES_CELL - 2, 4, app_chrome_dk);
                draw_round_rect(x + 2, y + 2, MINES_CELL - 4, MINES_CELL - 4, 3, app_chrome);
                if (flag_[r][c]) {
                    draw_rect(x + 13, y + 7, 2, 16, gfx_gray(0x30));
                    for (int i = 0; i < 7; i++)
                        draw_hline(x + 15, y + 7 + i, 9 - i, gfx_rgb(0xE0, 0x40, 0x40));
                }
            }
        }
    }
}

int mines_click(int bx, int by, int mx, int my, int flag) {
    int fx, fy;
    face_rect(bx, by, &fx, &fy);
    if (hit(mx, my, fx, fy, 32, 32)) {
        mines_new((unsigned)(mx * 131 + my * 7 + rnd));
        return 1;
    }
    if (state != 0)
        return 0;
    int gx, gy;
    grid_origin(bx, by, &gx, &gy);
    if (!hit(mx, my, gx, gy, MINES_COLS * MINES_CELL, MINES_ROWS * MINES_CELL))
        return 0;
    int c = (mx - gx) / MINES_CELL;
    int r = (my - gy) / MINES_CELL;
    if (open_[r][c])
        return 0;
    if (flag) {
        flag_[r][c] = !flag_[r][c];
        flags += flag_[r][c] ? 1 : -1;
        return 1;
    }
    if (flag_[r][c])
        return 0;
    if (mine[r][c]) {
        open_[r][c] = 1;
        state = 1;
        return 1;
    }
    reveal(r, c);
    if (opened == MINES_ROWS * MINES_COLS - NMINES)
        state = 2;
    return 1;
}
