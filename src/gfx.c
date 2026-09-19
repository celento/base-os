#include "gfx.h"
#include "font.h"
#include "layout.h"
#include "fs.h"

int fb_w = 1280;
int fb_h = 720;
int fb_bpp = 32;
int fb_pitch = 5120;
uint8_t *fb;
uint8_t *lfb;
uint32_t pal32[256];

static uint16_t pal16[256];
static uint8_t pal_shade1[256];
static uint8_t pal_shade2[256];
static void (*flip_hook)(void);
static void dac_load(void);
static struct { uint32_t key; uint8_t index; } rgb_cache[8192];
static int presented_valid;
static void palette_changed(void) {
    for(unsigned i=0;i<8192;i++)rgb_cache[i].key=0;
    presented_valid=0;
}

static const uint32_t pal_base[16] = {
    0x000000, 0x3C3C3C, 0x808080, 0xD4D4D4,
    0xD83A3A, 0xF08A2A, 0xF2C94C, 0x3FA35B,
    0x2AA7C8, 0x3B6FE0, 0xA84AC0, 0x8B5A2B,
    0x2F6B4A, 0x24407A, 0x7A3048, 0xFFFFFF
};

static const uint8_t bayer4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5}
};

static const uint8_t cube_lv[5] = { 0, 64, 128, 192, 255 };

