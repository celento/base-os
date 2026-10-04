/* Deterministic geometry fixtures; no device input or guest execution. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "canvas_view.h"

enum { TITLE_H = 32, TERM_PAD = 12, EDIT_LINE_H = 19 };

/* Frozen pre-extraction renderer formula, using its ordinary bounded inputs. */
static int old_geometry(int ww, int wh, int source_w, int source_h,
                        int *target_w, int *target_h) {
    *target_w = *target_h = 0;
    int height = source_w > 160 && wh > 550 ? 400 : wh > 360 ? 200 : 100;
    int available_h = wh - TITLE_H - 2 - TERM_PAD * 2 - EDIT_LINE_H * 2 - 8;
    int available_w = ww - 2 - TERM_PAD * 2;
    if (height > available_h) height = available_h;
    if (height <= 0 || available_w <= 0 || source_w <= 0 || source_h <= 0) return 0;
    int width = source_w * height / source_h;
    if (width > available_w) { width = available_w; height = source_h * width / source_w; }
    *target_w = width; *target_h = height;
    return height + 8;
}

static int layout(CanvasView *view, int wx, int wy, int ww, int wh, int sw, int sh) {
    return canvas_view_layout(view, wx, wy, ww, wh, sw, sh,
                              TITLE_H, TERM_PAD, EDIT_LINE_H);
}

static void at(const CanvasView *view, int x, int y, int expected_x, int expected_y) {
    int lx = 123, ly = 456;
    canvas_view_position(view, x, y, &lx, &ly);
    assert(lx == expected_x && ly == expected_y);
}

static int reference_position(int screen, int origin, int logical, int viewport) {
    int64_t n = ((int64_t)screen - origin) * logical;
    int64_t q = n / viewport;
    if (n < 0 && n % viewport) --q;
    if (q < INT32_MIN) return INT32_MIN;
    if (q > INT32_MAX) return INT32_MAX;
    return (int)q;
}

static void legacy_layouts(void) {
    /* Native legacy/expanded canvases plus the exact width threshold and a
     * nonsquare fractional fixture, in small, normal, snapped and full views. */
    static const int sources[][2] = {{160,100}, {320,200}, {161,100}, {197,113}};
    static const int widths[] = {0,26,27,39,106,137,185,186,187,199,265,266,267,
                                345,346,347,500,665,666,667,800,1024,1280};
    static const int heights[] = {0,103,104,105,106,108,140,203,204,205,300,
                                 359,360,361,549,550,551,600,720,768,800};
    for (unsigned s = 0; s < sizeof(sources) / sizeof(sources[0]); ++s)
        for (unsigned w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w)
            for (unsigned h = 0; h < sizeof(heights) / sizeof(heights[0]); ++h) {
                CanvasView view;
                int tw, th, sw = sources[s][0], sh = sources[s][1];
                int occupied = old_geometry(widths[w], heights[h], sw, sh, &tw, &th);
                assert(layout(&view, 17, 29, widths[w], heights[h], sw, sh) == occupied);
                assert(view.x == 30 && view.y == 74);
                assert(view.logical_w == sw && view.logical_h == sh);
                assert(view.viewport_w == tw && view.viewport_h == th);
                if (!tw || !th) {
                    assert(!canvas_view_contains(&view, view.x, view.y));
                    at(&view, view.x, view.y, 0, 0);
                    continue;
                }
                assert(canvas_view_contains(&view, view.x, view.y));
                assert(canvas_view_contains(&view, view.x + tw - 1, view.y + th - 1));
                assert(!canvas_view_contains(&view, view.x - 1, view.y));
                assert(!canvas_view_contains(&view, view.x, view.y - 1));
                assert(!canvas_view_contains(&view, view.x + tw, view.y));
                assert(!canvas_view_contains(&view, view.x, view.y + th));
                static const int offsets[] = {-1200,-401,-201,-3,-2,-1,0,1,2,3,99,199,401,1200};
                for (unsigned p = 0; p < sizeof(offsets) / sizeof(offsets[0]); ++p) {
                    int x = view.x + offsets[p], y = view.y + offsets[p];
                    at(&view, x, y, reference_position(x, view.x, sw, tw),
                       reference_position(y, view.y, sh, th));
                }
            }
}

