#include "app.h"
#include "breakout.h"

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

void bo_click(void) {
    launch();
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

void bo_mouse(int bx, int mx) {
    if (!started)
        bo_new();
    int nx = mx - bx - PAD_W / 2;
    if (nx < 0) nx = 0;
    if (nx > BO_W - PAD_W) nx = BO_W - PAD_W;
    pad_x = nx;
    if (state == 0)
        reset_ball();
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

void bo_draw(int bx, int by) {
    if (!started)
        bo_new();
    draw_rect(bx, by, BO_W, BO_H, gfx_rgb(0x14, 0x18, 0x22));
    static const uint32_t rowc[BO_ROWS] = { 0xE0663A, 0xF2A33A, 0xF2C94C, 0x3FA35B, 0x2AA7C8, 0x9448BE };
    int gx = bx + (BO_W - BO_COLS * (BRICK_W + 4)) / 2;
    for (int r = 0; r < BO_ROWS; r++) {
        uint8_t c = gfx_rgb((rowc[r] >> 16) & 255, (rowc[r] >> 8) & 255, rowc[r] & 255);
        uint8_t hi = gfx_lighter(c, 30);
        for (int col = 0; col < BO_COLS; col++) {
            if (!bricks[r][col])
                continue;
            int x = gx + col * (BRICK_W + 4);
            int y = by + BRICK_TOP + r * (BRICK_H + 4);
            draw_round_rect(x, y, BRICK_W, BRICK_H, 3, c);
            draw_hline(x + 3, y + 1, BRICK_W - 6, hi);
        }
    }
    draw_round_rect(bx + pad_x, by + BO_H - 30, PAD_W, PAD_H, 5, app_accent);
    draw_hline(bx + pad_x + 5, by + BO_H - 29, PAD_W - 10, PAL_ACCENT + 2);
    draw_round_rect(bx + (ball_x >> 4), by + (ball_y >> 4), BALL, BALL, BALL / 2, COLOR_WHITE);

    char buf[16];
    draw_string("Score", bx + 12, by + 8, gfx_gray(0x90));
    fmt_uint(buf, (unsigned)score);
    draw_string_bold(buf, bx + 12 + ui_string_w("Score") + 6, by + 8, COLOR_WHITE);
    for (int i = 0; i < lives; i++)
        draw_round_rect(bx + BO_W - 16 - i * 14, by + 12, 8, 8, 4, gfx_rgb(0xF0, 0x50, 0x48));

    const char *msg = 0;
    if (state == 0) msg = "Space or click to launch";
    else if (state == 2) msg = "Game over. Space to retry";
    else if (state == 3) msg = "You cleared it. Space to play again";
    if (msg) {
        int tw = ui_string_w(msg);
        draw_round_rect(bx + (BO_W - tw) / 2 - 12, by + BO_H / 2 + 10, tw + 24, 30, 6, gfx_gray(0x30));
        draw_string(msg, bx + (BO_W - tw) / 2, by + BO_H / 2 + 10 + (30 - UI_FONT_H) / 2, COLOR_WHITE);
    }
}