static int clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void pal_finish(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t rgb = pal32[i];
        unsigned r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
        pal16[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    for (int i = 0; i < 256; i++) {
        pal_shade1[i] = gfx_darker((uint8_t)i, 78);
        pal_shade2[i] = gfx_darker((uint8_t)i, 58);
    }
}

uint8_t gfx_rgb(int r, int g, int b) {
    r = clamp255(r);
    g = clamp255(g);
    b = clamp255(b);
    if (r == 0 && g == 0 && b == 0) return COLOR_BLACK;
    if (r == 255 && g == 255 && b == 255) return COLOR_WHITE;
    uint32_t key=((uint32_t)r<<16)|((uint32_t)g<<8)|(unsigned)b|0x1000000;
    unsigned slot=(key*2654435761u)>>19;
    if(rgb_cache[slot].key==key)return rgb_cache[slot].index;
    int best = 0;
    int bd = 0x7FFFFFFF;
    for (int i = 0; i < PAL_CUBE + 125; i++) {
        uint32_t c = pal32[i];
        int dr = r - (int)((c >> 16) & 255);
        int dg = g - (int)((c >> 8) & 255);
        int db = b - (int)(c & 255);
        int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    rgb_cache[slot].key=key;rgb_cache[slot].index=(uint8_t)best;
    return (uint8_t)best;
}

static uint32_t lerp_rgb(uint32_t a, uint32_t b, int t, int n) {
    int r0 = (a >> 16) & 255, g0 = (a >> 8) & 255, b0 = a & 255;
    int r1 = (b >> 16) & 255, g1 = (b >> 8) & 255, b1 = b & 255;
    int r = r0 + (r1 - r0) * t / n;
    int g = g0 + (g1 - g0) * t / n;
    int bl = b0 + (b1 - b0) * t / n;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

void gfx_set_ramp(int base, int n, uint32_t c0, uint32_t c1) {
    palette_changed();
    for (int i = 0; i < n; i++)
        pal32[base + i] = lerp_rgb(c0, c1, i, n > 1 ? n - 1 : 1);
}

void gfx_pal_commit(void) {
    palette_changed();
    pal_finish();
    if (fb_bpp == 8)
        dac_load();
}

uint8_t gfx_mix(uint8_t a, uint8_t b, int t) {
    uint32_t c = lerp_rgb(pal32[a], pal32[b], t, 256);
    return gfx_rgb((c >> 16) & 255, (c >> 8) & 255, c & 255);
}

uint8_t gfx_darker(uint8_t c, int pct) {
    uint32_t v = pal32[c];
    return gfx_rgb((int)((v >> 16) & 255) * pct / 100,
                   (int)((v >> 8) & 255) * pct / 100,
                   (int)(v & 255) * pct / 100);
}

uint8_t gfx_lighter(uint8_t c, int pct) {
    uint32_t v = pal32[c];
    int r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
    return gfx_rgb(r + (255 - r) * pct / 100,
                   g + (255 - g) * pct / 100,
                   b + (255 - b) * pct / 100);
}

int gfx_luma(uint8_t c) {
    uint32_t v = pal32[c];
    unsigned r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
    return (int)((r * 299u + g * 587u + b * 114u) / 1000u);
}

uint8_t gfx_gray(int v) {
    v = clamp255(v);
    return (uint8_t)(PAL_GRAY + (v * (PAL_GRAY_N - 1) + 127) / 255);
}

static void pal_build(void) {
    for (int i = 0; i < 16; i++)
        pal32[i] = pal_base[i];
    for (int i = 0; i < PAL_GRAY_N; i++) {
        unsigned v = (unsigned)(i * 255 / (PAL_GRAY_N - 1));
        pal32[PAL_GRAY + i] = (v << 16) | (v << 8) | v;
    }
    gfx_set_ramp(PAL_DESK, PAL_DESK_N, 0x4C8FD6, 0x0E2A52);
    gfx_set_ramp(PAL_ACCENT, PAL_ACCENT_N, 0x8AB4F0, 0x143C80);
    for (int i = 0; i < 125; i++) {
        pal32[PAL_CUBE + i] = ((uint32_t)cube_lv[i / 25] << 16) |
                              ((uint32_t)cube_lv[(i / 5) % 5] << 8) |
                              (uint32_t)cube_lv[i % 5];
    }
    for (int i = PAL_CUBE + 125; i < 256; i++)
        pal32[i] = 0;
    pal_finish();
}

static void dac_load(void) {
#ifndef GFX_HOST_TEST
    if (fb_bpp != 8)
        return;
    asm volatile("outb %0, %1" : : "a"((uint8_t)0), "Nd"((uint16_t)0x3C8));
    for (int i = 0; i < 256; i++) {
        uint32_t rgb = pal32[i];
        uint8_t r = (uint8_t)(((rgb >> 16) & 255) >> 2);
        uint8_t g = (uint8_t)(((rgb >> 8) & 255) >> 2);
        uint8_t b = (uint8_t)((rgb & 255) >> 2);
        asm volatile("outb %0, %1" : : "a"(r), "Nd"((uint16_t)0x3C9));
        asm volatile("outb %0, %1" : : "a"(g), "Nd"((uint16_t)0x3C9));
        asm volatile("outb %0, %1" : : "a"(b), "Nd"((uint16_t)0x3C9));
    }
#endif
}

void gfx_init(uint8_t *backbuf, uint8_t *linear, int w, int h, int bpp, int pitch) {
    fb = backbuf;
    lfb = linear;
    fb_w = w;
    fb_h = h;
    fb_bpp = bpp;
    fb_pitch = pitch;
    pal_build();
    dac_load();
    for (int i = 0; i < w * h; i++)
        fb[i] = 0;
}

void gfx_set_flip_hook(void (*fn)(void)) {
    flip_hook = fn;
}

void put_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= fb_w || y < 0 || y >= fb_h)
        return;
    fb[y * fb_w + x] = color;
}

uint8_t get_pixel(int x, int y) {
    if (x < 0 || x >= fb_w || y < 0 || y >= fb_h)
        return 0;
    return fb[y * fb_w + x];
}

static int clip_rect(int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > fb_w) *w = fb_w - *x;
    if (*y + *h > fb_h) *h = fb_h - *y;
    return *w > 0 && *h > 0;
}

void draw_rect(int x, int y, int w, int h, uint8_t color) {
    if (!clip_rect(&x, &y, &w, &h))
        return;
    for (int i = 0; i < h; i++) {
        uint8_t *row = fb + (y + i) * fb_w + x;
        kmemset(row,color,w);
    }
}

void draw_frame(int x, int y, int w, int h, uint8_t color) {
    draw_rect(x, y, w, 1, color);
    draw_rect(x, y + h - 1, w, 1, color);
    draw_rect(x, y, 1, h, color);
    draw_rect(x + w - 1, y, 1, h, color);
}

