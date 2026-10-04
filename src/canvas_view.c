#include "canvas_view.h"
#include <stdint.h>

/* i386 freestanding builds do not link a 64-bit division runtime. */
static uint64_t divide(uint64_t numerator, uint32_t denominator) {
    if (numerator <= UINT32_MAX) return (uint32_t)numerator / denominator;
    uint64_t quotient = 0, remainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
        remainder = (remainder << 1) | ((numerator >> bit) & 1);
        if (remainder >= denominator) {
            remainder -= denominator;
            quotient |= (uint64_t)1 << bit;
        }
    }
    return quotient;
}

static int saturated(int64_t value) {
    if (value < INT32_MIN) return INT32_MIN;
    if (value > INT32_MAX) return INT32_MAX;
    return (int)value;
}

int canvas_view_layout(CanvasView *out, int wx, int wy, int ww, int wh,
                       int source_w, int source_h, int title_h, int pad,
                       int line_h) {
    if (!out) return 0;
    out->x = saturated((int64_t)wx + 1 + pad);
    out->y = saturated((int64_t)wy + title_h + 1 + pad);
    out->logical_w = out->logical_h = 0;
    out->viewport_w = out->viewport_h = 0;
    if (source_w <= 0 || source_h <= 0) return 0;
    out->logical_w = source_w;
    out->logical_h = source_h;

    int height = source_w > 160 && wh > 550 ? 400 : wh > 360 ? 200 : 100;
    int64_t available_h = (int64_t)wh - title_h - 2 - (int64_t)pad * 2
                          - (int64_t)line_h * 2 - 8;
    int64_t available_w = (int64_t)ww - 2 - (int64_t)pad * 2;
    if (available_h <= 0 || available_w <= 0) return 0;
    if (height > available_h) height = (int)available_h;
    if (available_w > INT32_MAX) available_w = INT32_MAX;
    uint64_t width = divide((uint64_t)source_w * (unsigned)height,
                            (uint32_t)source_h);
    if (width > (uint64_t)available_w) {
        width = (uint64_t)available_w;
        height = (int)divide((uint64_t)source_h * width, (uint32_t)source_w);
    }
    out->viewport_w = (int)width;
    out->viewport_h = height;
    return height + 8;
}

static int nonempty(const CanvasView *view) {
    return view && view->logical_w > 0 && view->logical_h > 0 &&
           view->viewport_w > 0 && view->viewport_h > 0;
}

void canvas_view_native_layout(CanvasView *out,int x,int y,int width,int height,
                               int source_w,int source_h) {
    if(!out)return;
    *out=(CanvasView){x,y,0,0,0,0};
    if(source_w<=0||source_h<=0)return;
    out->logical_w=source_w;out->logical_h=source_h;
    if(width<=0||height<=0)return;
    uint64_t w=width,h=divide((uint64_t)(unsigned)source_h*(unsigned)width,(unsigned)source_w);
    if(h>(unsigned)height){h=(unsigned)height;w=divide((uint64_t)(unsigned)source_w*h,(unsigned)source_h);}
    out->viewport_w=(int)w;out->viewport_h=(int)h;
    out->x=saturated((int64_t)x+((int64_t)width-(int)w)/2);
    out->y=saturated((int64_t)y+((int64_t)height-(int)h)/2);
}

int canvas_view_contains(const CanvasView *view, int x, int y) {
    if (!nonempty(view)) return 0;
    int64_t dx = (int64_t)x - view->x, dy = (int64_t)y - view->y;
    return dx >= 0 && dy >= 0 && dx < view->viewport_w && dy < view->viewport_h;
}

static int position(int screen, int origin, int logical, int viewport) {
    int64_t delta = (int64_t)screen - origin;
    /* For signed 32-bit inputs the magnitude product is at most
     * (2^32 - 1) * (2^31 - 1), which fits in signed 64 bits. */
    uint64_t magnitude = (uint64_t)(delta < 0 ? -delta : delta) * (unsigned)logical;
    /* floor(-n/d) == -ceil(n/d); adding d-1 is also representable here. */
    if (delta < 0) magnitude += (unsigned)viewport - 1u;
    uint64_t quotient = divide(magnitude, (uint32_t)viewport);
    return saturated(delta < 0 ? -(int64_t)quotient : (int64_t)quotient);
}

void canvas_view_position(const CanvasView *view, int x, int y,
                          int *local_x, int *local_y) {
    int px = 0, py = 0;
    if (nonempty(view)) {
        px = position(x, view->x, view->logical_w, view->viewport_w);
        py = position(y, view->y, view->logical_h, view->viewport_h);
    }
    if (local_x) *local_x = px;
    if (local_y) *local_y = py;
}
