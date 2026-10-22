/* Gradient and picture wallpapers. The screen has 256 colors, so a gradient
 * is a ramp of PAL_WALL_N palette entries with an 8x8 ordered dither between
 * neighbors: 64 steps per pair keeps even dark, wide gradients free of bands.
 * Pictures use only the cube and gray ramp, which no theme rewrites. */
#include "ui/wallpaper.h"
#include "ui/theme.h"
#include "kernel/gfx.h"
#include "kernel/layout.h"
#include "kernel/platform.h"
#include "lib/image_decode.h"

#define STEP 64
#define SCALE ((PAL_WALL_N - 1) * STEP)

static const uint8_t bayer[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
};

typedef struct { const char *name; uint32_t stops[3]; } GradientPreset;
/* Entry 0 is a placeholder for the accent gradient. */
static const GradientPreset presets[UI_GRADIENT_N] = {
    {"Accent",   {0, 0, 0}},
    {"Dawn",     {0xFBD8BC, 0xF1BCCB, 0xB8B6E8}},
    {"Lagoon",   {0xB4E4DC, 0x5FA9C2, 0x2D5B8A}},
    {"Dusk",     {0x272958, 0x8A4B7F, 0xEE9D6E}},
    {"Meadow",   {0xEAF3D6, 0xA9D3A2, 0x5A9883}},
    {"Fog",      {0xF3F5F8, 0xD5DBE3, 0xA8B3C2}},
    {"Aurora",   {0x10263A, 0x1D6068, 0x6CC49B}},
    {"Midnight", {0x0B1022, 0x1F2B4D, 0x45417F}},
};

const char *ui_gradient_name(int gradient) {
    return gradient >= 0 && gradient < UI_GRADIENT_N ? presets[gradient].name : "";
}

void ui_gradient_stops(int gradient, uint32_t stops[3]) {
    if (gradient <= 0 || gradient >= UI_GRADIENT_N) {
        stops[0] = ui_token_rgb(TK_ACCENT_300);
        stops[1] = ui_token_rgb(TK_ACCENT);
        stops[2] = ui_token_rgb(TK_ACCENT_700);
        return;
    }
    for (int i = 0; i < 3; i++) stops[i] = presets[gradient].stops[i];
}

static int channel(uint32_t c, int shift) { return (int)((c >> shift) & 255); }
static int luma(uint32_t c) { return channel(c, 16) * 3 + channel(c, 8) * 6 + channel(c, 0); }

/* A radial gradient glows: its lighter end sits at the center. */
static int radial_flip;

/* Color at t in 0..SCALE along the three stops. */
static uint32_t gradient_at(const uint32_t stops[3], int t) {
    int half = SCALE / 2, k = t < half ? 0 : 1, f = t < half ? t : t - half;
    uint32_t a = stops[k], b = stops[k + 1], out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        int v = channel(a, shift) + (channel(b, shift) - channel(a, shift)) * f / half;
        out |= (uint32_t)v << shift;
    }
    return out;
}

void ui_gradient_install(int gradient) {
    uint32_t stops[3];
    ui_gradient_stops(gradient, stops);
    for (int i = 0; i < PAL_WALL_N; i++) gfx_set_color(PAL_WALL + i, gradient_at(stops, i * STEP));
    radial_flip = luma(stops[0]) < luma(stops[2]);
    gfx_set_color(TK_DESKTOP, stops[1]);
    gfx_pal_commit();
}

