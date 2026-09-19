#include "wordle.h"
#include "font.h"

#include "app.h"

#define WL_BLACK  0
#define WL_GRAY   2
#define WL_YELLOW 6
#define WL_GREEN  7
#define WL_WHITE  15

#define WL_ROWS 6
#define WL_COLS 5

#define CELL 44
#define CGAP 6
#define GRID_W (WL_COLS * CELL + (WL_COLS - 1) * CGAP)
#define GRID_X ((WORDLE_W - GRID_W) / 2)
#define GRID_Y 16
#define GRID_H (WL_ROWS * CELL + (WL_ROWS - 1) * CGAP)

#define CAP_Y (GRID_Y + GRID_H + 8)

#define KEY_S 28
#define KGAP 4
#define KB_Y 368

/* mark: 0 unplayed, 1 miss, 2 present, 3 exact */
#define MK_NONE 0
#define MK_MISS 1
#define MK_PRESENT 2
#define MK_EXACT 3

static const char *words[] = {
    "CRANE", "SLATE", "AUDIO", "BRICK", "CHARM",
    "DODGE", "EAGLE", "FLINT", "GRAPE", "HOUSE",
    "IVORY", "JOLLY", "KNIFE", "LEMON", "MIRTH",
    "NOBLE", "OCEAN", "PLUMB", "QUIET", "RIVER",
    "STONE", "TIGER", "UNITY", "VIVID", "WHALE",
    "XENON", "YACHT", "ZEBRA", "MAPLE", "NIGHT",
    "PRIDE", "QUILT", "ROBIN", "SUGAR", "TRUCK"
};
#define WORD_COUNT ((int)(sizeof(words) / sizeof(words[0])))

/* '\n' = Enter key, '\b' = Backspace key. */
static const char *kb_rows[3] = {
    "QWERTYUIOP",
    "ASDFGHJKL",
    "\nZXCVBNM\b"
};

static char target[WL_COLS + 1];
static char grid[WL_ROWS][WL_COLS];
static char mark[WL_ROWS][WL_COLS];
static int cur_row;
static int cur_col;
static int state; /* 0 playing, 1 won, 2 lost */
static char caption[32];

static int row_len(int r) {
    int n = 0;
    while (kb_rows[r][n])
        n++;
    return n;
}

static void key_rect(int bx, int by, int r, int c, int *x, int *y) {
    int n = row_len(r);
    int w = n * KEY_S + (n - 1) * KGAP;
    *x = bx + (WORDLE_W - w) / 2 + c * (KEY_S + KGAP);
    *y = by + KB_Y + r * (KEY_S + KGAP);
}

static void set_caption(const char *s) {
    int i = 0;
    while (s[i] && i < (int)sizeof(caption) - 1) {
        caption[i] = s[i];
        i++;
    }
    caption[i] = 0;
}

void wordle_new_game(unsigned int seed) {
    unsigned int r = seed * 1103515245u + 12345u;
    const char *w = words[(r >> 8) % (unsigned int)WORD_COUNT];
    for (int i = 0; i < WL_COLS; i++)
        target[i] = w[i];
    target[WL_COLS] = 0;
    for (int y = 0; y < WL_ROWS; y++) {
        for (int x = 0; x < WL_COLS; x++) {
            grid[y][x] = 0;
            mark[y][x] = MK_NONE;
        }
    }
    cur_row = 0;
    cur_col = 0;
    state = 0;
    caption[0] = 0;
}

static void score_row(int r) {
    char used[WL_COLS];
    for (int i = 0; i < WL_COLS; i++)
        used[i] = 0;
    for (int i = 0; i < WL_COLS; i++) {
        if (grid[r][i] == target[i]) {
            mark[r][i] = MK_EXACT;
            used[i] = 1;
        }
    }
    for (int i = 0; i < WL_COLS; i++) {
        if (mark[r][i] == MK_EXACT)
            continue;
        mark[r][i] = MK_MISS;
        for (int j = 0; j < WL_COLS; j++) {
            if (!used[j] && target[j] == grid[r][i]) {
                used[j] = 1;
                mark[r][i] = MK_PRESENT;
                break;
            }
        }
    }
}

static void submit(void) {
    int all_green = 1;
    score_row(cur_row);
    for (int i = 0; i < WL_COLS; i++) {
        if (mark[cur_row][i] != MK_EXACT)
            all_green = 0;
    }
    if (all_green) {
        state = 1;
        set_caption("You got it!");
        return;
    }
    cur_row++;
    cur_col = 0;
    if (cur_row >= WL_ROWS) {
        state = 2;
        set_caption("The word was ");
        int n = 0;
        while (caption[n])
            n++;
        for (int i = 0; i < WL_COLS; i++)
            caption[n + i] = target[i];
        caption[n + WL_COLS] = 0;
    }
}