void draw_hline(int x, int y, int w, uint8_t color) {
    draw_rect(x, y, w, 1, color);
}

void draw_vline(int x, int y, int h, uint8_t color) {
    draw_rect(x, y, 1, h, color);
}

void draw_line(int x1, int y1, int x2, int y2, uint8_t color) {
    if (y1 == y2) {
        if (x2 < x1) { int t = x1; x1 = x2; x2 = t; }
        draw_rect(x1, y1, x2 - x1 + 1, 1, color);
    } else if (x1 == x2) {
        if (y2 < y1) { int t = y1; y1 = y2; y2 = t; }
        draw_rect(x1, y1, 1, y2 - y1 + 1, color);
    } else {
        int dx = x2 > x1 ? x2 - x1 : x1 - x2;
        int dy = y2 > y1 ? y2 - y1 : y1 - y2;
        int sx = x1 < x2 ? 1 : -1;
        int sy = y1 < y2 ? 1 : -1;
        int err = dx - dy;
        while (1) {
            put_pixel(x1, y1, color);
            if (x1 == x2 && y1 == y2)
                break;
            int e2 = err * 2;
            if (e2 > -dy) { err -= dy; x1 += sx; }
            if (e2 < dx) { err += dx; y1 += sy; }
        }
    }
}

void draw_checker(int x, int y, int w, int h, uint8_t a, uint8_t b) {
    if (!clip_rect(&x, &y, &w, &h))
        return;
    for (int j = 0; j < h; j++) {
        uint8_t *row = fb + (y + j) * fb_w + x;
        for (int i = 0; i < w; i++)
            row[i] = ((x + i + y + j) & 1) ? a : b;
    }
}

void draw_dither(int x, int y, int w, int h) {
    draw_checker(x, y, w, h, COLOR_BLACK, COLOR_WHITE);
}

static int round_inset(int h, int r, int row, int bottom);
static int corner_cov(int cx,int cy,int r);
static void blend_cov(int x,int y,uint8_t color,int cov);

static void ramp_row(uint8_t *out4, int base, int n, int p0, int p1, int t, int steps, int row) {
    int pos = p0 + (p1 - p0) * t / (steps > 0 ? steps : 1);
    int max = (n - 1) * 16;
    if (pos < 0) pos = 0;
    if (pos > max) pos = max;
    int idx = pos >> 4;
    int frac = pos & 15;
    for (int k = 0; k < 4; k++) {
        int up = (idx < n - 1) && frac > bayer4[row & 3][k];
        out4[k] = (uint8_t)(base + idx + up);
    }
}

void draw_ramp_round(int x, int y, int w, int h, int r, int round_bottom,
                     int base, int n, int p0, int p1) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int steps = h > 1 ? h - 1 : 1;
    for (int j = 0; j < h; j++) {
        int corner_row=j<r?j:(round_bottom&&j>=h-r?h-1-j:-1);
        int in = corner_row>=0?r:0;
        int yy = y + j;
        if (yy < 0 || yy >= fb_h)
            continue;
        uint8_t c4[4];
        ramp_row(c4, base, n, p0, p1, j, steps, yy);
        int x0 = x + in, x1 = x + w - in;
        if (x0 < 0) x0 = 0;
        if (x1 > fb_w) x1 = fb_w;
        uint8_t *row = fb + yy * fb_w;
        for (int i = x0; i < x1; i++)
            row[i] = c4[i & 3];
        if(corner_row>=0)for(int cx=0;cx<r;cx++){
            int cov=corner_cov(cx,corner_row,r);
            int left=x+cx,right=x+w-1-cx;
            blend_cov(left,yy,c4[left&3],cov);blend_cov(right,yy,c4[right&3],cov);
        }
    }
}

void draw_ramp(int x, int y, int w, int h, int base, int n, int p0, int p1) {
    draw_ramp_round(x, y, w, h, 0, 0, base, n, p0, p1);
}

