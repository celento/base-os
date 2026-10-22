#ifndef UWIN_H
#define UWIN_H
#include <stdint.h>
#include "../sdk/baseos_sys.h"

/* Windows of ELF processes (BOS_SYS_WINDOW_*). The kernel keeps each
 * window's surface, settings and event queue; the shell adopts new windows,
 * shows their surfaces and feeds them input. Serialized kernel context only. */
#define UWIN_QUEUE 256u

typedef struct UWin {
    uint32_t handle, pid;
    BosWindowSpec spec;          /* as the program last set it */
    uint32_t *pixels;            /* spec.width * spec.height */
    uint32_t frame;              /* counts presented frames */
    uint32_t config;             /* counts title, size and flag changes */
    int adopted;                 /* the shell shows it */
    BosWindowEvent queue[UWIN_QUEUE];
    unsigned head, count;
    struct UWin *next;
} UWin;

struct Uproc;
/* System calls on the calling process's own thread. */
int uwin_create(struct Uproc *process, uint32_t spec);
int uwin_destroy(struct Uproc *process, uint32_t handle);
int uwin_present(struct Uproc *process, uint32_t handle, uint32_t pixels, uint32_t pitch);
int uwin_configure(struct Uproc *process, uint32_t handle, uint32_t spec);
int uwin_event(struct Uproc *process, uint32_t event, uint32_t timeout_ms);
int uwin_display_info(struct Uproc *process, uint32_t info);
/* The process is ending: its windows go away. */
void uwin_release_owner(uint32_t pid);

/* Shell side. A window not yet adopted, marked adopted; or 0. */
UWin *uwin_take_new(void);
/* The live window with this handle, or 0 once the program destroyed it. */
UWin *uwin_find(uint32_t handle);
/* Queue an event (time and window filled in) and wake a waiting thread. */
void uwin_push(UWin *window, BosWindowEvent event);
/* The client area a maximized window would get, for DISPLAY_INFO. */
void uwin_set_work_area(unsigned width, unsigned height);
#endif