static void type_letter(char ch) {
    if (cur_col >= WL_COLS)
        return;
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');
    if (ch < 'A' || ch > 'Z')
        return;
    grid[cur_row][cur_col++] = ch;
}

int wordle_key(int action, char ch) {
    if (state != 0)
        return 0;
    if (action == WORDLE_ACT_ENTER) {
        if (cur_col < WL_COLS)
            return 0;
        submit();
        return 1;
    }
    if (action == WORDLE_ACT_DELETE) {
        if (cur_col <= 0)
            return 0;
        grid[cur_row][--cur_col] = 0;
        return 1;
    }
    if (!ch)
        return 0;
    int before = cur_col;
    type_letter(ch);
    return cur_col != before;
}

int wordle_click(int bx, int by, int mx, int my) {
    for (int r = 0; r < 3; r++) {
        int n = row_len(r);
        for (int c = 0; c < n; c++) {
            int x, y;
            key_rect(bx, by, r, c, &x, &y);
            if (!hit(mx, my, x, y, KEY_S, KEY_S))
                continue;
            char k = kb_rows[r][c];
            if (k == '\n')
                return wordle_key(WORDLE_ACT_ENTER, 0);
            if (k == '\b')
                return wordle_key(WORDLE_ACT_DELETE, 0);
            return wordle_key(WORDLE_ACT_NONE, k);
        }
    }
    return 0;
}

static void draw_glyph(char ch, int x, int y, int w, int h, unsigned char color) {
    char s[2];
    s[0] = ch;
    s[1] = 0;
    draw_string_bold(s, x + (w - uib_string_w(s)) / 2, y + (h - UI_FONT_H) / 2, color);
}

static void draw_key_enter(int x, int y) {
    int cx = x + KEY_S / 2;
    int cy = y + KEY_S / 2;
    draw_rect(cx + 5, cy - 7, 3, 12, WL_BLACK);
    draw_rect(cx - 3, cy + 2, 11, 3, WL_BLACK);
    for (int k = 0; k < 6; k++)
        draw_rect(cx - 6 + k, cy + 3 - k, 1, 2 * k + 1, WL_BLACK);
}

static void draw_key_bksp(int x, int y) {
    int cx = x + KEY_S / 2;
    int cy = y + KEY_S / 2;
    for (int k = 0; k < 6; k++)
        draw_rect(cx - 7 + k, cy - k, 1, 2 * k + 1, WL_BLACK);
    draw_rect(cx - 1, cy - 1, 8, 3, WL_BLACK);
}

void wordle_draw(int bx, int by) {
    int gx = bx + GRID_X;
    int gy = by + GRID_Y;

    for (int r = 0; r < WL_ROWS; r++) {
        for (int c = 0; c < WL_COLS; c++) {
            int x = gx + c * (CELL + CGAP);
            int y = gy + r * (CELL + CGAP);
            unsigned char fill = WL_WHITE;
            unsigned char ink = gfx_gray(0x20);
            unsigned char edge = grid[r][c] ? gfx_gray(0x70) : gfx_gray(0xC4);
            if (mark[r][c] == MK_EXACT)
                fill = gfx_rgb(0x4C, 0xA8, 0x60);
            else if (mark[r][c] == MK_PRESENT)
                fill = gfx_rgb(0xD8, 0xB0, 0x40);
            else if (mark[r][c] == MK_MISS)
                fill = gfx_gray(0x78);
            if (mark[r][c] != MK_NONE) {
                ink = WL_WHITE;
                edge = fill;
            }
            draw_round_rect(x, y, CELL, CELL, 5, edge);
            draw_round_rect(x + 1, y + 1, CELL - 2, CELL - 2, 4, fill);
            if (grid[r][c])
                draw_glyph(grid[r][c], x, y, CELL, CELL, ink);
        }
    }

    if (caption[0]) {
        int tw = ui_string_w(caption);
        draw_string(caption, bx + (WORDLE_W - tw) / 2, by + CAP_Y, gfx_gray(0x40));
    }

    for (int r = 0; r < 3; r++) {
        int n = row_len(r);
        for (int c = 0; c < n; c++) {
            int x, y;
            key_rect(bx, by, r, c, &x, &y);
            draw_round_rect(x, y, KEY_S, KEY_S, 4, app_chrome_dk);
            draw_round_rect(x + 1, y + 1, KEY_S - 2, KEY_S - 2, 3, app_chrome);
            char k = kb_rows[r][c];
            if (k == '\n')
                draw_key_enter(x, y);
            else if (k == '\b')
                draw_key_bksp(x, y);
            else
                draw_glyph(k, x, y, KEY_S, KEY_S, gfx_gray(0x20));
        }
    }
}
