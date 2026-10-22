#include "ui/app.h"
#include "apps/breakout.h"
#include "apps/pane_chrome.h"

#define BO_COLS 10
#define BO_ROWS 6
#define BRICK_W 44
#define BRICK_H 16
#define BRICK_TOP 36
#define PAD_W 72
#define PAD_H 10
#define BALL 8

static int bricks[BO_ROWS][BO_COLS];
static int pad_x;
static int ball_x, ball_y;
static int vel_x, vel_y;
static int state;
static int lives;
static int score;
static int left;
static int started;
static unsigned rnd;

static unsigned rand_next(void) {
    rnd = rnd * 1103515245u + 12345u;
    return (rnd >> 16) & 0x7FFF;
}

static void reset_ball(void) {
    ball_x = (pad_x + PAD_W / 2 - BALL / 2) << 4;
    ball_y = (BO_H - 40 - BALL) << 4;
    vel_x = ((int)(rand_next() % 2) ? 1 : -1) * 40;
    vel_y = -48;
}

void bo_new(void) {
    for (int r = 0; r < BO_ROWS; r++)
        for (int c = 0; c < BO_COLS; c++)
            bricks[r][c] = 1;
    left = BO_ROWS * BO_COLS;
    pad_x = (BO_W - PAD_W) / 2;
    lives = 3;
    score = 0;
    state = 0;
    started = 1;
    reset_ball();
}

static void launch(void) {
    if (state == 0)
        state = 1;
    else if (state == 2 || state == 3)
        bo_new();
}

int bo_key(int sc) {
    if (!started)
        bo_new();
    if (sc == KEY_SPACE || sc == KEY_ENTER) {
        launch();
        return 1;
    }
    if (sc == KEY_LEFT) {
        pad_x -= 24;
        if (pad_x < 0) pad_x = 0;
        if (state == 0) reset_ball();
        return 1;
    }
    if (sc == KEY_RIGHT) {
        pad_x += 24;
        if (pad_x > BO_W - PAD_W) pad_x = BO_W - PAD_W;
        if (state == 0) reset_ball();
        return 1;
    }
    return 0;
}

void bo_tick(void) {
    if (state != 1)
        return;
    int nx = ball_x + vel_x;
    int ny = ball_y + vel_y;
    int px = nx >> 4, py = ny >> 4;
    if (px < 0) { px = 0; vel_x = -vel_x; nx = 0; }
    if (px > BO_W - BALL) { px = BO_W - BALL; vel_x = -vel_x; nx = px << 4; }
    if (py < 0) { py = 0; vel_y = -vel_y; ny = 0; }

    int pad_y = BO_H - 30;
    if (vel_y > 0 && py + BALL >= pad_y && py + BALL <= pad_y + PAD_H + 6 &&
        px + BALL >= pad_x && px <= pad_x + PAD_W) {
        vel_y = -vel_y;
        int hitpos = (px + BALL / 2) - (pad_x + PAD_W / 2);
        vel_x = hitpos * 2;
        if (vel_x > 70) vel_x = 70;
        if (vel_x < -70) vel_x = -70;
        if (vel_x > -12 && vel_x < 12) vel_x = vel_x < 0 ? -12 : 12;
        ny = (pad_y - BALL) << 4;
    }

    int cx = (px + BALL / 2);
    int cy = (py + BALL / 2);
    int gx = (BO_W - BO_COLS * (BRICK_W + 4)) / 2;
    int col = (cx - gx) / (BRICK_W + 4);
    int row = (cy - BRICK_TOP) / (BRICK_H + 4);
    if (row >= 0 && row < BO_ROWS && col >= 0 && col < BO_COLS && bricks[row][col]) {
        int bx0 = gx + col * (BRICK_W + 4);
        int by0 = BRICK_TOP + row * (BRICK_H + 4);
        if (cx >= bx0 && cx < bx0 + BRICK_W && cy >= by0 && cy < by0 + BRICK_H) {
            bricks[row][col] = 0;
            left--;
            score += 10 * (BO_ROWS - row);
            int dxl = cx - bx0, dxr = bx0 + BRICK_W - cx;
            int dyt = cy - by0, dyb = by0 + BRICK_H - cy;
            int mx = dxl < dxr ? dxl : dxr;
            int my = dyt < dyb ? dyt : dyb;
            if (mx < my) vel_x = -vel_x; else vel_y = -vel_y;
            if (left == 0)
                state = 3;
        }
    }

    if (py > BO_H) {
        lives--;
        if (lives <= 0) {
            state = 2;
        } else {
            state = 0;
            reset_ball();
        }
        return;
    }
    ball_x = nx;
    ball_y = ny;
}

/* Layout: a toolbar with the score, lives, bricks left and New game, the
 * playfield, and a status bar. Drawing and input share bo_layout. */
typedef struct { PaneRect again, field, card, card_button; } BoLayout;

static void bo_layout(int bx, int by, int bw, int bh, BoLayout *L) {
    int body = pane_body_y(by), bottom = pane_status_y(by, bh);
    int w = pane_tool_w("New game");
    L->again = (PaneRect){bx + bw - pane_pad() - w, pane_ctl_y(by), w, ui_theme.control_h};
    L->field = (PaneRect){bx + (bw - BO_W) / 2, body + (bottom - body - BO_H) / 2, BO_W, BO_H};
    pane_card_layout(L->field.x, L->field.y, BO_W, BO_H, &L->card, &L->card_button);
}

int bo_client_h(void) {
    return pane_body_y(0) + 2 * BO_MARGIN_Y + BO_H + pane_status_h();
}

