#include "display.h"
#include "gfx.h"
#include "layout.h"

/* QEMU standard VGA's documented Bochs DISPI interface.
 * https://www.qemu.org/docs/master/specs/standard-vga.html */
#define DISPI_INDEX 0x1ce
#define DISPI_DATA  0x1cf
static const DisplayMode modes[DISPLAY_MODE_COUNT] = {
    {800, 600, "800 x 600"}, {1024, 768, "1024 x 768"},
    {1280, 720, "1280 x 720"}, {1280, 800, "1280 x 800"}
};
static void reg_write(unsigned index, unsigned value) {
    __asm__ volatile("outw %0,%1"::"a"((unsigned short)index),"Nd"((unsigned short)DISPI_INDEX));
    __asm__ volatile("outw %0,%1"::"a"((unsigned short)value),"Nd"((unsigned short)DISPI_DATA));
}
static unsigned reg_read(unsigned index) {
    unsigned short value;
    __asm__ volatile("outw %0,%1"::"a"((unsigned short)index),"Nd"((unsigned short)DISPI_INDEX));
    __asm__ volatile("inw %1,%0":"=a"(value):"Nd"((unsigned short)DISPI_DATA));
    return value;
}
const DisplayMode *display_mode(int index) {
    return index >= 0 && index < DISPLAY_MODE_COUNT ? &modes[index] : 0;
}
int display_available(void) {
    unsigned id = reg_read(0);
    return id >= 0xb0c0 && id <= 0xb0c5;
}
int display_current_mode(void) {
    for (int i = 0; i < DISPLAY_MODE_COUNT; ++i)
        if (modes[i].width == fb_w && modes[i].height == fb_h) return i;
    return -1;
}
static void program(int width, int height, int bits) {
    reg_write(4, 0);
    reg_write(1, width); reg_write(2, height); reg_write(3, bits);
    reg_write(6, width); reg_write(8, 0); reg_write(9, 0);
    reg_write(4, 0x41);
}
int display_set_mode(int index) {
    const DisplayMode *mode = display_mode(index);
    if (!mode || !display_available() || !lfb ||
        (unsigned)mode->width * mode->height > FB_CAPACITY) return 0;
    int old_width = fb_w, old_height = fb_h, old_bits = fb_bpp;
    program(mode->width, mode->height, 32);
    if (reg_read(1) != (unsigned)mode->width || reg_read(2) != (unsigned)mode->height ||
        reg_read(3) != 32 || !(reg_read(4) & 1)) {
        program(old_width, old_height, old_bits);
        return 0;
    }
    gfx_init((unsigned char *)FB_BASE, lfb, mode->width, mode->height, 32, mode->width * 4);
    return 1;
}
