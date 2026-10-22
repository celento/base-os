/* Clock and the games (Mines, 2048, Breakout, Wordle) with the real kit,
 * theme and software renderer in both styles: default windows fit an
 * 800x600 desktop, and clicks land on the rectangles the views draw. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kernel/layout.h"
static unsigned char presented[FB_CAPACITY];
#undef PRESENT_BASE
#define PRESENT_BASE ((uintptr_t)presented)
#define GFX_HOST_TEST
#include "../src/kernel/gfx.c"
#include "../src/ui/icons.c"
#include "../src/ui/theme.c"
#include "../src/ui/kit.c"
#define csin clock_sin
#define ccos clock_cos
#include "../src/apps/clock.c"
#undef csin
#undef ccos
#define rand_next mines_rand_next
#define rnd mines_rnd
#define state mines_state
#define started mines_started
#define count mines_count
#include "../src/apps/mines.c"
#undef rand_next
#undef rnd
#undef state
#undef started
#undef count
#define rand_next g2048_rand_next
#define rnd g2048_rnd
#define started g2048_started
#define score g2048_score
#include "../src/apps/game2048.c"
#undef rand_next
#undef rnd
#undef started
#undef score
#define rand_next bo_rand_next
#define rnd bo_rnd
#define started bo_started
#define score bo_score
#define state bo_state
#define left bo_left
#include "../src/apps/breakout.c"
#undef rand_next
#undef rnd
#undef started
#undef score
#undef state
#undef left
#define state wl_state
#include "../src/apps/wordle.c"
#undef state

static unsigned char back[800 * 600], linear[800 * 600 * 4];
void kmemset(void *d, int v, int n) { memset(d, v, (size_t)n); }
void kmemcpy(void *d, const void *s, int n) { memcpy(d, s, (size_t)n); }
void platform_log(const char *s) { (void)s; }
uint8_t rtc_test_read(uint8_t reg) {
    switch (reg) { case 0xb: return 6; case 4: return 10; case 2: return 8; case 0: return 30;
        case 7: return 22; case 8: return 10; case 9: return 26; case 6: return 5; default: return 0; }
}

/* The window chrome of the active style and the room the desktop leaves on
 * an 800x600 screen (Modern reserves 60px for its dock). */
static int title_h(void) { return ui_theme.border + ui_theme.title_h + ui_theme.menu_h + (ui_theme.menu_h ? 0 : 1); }
static int room_h(void) {
    return 600 - ui_theme.topbar_h - (ui_theme.style == UI_STYLE_MODERN ? 60 : ui_theme.taskbar_h) - 8;
}
static void fits(int client_w, int client_h) {
    if (title_h() + client_h + 1 > room_h()) fprintf(stderr, "style %d: client %dx%d does not fit %d\n", ui_theme.style, client_w, client_h, room_h());
    assert(client_w + 2 <= 800 - 24);
    assert(title_h() + client_h + 1 <= room_h());
}
static void press(const PaneRect *r, int *x, int *y) { *x = r->x + r->w / 2; *y = r->y + r->h / 2; }