void bo_mouse(int bx, int by, int bw, int bh, int mx) {
    if (!started)
        bo_new();
    BoLayout L;
    bo_layout(bx, by, bw, bh, &L);
    int nx = mx - L.field.x - PAD_W / 2;
    if (nx < 0) nx = 0;
    if (nx > BO_W - PAD_W) nx = BO_W - PAD_W;
    pad_x = nx;
    if (state == 0)
        reset_ball();
}

int bo_click(int bx, int by, int bw, int bh, int mx, int my) {
    if (!started)
        bo_new();
    BoLayout L;
    bo_layout(bx, by, bw, bh, &L);
    if (pane_hit(mx, my, &L.again)) {
        bo_new();
        return 1;
    }
    if ((state == 2 || state == 3) && !pane_hit(mx, my, &L.card_button))
        return 0;
    if (pane_hit(mx, my, &L.field)) {
        launch();
        return 1;
    }
    return 0;
}

void bo_draw(int bx, int by, int bw, int bh, PaneHot hot) {
    if (!started)
        bo_new();
    int modern = PANE_MODERN;
    draw_rect(bx, by, bw, bh, ui_theme.content);
    BoLayout L;
    bo_layout(bx, by, bw, bh, &L);
    char buf[16], text[48];
    /* Toolbar: score, lives as balls, bricks left, New game. */
    pane_toolbar(bx, by, bw);
    int x = bx + pane_pad() + 4, cy = L.again.y, ch = L.again.h;
    pane_num(buf, (unsigned)score);
    x += pane_inline_stat(x, cy, ch, "Score", buf) + 20;
    kit_text("Lives", x, kit_ty(cy, ch), TK_TEXT2, 60, 0);
    x += kit_text_w("Lives", 0) + 8;
    for (int i = 0; i < 3; i++) {
        kit_fill(x, cy + ch / 2 - 4, 9, 9, 4, i < lives ? gfx_rgb(0xF0, 0x50, 0x48) : TK_LINE, 1);
        x += 14;
    }
    x += 14;
    pane_num(buf, (unsigned)left);
    pane_inline_stat(x, cy, ch, "Bricks", buf);
    pane_tool(&L.again, ICON_REFRESH, "New game", state >= 2 ? KB_PRIMARY : KB_DEFAULT,
              pane_hot(hot, L.again.x, L.again.y, L.again.w, L.again.h));

    /* Playfield art: dark field, bright bricks, accent paddle. */
    int fx = L.field.x, fy = L.field.y;
    kit_fill(fx - 1, fy - 1, BO_W + 2, BO_H + 2, modern ? 8 : KIT_R_SQUARE, TK_LINE, 1);
    kit_fill(fx, fy, BO_W, BO_H, modern ? 7 : KIT_R_SQUARE, gfx_rgb(0x16, 0x1A, 0x24), 1);
    static const uint32_t rowc[BO_ROWS] = { 0xFF4040, 0xFF8000, 0xFFC000, 0x40C040, 0x00C0FF, 0xC040FF };
    int gx = fx + (BO_W - BO_COLS * (BRICK_W + 4)) / 2;
    for (int r = 0; r < BO_ROWS; r++) {
        uint8_t c = gfx_rgb((rowc[r] >> 16) & 255, (rowc[r] >> 8) & 255, rowc[r] & 255);
        uint8_t hi = gfx_lighter(c, 30);
        for (int col = 0; col < BO_COLS; col++) {
            if (!bricks[r][col])
                continue;
            int bx0 = gx + col * (BRICK_W + 4);
            int by0 = fy + BRICK_TOP + r * (BRICK_H + 4);
            draw_round_rect(bx0, by0, BRICK_W, BRICK_H, 3, c);
            draw_hline(bx0 + 3, by0 + 1, BRICK_W - 6, hi);
        }
    }
    draw_round_rect(fx + pad_x, fy + BO_H - 30, PAD_W, PAD_H, 5, TK_ACCENT);
    draw_hline(fx + pad_x + 5, fy + BO_H - 29, PAD_W - 10, TK_ACCENT_300);
    if ((ball_y >> 4) + BALL <= BO_H)   /* a lost ball leaves the field */
        draw_round_rect(fx + (ball_x >> 4), fy + (ball_y >> 4), BALL, BALL, BALL / 2, COLOR_WHITE);

    if (state == 0) {
        /* Launch hint: a small overlay pill above the paddle. */
        const char *msg = "Space or click to launch";
        int tw = kit_text_w(msg, 0) + 28, th = ui_theme.control_h;
        int px = fx + (BO_W - tw) / 2, py = fy + BO_H / 2 + 20;
        kit_box(px, py, tw, th, modern ? th / 2 : KIT_R_SQUARE, ui_theme.overlay, modern ? 0 : TK_LINE_STRONG);
        kit_text_center(msg, px, kit_ty(py, th), tw, TK_TEXT, 0);
    } else if (state >= 2) {
        shade_rect(fx, fy, BO_W, BO_H, 1);
        pane_num(buf, (unsigned)score);
        pane_join(text, sizeof text, state == 3 ? "Every brick cleared. Score " : "Out of balls. Score ", buf, ".");
        pane_card(&L.card, &L.card_button, state == 3 ? "You cleared it" : "Game over", text, "Play again",
                  pane_hot(hot, L.card_button.x, L.card_button.y, L.card_button.w, L.card_button.h));
    }
    pane_statusbar(bx, by, bw, bh, "Arrow keys move the paddle; Space or click launches", TK_TEXT2,
                   state == 1 ? "Playing" : state == 0 ? "Ready" : state == 3 ? "Cleared" : "Game over",
                   state == 2 ? TK_RED_700 : state == 3 ? TK_GREEN_700 : TK_TEXT2);
}