void draw_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    int steps = h > 1 ? h - 1 : 1;
    uint8_t last = 0;
    uint32_t lastc = 0xFFFFFFFFu;
    for (int j = 0; j < h; j++) {
        uint32_t c = lerp_rgb(top, bottom, j, steps);
        if (c != lastc) {
            last = gfx_rgb((c >> 16) & 255, (c >> 8) & 255, c & 255);
            lastc = c;
        }
        draw_rect(x, y + j, w, 1, last);
    }
}

void draw_vgradient_round(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom, int round_bottom) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int steps = h > 1 ? h - 1 : 1;
    for (int j = 0; j < h; j++) {
        int in = round_inset(h, r, j, round_bottom);
        uint32_t c = lerp_rgb(top, bottom, j, steps);
        draw_rect(x + in, y + j, w - 2 * in, 1, gfx_rgb((c >> 16) & 255, (c >> 8) & 255, c & 255));
    }
}

static int round_inset(int h, int r, int row, int bottom);

/* 4x4 supersample coverage (0..16) of pixel (cx,cy) inside a circle of
 * radius r centred at (r,r), measured from a corner box origin. */
static int corner_cov_raw(int cx, int cy, int r) {
    int cov = 0;
    int cen = r * 8;
    int rr = (r * 8) * (r * 8);
    for (int sy = 0; sy < 4; sy++) {
        int py = cy * 8 + 1 + sy * 2;
        int dy = cen - py;
        for (int sx = 0; sx < 4; sx++) {
            int px = cx * 8 + 1 + sx * 2;
            int dx = cen - px;
            if (dx * dx + dy * dy <= rr)
                cov++;
        }
    }
    return cov;
}

static uint8_t coverage[17][16][16], coverage_ready[17];
static int corner_cov(int x,int y,int r) {
    if(r<1||r>16)return corner_cov_raw(x,y,r);
    if(!coverage_ready[r]){
        for(int cy=0;cy<r;cy++)for(int cx=0;cx<r;cx++)coverage[r][cy][cx]=corner_cov_raw(cx,cy,r);
        coverage_ready[r]=1;
    }
    return coverage[r][y][x];
}
/* Preserve the real pixels behind all four corners before app content draws. */
void gfx_window_corners(int x,int y,int w,int h,uint8_t saved[256],int finish,uint8_t border) {
    for(int corner=0;corner<4;corner++)for(int cy=0;cy<8;cy++)for(int cx=0;cx<8;cx++){
        int px=(corner&1)?x+w-1-cx:x+cx,py=(corner&2)?y+h-1-cy:y+cy;
        int i=corner*64+cy*8+cx;
        if(!finish){saved[i]=get_pixel(px,py);continue;}
        int cov=corner_cov(cx,cy,8);
        if(cov<16)put_pixel(px,py,cov?gfx_mix(saved[i],border,cov*16):saved[i]);
    }
}

static void blend_span(int x, int y, int w, uint8_t color) {
    draw_rect(x, y, w, 1, color);
}

static void blend_cov(int x, int y, uint8_t color, int cov) {
    if (x < 0 || x >= fb_w || y < 0 || y >= fb_h || cov <= 0)
        return;
    uint8_t *d = &fb[y * fb_w + x];
    if (cov >= 16)
        *d = color;
    else
        *d = gfx_mix(*d, color, cov * 16);
}

static void corner_aa(int x, int y, int r, int color, int flip_x, int flip_y) {
    for (int cy = 0; cy < r; cy++) {
        for (int cx = 0; cx < r; cx++) {
            int cov = corner_cov(cx, cy, r);
            if (cov <= 0)
                continue;
            int px = flip_x ? x + r - 1 - cx : x + cx;
            int py = flip_y ? y + r - 1 - cy : y + cy;
            blend_cov(px, py, (uint8_t)color, cov);
        }
    }
}

