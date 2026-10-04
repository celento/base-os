/* A small image client for the existing cooperative BaseOS window manager. */
#include "app.h"
#include "image_viewer.h"
#include "layout.h"
#include "platform.h"

/* The integration commit adds these to the shared E820-checked memory map. */
#ifndef IMAGE_BASE
#define IMAGE_BASE 0x1900000
#endif
#ifndef IMAGE_CAPACITY
#define IMAGE_CAPACITY 0x700000
#endif
#define IMAGE_PIXELS_BYTES IMAGE_MAX_PIXELS
#define TOOL_H 40
#define FOOT_H 48
#define PAD 10
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

static void geometry(int w, int h) {
    viewer.viewport_w = w - 2 * PAD;
    viewer.viewport_h = h - TOOL_H - FOOT_H;
    if (viewer.viewport_w < 1) viewer.viewport_w = 1;
    if (viewer.viewport_h < 1) viewer.viewport_h = 1;
    if (!viewer.loaded) return;
    if (viewer.fit) {
        unsigned xs = (unsigned)viewer.viewport_w * 100 / viewer.width;
        unsigned ys = (unsigned)viewer.viewport_h * 100 / viewer.height;
        viewer.scale = minimum(xs, ys);
        /* Never shrink away a one-pixel side; tiny Paint drawings can grow. */
        if (viewer.scale < 1) viewer.scale = 1;
        if (viewer.scale > 400) viewer.scale = 400;
    }
    int sw = (int)(viewer.width * viewer.scale / 100);
    int sh = (int)(viewer.height * viewer.scale / 100);
    viewer.pan_x = clamp(viewer.pan_x, 0, sw > viewer.viewport_w ? sw - viewer.viewport_w : 0);
    viewer.pan_y = clamp(viewer.pan_y, 0, sh > viewer.viewport_h ? sh - viewer.viewport_h : 0);
    set_status();
}

static void button(int x, int y, int w, const char *label, int selected, int enabled) {
    uint8_t background = selected ? app_accent : app_chrome;
    uint8_t text = !enabled ? app_text_dim : selected ? COLOR_WHITE : app_text;
    draw_round_rect(x, y, w, 28, 6, background);
    draw_round_frame(x, y, w, 28, 6, selected ? app_accent : app_chrome_dk);
    draw_string(label, x + (w - ui_string_w(label)) / 2, y + 5, text);
}

static void message_lines(const char *text, int x, int y, int width, int height) {
    while (*text && height >= 20 && width > 16) {
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
        draw_string(line, x, y, gfx_gray(200));
        text += used;
        while (*text == ' ') text++;
        y += 22; height -= 22;
    }
}

void image_viewer_draw(int x, int y, int w, int h) {
    if (w < 1 || h < 1) return;
    geometry(w, h);
    uint8_t well = gfx_gray(30);
    draw_rect(x, y, w, h, app_chrome);
    button(x + 10, y + 6, 44, "Fit", viewer.fit && viewer.loaded, viewer.loaded);
    button(x + 60, y + 6, 44, "1:1", !viewer.fit && viewer.scale == 100 && viewer.loaded, viewer.loaded);
    button(x + 110, y + 6, 30, "-", 0, viewer.loaded);
    char percent[16] = "";
    if (viewer.loaded) { append_number(percent, sizeof percent, viewer.scale); append(percent, sizeof percent, "%"); }
    else copy(percent, sizeof percent, "--");
    draw_string(percent, x + 146 + (58 - ui_string_w(percent)) / 2, y + 11, app_text_dim);
    button(x + 210, y + 6, 30, "+", 0, viewer.loaded);
    if (w >= 500) draw_string_clip("IMAGE VIEWER", x + 260, y + 11, app_text_dim, x + w - PAD);
    int vx = x + PAD, vy = y + TOOL_H;
    int vw = viewer.viewport_w, vh = viewer.viewport_h;
    draw_rect(vx, vy, vw, vh, well);
    if (viewer.loaded) {
        int sw = (int)(viewer.width * viewer.scale / 100);
        int sh = (int)(viewer.height * viewer.scale / 100);
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
        int left = vx + (sw < vw ? (vw - sw) / 2 : -viewer.pan_x);
        int top = vy + (sh < vh ? (vh - sh) / 2 : -viewer.pan_y);
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
            for (int dx = x0; dx < x1; dx++) {
                unsigned sx = (unsigned)(dx - left) * viewer.width / (unsigned)sw;
                destination[dx] = source[sx];
            }
        }
    } else {
        const char *heading = viewer.title[0] ? "Image could not be opened" : "Open an image from Files";
        draw_string_clip(heading, vx + 16, vy + 22, COLOR_WHITE, vx + vw - 12);
        message_lines(viewer.status[0] ? viewer.status : "JPEG, PNG, BMP, GIF and BaseOS Paint",
                      vx + 16, vy + 48, vw - 32, vh - 56);
    }
    const char *status = viewer.status[0] ? viewer.status : "No image open";
    draw_string_clip(status, x + PAD, y + h - FOOT_H + 7, app_text, x + w - PAD);
    draw_string_clip("F: fit   1: actual   +/-: zoom   Arrows / wheel: pan",
                     x + PAD, y + h - 22, app_text_dim, x + w - PAD);
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
    geometry(viewer.viewport_w + 2 * PAD, viewer.viewport_h + TOOL_H + FOOT_H);
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
    geometry(viewer.viewport_w + 2 * PAD, viewer.viewport_h + TOOL_H + FOOT_H);
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
int image_viewer_click(int x, int y, int w, int h, int mx, int my) {
    geometry(w, h);
    if (hit(mx, my, x + 10, y + 6, 44, 28)) return set_zoom(viewer.scale, 1);
    if (hit(mx, my, x + 60, y + 6, 44, 28)) return set_zoom(100, 0);
    if (hit(mx, my, x + 110, y + 6, 30, 28)) return step_zoom(-1);
    if (hit(mx, my, x + 210, y + 6, 30, 28)) return step_zoom(1);
    return 0;
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
