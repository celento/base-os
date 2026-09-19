#ifndef GFX_H
#define GFX_H

#include <stdint.h>

#define COLOR_BLACK    0
#define COLOR_DKGRAY   1
#define COLOR_GRAY     2
#define COLOR_LTGRAY   3
#define COLOR_RED      4
#define COLOR_ORANGE   5
#define COLOR_YELLOW   6
#define COLOR_GREEN    7
#define COLOR_CYAN     8
#define COLOR_BLUE     9
#define COLOR_MAGENTA  10
#define COLOR_BROWN    11
#define COLOR_PINE     12
#define COLOR_NAVY     13
#define COLOR_WINE     14
#define COLOR_WHITE    15

#define PAL_GRAY       16
#define PAL_GRAY_N     32
#define PAL_DESK       48
#define PAL_DESK_N     64
#define PAL_ACCENT     112
#define PAL_ACCENT_N   16
#define PAL_CUBE       128

#define RAMP_END(n)    (((n) - 1) * 16)

extern int fb_w;
extern int fb_h;
extern int fb_bpp;
extern int fb_pitch;
extern uint8_t *fb;
extern uint8_t *lfb;
extern uint32_t pal32[256];

void gfx_init(uint8_t *backbuf, uint8_t *linear, int w, int h, int bpp, int pitch);
void gfx_set_flip_hook(void (*fn)(void));

/* Closest palette index to a 24-bit colour (cube + grey ramp). */
uint8_t gfx_rgb(int r, int g, int b);
uint8_t gfx_gray(int v);
void gfx_set_ramp(int base, int n, uint32_t c0, uint32_t c1);
void gfx_pal_commit(void);
uint8_t gfx_mix(uint8_t a, uint8_t b, int t);   /* t 0..256 toward b */
uint8_t gfx_darker(uint8_t c, int pct);
uint8_t gfx_lighter(uint8_t c, int pct);
int gfx_luma(uint8_t c);

void put_pixel(int x, int y, uint8_t color);
uint8_t get_pixel(int x, int y);
void draw_rect(int x, int y, int w, int h, uint8_t color);
void draw_frame(int x, int y, int w, int h, uint8_t color);
void draw_line(int x1, int y1, int x2, int y2, uint8_t color);
void draw_hline(int x, int y, int w, uint8_t color);
void draw_vline(int x, int y, int h, uint8_t color);
void draw_dither(int x, int y, int w, int h);
void draw_checker(int x, int y, int w, int h, uint8_t a, uint8_t b);
void draw_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void draw_vgradient_round(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom, int round_bottom);
void draw_ramp(int x, int y, int w, int h, int base, int n, int p0, int p1);
void draw_ramp_round(int x, int y, int w, int h, int r, int round_bottom, int base, int n, int p0, int p1);
void draw_round_rect(int x, int y, int w, int h, int r, uint8_t color);
void draw_round_frame(int x, int y, int w, int h, int r, uint8_t color);
void draw_round_top(int x, int y, int w, int h, int r, uint8_t color);
void draw_bevel(int x, int y, int w, int h, uint8_t light, uint8_t dark);
void shade_rect(int x, int y, int w, int h, int level);
void draw_shadow(int x, int y, int w, int h);
void blit_bits(int ox, int oy, int w, int h, const unsigned char *bits);
void blit_bits_color(int ox, int oy, int w, int h, const unsigned char *bits, uint8_t color);

int ui_index(char c);
int ui_advance(char c);
int ui_string_w(const char *s);
int uib_string_w(const char *s);
int logo_string_w(const char *s);
int glyph_level(const uint8_t *px, int bpr, int row, int col);
const uint8_t *ui_glyph(char c, int *bpr, int *h, int *w);
void draw_string_bold(const char *str, int x, int y, uint8_t color);
void draw_string_bold_clip(const char *str, int x, int y, uint8_t color, int xmax);
void draw_logo_string(const char *str, int x, int y, uint8_t color);
void draw_char(char c, int x, int y, uint8_t color);
void draw_edit_char(char c, int x, int y, uint8_t color);
void draw_string(const char *str, int x, int y, uint8_t color);
void draw_string_clip(const char *str, int x, int y, uint8_t color, int xmax);
void draw_string_centered(const char *str, int y, uint8_t color);
void draw_string_shadow(const char *str, int x, int y, uint8_t color, uint8_t shadow);

int hit(int px, int py, int x, int y, int w, int h);

void flip_rect(int x0, int y0, int rw, int rh);
void flip_vga(void);
void gfx_present(void);
void gfx_window_corners(int x,int y,int w,int h,uint8_t saved[256],int finish,uint8_t border);

#endif
