/* Wordle: six guesses at a five-letter word, an on-screen keyboard that
 * shows what each letter scored, and a result card when the game ends. */
#include "apps/wordle.h"
#include "kernel/font.h"

#include "ui/app.h"
#include "apps/pane_chrome.h"

#define WL_ROWS 6
#define WL_COLS 5

#define CELL 38
#define CGAP 5
#define GRID_W (WL_COLS * CELL + (WL_COLS - 1) * CGAP)
#define GRID_H (WL_ROWS * CELL + (WL_ROWS - 1) * CGAP)

#define KEY_W 32
#define KEY_H 30
#define KEY_WIDE 50   /* Enter and Backspace */
#define KGAP 4
#define KB_GAP 14     /* between the grid and the keyboard */

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

/* Layout: a toolbar with the guess count and New game, the grid, the
 * keyboard (or the result card once the game ends), and a status bar.
 * Drawing and clicks share wordle_layout and key_rect. */
typedef struct { PaneRect again, card, card_button; int gx, gy, ky; int cx, cw; } WordleLayout;

static void wordle_layout(int bx, int by, int bw, int bh, WordleLayout *L) {
    int body = pane_body_y(by), bottom = pane_status_y(by, bh);
    int content = GRID_H + KB_GAP + 3 * KEY_H + 2 * KGAP;
    int w = pane_tool_w("New game");
    L->again = (PaneRect){bx + bw - pane_pad() - w, pane_ctl_y(by), w, ui_theme.control_h};
    L->cx = bx; L->cw = bw;
    L->gx = bx + (bw - GRID_W) / 2;
    L->gy = body + (bottom - body - content) / 2;
    L->ky = L->gy + GRID_H + KB_GAP;
    int top = L->gy + GRID_H + 4;
    pane_card_layout(bx, top, bw, bottom - top, &L->card, &L->card_button);
}

int wordle_client_h(void) {
    return pane_body_y(0) + WORDLE_BODY_H + pane_status_h();
}

static int key_w(char k) { return k == '\n' || k == '\b' ? KEY_WIDE : KEY_W; }

static void key_rect(const WordleLayout *L, int r, int c, PaneRect *out) {
    int n = row_len(r), w = (n - 1) * KGAP, x;
    for (int i = 0; i < n; i++) w += key_w(kb_rows[r][i]);
    x = L->cx + (L->cw - w) / 2;
    for (int i = 0; i < c; i++) x += key_w(kb_rows[r][i]) + KGAP;
    *out = (PaneRect){x, L->ky + r * (KEY_H + KGAP), key_w(kb_rows[r][c]), KEY_H};
}

int wordle_click(int bx, int by, int bw, int bh, int mx, int my, unsigned int seed) {
    WordleLayout L;
    wordle_layout(bx, by, bw, bh, &L);
    if (pane_hit(mx, my, &L.again) || (state != 0 && pane_hit(mx, my, &L.card_button))) {
        wordle_new_game(seed);
        return 1;
    }
    if (state != 0)
        return 0;
    for (int r = 0; r < 3; r++) {
        int n = row_len(r);
        for (int c = 0; c < n; c++) {
            PaneRect k;
            key_rect(&L, r, c, &k);
            if (!pane_hit(mx, my, &k))
                continue;
            char ch = kb_rows[r][c];
            if (ch == '\n')
                return wordle_key(WORDLE_ACT_ENTER, 0);
            if (ch == '\b')
                return wordle_key(WORDLE_ACT_DELETE, 0);
            return wordle_key(WORDLE_ACT_NONE, ch);
        }
    }
    return 0;
}

/* Tile colors are playfield art: exact cube colors keep their meaning in
 * every theme. */
static uint8_t mark_fill(int m) {
    return m == MK_EXACT ? gfx_rgb(0x40, 0xC0, 0x80) : m == MK_PRESENT ? gfx_rgb(0xFF, 0xC0, 0x40) : gfx_gray(0x50);
}
static uint8_t mark_ink(int m) { return m == MK_MISS ? COLOR_WHITE : gfx_gray(0x10); }

/* The best mark a letter has earned so far, for the keyboard. */
static int letter_mark(char ch) {
    int best = MK_NONE;
    for (int r = 0; r < WL_ROWS; r++)
        for (int c = 0; c < WL_COLS; c++)
            if (grid[r][c] == ch && mark[r][c] > best) best = mark[r][c];
    return best;
}

static void draw_key_enter(int cx, int cy, uint8_t ink) {
    draw_rect(cx + 5, cy - 7, 3, 12, ink);
    draw_rect(cx - 3, cy + 2, 11, 3, ink);
    for (int k = 0; k < 6; k++)
        draw_rect(cx - 6 + k, cy + 3 - k, 1, 2 * k + 1, ink);
}

