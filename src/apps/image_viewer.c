/* A small image client for the existing cooperative BaseOS window manager. */
#include "ui/app.h"
#include "apps/image_viewer.h"
#include "kernel/layout.h"
#include "kernel/platform.h"
#include "ui/icon_draw.h"
#include "ui/icons.h"
#include "ui/kit.h"
#include "ui/ui_icons.h"

/* The integration commit adds these to the shared E820-checked memory map. */
#ifndef IMAGE_BASE
#define IMAGE_BASE 0x1900000
#endif
#ifndef IMAGE_CAPACITY
#define IMAGE_CAPACITY 0x700000
#endif
#define IMAGE_PIXELS_BYTES IMAGE_MAX_PIXELS
#define MODERN (ui_theme.style == UI_STYLE_MODERN)
#define SB (ui_theme.scrollbar)
#define ZOOM_COUNT 10
static const unsigned zooms[ZOOM_COUNT] = {10, 25, 50, 75, 100, 125, 150, 200, 300, 400};

#ifdef IMAGE_VIEWER_HOST_TEST
static uint8_t host_image_memory[IMAGE_CAPACITY];
#define IMAGE_MEMORY host_image_memory
#else
#define IMAGE_MEMORY ((uint8_t *)IMAGE_BASE)
#endif
_Static_assert(IMAGE_PIXELS_BYTES < IMAGE_CAPACITY, "image workspace missing");

static struct {
    unsigned width, height, format, scale;
    int loaded, fit, pan_x, pan_y, viewport_w, viewport_h;
    int client_w, client_h, hbar, vbar;
    char title[96], status[160];
} viewer;

static void copy(char *out, unsigned size, const char *text) {
    if (!size) return;
    unsigned n = 0;
    if (text) while (text[n] && n + 1 < size) { out[n] = text[n]; n++; }
    out[n] = 0;
}
static void append(char *out, unsigned size, const char *text) {
    unsigned n = 0; while (n < size && out[n]) n++;
    if (n < size) copy(out + n, size - n, text);
}
static void append_number(char *out, unsigned size, unsigned value) {
    char number[12]; fmt_uint(number, value); append(out, size, number);
}
static unsigned minimum(unsigned a, unsigned b) { return a < b ? a : b; }
static int clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

/* A fixed palette subset avoids recoloring photos when desktop theme ramps
 * change. Cube and gray candidates are computed directly, with weighted RGB
 * distance; no per-pixel 256-entry search is needed in the cooperative UI. */
static uint8_t palette_color(unsigned red, unsigned green, unsigned blue) {
    static const unsigned levels[5] = {0, 64, 128, 192, 255};
    unsigned ri = minimum((red + 32) / 64, 4);
    unsigned gi = minimum((green + 32) / 64, 4);
    unsigned bi = minimum((blue + 32) / 64, 4);
    int dr = (int)red - (int)levels[ri];
    int dg = (int)green - (int)levels[gi];
    int db = (int)blue - (int)levels[bi];
    unsigned cube_distance = (unsigned)(3 * dr * dr + 4 * dg * dg + 2 * db * db);
    unsigned gray = (3 * red + 4 * green + 2 * blue + 4) / 9;
    unsigned gray_index = (gray * (PAL_GRAY_N - 1) + 127) / 255;
    gray = gray_index * 255 / (PAL_GRAY_N - 1);
    dr = (int)red - (int)gray;
    dg = (int)green - (int)gray;
    db = (int)blue - (int)gray;
    unsigned gray_distance = (unsigned)(3 * dr * dr + 4 * dg * dg + 2 * db * db);
    return (uint8_t)(gray_distance <= cube_distance ? PAL_GRAY + gray_index :
                     PAL_CUBE + ri * 25 + gi * 5 + bi);
}

/* Serpentine error diffusion preserves gradients and average photo colors in
 * the 256-color desktop. The two small error rows live only during conversion,
 * after decoding has returned, so they never overlap the decoder's stack use. */