static void round_fill(int x, int y, int w, int h, int r, uint8_t color, int bottom) {
    if (r < 0) r = 0;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int top_r = r;
    int bot_r = bottom ? r : 0;
    for (int j = 0; j < h; j++) {
        if (j < top_r) {
            blend_span(x + r, y + j, w - 2 * r, color);
        } else if (bot_r && j >= h - bot_r) {
            blend_span(x + r, y + j, w - 2 * r, color);
        } else {
            blend_span(x, y + j, w, color);
        }
    }
    corner_aa(x, y, top_r, color, 0, 0);
    corner_aa(x + w - top_r, y, top_r, color, 1, 0);
    if (bot_r) {
        corner_aa(x, y + h - bot_r, bot_r, color, 0, 1);
        corner_aa(x + w - bot_r, y + h - bot_r, bot_r, color, 1, 1);
    }
}

static int round_inset(int h, int r, int row, int bottom) {
    if (r <= 0)
        return 0;
    if (r * 2 > h) r = h / 2;
    int dy = -1;
    if (row < r)
        dy = r - row;
    else if (bottom && row >= h - r)
        dy = r - (h - 1 - row);
    if (dy < 0)
        return 0;
    for (int ix = 0; ix < r; ix++) {
        int cx = r - ix;
        if (cx * cx + dy * dy <= r * r)
            return ix;
    }
    return r;
}

void draw_round_rect(int x, int y, int w, int h, int r, uint8_t color) {
    round_fill(x, y, w, h, r, color, 1);
}

void draw_round_top(int x, int y, int w, int h, int r, uint8_t color) {
    round_fill(x, y, w, h, r, color, 0);
}

static int round_coverage(int col,int row,int w,int h,int r) {
    if(col<0||row<0||col>=w||row>=h)return 0;
    if(col>=w-r)col=w-1-col;
    if(row>=h-r)row=h-1-row;
    return col<r&&row<r?corner_cov(col,row,r):16;
}

void draw_round_frame(int x, int y, int w, int h, int r, uint8_t color) {
    if(w<=0||h<=0)return;
    if(r<0)r=0;
    if(r*2>w)r=w/2;
    if(r*2>h)r=h/2;
    for(int j=0;j<h;j++) {
        if(j>0&&j<h-1&&j>=r&&j<h-r) {
            put_pixel(x,y+j,color);put_pixel(x+w-1,y+j,color);
            continue;
        }
        for(int i=0;i<w;i++) {
            /* Subtract the inner shape, preserving the original pixels inside
             * the ring. Filled-corner AA incorrectly painted this area solid. */
            int outer=round_coverage(i,j,w,h,r);
            int inner=round_coverage(i-1,j-1,w-2,h-2,r>0?r-1:0);
            if(outer>inner)blend_cov(x+i,y+j,color,outer-inner);
        }
    }
}

void draw_bevel(int x, int y, int w, int h, uint8_t light, uint8_t dark) {
    draw_rect(x, y, w, 1, light);
    draw_rect(x, y, 1, h, light);
    draw_rect(x, y + h - 1, w, 1, dark);
    draw_rect(x + w - 1, y, 1, h, dark);
}

void shade_rect(int x, int y, int w, int h, int level) {
    if (!clip_rect(&x, &y, &w, &h))
        return;
    const uint8_t *tab = level >= 2 ? pal_shade2 : pal_shade1;
    for (int j = 0; j < h; j++) {
        uint8_t *row = fb + (y + j) * fb_w + x;
        for (int i = 0; i < w; i++)
            row[i] = tab[row[i]];
    }
}

/* Flat chrome: do not darken the desktop around windows or popups. */
void draw_shadow(int x,int y,int w,int h) {(void)x;(void)y;(void)w;(void)h;}

void blit_bits_color(int ox, int oy, int w, int h, const unsigned char *bits, uint8_t color) {
    int bpr = (w + 7) / 8;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (bits[y * bpr + x / 8] & (unsigned char)(0x80 >> (x & 7)))
                put_pixel(ox + x, oy + y, color);
        }
    }
}

void blit_bits(int ox, int oy, int w, int h, const unsigned char *bits) {
    blit_bits_color(ox, oy, w, h, bits, COLOR_BLACK);
}