static void bounds_and_scaling(void) {
    CanvasView view;
    assert(layout(&view, 10, 20, 800, 600, 160, 100) == 208);
    assert(view.viewport_w == 320 && view.viewport_h == 200);
    at(&view, 22, 64, -1, -1); /* Negative half pixels round down. */
    at(&view, 23, 65, 0, 0);
    at(&view, 24, 66, 0, 0);
    at(&view, 25, 67, 1, 1);
    at(&view, 343, 265, 160, 100); /* Outside capture stays unclamped. */
    assert(!canvas_view_contains(&view, 10, 20)); /* Title/chrome. */
    assert(!canvas_view_contains(&view, 22, 80)); /* Left padding. */
    assert(!canvas_view_contains(&view, 23, 64)); /* Top padding. */
    assert(!canvas_view_contains(&view, 500, 80)); /* Right letterbox. */
    assert(!canvas_view_contains(&view, 23, 268)); /* Trailing text gap. */

    assert(layout(&view, 10, 20, 199, 400, 320, 200) == 116);
    assert(view.viewport_w == 173 && view.viewport_h == 108);
    at(&view, 22, 64, -2, -2);
    at(&view, 24, 66, 1, 1);
    at(&view, 195, 172, 318, 198);
    at(&view, 196, 173, 320, 200);
    assert(!canvas_view_contains(&view, 196, 100));
    assert(!canvas_view_contains(&view, 100, 173));

    assert(layout(&view, -100, -70, 199, 400, 320, 200) == 116);
    assert(view.x == -87 && view.y == -25);
    assert(canvas_view_contains(&view, 0, 0));
    at(&view, 0, 0, 160, 46); /* Visible portion of an offscreen window. */
    at(&view, -88, -26, -2, -2);
    assert(!canvas_view_contains(&view, 86, 0));
    assert(!canvas_view_contains(&view, 0, 83));

    /* Parameters are shared with the renderer, not hardcoded in the helper. */
    assert(canvas_view_layout(&view, 4, 7, 220, 180, 160, 100, 20, 5, 10) == 108);
    assert(view.x == 10 && view.y == 33);
    assert(view.viewport_w == 160 && view.viewport_h == 100);
}

static void empty_views(void) {
    static const int sources[][2] = {{0,0}, {0,100}, {160,0}};
    for (unsigned i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        CanvasView view = {1,2,3,4,5,6};
        assert(layout(&view, 10, 20, 800, 600, sources[i][0], sources[i][1]) == 0);
        assert(view.x == 23 && view.y == 65);
        assert(!view.logical_w && !view.logical_h && !view.viewport_w && !view.viewport_h);
        assert(!canvas_view_contains(&view, view.x, view.y));
        at(&view, -50, 500, 0, 0);
    }
    CanvasView view;
    assert(layout(&view, 0, 0, 26, 600, 160, 100) == 0);
    assert(view.logical_w == 160 && view.logical_h == 100);
    assert(!view.viewport_w && !view.viewport_h);
    assert(layout(&view, 0, 0, 800, 104, 160, 100) == 0);
    assert(!view.viewport_w && !view.viewport_h);
    /* Preserve old rounding/occupied-gap results even when one extent rounds
     * to zero; neither degenerate rectangle can acquire input. */
    assert(layout(&view, 0, 0, 27, 600, 320, 200) == 8);
    assert(view.viewport_w == 1 && view.viewport_h == 0);
    assert(!canvas_view_contains(&view, view.x, view.y));
    assert(layout(&view, 0, 0, 800, 200, 1, 200) == 104);
    assert(view.viewport_w == 0 && view.viewport_h == 96);
    assert(!canvas_view_contains(&view, view.x, view.y));
    at(&view, 100, 100, 0, 0);
    assert(!canvas_view_contains(0, 0, 0));
    at(0, 100, 100, 0, 0);
    canvas_view_position(&view, 0, 0, 0, 0);
    assert(!canvas_view_layout(0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
}

static void widened_coordinates(void) {
    /* Explicit arithmetic boundary fixtures document the signed wire range. */
    CanvasView view = {INT32_MIN, INT32_MAX, 320, 200, 640, 400};
    at(&view, INT32_MAX, INT32_MIN, INT32_MAX, INT32_MIN);
    assert(canvas_view_contains(&view, INT32_MIN, INT32_MAX));
    assert(canvas_view_contains(&view, INT32_MIN + 639, INT32_MAX));
    assert(!canvas_view_contains(&view, INT32_MIN + 640, INT32_MAX));
    assert(!canvas_view_contains(&view, INT32_MIN, INT32_MAX - 1));
    view = (CanvasView){INT32_MIN, INT32_MAX, INT32_MAX, INT32_MAX, 1, 1};
    at(&view, INT32_MAX, INT32_MIN, INT32_MAX, INT32_MIN);
    view = (CanvasView){INT32_MIN, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX};
    at(&view, INT32_MAX, INT32_MIN, INT32_MAX, INT32_MIN);
    assert(layout(&view, INT32_MAX, INT32_MIN, INT32_MAX, 600, INT32_MAX, 1) == 8);
    assert(view.x == INT32_MAX && view.y == INT32_MIN + 45);
    assert(view.viewport_w == INT32_MAX - 26 && view.viewport_h == 0);
    int ly = 1;
    canvas_view_position(&view, 0, 0, 0, &ly);
    assert(!ly);
}

int main(void) {
    legacy_layouts();
    bounds_and_scaling();
    empty_views();
    widened_coordinates();
    puts("Canvas view: legacy layout, fractional floor, half-open bounds, offscreen origins and widened coordinates passed.");
    return 0;
}