static void quantize(const ImageDecoded *image) {
    int16_t rows[2][(IMAGE_MAX_DIMENSION + 2) * 3];
    int16_t *current = rows[0], *next = rows[1];
    unsigned row_entries = (image->width + 2) * 3;
    for (unsigned i = 0; i < row_entries; i++) current[i] = 0;
    for (unsigned y = 0; y < image->height; y++) {
        if (!(y & 7)) platform_poll();
        for (unsigned i = 0; i < row_entries; i++) next[i] = 0;
        int direction = y & 1 ? -1 : 1;
        int x = direction > 0 ? 0 : (int)image->width - 1;
        for (unsigned step = 0; step < image->width; step++, x += direction) {
            const uint8_t *source = image->pixels + (y * image->width + (unsigned)x) * image->channels;
            unsigned rgb[3] = {source[0], source[0], source[0]}, alpha = 255;
            if (image->channels >= 3) { rgb[1] = source[1]; rgb[2] = source[2]; }
            if (image->channels == 2 || image->channels == 4) alpha = source[image->channels - 1];
            unsigned at = ((unsigned)x + 1) * 3;
            for (unsigned channel = 0; channel < 3; channel++) {
                if (alpha != 255) {
                    unsigned background = (((unsigned)x / 12 + y / 12) & 1) ? 208 : 232;
                    rgb[channel] = (rgb[channel] * alpha + background * (255 - alpha) + 127) / 255;
                }
                int error = current[at + channel];
                int adjusted = (int)rgb[channel] + (error + (error >= 0 ? 8 : -8)) / 16;
                rgb[channel] = (unsigned)clamp(adjusted, 0, 255);
            }
            uint8_t index = palette_color(rgb[0], rgb[1], rgb[2]);
            IMAGE_MEMORY[y * image->width + (unsigned)x] = index;
            unsigned color = pal32[index];
            for (unsigned channel = 0; channel < 3; channel++) {
                int error = (int)rgb[channel] - (int)((color >> (16 - 8 * channel)) & 255);
                int offset = (int)at + (int)channel;
                current[offset + 3 * direction] += (int16_t)(7 * error);
                next[offset - 3 * direction] += (int16_t)(3 * error);
                next[offset] += (int16_t)(5 * error);
                next[offset + 3 * direction] += (int16_t)error;
            }
        }
        int16_t *swap = current; current = next; next = swap;
    }
}

static void set_status(void) {
    if (!viewer.loaded) return;
    copy(viewer.status, sizeof viewer.status, image_format_name(viewer.format));
    append(viewer.status, sizeof viewer.status, "  |  ");
    append_number(viewer.status, sizeof viewer.status, viewer.width);
    append(viewer.status, sizeof viewer.status, " x ");
    append_number(viewer.status, sizeof viewer.status, viewer.height);
    append(viewer.status, sizeof viewer.status, " px  |  ");
    append_number(viewer.status, sizeof viewer.status, viewer.scale);
    append(viewer.status, sizeof viewer.status, viewer.fit ? "% fit" : "%");
}

int image_viewer_open(const void *file, unsigned bytes, const char *name) {
    viewer.loaded = 0;
    viewer.width = viewer.height = viewer.format = 0;
    viewer.pan_x = viewer.pan_y = 0;
    viewer.fit = 1; viewer.scale = 100;
    copy(viewer.title, sizeof viewer.title, name && *name ? name : "Image Viewer");
    ImageDecoded image;
    ImagePollHook previous = image_decode_set_poll_hook(platform_poll);
    int error = image_decode(file, bytes, IMAGE_MEMORY + IMAGE_PIXELS_BYTES,
                             IMAGE_CAPACITY - IMAGE_PIXELS_BYTES, &image);
    image_decode_set_poll_hook(previous);
    if (error != IMAGE_OK) {
        copy(viewer.status, sizeof viewer.status, image_error_string(error));
        return error;
    }
    unsigned count = image.width * image.height;
    uint8_t *destination = IMAGE_MEMORY;
    if (!image.channels) {
        for (unsigned i = 0; i < count; i++) {
            if (!(i & 4095)) platform_poll();
            destination[i] = image.pixels[i];
        }
    } else quantize(&image);
    viewer.width = image.width; viewer.height = image.height; viewer.format = image.format;
    viewer.loaded = 1;
    set_status();
    return IMAGE_OK;
}

/* Layout: a toolbar (Fit / 1:1, zoom out, zoom level, zoom in), the
 * viewport, and a status bar. In Modern the toolbar continues the window
 * header. Scrollbars appear along the viewport when the image is larger
 * than it; they shrink the viewport, so geometry settles them first. */
static int tool_h(void) { return MODERN ? 44 : 34; }
static int status_h(void) { return MODERN ? 28 : 24; }
static int icon_size(void) { return MODERN ? 30 : 26; }

