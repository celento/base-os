#include "ui/app.h"
#include "apps/mines.h"
#include "apps/pane_chrome.h"

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

/* Layout: a toolbar with the mine counter and New game, the board, and a
 * status bar. Drawing and clicks share mines_layout. */
typedef struct { PaneRect again; int gx, gy; } MinesLayout;

static void mines_layout(int bx, int by, int bw, int bh, MinesLayout *L) {
    int body = pane_body_y(by), bottom = pane_status_y(by, bh);
    int w = pane_tool_w("New game");
    L->again = (PaneRect){bx + bw - pane_pad() - w, pane_ctl_y(by), w, ui_theme.control_h};
    L->gx = bx + (bw - MINES_COLS * MINES_CELL) / 2;
    L->gy = body + (bottom - body - MINES_ROWS * MINES_CELL) / 2;
}

int mines_client_h(void) {
    return pane_body_y(0) + MINES_PAD * 2 + MINES_ROWS * MINES_CELL + pane_status_h();
}

void mines_draw(int bx, int by, int bw, int bh, PaneHot hot) {
    if (!started)
        mines_new(1);
    int modern = PANE_MODERN;
    draw_rect(bx, by, bw, bh, ui_theme.content);
    MinesLayout L;
    mines_layout(bx, by, bw, bh, &L);
    /* Toolbar */
    pane_toolbar(bx, by, bw);
    char buf[12], text[40];
    int left = NMINES - flags;
    if (left < 0) left = 0;
    pane_num(buf, (unsigned)left);
    int x = bx + pane_pad() + 4;
    x += pane_inline_stat(x, L.again.y, L.again.h, "Mines left", buf) + 20;
    pane_num(buf, (unsigned)flags);
    pane_inline_stat(x, L.again.y, L.again.h, "Flags", buf);
    pane_tool(&L.again, ICON_REFRESH, "New game", state ? KB_PRIMARY : KB_DEFAULT,
              pane_hot(hot, L.again.x, L.again.y, L.again.w, L.again.h));

    /* Board: playfield art on kit surfaces. Closed cells are raised tiles,
     * open cells sink into the board. */
    static const uint8_t numrgb[9][3] = {
        {0, 0, 0}, {0x40, 0x80, 0xFF}, {0x40, 0xA0, 0x40}, {0xFF, 0x40, 0x40},
        {0x80, 0x80, 0xFF}, {0xFF, 0x80, 0x00}, {0x00, 0xA0, 0xA0}, {0xC0, 0x40, 0xC0}, {0x80, 0x80, 0x80}
    };
    int gx = L.gx, gy = L.gy, size = MINES_COLS * MINES_CELL, r = modern ? 4 : KIT_R_SQUARE;
    kit_box(gx - 1, gy - 1, size + 2, MINES_ROWS * MINES_CELL + 2, modern ? 6 : KIT_R_SQUARE,
            TK_RAISED, modern ? TK_LINE : TK_LINE_STRONG);
    for (int row = 0; row < MINES_ROWS; row++) {
        for (int c = 0; c < MINES_COLS; c++) {
            int cx = gx + c * MINES_CELL;
            int cy = gy + row * MINES_CELL;
            int show = open_[row][c] || (state == 1 && mine[row][c]);
            if (show) {
                if (c) draw_vline(cx, cy, MINES_CELL, TK_LINE_SUBTLE);
                if (row) draw_hline(cx, cy, MINES_CELL, TK_LINE_SUBTLE);
                if (mine[row][c]) {
                    if (open_[row][c]) kit_fill(cx + 1, cy + 1, MINES_CELL - 2, MINES_CELL - 2, r, TK_RED_50, 1);
                    uint8_t mc = open_[row][c] ? gfx_rgb(0xE8, 0x40, 0x38) : TK_TEXT2;
                    draw_round_rect(cx + 9, cy + 9, MINES_CELL - 18, MINES_CELL - 18, (MINES_CELL - 18) / 2, mc);
                    draw_hline(cx + 7, cy + MINES_CELL / 2, MINES_CELL - 14, mc);
                    draw_vline(cx + MINES_CELL / 2, cy + 7, MINES_CELL - 14, mc);
                } else {
                    int n = count(row, c);
                    if (n) {
                        char s[2] = { (char)('0' + n), 0 };
                        kit_text_center(s, cx, cy + (MINES_CELL - UI_FONT_H) / 2, MINES_CELL,
                                        gfx_rgb(numrgb[n][0], numrgb[n][1], numrgb[n][2]), 1);
                    }
                }
            } else {
                if (modern) kit_fill(cx + 1, cy + 1, MINES_CELL - 2, MINES_CELL - 2, r, TK_HOVER, 1);
                else {
                    /* Classic bevel: light top and left, dark bottom and right. */
                    draw_rect(cx, cy, MINES_CELL, MINES_CELL, TK_BASE);
                    draw_hline(cx, cy, MINES_CELL - 1, TK_RAISED);
                    draw_vline(cx, cy, MINES_CELL - 1, TK_RAISED);
                    draw_hline(cx, cy + MINES_CELL - 1, MINES_CELL, TK_LINE_STRONG);
                    draw_vline(cx + MINES_CELL - 1, cy, MINES_CELL, TK_LINE_STRONG);
                }
                if (flag_[row][c]) {
                    draw_rect(cx + 13, cy + 7, 2, 16, TK_TEXT);
                    for (int i = 0; i < 7; i++)
                        draw_hline(cx + 15, cy + 7 + i, 9 - i, gfx_rgb(0xE0, 0x40, 0x40));
                    draw_hline(cx + 10, cy + 22, 8, TK_TEXT);
                }
            }
        }
    }
    /* Status bar */
    const char *right = text;
    uint8_t ink = TK_TEXT2;
    if (state == 1) { right = "You hit a mine"; ink = TK_RED_700; }
    else if (state == 2) { right = "Board cleared"; ink = TK_GREEN_700; }
    else {
        pane_num(buf, (unsigned)opened);
        pane_join(text, sizeof text, buf, " of ", "");
        pane_num(buf, (unsigned)(MINES_ROWS * MINES_COLS - NMINES));
        pane_append(text, sizeof text, buf);
        pane_append(text, sizeof text, " cells open");
    }
    pane_statusbar(bx, by, bw, bh, state ? "Space or New game starts again" : "Click reveals, Shift+click flags",
                   TK_TEXT2, right, ink);
}

int mines_click(int bx, int by, int bw, int bh, int mx, int my, int flag) {
    MinesLayout L;
    mines_layout(bx, by, bw, bh, &L);
    if (pane_hit(mx, my, &L.again)) {
        mines_new((unsigned)(mx * 131 + my * 7 + rnd));
        return 1;
    }
    if (state != 0)
        return 0;
    int gx = L.gx, gy = L.gy;
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
