#include "ui/app.h"
#include "apps/game2048.h"
#include "apps/pane_chrome.h"

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

/* Nightfall: tiles walk the hue wheel from cool indigo to hot amber and a
 * bright cyan glow, using exact colour-cube entries so every theme renders
 * them identically. Bright tiles take a deep ink; deep tiles take white. */
static void tile_style(int v, uint8_t *fill, uint8_t *ink) {
    uint8_t deep = gfx_gray(0x08);
    switch (v) {
    case 2: *fill = gfx_rgb(0x40, 0x40, 0x80); *ink = COLOR_WHITE; break;
    case 4: *fill = gfx_rgb(0x40, 0x40, 0xC0); *ink = COLOR_WHITE; break;
    case 8: *fill = gfx_rgb(0x80, 0x40, 0xC0); *ink = COLOR_WHITE; break;
    case 16: *fill = gfx_rgb(0xC0, 0x40, 0xC0); *ink = COLOR_WHITE; break;
    case 32: *fill = gfx_rgb(0xC0, 0x40, 0x80); *ink = COLOR_WHITE; break;
    case 64: *fill = gfx_rgb(0xFF, 0x40, 0x40); *ink = COLOR_WHITE; break;
    case 128: *fill = gfx_rgb(0xFF, 0x80, 0x00); *ink = deep; break;
    case 256: *fill = gfx_rgb(0xFF, 0xC0, 0x00); *ink = deep; break;
    case 512: *fill = gfx_rgb(0xFF, 0xFF, 0x40); *ink = deep; break;
    case 1024: *fill = gfx_rgb(0xC0, 0xFF, 0x40); *ink = deep; break;
    default: *fill = gfx_rgb(0x40, 0xFF, 0xFF); *ink = deep; break;
    }
}

/* Layout: a toolbar with the score, best and New game, the board, and a
 * status bar. Drawing and clicks share g2048_layout. */
typedef struct { PaneRect again, board, card, card_button; } G2048Layout;

static void g2048_layout(int bx, int by, int bw, int bh, G2048Layout *L) {
    int body = pane_body_y(by), bottom = pane_status_y(by, bh);
    int w = pane_tool_w("New game");
    L->again = (PaneRect){bx + bw - pane_pad() - w, pane_ctl_y(by), w, ui_theme.control_h};
    L->board = (PaneRect){bx + (bw - G2048_BOARD) / 2, body + (bottom - body - G2048_BOARD) / 2,
                          G2048_BOARD, G2048_BOARD};
    pane_card_layout(L->board.x, L->board.y, L->board.w, L->board.h, &L->card, &L->card_button);
}

int g2048_client_h(void) {
    return pane_body_y(0) + 2 * G2048_PAD + G2048_BOARD + pane_status_h();
}

int g2048_click(int bx, int by, int bw, int bh, int mx, int my) {
    G2048Layout L;
    g2048_layout(bx, by, bw, bh, &L);
    if (pane_hit(mx, my, &L.again) || (over && pane_hit(mx, my, &L.card_button))) {
        g2048_new(rnd + (unsigned)(mx * 131 + my));
        return 1;
    }
    return 0;
}

void g2048_draw(int bx, int by, int bw, int bh, PaneHot hot) {
    if (!started)
        g2048_new(7);
    int modern = PANE_MODERN;
    draw_rect(bx, by, bw, bh, ui_theme.content);
    G2048Layout L;
    g2048_layout(bx, by, bw, bh, &L);
    char buf[16], text[40];
    /* Toolbar */
    pane_toolbar(bx, by, bw);
    int x = bx + pane_pad() + 4;
    pane_num(buf, (unsigned)score);
    x += pane_inline_stat(x, L.again.y, L.again.h, "Score", buf) + 20;
    pane_num(buf, (unsigned)best);
    pane_inline_stat(x, L.again.y, L.again.h, "Best", buf);
    pane_tool(&L.again, ICON_REFRESH, "New game", over ? KB_PRIMARY : KB_DEFAULT,
              pane_hot(hot, L.again.x, L.again.y, L.again.w, L.again.h));

    /* Board */
    int gx = L.board.x, gy = L.board.y, top = 0;
    kit_box(gx, gy, G2048_BOARD, G2048_BOARD, modern ? 10 : KIT_R_SQUARE, TK_SUNKEN,
            modern ? 0 : TK_LINE);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int x = gx + G2048_GAP + c * (G2048_CELL + G2048_GAP);
            int y = gy + G2048_GAP + r * (G2048_CELL + G2048_GAP);
            int v = board[r][c];
            if (v > top) top = v;
            if (!v) {
                kit_fill(x, y, G2048_CELL, G2048_CELL, 6, TK_HOVER, 1);
                continue;
            }
            uint8_t fill, ink;
            tile_style(v, &fill, &ink);
            kit_fill(x, y, G2048_CELL, G2048_CELL, 6, fill, 1);
            fmt_uint(buf, (unsigned)v);
            if (v < 1000) {
                int tw = logo_string_w(buf);
                draw_logo_string(buf, x + (G2048_CELL - tw) / 2, y + (G2048_CELL - LOGO_FONT_H) / 2, ink);
            } else {
                kit_text_center(buf, x, y + (G2048_CELL - UI_FONT_H) / 2, G2048_CELL, ink, 1);
            }
        }
    }
    if (over) {
        shade_rect(gx, gy, G2048_BOARD, G2048_BOARD, 1);
        pane_num(buf, (unsigned)score);
        pane_join(text, sizeof text, "No moves left. Score ", buf, ".");
        pane_card(&L.card, &L.card_button, "Game over", text, "New game",
                  pane_hot(hot, L.card_button.x, L.card_button.y, L.card_button.w, L.card_button.h));
    }
    /* Status bar */
    pane_num(buf, (unsigned)top);
    pane_join(text, sizeof text, "Highest tile ", buf, 0);
    pane_statusbar(bx, by, bw, bh, over ? "Space or Enter starts a new game" : "Arrow keys slide the tiles",
                   TK_TEXT2, text, TK_TEXT2);
}