static void geometry(int w, int h) {
    viewer.client_w = w; viewer.client_h = h;
    int full_w = w, full_h = h - tool_h() - status_h();
    if (full_w < 1) full_w = 1;
    if (full_h < 1) full_h = 1;
    viewer.viewport_w = full_w; viewer.viewport_h = full_h;
    viewer.hbar = viewer.vbar = 0;
    if (!viewer.loaded) return;
    if (viewer.fit) {
        unsigned xs = (unsigned)full_w * 100 / viewer.width;
        unsigned ys = (unsigned)full_h * 100 / viewer.height;
        viewer.scale = minimum(xs, ys);
        /* Never shrink away a one-pixel side; tiny Paint drawings can grow. */
        if (viewer.scale < 1) viewer.scale = 1;
        if (viewer.scale > 400) viewer.scale = 400;
    }
    int sw = (int)(viewer.width * viewer.scale / 100);
    int sh = (int)(viewer.height * viewer.scale / 100);
    /* A bar on one side can make the other side overflow too. */
    for (int pass = 0; pass < 2; pass++) {
        viewer.hbar = sw > viewer.viewport_w && full_h > 2 * SB;
        viewer.vbar = sh > viewer.viewport_h && full_w > 2 * SB;
        viewer.viewport_w = full_w - (viewer.vbar ? SB : 0);
        viewer.viewport_h = full_h - (viewer.hbar ? SB : 0);
    }
    viewer.pan_x = clamp(viewer.pan_x, 0, sw > viewer.viewport_w ? sw - viewer.viewport_w : 0);
    viewer.pan_y = clamp(viewer.pan_y, 0, sh > viewer.viewport_h ? sh - viewer.viewport_h : 0);
    set_status();
}
static void regeometry(void) { geometry(viewer.client_w, viewer.client_h); }

/* Toolbar geometry, shared by drawing and hit testing: Fit and 1:1 form a
 * segmented pair (their icons alone look alike), then a divider, zoom out,
 * the zoom percentage and zoom in. Segment widths match kit_segmented. */
enum { VB_FIT, VB_ACTUAL, VB_ZOOM_OUT, VB_ZOOM_IN, VB_COUNT };
#define ZOOM_TEXT_W 52
typedef struct { int x, y, w, h; } ViewRect;
static const char *const tool_labels[2] = { "Fit", "1:1" };
static int segment_w(int index) { return uib_string_w(tool_labels[index]) + 24; }
static int segment_x(int x) { return x + (MODERN ? 12 : 8); }
static int segment_y(int y) { return y + (tool_h() - 1 - kit_segmented_h()) / 2; }
static int tool_divider_x(int x) {
    int seam = MODERN ? 2 : 1;
    return segment_x(x) + 2 * seam + seam + segment_w(VB_FIT) + segment_w(VB_ACTUAL) + 10;
}
static ViewRect tool_rect(int x, int y, int index) {
    int seam = MODERN ? 2 : 1, s = icon_size(), iy = y + (tool_h() - 1 - s) / 2;
    int inner = MODERN ? 2 : 1, sy = segment_y(y) + inner, sh = kit_segmented_h() - 2 * inner;
    switch (index) {
    case VB_FIT: return (ViewRect){segment_x(x) + seam, sy, segment_w(VB_FIT), sh};
    case VB_ACTUAL: return (ViewRect){segment_x(x) + 2 * seam + segment_w(VB_FIT), sy, segment_w(VB_ACTUAL), sh};
    case VB_ZOOM_OUT: return (ViewRect){tool_divider_x(x) + 11, iy, s, s};
    default: return (ViewRect){tool_divider_x(x) + 11 + s + ZOOM_TEXT_W, iy, s, s};
    }
}
static int tool_hit(int x, int y, int mx, int my) {
    for (int i = 0; i < VB_COUNT; i++) {
        ViewRect r = tool_rect(x, y, i);
        if (hit(mx, my, r.x, r.y, r.w, r.h)) return i;
    }
    return -1;
}
/* The desktop passes its hover tracker while the window is active, so
 * pointer motion over a control repaints it. Without one nothing hovers. */
static int (*hover_fn)(int x, int y, int w, int h);
void image_viewer_set_hover(int (*fn)(int x, int y, int w, int h)) { hover_fn = fn; }
static int hover(ViewRect r) { return hover_fn ? hover_fn(r.x, r.y, r.w, r.h) : 0; }

