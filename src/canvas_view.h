#ifndef BASEOS_CANVAS_VIEW_H
#define BASEOS_CANVAS_VIEW_H

/* Shared, owner-independent geometry for a published Terminal canvas. */
typedef struct {
    int x, y, logical_w, logical_h, viewport_w, viewport_h;
} CanvasView;

/* Preserve the Terminal's existing left-aligned 100/200/400-pixel layout and
 * integer rounding, including its eight-pixel trailing text gap. source_w/h
 * describe the published frame, never an in-progress working resize. If either
 * source dimension is nonpositive, all four dimensions are zero. A published
 * frame with insufficient window space retains its logical dimensions.
 * Returns the occupied height including the gap, or zero if no room/source.
 * Unrepresentable origins/widths are saturated to the signed 32-bit range;
 * ordinary screen/window/canvas dimensions do not reach that limit. */
int canvas_view_layout(CanvasView *out, int wx, int wy, int ww, int wh,
                       int source_w, int source_h, int title_h, int pad,
                       int line_h);

/* Owned-window client: fit and center a published frame inside this explicit
 * client rectangle. The caller excludes chrome/Output controls. Fractional
 * downscale is permitted; padding outside the viewport is never app input. */
void canvas_view_native_layout(CanvasView *out, int x, int y, int width, int height,
                               int source_w, int source_h);

/* Half-open viewport bounds. The caller supplies visible-screen coordinates
 * and checks WM ownership/availability separately; this helper has no screen
 * dimensions and performs no occlusion test. Empty views never contain a point.
 */
int canvas_view_contains(const CanvasView *view, int x, int y);

/* Mathematical floor of (screen - origin) * logical / viewport, including
 * negative captured positions. Coordinates are not clamped to the canvas.
 * Results outside the signed 32-bit API range saturate; invalid/empty views
 * produce (0, 0). Either output pointer may be null. */
void canvas_view_position(const CanvasView *view, int x, int y,
                          int *local_x, int *local_y);

#endif