static void draw_key_bksp(int cx, int cy, uint8_t ink) {
    for (int k = 0; k < 6; k++)
        draw_rect(cx - 7 + k, cy - k, 1, 2 * k + 1, ink);
    draw_rect(cx - 1, cy - 1, 8, 3, ink);
}

void wordle_draw(int bx, int by, int bw, int bh, PaneHot hot) {
    int modern = PANE_MODERN, r6 = modern ? 6 : KIT_R_SQUARE;
    draw_rect(bx, by, bw, bh, ui_theme.content);
    WordleLayout L;
    wordle_layout(bx, by, bw, bh, &L);
    char buf[12], text[40];
    /* Toolbar */
    pane_toolbar(bx, by, bw);
    int guess = state == 0 ? cur_row + 1 : state == 1 ? cur_row + 1 : WL_ROWS;
    pane_num(buf, (unsigned)guess);
    pane_join(text, sizeof text, buf, " of ", "6");
    pane_inline_stat(bx + pane_pad() + 4, L.again.y, L.again.h, "Guess", text);
    pane_tool(&L.again, ICON_REFRESH, "New game", state ? KB_PRIMARY : KB_DEFAULT,
              pane_hot(hot, L.again.x, L.again.y, L.again.w, L.again.h));

    /* Grid */
    for (int r = 0; r < WL_ROWS; r++) {
        for (int c = 0; c < WL_COLS; c++) {
            int x = L.gx + c * (CELL + CGAP);
            int y = L.gy + r * (CELL + CGAP);
            int m = mark[r][c];
            char s[2] = {grid[r][c], 0};
            if (m != MK_NONE) {
                kit_fill(x, y, CELL, CELL, r6, mark_fill(m), 1);
                kit_text_center(s, x, y + (CELL - UI_FONT_H) / 2, CELL, mark_ink(m), 1);
            } else {
                int current = state == 0 && r == cur_row;
                uint8_t edge = grid[r][c] ? TK_LINE_STRONG : current ? TK_LINE : TK_LINE_SUBTLE;
                kit_box(x, y, CELL, CELL, r6, TK_RAISED, edge);
                if (grid[r][c]) kit_text_center(s, x, y + (CELL - UI_FONT_H) / 2, CELL, TK_TEXT, 1);
                else if (current && c == cur_col) kit_fill(x + CELL / 2 - 6, y + CELL - 9, 12, 2, KIT_R_SQUARE, TK_ACCENT, 0);
            }
        }
    }

    if (state == 0) {
        /* Keyboard: kit keys tinted by what each letter scored. */
        for (int r = 0; r < 3; r++) {
            int n = row_len(r);
            for (int c = 0; c < n; c++) {
                PaneRect k;
                key_rect(&L, r, c, &k);
                char ch = kb_rows[r][c];
                int m = ch >= 'A' && ch <= 'Z' ? letter_mark(ch) : MK_NONE;
                int st = pane_hot(hot, k.x, k.y, k.w, k.h);
                uint8_t ink = TK_TEXT;
                if (m == MK_NONE) {
                    uint8_t fill = st & KIT_PRESSED ? TK_LINE : st & KIT_HOVER ? TK_HOVER : modern ? TK_SUNKEN : TK_BASE;
                    kit_box(k.x, k.y, k.w, k.h, modern ? 6 : KIT_R_SQUARE, fill, modern ? 0 : TK_LINE);
                } else {
                    kit_fill(k.x, k.y, k.w, k.h, modern ? 6 : KIT_R_SQUARE, mark_fill(m), 1);
                    ink = mark_ink(m);
                }
                if (ch == '\n')
                    draw_key_enter(k.x + k.w / 2, k.y + k.h / 2, ink);
                else if (ch == '\b')
                    draw_key_bksp(k.x + k.w / 2, k.y + k.h / 2, ink);
                else {
                    char s[2] = {ch, 0};
                    kit_text_center(s, k.x, kit_ty(k.y, k.h), k.w, ink, 1);
                }
            }
        }
    } else {
        if (state == 1) {
            pane_num(buf, (unsigned)(cur_row + 1));
            pane_join(text, sizeof text, "Solved in ", buf, cur_row ? " guesses." : " guess.");
        }
        pane_card(&L.card, &L.card_button, state == 1 ? "You got it!" : "Out of guesses",
                  state == 1 ? text : caption, "New game",
                  pane_hot(hot, L.card_button.x, L.card_button.y, L.card_button.w, L.card_button.h));
    }
    pane_statusbar(bx, by, bw, bh,
                   state ? "Ctrl+N or New game starts again" : "Type letters; Enter checks, Backspace deletes",
                   TK_TEXT2, state == 1 ? "Solved" : state == 2 ? "Not solved" : 0,
                   state == 1 ? TK_GREEN_700 : TK_RED_700);
}
