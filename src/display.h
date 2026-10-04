#ifndef DISPLAY_H
#define DISPLAY_H
#define DISPLAY_MODE_COUNT 4
typedef struct { int width, height; const char *name; } DisplayMode;
const DisplayMode *display_mode(int index);
int display_available(void);
int display_current_mode(void);
/* QEMU standard VGA only; keeps the validated framebuffer mapping. */
int display_set_mode(int index);
#endif