static unsigned isqrt(unsigned v) {
    unsigned r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* Position along the gradient, 0..SCALE, of (x, y) in a w x h area. Linear
 * runs top to bottom, diagonal from the top-left corner to the bottom-right
 * one, radial out from a point a little above the center (or in toward it
 * when flipped). */
typedef struct { int shape, w, h, cx, cy, flip; unsigned span; } Mapping;

static void mapping_init(Mapping *m, int w, int h, int shape, int flip) {
    m->shape = shape; m->w = w; m->h = h; m->flip = flip;
    m->cx = w / 2; m->cy = h * 2 / 5;
    if (shape == UI_SHAPE_DIAGONAL) m->span = (unsigned)(w * w + h * h);
    else if (shape == UI_SHAPE_RADIAL) {
        unsigned dx = (unsigned)(w - m->cx), dy = (unsigned)(h - m->cy);
        m->span = isqrt((dx * dx + dy * dy) * 64);
    } else m->span = (unsigned)(h > 1 ? h - 1 : 1);
    if (!m->span) m->span = 1;
}

static int mapping_t(const Mapping *m, int x, int y) {
    unsigned t;
    if (m->shape == UI_SHAPE_DIAGONAL) t = ((unsigned)(x * m->w + y * m->h)) * SCALE / m->span;
    else if (m->shape == UI_SHAPE_RADIAL) {
        int dx = x - m->cx, dy = y - m->cy;
        t = isqrt((unsigned)(dx * dx + dy * dy) * 64) * SCALE / m->span;
        if (t > SCALE) t = SCALE;
        if (m->flip) t = SCALE - t;
    } else t = (unsigned)y * SCALE / m->span;
    return t > SCALE ? SCALE : (int)t;
}

void ui_gradient_fill(int x, int y, int w, int h, int shape) {
    Mapping m;
    mapping_init(&m, w, h, shape, radial_flip);
    for (int j = 0; j < h; j++) {
        int sy = y + j;
        if (sy < 0 || sy >= fb_h) continue;
        if (!(j & 63)) platform_poll();
        uint8_t *row = fb + sy * fb_w;
        const uint8_t *b = bayer[sy & 7];
        int row_t = shape == UI_SHAPE_LINEAR ? mapping_t(&m, 0, j) : 0;
        for (int i = 0; i < w; i++) {
            int sx = x + i;
            if (sx < 0 || sx >= fb_w) continue;
            int t = shape == UI_SHAPE_LINEAR ? row_t : mapping_t(&m, i, j);
            int k = t / STEP, f = t % STEP;
            row[sx] = (uint8_t)(PAL_WALL + k + (f > b[sx & 7]));
        }
    }
}

void ui_gradient_swatch(int x, int y, int w, int h, int gradient, int shape, int round) {
    uint32_t stops[3];
    ui_gradient_stops(gradient, stops);
    Mapping m;
    mapping_init(&m, w, h, shape, luma(stops[0]) < luma(stops[2]));
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int dx = 2 * i + 1 - w, dy = 2 * j + 1 - h;
            if (round && dx * dx + dy * dy > w * w) continue;
            uint32_t c = gradient_at(stops, mapping_t(&m, i, j));
            int d = (bayer[(y + j) & 7][(x + i) & 7] - 32) * 3 / 4;
            put_pixel(x + i, y + j, gfx_rgb(channel(c, 16) + d, channel(c, 8) + d, channel(c, 0) + d));
        }
}

/* ---------- Pictures ---------- */

/* Decoding borrows the image region past the viewer's pixels: the viewer
 * uses that part only while image_viewer_open decodes, and both run in the
 * same cooperative turn, never at once. */
#define WORKSPACE ((uint8_t *)IMAGE_BASE + IMAGE_MAX_PIXELS)
#define WORKSPACE_BYTES (IMAGE_CAPACITY - IMAGE_MAX_PIXELS)