int ui_index(char c) {
    unsigned u = (unsigned char)c;
    if (u < UI_FONT_FIRST || u >= UI_FONT_FIRST + UI_FONT_COUNT)
        return -1;
    return (int)(u - UI_FONT_FIRST);
}

int ui_advance(char c) {
    int i = ui_index(c);
    if (c == '\n' || c == '\t')
        return 0;
    if (i < 0)
        return 7;
    return ui_font_adv[i];
}

int ui_string_w(const char *s) {
    int w = 0;
    while (*s)
        w += ui_advance(*s++);
    return w;
}

int uib_string_w(const char *s) {
    int w = 0;
    while (*s) {
        int i = ui_index(*s++);
        w += i < 0 ? 7 : uib_font_adv[i];
    }
    return w;
}

static uint8_t blend_px(uint8_t bg,uint8_t fg,int level) {
    return level>=15?fg:gfx_mix(bg,fg,(level*256+7)/15);
}

int glyph_level(const uint8_t *px, int bpr, int row, int col) {
    return (px[row * bpr + (col >> 1)] >> (4 - 4 * (col & 1))) & 15;
}

static void draw_glyph(const uint8_t *px, int bpr, int h, int w, int x, int y, uint8_t color) {
    for (int row = 0; row < h; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= fb_h)
            continue;
        const uint8_t *src = px + row * bpr;
        int any = 0;
        for (int b = 0; b < bpr; b++)
            any |= src[b];
        if (!any)
            continue;
        uint8_t *dst = fb + yy * fb_w;
        for (int col = 0; col < w; col++) {
            int lvl = (src[col >> 1] >> (4 - 4 * (col & 1))) & 15;
            if (!lvl)
                continue;
            int xx = x + col;
            if (xx < 0 || xx >= fb_w)
                continue;
            dst[xx] = blend_px(dst[xx], color, lvl);
        }
    }
}

void draw_char(char c, int x, int y, uint8_t color) {
    int i = ui_index(c);
    if (c == ' ' || c == '\n' || c == '\t')
        return;
    if (i < 0) {
        draw_rect(x, y + 3, 6, UI_FONT_H - 6, color);
        return;
    }
    draw_glyph(ui_font_px[i], UI_FONT_BPR, UI_FONT_H, UI_FONT_W, x, y, color);
}

static void draw_char_bold(char c, int x, int y, uint8_t color) {
    int i = ui_index(c);
    if (i < 0 || c == ' ')
        return;
    draw_glyph(uib_font_px[i], UI_FONT_BPR, UI_FONT_H, UI_FONT_W, x, y, color);
}

void draw_edit_char(char c, int x, int y, uint8_t color) {
    int i = ui_index(c);
    if (c == ' ' || c == '\n' || c == '\t' || i < 0)
        return;
    draw_glyph(edit_font_px[i], EDIT_FONT_BPR, EDIT_FONT_H, EDIT_FONT_W, x, y, color);
}

void draw_string(const char *str, int x, int y, uint8_t color) {
    while (*str) {
        draw_char(*str, x, y, color);
        x += ui_advance(*str++);
    }
}

void draw_string_bold(const char *str, int x, int y, uint8_t color) {
    while (*str) {
        int i = ui_index(*str);
        draw_char_bold(*str, x, y, color);
        x += i < 0 ? 7 : uib_font_adv[i];
        str++;
    }
}

void draw_string_clip(const char *str, int x, int y, uint8_t color, int xmax) {
    while (*str) {
        int adv = ui_advance(*str);
        if (x + adv > xmax)
            break;
        draw_char(*str, x, y, color);
        x += adv;
        str++;
    }
}

void draw_string_bold_clip(const char *str, int x, int y, uint8_t color, int xmax) {
    while (*str) {
        int i = ui_index(*str);
        int adv = i < 0 ? 7 : uib_font_adv[i];
        if (x + adv > xmax)
            break;
        draw_char_bold(*str, x, y, color);
        x += adv;
        str++;
    }
}

void draw_string_centered(const char *str, int y, uint8_t color) {
    draw_string(str, (fb_w - ui_string_w(str)) / 2, y, color);
}