int main(void) {
    gfx_init(back, linear, 800, 600, 32, 800 * 4);
    for (int style = UI_STYLE_CLASSIC; style < UI_STYLE_COUNT; ++style) {
        ui_theme_install(style, 0, 0);
        const int X = 20, Y = 30;
        int x, y;
        /* Clock */
        fits(CLOCK_W, CLOCK_H);
        clock_draw(X, Y, CLOCK_W, CLOCK_H);
        assert(get_pixel(X + 2, pane_status_y(Y, CLOCK_H) + 4) == ui_theme.statusbar);
        assert(iso_week(2026, 1, 1) == 1 && iso_week(2027, 1, 1) == 53 && iso_week(2026, 12, 31) == 53);
        assert(iso_week(2024, 12, 30) == 1 && day_of_year(2024, 12, 31) == 366);

        /* Mines: the board takes reveals and flags, New game resets. */
        int mh = mines_client_h();
        assert(mh <= MINES_H);
        fits(MINES_W, mh);
        mines_new(5);
        MinesLayout ml; mines_layout(X, Y, MINES_W, mh, &ml);
        assert(ml.gy >= pane_body_y(Y) && ml.gy + MINES_ROWS * MINES_CELL <= pane_status_y(Y, mh));
        int cell_x = ml.gx + 5, cell_y = ml.gy + 5;
        assert(mines_click(X, Y, MINES_W, mh, cell_x, cell_y, 1) && flag_[0][0] && flags == 1);
        assert(!mines_click(X, Y, MINES_W, mh, cell_x, cell_y, 0) && !open_[0][0]);
        mines_draw(X, Y, MINES_W, mh, 0);
        press(&ml.again, &x, &y);
        assert(mines_click(X, Y, MINES_W, mh, x, y, 0) && !flags && !mines_state);

        /* 2048: the card's button starts again once no move is left. */
        int gh = g2048_client_h();
        assert(gh <= G2048_H);
        fits(G2048_W, gh);
        g2048_new(3);
        G2048Layout gl; g2048_layout(X, Y, G2048_W, gh, &gl);
        assert(gl.board.y >= pane_body_y(Y) && gl.board.y + G2048_BOARD <= pane_status_y(Y, gh));
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) board[r][c] = 2 << ((r * 4 + c) % 8 + (r & 1));
        over = 1; g2048_draw(X, Y, G2048_W, gh, 0);
        press(&gl.card_button, &x, &y);
        assert(get_pixel(x - gl.card_button.w / 2 + 3, y) == TK_ACCENT || style == UI_STYLE_CLASSIC);
        assert(g2048_click(X, Y, G2048_W, gh, x, y) && !over && g2048_score == 0);
        assert(!g2048_click(X, Y, G2048_W, gh, gl.board.x + 4, gl.board.y + 4));

        /* Breakout: a click on the field launches, New game resets. */
        int bh = bo_client_h();
        assert(bh <= BO_CLIENT_H);
        fits(BO_CLIENT_W, bh);
        bo_new();
        BoLayout bl; bo_layout(X, Y, BO_CLIENT_W, bh, &bl);
        assert(bl.field.y >= pane_body_y(Y) && bl.field.y + BO_H <= pane_status_y(Y, bh));
        bo_mouse(X, Y, BO_CLIENT_W, bh, bl.field.x);
        assert(pad_x == 0);
        assert(bo_click(X, Y, BO_CLIENT_W, bh, bl.field.x + 100, bl.field.y + 100) && bo_state == 1);
        for (int i = 0; i < 40; i++) bo_tick();
        bo_draw(X, Y, BO_CLIENT_W, bh, 0);
        press(&bl.again, &x, &y);
        assert(bo_click(X, Y, BO_CLIENT_W, bh, x, y) && bo_state == 0 && bo_score == 0);

        /* Wordle: keys type, Enter checks, the card's button starts again. */
        int wh = wordle_client_h();
        assert(wh <= WORDLE_CLIENT_H);
        fits(WORDLE_W, wh);
        wordle_new_game(11);
        WordleLayout wl; wordle_layout(X, Y, WORDLE_W, wh, &wl);
        PaneRect k, enter, back_key;
        key_rect(&wl, 2, 0, &enter); key_rect(&wl, 2, 8, &back_key);
        key_rect(&wl, 2, 7, &k);
        assert(k.x + k.w <= back_key.x && enter.w == KEY_WIDE && back_key.y + back_key.h <= pane_status_y(Y, wh));
        for (int i = 0; i < 6; i++) {
            const char *guess = i < 5 ? "QQQQQ" : target;
            for (int c = 0; c < 5; c++) {
                for (int r = 0; r < 3; r++) for (int j = 0; j < row_len(r); j++)
                    if (kb_rows[r][j] == guess[c]) key_rect(&wl, r, j, &k);
                press(&k, &x, &y);
                assert(wordle_click(X, Y, WORDLE_W, wh, x, y, 1));
            }
            press(&enter, &x, &y);
            assert(wordle_click(X, Y, WORDLE_W, wh, x, y, 1));
        }
        assert(wordle_over() && wl_state == 1 && letter_mark('Q') == MK_MISS);
        wordle_draw(X, Y, WORDLE_W, wh, 0);
        press(&wl.card_button, &x, &y);
        assert(wordle_click(X, Y, WORDLE_W, wh, x, y, 2) && !wordle_over() && grid[0][0] == 0);
        printf("Games %s: layouts fit, clicks hit the drawn controls.\n", ui_style_name(style));
    }
    puts("All games UI checks passed.");
    return 0;
}