/* Nearest cube or gray entry, as the image viewer quantizes. */
static uint8_t fixed_color(int r, int g, int b) {
    static const int levels[5] = {0, 64, 128, 192, 255};
    int ri = (r + 32) / 64, gi = (g + 32) / 64, bi = (b + 32) / 64;
    if (ri > 4) ri = 4;
    if (gi > 4) gi = 4;
    if (bi > 4) bi = 4;
    int dr = r - levels[ri], dg = g - levels[gi], db = b - levels[bi];
    int cube = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
    int gray = (3 * r + 4 * g + 2 * b + 4) / 9;
    int gray_index = (gray * (PAL_GRAY_N - 1) + 127) / 255;
    gray = gray_index * 255 / (PAL_GRAY_N - 1);
    dr = r - gray; dg = g - gray; db = b - gray;
    int gray_distance = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
    return (uint8_t)(gray_distance <= cube ? PAL_GRAY + gray_index : PAL_CUBE + ri * 25 + gi * 5 + bi);
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Scale to cover w x h (crop the overflow, centered) with box sampling when
 * shrinking, then Floyd-Steinberg dither into out. */
static void picture_convert(const ImageDecoded *image, uint8_t *out, int w, int h) {
    int iw = (int)image->width, ih = (int)image->height, ch = (int)image->channels;
    /* Source span per destination pixel, in 1/256 source pixels. */
    unsigned step = (unsigned)iw * (unsigned)h > (unsigned)ih * (unsigned)w
        ? (unsigned)ih * 256u / (unsigned)h : (unsigned)iw * 256u / (unsigned)w;
    if (!step) step = 1;
    int ox = (int)(((unsigned)iw * 256u - step * (unsigned)w) / 2);
    int oy = (int)(((unsigned)ih * 256u - step * (unsigned)h) / 2);
    int box = (int)(step / 256u);
    if (box < 1) box = 1;
    if (box > 4) box = 4;
    uint32_t back = ui_token_rgb(TK_DESKTOP);
    int16_t rows[2][(1280 + 2) * 3];
    int16_t *current = rows[0], *next = rows[1];
    for (int i = 0; i < (w + 2) * 3; i++) current[i] = 0;
    for (int y = 0; y < h; y++) {
        if (!(y & 7)) platform_poll();
        for (int i = 0; i < (w + 2) * 3; i++) next[i] = 0;
        int sy0 = (int)(((unsigned)oy + (unsigned)y * step) >> 8);
        for (int x = 0; x < w; x++) {
            int sx0 = (int)(((unsigned)ox + (unsigned)x * step) >> 8);
            int sum[4] = {0, 0, 0, 0}, n = 0;
            for (int by = 0; by < box; by++)
                for (int bx = 0; bx < box; bx++) {
                    int sx = sx0 + bx, sy = sy0 + by;
                    if (sx >= iw) sx = iw - 1;
                    if (sy >= ih) sy = ih - 1;
                    const uint8_t *p = image->pixels + ((unsigned)sy * (unsigned)iw + (unsigned)sx) * (unsigned)(ch ? ch : 1);
                    if (!ch) {
                        uint32_t c = pal32[p[0]];
                        sum[0] += channel(c, 16); sum[1] += channel(c, 8); sum[2] += channel(c, 0); sum[3] += 255;
                    } else {
                        int a = ch == 2 || ch == 4 ? p[ch - 1] : 255;
                        sum[0] += p[0]; sum[1] += ch >= 3 ? p[1] : p[0]; sum[2] += ch >= 3 ? p[2] : p[0];
                        sum[3] += a;
                    }
                    n++;
                }
            int rgb[3], a = sum[3] / n, at = (x + 1) * 3;
            for (int c = 0; c < 3; c++) {
                int v = sum[c] / n, bg = channel(back, 16 - 8 * c);
                if (a != 255) v = (v * a + bg * (255 - a) + 127) / 255;
                int e = current[at + c];
                rgb[c] = clamp255(v + (e + (e >= 0 ? 8 : -8)) / 16);
            }
            uint8_t index = fixed_color(rgb[0], rgb[1], rgb[2]);
            out[y * w + x] = index;
            uint32_t got = pal32[index];
            for (int c = 0; c < 3; c++) {
                int e = rgb[c] - channel(got, 16 - 8 * c);
                current[at + c + 3] += (int16_t)(7 * e);
                next[at + c - 3] += (int16_t)(3 * e);
                next[at + c] += (int16_t)(5 * e);
                next[at + c + 3] += (int16_t)e;
            }
        }
        int16_t *swap = current; current = next; next = swap;
    }
}

int ui_picture_render(const void *file, unsigned bytes) {
    if (fb_w > 1280) return IMAGE_TOO_LARGE;
    ImageDecoded image;
    ImagePollHook previous = image_decode_set_poll_hook(platform_poll);
    int error = image_decode(file, bytes, WORKSPACE, WORKSPACE_BYTES, &image);
    image_decode_set_poll_hook(previous);
    if (error == IMAGE_OK && (!image.width || !image.height)) error = IMAGE_BAD_FILE;
    if (error == IMAGE_OK) picture_convert(&image, fb, fb_w, fb_h);
    return error;
}
