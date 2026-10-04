/* Valid ordinary drawings only: production fill/input/history, no fuzzing. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "layout.h"
#include "gfx.h"
#include "fs.h"
#include "history.h"

static struct {
    uint8_t before[32];
    _Alignas(uint16_t) uint8_t data[PAINT_CAPACITY];
    uint8_t after[32];
} arena;
static struct {
    uint8_t before[32], data[PAINT_HISTORY_CAPACITY], after[32];
} undo_arena;
#undef PAINT_MEM
#undef PAINT_HISTORY_BASE
#define PAINT_MEM ((uintptr_t)arena.data)
#define PAINT_HISTORY_BASE ((uintptr_t)undo_arena.data)

void kmemcpy(void *d, const void *s, int n) { memmove(d, s, (size_t)n); }
void kmemset(void *d, int value, int n) { memset(d, value, (size_t)n); }
/* Rendering and RGB conversion are outside this extracted input/fill test. */
static uint8_t idx24(uint32_t color) { return (uint8_t)color; }
void draw_rect(int x, int y, int w, int h, uint8_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
    assert(!"Fill must not draw a screen overlay");
}
int hit(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}
static int dirty, mouse_x, mouse_y;
#include "paint_fill_kernel.inc"

#define PIXELS (PAINT_W * PAINT_H)
static uint8_t expected[PIXELS], previous[PIXELS], visited[PIXELS];
static uint8_t saved_arena[PAINT_CAPACITY], saved_history[PAINT_HISTORY_CAPACITY];
static unsigned checks;
static const int wx = 100, wy = 70, ww = 580, wh = 470;