/* A flat toolbar tool with a toolbar glyph: no fill until hovered. */
static void icon_tool(ViewRect r, int glyph, int state) {
    int dis = state & KIT_DISABLED;
    if (!dis && (state & (KIT_HOVER | KIT_PRESSED)))
        kit_fill(r.x, r.y, r.w, r.h, MODERN ? ui_theme.r_control : KIT_R_SQUARE,
                 (state & KIT_PRESSED) ? TK_LINE_SUBTLE : TK_HOVER, 1);
    icon_draw(glyph, r.x + (r.w - ICON_SIZE) / 2, r.y + (r.h - ICON_SIZE) / 2,
              dis ? (MODERN ? TK_LINE_STRONG : TK_LINE) : TK_ICON);
}

static int message_lines(const char *text, int x, int y, int width, int height, int draw) {
    int lines = 0;
    while (*text && height >= UI_FONT_H && width > 16) {
        char line[160]; unsigned used = 0, split = 0;
        while (text[used] && used + 1 < sizeof line) {
            line[used] = text[used]; line[used + 1] = 0;
            if (ui_string_w(line) > width) break;
            if (text[used] == ' ') split = used;
            used++;
        }
        if (text[used] && split) used = split;
        if (!used) break;
        line[used] = 0;
        if (draw) kit_text_center(line, x, y, width, TK_TEXT2, 0);
        text += used;
        while (*text == ' ') text++;
        y += 20; height -= 20; lines++;
    }
    return lines;
}

/* No image, or an image that failed: a large icon, a title and a hint,
 * centred in the viewport. */
static void empty_state(int vx, int vy, int vw, int vh) {
    int failed = viewer.title[0] != 0;
    const char *heading = failed ? "Image could not be opened" : "No image open";
    const char *hint = failed ? (viewer.status[0] ? viewer.status : "The file could not be decoded.") :
        "Open an image from Files. JPEG, PNG, BMP, GIF and BaseOS Paint are supported.";
    int tw = vw - 48 < 420 ? vw - 48 : 420;
    int lines = message_lines(hint, 0, 0, tw, vh, 0);
    int need = 32 + 14 + UI_FONT_H + 8 + lines * 20, y = vy + (vh - need) / 2;
    if (y < vy + 8) y = vy + 8;
    if (vw < 64 || y + 32 > vy + vh) return;
    uint8_t well = MODERN ? TK_SUNKEN : TK_BASE;
    ui_icon_draw(failed ? UI_ICON_A_ERR : UI_ICON_A_IMG, vx + (vw - 32) / 2, y, 32, TK_ICON, well);
    y += 46;
    if (y + UI_FONT_H > vy + vh) return;
    kit_text_center(heading, vx + 8, y, vw - 16, TK_TEXT, 1);
    y += UI_FONT_H + 8;
    message_lines(hint, vx + (vw - tw) / 2, y, tw, vy + vh - y, 1);
}

