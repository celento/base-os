#include "kernel/uwin.h"
#include "kernel/uproc.h"
#include "kernel/kheap.h"
#include "kernel/platform.h"
#include "kernel/gfx.h"
#include "kernel/fs.h"

#define SIZE_MAX_SIDE 4096u

static UWin *windows;
static uint32_t next_handle = 1;
static unsigned work_width = 640, work_height = 480;
/* Threads waiting in WINDOW_EVENT; each rechecks its own windows. */
static char event_channel;

static UWin *owned(Uproc *process, uint32_t handle) {
    for (UWin *window = windows; window; window = window->next)
        if (window->handle == handle && window->pid == process->pid) return window;
    return 0;
}

static int spec_in(Uproc *process, uint32_t address, BosWindowSpec *spec) {
    int result = uproc_copy_in(process, spec, address, sizeof *spec);
    if (result) return result;
    spec->title[BOS_WINDOW_TITLE - 1] = 0;
    if (!spec->width || !spec->height || spec->width > SIZE_MAX_SIDE || spec->height > SIZE_MAX_SIDE)
        return -BOS_EINVAL;
    return 0;
}

static void free_window(UWin *window) {
    gfx_overlay_forget(window);
    kfree(window->pixels);
    kfree(window);
}

int uwin_create(Uproc *process, uint32_t address) {
    BosWindowSpec spec;
    int result = spec_in(process, address, &spec);
    if (result) return result;
    UWin *window = kmalloc(sizeof *window);
    if (!window) return -BOS_ENOMEM;
    window->pixels = kmalloc(spec.width * spec.height * 4u);
    if (!window->pixels) { kfree(window); return -BOS_ENOMEM; }
    window->handle = next_handle++;
    window->pid = process->pid;
    window->spec = spec;
    window->next = windows;
    windows = window;
    return (int)window->handle;
}

int uwin_destroy(Uproc *process, uint32_t handle) {
    for (UWin **link = &windows; *link; link = &(*link)->next) {
        UWin *window = *link;
        if (window->handle == handle && window->pid == process->pid) {
            *link = window->next;
            free_window(window);
            return 0;
        }
    }
    return -BOS_EBADF;
}

void uwin_release_owner(uint32_t pid) {
    for (UWin **link = &windows; *link;) {
        UWin *window = *link;
        if (window->pid == pid) { *link = window->next; free_window(window); }
        else link = &window->next;
    }
}

int uwin_present(Uproc *process, uint32_t handle, uint32_t pixels, uint32_t pitch) {
    UWin *window = owned(process, handle);
    if (!window) return -BOS_EBADF;
    uint32_t row = window->spec.width * 4u, rows = window->spec.height;
    if (pitch < row || pitch > 65536u) return -BOS_EINVAL;
    /* Map the whole span first so a bad pointer never leaves half a frame. */
    int result = vm_prepare(&process->space, pixels, (rows - 1) * pitch + row, 0);
    if (result) return result;
    for (uint32_t y = 0; y < rows; y++)
        kmemcpy((uint8_t *)window->pixels + y * row, (const void *)(uintptr_t)(pixels + y * pitch), (int)row);
    ++window->frame;
    return 0;
}

int uwin_configure(Uproc *process, uint32_t handle, uint32_t address) {
    UWin *window = owned(process, handle);
    if (!window) return -BOS_EBADF;
    BosWindowSpec spec;
    int result = spec_in(process, address, &spec);
    if (result) return result;
    if (spec.width != window->spec.width || spec.height != window->spec.height) {
        uint32_t *pixels = kmalloc(spec.width * spec.height * 4u);
        if (!pixels) return -BOS_ENOMEM;
        gfx_overlay_forget(window);
        kfree(window->pixels);
        window->pixels = pixels;
        ++window->frame;
    }
    window->spec = spec;
    ++window->config;
    return 0;
}

static uint32_t deadline(uint32_t milliseconds) {
    uint32_t at = timer_milliseconds() + milliseconds + 1;
    return at ? at : 1;
}

static int take_event(Uproc *process, BosWindowEvent *out) {
    for (UWin *window = windows; window; window = window->next)
        if (window->pid == process->pid && window->count) {
            *out = window->queue[window->head];
            window->head = (window->head + 1) % UWIN_QUEUE;
            --window->count;
            return 1;
        }
    return 0;
}

int uwin_event(Uproc *process, uint32_t address, uint32_t timeout_ms) {
    BosWindowEvent event;
    uint32_t wake = timeout_ms && timeout_ms != ~0u ? deadline(timeout_ms > 262144000u ? 262144000u : timeout_ms) : 0;
    while (!take_event(process, &event)) {
        if (!timeout_ms) return 0;
        if (thread_wait((uintptr_t)&event_channel, wake)) {
            if (!take_event(process, &event)) return 0;
            break;
        }
        if (process->state != UPROC_RUNNING) return -BOS_EINTR;
    }
    int result = uproc_copy_out(process, address, &event, sizeof event);
    return result ? result : 1;
}

int uwin_display_info(Uproc *process, uint32_t address) {
    BosDisplayInfo info = {(unsigned)fb_w, (unsigned)fb_h, work_width, work_height, 60};
    return uproc_copy_out(process, address, &info, sizeof info);
}

UWin *uwin_take_new(void) {
    for (UWin *window = windows; window; window = window->next)
        if (!window->adopted) { window->adopted = 1; return window; }
    return 0;
}

UWin *uwin_find(uint32_t handle) {
    for (UWin *window = windows; window; window = window->next)
        if (window->handle == handle) return window;
    return 0;
}

void uwin_push(UWin *window, BosWindowEvent event) {
    event.window = window->handle;
    event.time_ms = timer_milliseconds();
    if (window->count == UWIN_QUEUE) {
        /* A program that stopped reading loses its oldest input, not its newest. */
        window->head = (window->head + 1) % UWIN_QUEUE;
        --window->count;
    }
    window->queue[(window->head + window->count) % UWIN_QUEUE] = event;
    ++window->count;
    thread_wake_channel((uintptr_t)&event_channel, ~0u);
}

void uwin_set_work_area(unsigned width, unsigned height) {
    work_width = width;
    work_height = height;
}
