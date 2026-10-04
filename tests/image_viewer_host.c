/* Normal open, resize, zoom, pan, source-detachment, theme and close flows. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "layout.h"
static unsigned char host_presented[FB_CAPACITY];
#undef PRESENT_BASE
#define PRESENT_BASE ((uintptr_t)host_presented)
#define GFX_HOST_TEST
#include "../src/gfx.c"
#define IMAGE_VIEWER_HOST_TEST
#include "../src/image_viewer.c"

uint8_t app_accent, app_accent_dk, app_text, app_text_dim, app_chrome, app_chrome_dk;
void kmemset(void *to, int byte, int count) { memset(to, byte, (size_t)count); }
void kmemcpy(void *to, const void *from, int count) { memmove(to, from, (size_t)count); }
void fmt_uint(char *out, unsigned number) { sprintf(out, "%u", number); }
static unsigned char back[800 * 600], linear[800 * 600 * 4];

int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb"); assert(file);
    fseek(file, 0, SEEK_END); long length = ftell(file); rewind(file);
    unsigned char *source = malloc((size_t)length); assert(source);
    assert(fread(source, 1, (size_t)length, file) == (size_t)length); fclose(file);
    gfx_init(back, linear, 800, 600, 32, 3200);
    app_accent = gfx_rgb(40, 100, 200); app_text = gfx_gray(30);
    app_text_dim = gfx_gray(95); app_chrome = gfx_gray(239); app_chrome_dk = gfx_gray(195);
    image_viewer_close();
    assert(!image_viewer_loaded());
    assert(!image_viewer_key(0, '+'));
    assert(image_viewer_open(source, (unsigned)length, "landscape.png") == IMAGE_OK);
    assert(image_viewer_loaded());
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
    image_viewer_draw(-12, -10, 360, 200);
    image_viewer_draw(730, 540, 360, 200);
    /* The retained image cannot use the palette ramps that a theme replaces. */
    for (unsigned i = 0; i < count; i++)
        assert(IMAGE_MEMORY[i] < PAL_DESK || IMAGE_MEMORY[i] >= PAL_CUBE);
    gfx_set_ramp(PAL_DESK, PAL_DESK_N, 0xff0000, 0x00ff00);
    gfx_set_ramp(PAL_ACCENT, PAL_ACCENT_N, 0x00ff00, 0x0000ff); gfx_pal_commit();
    assert(!memcmp(owned, IMAGE_MEMORY, count));
    assert(image_viewer_open("plain text", 10, "notes.txt") == IMAGE_UNSUPPORTED);
    assert(!image_viewer_loaded() && strstr(image_viewer_status(), "Supported"));
    image_viewer_draw(30, 20, 360, 200);
    /* BOS1 bytes retain their original palette identity. */
    unsigned char bos[] = {'B','O','S','1',2,0,2,0,4,7,9,15};
    assert(image_viewer_open(bos, sizeof bos, "paint.pbm") == IMAGE_OK);
    assert(!memcmp(IMAGE_MEMORY, bos + 8, 4));
    image_viewer_draw(30, 20, 360, 200);
    image_viewer_close(); assert(!image_viewer_loaded());
    assert(!strcmp(image_viewer_title(), "Image Viewer"));
    free(owned); free(source);
    puts("image viewer: source ownership, geometry, zoom/pan, palette stability, errors and BOS1 passed");
}