void image_viewer_draw(int x, int y, int w, int h) {
    if (w < 1 || h < 1) return;
    geometry(w, h);
    draw_rect(x, y, w, h, ui_theme.content);
    if (w < 200 || h < tool_h() + status_h() + 16) return;
    int loaded = viewer.loaded;
    /* Toolbar */
    kit_toolbar(x, y, w, tool_h());
    int sel = !loaded ? -1 : viewer.fit ? VB_FIT : viewer.scale == 100 ? VB_ACTUAL : -1, hot = -1;
    for (int i = VB_FIT; i <= VB_ACTUAL; i++) if (loaded && i != sel && (hover(tool_rect(x, y, i)) & KIT_HOVER)) hot = i;
    kit_segmented(segment_x(x), segment_y(y), tool_labels, 2, sel, hot, 0, 0);
    if (!loaded) {
        /* Disabled: draw the labels over the track in the disabled ink. */
        for (int i = VB_FIT; i <= VB_ACTUAL; i++) {
            ViewRect r = tool_rect(x, y, i);
            kit_fill(r.x, r.y, r.w, r.h, MODERN ? 6 : KIT_R_SQUARE, MODERN ? TK_HOVER : TK_RAISED, 1);
            kit_text_center(tool_labels[i], r.x, kit_ty(r.y, r.h), r.w, TK_LINE_STRONG, 1);
        }
    }
    int dx = tool_divider_x(x), s = icon_size();
    kit_vseparator(dx, y + (tool_h() - 1 - s) / 2 + 4, s - 8);
    ViewRect out = tool_rect(x, y, VB_ZOOM_OUT), in = tool_rect(x, y, VB_ZOOM_IN);
    icon_tool(out, ICON_ZOOM_OUT, loaded && viewer.scale > zooms[0] ? hover(out) : KIT_DISABLED);
    icon_tool(in, ICON_ZOOM_IN, loaded && viewer.scale < zooms[ZOOM_COUNT - 1] ? hover(in) : KIT_DISABLED);
    char percent[16] = "";
    if (loaded) { append_number(percent, sizeof percent, viewer.scale); append(percent, sizeof percent, "%"); }
    else copy(percent, sizeof percent, "--");
    kit_text_center(percent, out.x + out.w, kit_ty(out.y, out.h), ZOOM_TEXT_W, loaded ? TK_TEXT : TK_LINE_STRONG, 0);
    /* Viewport */
    int vx = x, vy = y + tool_h();
    int vw = viewer.viewport_w, vh = viewer.viewport_h;
    uint8_t well = MODERN ? TK_SUNKEN : TK_BASE;
    draw_rect(vx, vy, w, h - tool_h() - status_h(), well);
    if (viewer.loaded) {
        int sw = (int)(viewer.width * viewer.scale / 100);
        int sh = (int)(viewer.height * viewer.scale / 100);
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
        int left = vx + (sw < vw ? (vw - sw) / 2 : -viewer.pan_x);
        int top = vy + (sh < vh ? (vh - sh) / 2 : -viewer.pan_y);
        /* A hairline frame separates a whole image from the well. */
        if (sw + 2 <= vw && sh + 2 <= vh) draw_frame(left - 1, top - 1, sw + 2, sh + 2, TK_LINE_SUBTLE);
        int x0 = left > vx ? left : vx, y0 = top > vy ? top : vy;
        int x1 = left + sw < vx + vw ? left + sw : vx + vw;
        int y1 = top + sh < vy + vh ? top + sh : vy + vh;
        /* Clip to both client viewport and framebuffer before direct writes. */
        x0 = clamp(x0, 0, fb_w); x1 = clamp(x1, 0, fb_w);
        y0 = clamp(y0, 0, fb_h); y1 = clamp(y1, 0, fb_h);
        for (int dy = y0; dy < y1; dy++) {
            unsigned sy = (unsigned)(dy - top) * viewer.height / (unsigned)sh;
            const uint8_t *source = IMAGE_MEMORY + sy * viewer.width;
            uint8_t *destination = fb + dy * fb_w;
            for (int dx2 = x0; dx2 < x1; dx2++) {
                unsigned sx = (unsigned)(dx2 - left) * viewer.width / (unsigned)sw;
                destination[dx2] = source[sx];
            }
        }
        if (viewer.vbar) kit_vscroll(vx + vw, vy, vh, viewer.pan_y, vh, sh, 0);
        if (viewer.hbar) kit_hscroll(vx, vy + vh, vw, viewer.pan_x, vw, sw, 0);
        if (viewer.vbar && viewer.hbar) draw_rect(vx + vw, vy + vh, SB, SB, MODERN ? well : TK_BASE);
    } else empty_state(vx, vy, vw, vh);
    /* Status bar */
    int sy = y + h - status_h();
    if (MODERN) {
        draw_hline(x, sy, w, TK_LINE_SUBTLE);
        kit_fill_bottom(x, sy + 1, w, status_h() - 1, ui_theme.r_window > 1 ? ui_theme.r_window - 1 : 0, ui_theme.statusbar);
    } else kit_statusbar(x, sy, w, status_h());
    int ty = kit_ty(sy + 1, status_h() - 1), pad = MODERN ? 12 : 8;
    const char *status = viewer.loaded ? viewer.status : viewer.title[0] ? "Not opened" : "No image open";
    int lw = kit_text(status, x + pad, ty, TK_TEXT2, w - 2 * pad, 0);
    const char *keys = "F: fit   1: actual size   +/-: zoom   Arrows: pan";
    int kw = kit_text_w(keys, 0);
    if (viewer.loaded && kw <= w - 3 * pad - lw) kit_text(keys, x + w - pad - kw, ty, TK_TEXT2, kw, 0);
}