static void expect_byte(const uint8_t *p, unsigned size, uint8_t value) {
    for (unsigned i = 0; i < size; ++i) assert(p[i] == value);
}
static void guards(void) {
    expect_byte(arena.before, sizeof arena.before, 0xa5);
    expect_byte(arena.after, sizeof arena.after, 0xa5);
    expect_byte(undo_arena.before, sizeof undo_arena.before, 0x5c);
    expect_byte(undo_arena.after, sizeof undo_arena.after, 0x5c);
    expect_byte(arena.data + PIXELS, PAINT_FILL_OFFSET - PIXELS, 0xa5);
    /* Includes the old save staging area; Fill must leave every byte intact. */
    unsigned queue_end = PAINT_FILL_OFFSET + sizeof(uint16_t) * PIXELS;
    expect_byte(arena.data + queue_end, PAINT_CAPACITY - queue_end, 0xa5);
    expect_byte(undo_arena.data + 8 * PIXELS, PAINT_HISTORY_CAPACITY - 8 * PIXELS, 0x5c);
}
static void reset(uint8_t background) {
    memset(&arena, 0xa5, sizeof arena);
    memset(&undo_arena, 0x5c, sizeof undo_arena);
    memset(&paint_history, 0, sizeof paint_history);
    paint_pix = NULL; paint_ready = 0;
    paint_dragging = paint_shape_drag = paint_text_on = dirty = 0;
    paint_init();
    memset(paint_pix, background, PIXELS);
    assert(paint_pix == arena.data);
    assert(paint_history.items == undo_arena.data && paint_history.capacity == 8);
    guards();
}
static void click_canvas(int x, int y) {
    int px, py, pw, ph, cx, cy;
    paint_canvas_geom(wx, wy, ww, wh, &px, &py, &pw, &ph, &cx, &cy);
    mouse_x = cx + x * PAINT_SCALE + 1;
    mouse_y = cy + y * PAINT_SCALE + 1;
    handle_paint_click(wx, wy, ww, wh);
}
static void fill_and_undo(int x, int y, uint8_t color) {
    memcpy(previous, paint_pix, PIXELS);
    memcpy(saved_arena, arena.data, sizeof saved_arena);
    uint8_t original = previous[y * PAINT_W + x];
    assert(original != color);
    unsigned count = 0;
    for (int p = 0; p < PIXELS; ++p) {
        if (previous[p] != expected[p]) {
            assert(previous[p] == original && expected[p] == color);
            ++count;
        }
    }
    assert(count);
    unsigned old_cursor = paint_history.cursor;
    paint_tool = PT_FILL; paint_color = color; dirty = 0;
    click_canvas(x, y);
    assert(dirty && paint_tool == PT_FILL && paint_color == color);
    assert(!memcmp(paint_pix, expected, PIXELS));
    assert(paint_history.cursor == (old_cursor < 8 ? old_cursor + 1 : 8));
    assert(paint_history.count == paint_history.cursor);
    assert(!memcmp(paint_history.items + (paint_history.cursor - 1) * PIXELS, previous, PIXELS));
    /* Exactly one queued index per changed pixel, all within the selected region. */
    const uint16_t *queue = (const uint16_t *)(arena.data + PAINT_FILL_OFFSET);
    memset(visited, 0, sizeof visited);
    assert(queue[0] == y * PAINT_W + x);
    for (unsigned i = 0; i < count; ++i) {
        unsigned p = queue[i];
        assert(p < PIXELS && !visited[p]);
        assert(previous[p] == original && expected[p] == color);
        visited[p] = 1;
    }
    for (int p = 0; p < PIXELS; ++p)
        assert(visited[p] == (previous[p] != expected[p]));
    unsigned end = PAINT_FILL_OFFSET + count * sizeof(uint16_t);
    assert(!memcmp(arena.data + end, saved_arena + end, PAINT_CAPACITY - end));
    guards();
    paint_dragging = paint_shape_drag = paint_text_on = 1; dirty = 0;
    paint_undo(0);
    assert(dirty && !paint_dragging && !paint_shape_drag && !paint_text_on);
    assert(!memcmp(paint_pix, previous, PIXELS));
    dirty = 0; paint_undo(1);
    assert(dirty && !memcmp(paint_pix, expected, PIXELS));
    guards(); ++checks;
}
static void no_op(int x, int y) {
    memcpy(saved_arena, arena.data, sizeof saved_arena);
    memcpy(saved_history, undo_arena.data, sizeof saved_history);
    History history = paint_history;
    paint_tool = PT_FILL; paint_color = paint_pix[y * PAINT_W + x]; dirty = 0;
    click_canvas(x, y);
    assert(!dirty && !memcmp(saved_arena, arena.data, sizeof saved_arena));
    assert(!memcmp(saved_history, undo_arena.data, sizeof saved_history));
    assert(!memcmp(&history, &paint_history, sizeof history));
    /* Direct helper shares the same no-op contract. */
    paint_flood(x, y, paint_color);
    assert(!memcmp(saved_arena, arena.data, sizeof saved_arena));
    guards(); ++checks;
}
static void blank_canvas(void) {
    const int starts[][2] = {{60,48}, {0,0}, {159,0}, {0,99}, {159,99},
                            {80,0}, {0,50}, {159,50}, {80,99}, {80,50}};
    const uint8_t colors[] = {COLOR_BLACK, COLOR_RED, 128, 255};
    for (unsigned c = 0; c < sizeof colors; ++c) {
        for (unsigned i = 0; i < sizeof starts / sizeof starts[0]; ++i) {
            reset(COLOR_WHITE); memset(expected, colors[c], sizeof expected);
            fill_and_undo(starts[i][0], starts[i][1], colors[c]);
            no_op(starts[i][0], starts[i][1]);
        }
    }
    reset(COLOR_BLACK); memset(expected, COLOR_WHITE, sizeof expected);
    fill_and_undo(60, 48, COLOR_WHITE);
    puts("Full 16,000-pixel canvases: original (60,48), center, four corners/four edges, palette colors passed");
}
static void enclosed_regions(void) {
    reset(COLOR_WHITE); plot_dest = 0; plot_col = COLOR_BLACK;
    rect_lg(12, 10, 147, 89); rect_lg(60, 40, 99, 59);
    memcpy(expected, paint_pix, PIXELS);
    for (int y = 11; y < 89; ++y)
        for (int x = 13; x < 147; ++x)
            if (!(x >= 60 && x <= 99 && y >= 40 && y <= 59))
                expected[y * PAINT_W + x] = COLOR_BLUE;
    fill_and_undo(13, 11, COLOR_BLUE);
    /* Fill the nested region without changing the enclosing ring or outside. */
    for (int y = 41; y < 59; ++y)
        for (int x = 61; x < 99; ++x) expected[y * PAINT_W + x] = COLOR_GREEN;
    fill_and_undo(70, 45, COLOR_GREEN);
    for (int y = 0; y < PAINT_H; ++y)
        for (int x = 0; x < PAINT_W; ++x)
            if (x < 12 || x > 147 || y < 10 || y > 89)
                expected[y * PAINT_W + x] = COLOR_ORANGE;
    fill_and_undo(0, 0, COLOR_ORANGE);
    puts("Nested enclosed shapes: inner, surrounding and exterior regions preserve every boundary");
}
static void narrow_corridor(void) {
    reset(COLOR_BLACK); plot_dest = 0; plot_col = COLOR_WHITE;
    /* A one-pixel serpentine corridor joined only at alternate row ends. */
    for (int y = 0; y < PAINT_H; y += 2) {
        bresenham_lg(0, y, PAINT_W - 1, y);
        if (y + 1 < PAINT_H) {
            int x = (y / 2) % 2 ? 0 : PAINT_W - 1;
            plot_lg(x, y + 1);
        }
    }
    for (int p = 0; p < PIXELS; ++p)
        expected[p] = paint_pix[p] == COLOR_WHITE ? COLOR_CYAN : COLOR_BLACK;
    fill_and_undo(0, 0, COLOR_CYAN);
    /* Refill the complete same corridor from its opposite endpoint. */
    for (int p = 0; p < PIXELS; ++p)
        if (expected[p] == COLOR_CYAN) expected[p] = COLOR_MAGENTA;
    fill_and_undo(0, 99, COLOR_MAGENTA);
    puts("One-pixel winding corridor: both ends, 8,050 connected pixels, no leaks passed");
}
static void separate_regions(void) {
    reset(COLOR_WHITE); plot_dest = 0; plot_col = COLOR_BLACK;
    bresenham_lg(80, 0, 80, 99);
    memcpy(expected, paint_pix, PIXELS);
    for (int y = 0; y < PAINT_H; ++y)
        for (int x = 0; x < 80; ++x) expected[y * PAINT_W + x] = COLOR_RED;
    fill_and_undo(0, 99, COLOR_RED);
    for (int y = 0; y < PAINT_H; ++y)
        for (int x = 81; x < PAINT_W; ++x) expected[y * PAINT_W + x] = COLOR_RED;
    fill_and_undo(159, 0, COLOR_RED);
    /* Diagonal contact and row-end adjacency are not four-way connectivity. */
    reset(COLOR_BLACK);
    paint_pix[10 * PAINT_W + 10] = paint_pix[11 * PAINT_W + 11] = COLOR_WHITE;
    paint_pix[20 * PAINT_W + 159] = paint_pix[21 * PAINT_W] = COLOR_WHITE;
    memcpy(expected, paint_pix, PIXELS); expected[10 * PAINT_W + 10] = 255;
    fill_and_undo(10, 10, 255);
    expected[20 * PAINT_W + 159] = COLOR_GREEN;
    fill_and_undo(159, 20, COLOR_GREEN);
    puts("Separate regions, one-pixel islands, diagonal contact and row-wrap boundaries passed");
}
static void history_chain(void) {
    reset(COLOR_WHITE); no_op(0, 0);
    memset(expected, COLOR_RED, sizeof expected); fill_and_undo(0, 0, COLOR_RED);
    memset(expected, COLOR_BLUE, sizeof expected); fill_and_undo(159, 99, COLOR_BLUE);
    paint_undo(0); expect_byte(paint_pix, PIXELS, COLOR_RED);
    no_op(80, 50); /* Must preserve the pending Blue redo. */
    paint_undo(1); expect_byte(paint_pix, PIXELS, COLOR_BLUE);
    for (int i = 0; i < 12; ++i) {
        uint8_t color = (uint8_t)(128 + i);
        memset(expected, color, sizeof expected); fill_and_undo(60, 48, color); no_op(60, 48);
    }
    assert(paint_history.count == 8 && paint_history.cursor == 8);
    for (int i = 0; i < 8; ++i) {
        paint_undo(0); expect_byte(paint_pix, PIXELS, (uint8_t)(138 - i));
    }
    dirty = 0; paint_undo(0); assert(!dirty);
    for (int i = 0; i < 8; ++i) {
        paint_undo(1); expect_byte(paint_pix, PIXELS, (uint8_t)(132 + i));
    }
    dirty = 0; paint_undo(1); assert(!dirty);
    paint_undo(0); memset(expected, COLOR_GREEN, sizeof expected);
    fill_and_undo(80, 50, COLOR_GREEN);
    dirty = 0; paint_undo(1); assert(!dirty); /* New edit correctly drops old redo. */
    guards();
    puts("Eight-step undo/redo, history rollover, new-edit redo truncation and same-color no-op preservation passed");
}
int main(void) {
    blank_canvas(); enclosed_regions(); narrow_corridor(); separate_regions(); history_chain();
    printf("Paint fill host tests passed: %u exact fill/no-op cases, ordinary inputs only\n", checks);
    return 0;
}