void draw_string_shadow(const char *str, int x, int y, uint8_t color, uint8_t shadow) {
    draw_string(str, x + 1, y + 1, shadow);
    draw_string(str, x, y, color);
}

static int logo_index(char c) {
    for (int i = 0; i < LOGO_FONT_COUNT; i++)
        if (logo_font_chars[i] == c)
            return i;
    return -1;
}

int logo_string_w(const char *s) {
    int w = 0;
    while (*s) {
        int i = logo_index(*s++);
        w += i < 0 ? 10 : logo_font_adv[i];
    }
    return w;
}

void draw_logo_string(const char *str, int x, int y, uint8_t color) {
    while (*str) {
        int i = logo_index(*str++);
        if (i < 0) {
            x += 10;
            continue;
        }
        draw_glyph(logo_font_px[i], LOGO_FONT_BPR, LOGO_FONT_H, LOGO_FONT_W, x, y, color);
        x += logo_font_adv[i];
    }
}

const uint8_t *ui_glyph(char c, int *bpr, int *h, int *w) {
    int i = ui_index(c);
    *bpr = UI_FONT_BPR;
    *h = UI_FONT_H;
    *w = UI_FONT_W;
    return i < 0 ? 0 : ui_font_px[i];
}

int hit(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

void flip_rect(int x0, int y0, int rw, int rh) {
    if (!lfb)
        return;
    if (!clip_rect(&x0, &y0, &rw, &rh))
        return;
    int bpp = fb_bpp;
    int pitch = fb_pitch;
    for (int y = y0; y < y0 + rh; y++) {
        uint8_t *src = fb + y * fb_w + x0;
        uint8_t *dst = lfb + y * pitch;
        if (bpp == 32) {
            uint32_t *d = (uint32_t *)(dst + x0 * 4);
            for (int x = 0; x < rw; x++)
                d[x] = pal32[src[x]];
        } else if (bpp == 16) {
            uint16_t *d = (uint16_t *)(dst + x0 * 2);
            for (int x = 0; x < rw; x++)
                d[x] = pal16[src[x]];
        } else if (bpp == 24) {
            uint8_t *d = dst + x0 * 3;
            for (int x = 0; x < rw; x++) {
                uint32_t c = pal32[src[x]];
                d[x * 3 + 0] = (uint8_t)(c);
                d[x * 3 + 1] = (uint8_t)(c >> 8);
                d[x * 3 + 2] = (uint8_t)(c >> 16);
            }
        } else {
            uint8_t *d = dst + x0;
            for (int x = 0; x < rw; x++)
                d[x] = src[x];
        }
        uint8_t *mirror=(uint8_t *)PRESENT_BASE+y*fb_w+x0;
        for(int x=0;x<rw;x++)mirror[x]=src[x];
        if ((y & 63) == 0 && flip_hook)
            flip_hook();
    }
}

void flip_vga(void) {
    flip_rect(0, 0, fb_w, fb_h);
    presented_valid=1;
}

/* Scan in RAM, then write only changed spans to the emulated video device. */
typedef struct __attribute__((packed)) { uint32_t value; } UnalignedWord;
static int same_span(const uint8_t *a,const uint8_t *b,int n) {
    while(n>=4){if(((const UnalignedWord *)a)->value!=((const UnalignedWord *)b)->value)return 0;a+=4;b+=4;n-=4;}
    while(n--)if(*a++!=*b++)return 0;
    return 1;
}
void gfx_present(void) {
    if(!presented_valid){flip_vga();return;}
    const uint8_t *old=(const uint8_t *)PRESENT_BASE;
    for(int y=0;y<fb_h;y++){
        const uint8_t *a=fb+y*fb_w,*b=old+y*fb_w;int run=-1;
        for(int x=0;x<fb_w;x+=32){
            int n=fb_w-x<32?fb_w-x:32;
            int changed=!same_span(a+x,b+x,n);
            if(changed&&run<0)run=x;
            if(!changed&&run>=0){flip_rect(run,y,x-run,1);run=-1;}
        }
        if(run>=0)flip_rect(run,y,fb_w-run,1);
    }
}
