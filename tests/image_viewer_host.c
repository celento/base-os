/* Normal open, resize, zoom, pan, source-detachment, theme and close flows. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/layout.h"
static unsigned char host_presented[FB_CAPACITY];
#undef PRESENT_BASE
#define PRESENT_BASE ((uintptr_t)host_presented)
#define GFX_HOST_TEST
#include "../src/kernel/gfx.c"
#include "../src/ui/icons.c"
#include "../src/ui/theme.c"
#include "../src/ui/kit.c"
#define IMAGE_VIEWER_HOST_TEST
#include "../src/apps/image_viewer.c"

uint8_t app_accent, app_accent_dk, app_text, app_text_dim, app_chrome, app_chrome_dk;
uint8_t app_body, app_field, app_paper, app_border, app_on_accent;
void kmemset(void *to, int byte, int count) { memset(to, byte, (size_t)count); }
void kmemcpy(void *to, const void *from, int count) { memmove(to, from, (size_t)count); }
void fmt_uint(char *out, unsigned number) { sprintf(out, "%u", number); }
static unsigned progress_polls;
void platform_poll(void) { progress_polls++; }
static unsigned char back[800 * 600], linear[800 * 600 * 4];

int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb"); assert(file);
    fseek(file, 0, SEEK_END); long length = ftell(file); rewind(file);
    unsigned char *source = malloc((size_t)length); assert(source);
    assert(fread(source, 1, (size_t)length, file) == (size_t)length); fclose(file);
    gfx_init(back, linear, 800, 600, 32, 3200);
    ui_theme_install(UI_STYLE_CLASSIC, 0, 0);
    app_accent = gfx_rgb(40, 100, 200); app_text = gfx_gray(30);
    app_text_dim = gfx_gray(95); app_chrome = gfx_gray(239); app_chrome_dk = gfx_gray(195);
    image_viewer_close();
    assert(!image_viewer_loaded());
    assert(!image_viewer_key(0, '+'));
    assert(image_viewer_open(source, (unsigned)length, "landscape.png") == IMAGE_OK);
    assert(image_viewer_loaded());
    assert(progress_polls > 10);
    assert(!strcmp(image_viewer_title(), "landscape.png"));
    unsigned count = viewer.width * viewer.height;
    unsigned char *owned = malloc(count); assert(owned);
    memcpy(owned, IMAGE_MEMORY, count);
    memset(source, 0, (size_t)length);
    assert(!memcmp(owned, IMAGE_MEMORY, count));
    memset(back, COLOR_MAGENTA, sizeof back);
    image_viewer_draw(30, 20, 720, 520);
    assert(viewer.fit && strstr(image_viewer_status(), "PNG"));
    assert(strstr(image_viewer_status(), "640 x 360"));
    for (int y = 0; y < 600; y++) for (int x = 0; x < 800; x++)
        if (x < 30 || x >= 750 || y < 20 || y >= 540) assert(back[y * 800 + x] == COLOR_MAGENTA);
    assert(image_viewer_key(0, '1') && viewer.scale == 100 && !viewer.fit);
    image_viewer_draw(30, 20, 360, 200);
    assert(image_viewer_key(0, '+') && viewer.scale == 125);
    assert(image_viewer_key(KEY_RIGHT, 0)); assert(image_viewer_scroll(3));
    image_viewer_draw(30, 20, 360, 200);
    assert(viewer.pan_x > 0 && viewer.pan_y > 0);
    for (int i = 0; i < 20; i++) image_viewer_key(0, '+');
    assert(viewer.scale == 400);
    for (int i = 0; i < 20; i++) image_viewer_key(0, '-');
    assert(viewer.scale == 10);
    assert(!viewer.pan_x && !viewer.pan_y);
    assert(image_viewer_click(30, 20, 360, 200, 50, 35) && viewer.fit);
    /* Toolbar buttons hit exactly where they are drawn: 1:1, zoom in/out, Fit. */
    ViewRect r = tool_rect(30, 20, VB_ACTUAL);
    assert(image_viewer_click(30, 20, 360, 200, r.x + r.w / 2, r.y + r.h / 2) && viewer.scale == 100 && !viewer.fit);
    r = tool_rect(30, 20, VB_ZOOM_IN);
    assert(image_viewer_click(30, 20, 360, 200, r.x + r.w / 2, r.y + r.h / 2) && viewer.scale == 125);
    r = tool_rect(30, 20, VB_ZOOM_OUT);
    assert(image_viewer_click(30, 20, 360, 200, r.x + r.w / 2, r.y + r.h / 2) && viewer.scale == 100);
    assert(!image_viewer_click(30, 20, 360, 200, r.x - 4, r.y + r.h / 2));
    r = tool_rect(30, 20, VB_FIT);
    assert(image_viewer_click(30, 20, 360, 200, r.x + r.w / 2, r.y + r.h / 2) && viewer.fit);
    r = tool_rect(30, 20, VB_ZOOM_IN);
    assert(r.x + r.w <= 30 + 360 - 8 && r.y >= 20 && r.y + r.h <= 20 + tool_h());
    /* Zoomed past the viewport, scrollbars appear and their tracks pan. */
    assert(image_viewer_key(0, '1') && image_viewer_key(0, '+'));
    image_viewer_draw(30, 20, 360, 200);
    assert(viewer.hbar && viewer.vbar && viewer.viewport_w == 360 - SB);
    int before = viewer.pan_x;
    assert(image_viewer_click(30, 20, 360, 200, 30 + viewer.viewport_w - 4, 20 + tool_h() + viewer.viewport_h + 2));
    assert(viewer.pan_x > before);
    assert(image_viewer_key(0, 'f') && viewer.fit);
    image_viewer_draw(30, 20, 360, 200);
    assert(!viewer.hbar && !viewer.vbar);
    image_viewer_draw(-12, -10, 360, 200);
    image_viewer_draw(730, 540, 360, 200);
    /* The retained image cannot use the palette ramps that a theme replaces. */
    for (unsigned i = 0; i < count; i++)
        assert(IMAGE_MEMORY[i] < PAL_TOKEN || (IMAGE_MEMORY[i] >= PAL_GRAY && IMAGE_MEMORY[i] < PAL_ICON_X) ||
               IMAGE_MEMORY[i] >= PAL_CUBE);
    gfx_set_ramp(PAL_TOKEN, PAL_TOKEN_N, 0xff0000, 0x00ff00);
    gfx_set_ramp(PAL_ANSI, PAL_ANSI_N, 0x00ff00, 0x0000ff); gfx_pal_commit();
    assert(!memcmp(owned, IMAGE_MEMORY, count));
    assert(image_viewer_open("plain text", 10, "notes.txt") == IMAGE_UNSUPPORTED);
    assert(image_decode_set_poll_hook(NULL) == NULL);
    assert(!image_viewer_loaded() && strstr(image_viewer_status(), "Supported"));
    image_viewer_draw(30, 20, 360, 200);
    /* BOS1 bytes retain their original palette identity. */
    unsigned char bos[] = {'B','O','S','1',2,0,2,0,4,7,9,15};
    assert(image_viewer_open(bos, sizeof bos, "paint.pbm") == IMAGE_OK);
    assert(!memcmp(IMAGE_MEMORY, bos + 8, 4));
    image_viewer_draw(30, 20, 360, 200);
    /* Modern: the same flows stay inside the client and hit where drawn. */
    ui_theme_install(UI_STYLE_MODERN, 0, 0);
    for (int state = 0; state < 3; state++) {
        if (state == 1) assert(image_viewer_open("plain text", 10, "notes.txt") == IMAGE_UNSUPPORTED);
        if (state == 2) image_viewer_close();
        memset(back, COLOR_MAGENTA, sizeof back);
        image_viewer_draw(30, 20, 720, 520);
        image_viewer_draw(30, 20, 360, 200);
        for (int y = 0; y < 600; y++) for (int x = 0; x < 800; x++)
            if (x < 30 || x >= 750 || y < 20 || y >= 540) assert(back[y * 800 + x] == COLOR_MAGENTA);
    }
    assert(image_viewer_open(bos, sizeof bos, "paint.pbm") == IMAGE_OK);
    image_viewer_draw(30, 20, 360, 200);
    r = tool_rect(30, 20, VB_ACTUAL);
    assert(image_viewer_click(30, 20, 360, 200, r.x + r.w / 2, r.y + r.h / 2) && viewer.scale == 100 && !viewer.fit);
    r = tool_rect(30, 20, VB_FIT);
    assert(image_viewer_click(30, 20, 360, 200, r.x + r.w / 2, r.y + r.h / 2) && viewer.fit);
    if (getenv("VIEWER_SNAPSHOT")) {
        FILE *f = fopen(getenv("VIEWER_SNAPSHOT"), "wb"); assert(f);
        memset(back, COLOR_MAGENTA, sizeof back);
        image_viewer_close(); image_viewer_draw(0, 0, 720, 520);
        fprintf(f, "P6\n720 520\n255\n");
        for (int y = 0; y < 520; y++) for (int x = 0; x < 720; x++) {
            unsigned c = pal32[back[y * 800 + x]]; fputc((c >> 16) & 255, f); fputc((c >> 8) & 255, f); fputc(c & 255, f);
        }
        fclose(f);
    }
    image_viewer_close(); assert(!image_viewer_loaded());
    assert(!strcmp(image_viewer_title(), "Image Viewer"));
    free(owned); free(source);
    puts("image viewer: Classic and Modern, scrollbars, source ownership, geometry, zoom/pan, palette stability, errors and BOS1 passed");
}
