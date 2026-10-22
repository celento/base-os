/* A window drawn in true color, logging the events it receives.
 * Keys: r resizes the surface, m toggles relative mouse mode; closing the
 * window ends the program. Build with i686-baseos-gcc. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sdk/baseos_sys.h"

static long sys(long number, long a, long b, long c) {
    long result;
    __asm__ volatile("int $0x81" : "=a"(result) : "a"(number), "b"(a), "c"(b), "d"(c) : "memory");
    return result;
}

static void say(const char *text) {
    sys(BOS_SYS_LOG, (long)text, (long)strlen(text), 0);
}

static unsigned width = 320, height = 200;
static unsigned *pixels;

static void draw(unsigned frame) {
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++)
            pixels[y * width + x] = 0x123456;   /* not a palette color */
    unsigned bx = frame % (width - 20);
    for (unsigned y = 90; y < 110; y++)
        for (unsigned x = bx; x < bx + 20; x++)
            pixels[y * width + x] = 0xE07010;
}

int main(void) {
    BosWindowSpec spec = {width, height, BOS_WINDOW_RESIZABLE, "Window test"};
    int window = (int)sys(BOS_SYS_WINDOW_CREATE, (long)&spec, 0, 0);
    if (window <= 0) { say("WIN create failed"); return 1; }
    BosDisplayInfo display;
    sys(BOS_SYS_DISPLAY_INFO, (long)&display, 0, 0);
    char line[160];
    snprintf(line, sizeof line, "WIN created %d on %ux%u", window, display.width, display.height);
    say(line);
    pixels = malloc(1024 * 1024 * 4);
    unsigned frame = 0;
    for (;;) {
        draw(frame++);
        sys(BOS_SYS_WINDOW_PRESENT, window, (long)pixels, width * 4);
        BosWindowEvent event;
        while (sys(BOS_SYS_WINDOW_EVENT, (long)&event, 16, 0) == 1) {
            switch (event.type) {
            case BOS_EVENT_KEY_DOWN:
                snprintf(line, sizeof line, "WIN key down %d %d", event.a, event.b);
                say(line);
                if (event.b == 'r') {
                    spec.width = width = 400; spec.height = height = 300;
                    sys(BOS_SYS_WINDOW_CONFIGURE, window, (long)&spec, 0);
                }
                if (event.b == 'm') {
                    spec.flags ^= BOS_WINDOW_RELATIVE_MOUSE;
                    sys(BOS_SYS_WINDOW_CONFIGURE, window, (long)&spec, 0);
                }
                break;
            case BOS_EVENT_KEY_UP:
                snprintf(line, sizeof line, "WIN key up %d", event.a);
                say(line);
                break;
            case BOS_EVENT_MOUSE_DOWN:
            case BOS_EVENT_MOUSE_UP:
                snprintf(line, sizeof line, "WIN mouse %s %d at %d,%d",
                         event.type == BOS_EVENT_MOUSE_DOWN ? "down" : "up", event.a, event.b, event.c);
                say(line);
                break;
            case BOS_EVENT_MOUSE_MOVE:
                if (spec.flags & BOS_WINDOW_RELATIVE_MOUSE) {
                    snprintf(line, sizeof line, "WIN motion %d,%d", event.c, event.d);
                    say(line);
                }
                break;
            case BOS_EVENT_FOCUS_IN: say("WIN focus in"); break;
            case BOS_EVENT_FOCUS_OUT: say("WIN focus out"); break;
            case BOS_EVENT_RESIZED:
                snprintf(line, sizeof line, "WIN resized %d %d", event.a, event.b);
                say(line);
                break;
            case BOS_EVENT_CLOSE:
                say("WIN close");
                sys(BOS_SYS_WINDOW_DESTROY, window, 0, 0);
                return 0;
            }
        }
    }
}