static int set_zoom(unsigned scale, int fit) {
    if (!viewer.loaded) return 0;
    unsigned old = viewer.scale ? viewer.scale : 100;
    int center_x = viewer.pan_x + viewer.viewport_w / 2;
    int center_y = viewer.pan_y + viewer.viewport_h / 2;
    int old_w = (int)(viewer.width * old / 100), old_h = (int)(viewer.height * old / 100);
    viewer.scale = scale; viewer.fit = fit;
    if (fit) viewer.pan_x = viewer.pan_y = 0;
    else {
        viewer.pan_x = old_w < viewer.viewport_w ?
            ((int)(viewer.width * scale / 100) - viewer.viewport_w) / 2 :
            (int)((unsigned)center_x * scale / old) - viewer.viewport_w / 2;
        viewer.pan_y = old_h < viewer.viewport_h ?
            ((int)(viewer.height * scale / 100) - viewer.viewport_h) / 2 :
            (int)((unsigned)center_y * scale / old) - viewer.viewport_h / 2;
    }
    regeometry();
    return 1;
}
static int step_zoom(int direction) {
    if (!viewer.loaded) return 0;
    unsigned next = viewer.scale;
    if (direction > 0) {
        for (unsigned i = 0; i < ZOOM_COUNT; i++) if (zooms[i] > next) { next = zooms[i]; break; }
    } else {
        for (unsigned i = ZOOM_COUNT; i; i--) if (zooms[i - 1] < next) { next = zooms[i - 1]; break; }
    }
    return set_zoom(next, 0);
}
static int pan(int dx, int dy) {
    if (!viewer.loaded || viewer.fit) return 0;
    viewer.pan_x += dx; viewer.pan_y += dy;
    regeometry();
    return 1;
}
int image_viewer_key(int sc, char ch) {
    if (ch == 'f' || ch == 'F' || ch == '0') return set_zoom(viewer.scale, 1);
    if (ch == '1') return set_zoom(100, 0);
    if (ch == '+' || ch == '=') return step_zoom(1);
    if (ch == '-' || ch == '_') return step_zoom(-1);
    if (sc == KEY_LEFT) return pan(-48, 0);
    if (sc == KEY_RIGHT) return pan(48, 0);
    if (sc == KEY_UP) return pan(0, -48);
    if (sc == KEY_DOWN) return pan(0, 48);
    if (sc == 0x49) return pan(0, -viewer.viewport_h * 3 / 4); /* Page Up */
    if (sc == 0x51) return pan(0, viewer.viewport_h * 3 / 4); /* Page Down */
    if (sc == 0x47) { viewer.pan_x = viewer.pan_y = 0; return viewer.loaded; } /* Home */
    return 0;
}
/* A click on a scrollbar track centres the view on that point. */
static int scroll_click(int x, int y, int mx, int my) {
    int vx = x, vy = y + tool_h(), vw = viewer.viewport_w, vh = viewer.viewport_h;
    int sw = (int)(viewer.width * viewer.scale / 100), sh = (int)(viewer.height * viewer.scale / 100);
    if (viewer.vbar && hit(mx, my, vx + vw, vy, SB, vh)) {
        viewer.pan_y = (int)((unsigned)(my - vy) * (unsigned)sh / (unsigned)vh) - vh / 2;
        regeometry(); return 1;
    }
    if (viewer.hbar && hit(mx, my, vx, vy + vh, vw, SB)) {
        viewer.pan_x = (int)((unsigned)(mx - vx) * (unsigned)sw / (unsigned)vw) - vw / 2;
        regeometry(); return 1;
    }
    return 0;
}
int image_viewer_click(int x, int y, int w, int h, int mx, int my) {
    geometry(w, h);
    if (w < 200 || h < tool_h() + status_h() + 16) return 0;
    switch (tool_hit(x, y, mx, my)) {
    case VB_FIT: return set_zoom(viewer.scale, 1);
    case VB_ACTUAL: return set_zoom(100, 0);
    case VB_ZOOM_OUT: return step_zoom(-1);
    case VB_ZOOM_IN: return step_zoom(1);
    default: return viewer.loaded ? scroll_click(x, y, mx, my) : 0;
    }
}
int image_viewer_scroll(int lines) { return pan(0, clamp(lines, -32, 32) * 36); }
void image_viewer_close(void) {
    viewer.loaded = 0; viewer.title[0] = viewer.status[0] = 0;
    viewer.width = viewer.height = viewer.format = 0;
    viewer.pan_x = viewer.pan_y = 0; viewer.fit = 1; viewer.scale = 100;
}
int image_viewer_loaded(void) { return viewer.loaded; }
const char *image_viewer_title(void) { return viewer.title[0] ? viewer.title : "Image Viewer"; }
const char *image_viewer_status(void) { return viewer.status[0] ? viewer.status : "No image open"; }
