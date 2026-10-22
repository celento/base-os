/* SDL video driver for BaseOS: each SDL window is a kernel window
 * (BOS_SYS_WINDOW_*) showing the window framebuffer, and kernel window
 * events become SDL events. Copied into SDL's src/video/baseos by
 * ports/build.py. */
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_BASEOS

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/scancodes_linux.h"
#include <baseos_sys.h>

typedef struct {
    int handle;
    BosWindowSpec spec;
    SDL_Surface *surface;
} BaseosWindow;

static int cursor_hidden;

static long sys(long number, long a, long b, long c)
{
    long result;
    __asm__ volatile("int $0x81" : "=a"(result) : "a"(number), "b"(a), "c"(b), "d"(c) : "memory");
    return result;
}

static void configure(BaseosWindow *data)
{
    sys(BOS_SYS_WINDOW_CONFIGURE, data->handle, (long)&data->spec, 0);
}

static void set_flag(BaseosWindow *data, unsigned flag, int on)
{
    unsigned flags = on ? data->spec.flags | flag : data->spec.flags & ~flag;
    if (flags != data->spec.flags) {
        data->spec.flags = flags;
        configure(data);
    }
}

static void copy_title(BaseosWindow *data, const char *title)
{
    SDL_strlcpy(data->spec.title, title && *title ? title : "SDL", sizeof(data->spec.title));
}

static int BASEOS_CreateWindow(_THIS, SDL_Window *window)
{
    BaseosWindow *data = (BaseosWindow *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return SDL_OutOfMemory();
    }
    data->spec.width = window->w > 0 ? window->w : 1;
    data->spec.height = window->h > 0 ? window->h : 1;
    data->spec.flags = (window->flags & SDL_WINDOW_RESIZABLE) ? BOS_WINDOW_RESIZABLE : 0;
    if (window->flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) {
        data->spec.flags |= BOS_WINDOW_MAXIMIZED;
    }
    if (cursor_hidden) {
        data->spec.flags |= BOS_WINDOW_HIDE_CURSOR;
    }
    copy_title(data, window->title);
    data->handle = (int)sys(BOS_SYS_WINDOW_CREATE, (long)&data->spec, 0, 0);
    if (data->handle <= 0) {
        SDL_free(data);
        return SDL_SetError("BaseOS: cannot create a window");
    }
    window->driverdata = data;
    SDL_SetMouseFocus(window);
    SDL_SetKeyboardFocus(window);
    return 0;
}

static void BASEOS_DestroyWindow(_THIS, SDL_Window *window)
{
    BaseosWindow *data = (BaseosWindow *)window->driverdata;
    if (data) {
        sys(BOS_SYS_WINDOW_DESTROY, data->handle, 0, 0);
        SDL_FreeSurface(data->surface);
        SDL_free(data);
        window->driverdata = NULL;
    }
}

static void BASEOS_SetWindowTitle(_THIS, SDL_Window *window)
{
    BaseosWindow *data = (BaseosWindow *)window->driverdata;
    copy_title(data, window->title);
    configure(data);
}

static void BASEOS_SetWindowFullscreen(_THIS, SDL_Window *window, SDL_VideoDisplay *display, SDL_bool fullscreen)
{
    (void)display;
    set_flag((BaseosWindow *)window->driverdata, BOS_WINDOW_MAXIMIZED, fullscreen);
}

static void BASEOS_MaximizeWindow(_THIS, SDL_Window *window)
{
    set_flag((BaseosWindow *)window->driverdata, BOS_WINDOW_MAXIMIZED, 1);
}

static void BASEOS_RestoreWindow(_THIS, SDL_Window *window)
{
    set_flag((BaseosWindow *)window->driverdata, BOS_WINDOW_MAXIMIZED,
             (window->flags & SDL_WINDOW_FULLSCREEN) != 0);
}

static void BASEOS_SetWindowResizable(_THIS, SDL_Window *window, SDL_bool resizable)
{
    set_flag((BaseosWindow *)window->driverdata, BOS_WINDOW_RESIZABLE, resizable);
}

/* The surface always matches the SDL window size: a new framebuffer after a
 * resize also resizes the kernel window's surface. */
static int BASEOS_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch)
{
    BaseosWindow *data = (BaseosWindow *)window->driverdata;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    SDL_FreeSurface(data->surface);
    data->surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 0, SDL_PIXELFORMAT_RGB888);
    if (!data->surface) {
        return -1;
    }
    if ((unsigned)w != data->spec.width || (unsigned)h != data->spec.height) {
        data->spec.width = (unsigned)w;
        data->spec.height = (unsigned)h;
        configure(data);
    }
    *format = SDL_PIXELFORMAT_RGB888;
    *pixels = data->surface->pixels;
    *pitch = data->surface->pitch;
    return 0;
}

static int BASEOS_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    BaseosWindow *data = (BaseosWindow *)window->driverdata;
    (void)rects;
    (void)numrects;
    if (!data->surface) {
        return SDL_SetError("BaseOS: no framebuffer");
    }
    if (sys(BOS_SYS_WINDOW_PRESENT, data->handle, (long)data->surface->pixels, data->surface->pitch) < 0) {
        return SDL_SetError("BaseOS: present failed");
    }
    return 0;
}

static void BASEOS_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    BaseosWindow *data = (BaseosWindow *)window->driverdata;
    if (data) {
        SDL_FreeSurface(data->surface);
        data->surface = NULL;
    }
}

static SDL_Window *window_for(_THIS, unsigned handle)
{
    for (SDL_Window *window = _this->windows; window; window = window->next) {
        BaseosWindow *data = (BaseosWindow *)window->driverdata;
        if (data && (unsigned)data->handle == handle) {
            return window;
        }
    }
    return NULL;
}

/* PC set 1 scancodes; keys with an E0 prefix carry BOS_KEY_EXTENDED. */
static SDL_Scancode scancode(int code)
{
    if (code & BOS_KEY_EXTENDED) {
        switch (code & 0xff) {
        case 0x1c: return SDL_SCANCODE_KP_ENTER;
        case 0x1d: return SDL_SCANCODE_RCTRL;
        case 0x35: return SDL_SCANCODE_KP_DIVIDE;
        case 0x37: return SDL_SCANCODE_PRINTSCREEN;
        case 0x38: return SDL_SCANCODE_RALT;
        case 0x47: return SDL_SCANCODE_HOME;
        case 0x48: return SDL_SCANCODE_UP;
        case 0x49: return SDL_SCANCODE_PAGEUP;
        case 0x4b: return SDL_SCANCODE_LEFT;
        case 0x4d: return SDL_SCANCODE_RIGHT;
        case 0x4f: return SDL_SCANCODE_END;
        case 0x50: return SDL_SCANCODE_DOWN;
        case 0x51: return SDL_SCANCODE_PAGEDOWN;
        case 0x52: return SDL_SCANCODE_INSERT;
        case 0x53: return SDL_SCANCODE_DELETE;
        case 0x5b: return SDL_SCANCODE_LGUI;
        case 0x5c: return SDL_SCANCODE_RGUI;
        case 0x5d: return SDL_SCANCODE_APPLICATION;
        default: return SDL_SCANCODE_UNKNOWN;
        }
    }
    if (code > 0 && code < (int)SDL_arraysize(linux_scancode_table)) {
        return linux_scancode_table[code];
    }
    return SDL_SCANCODE_UNKNOWN;
}

static Uint8 button(int number)
{
    return number == 1 ? SDL_BUTTON_LEFT : number == 2 ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE;
}

static void BASEOS_PumpEvents(_THIS)
{
    BosWindowEvent event;
    while (sys(BOS_SYS_WINDOW_EVENT, (long)&event, 0, 0) == 1) {
        SDL_Window *window = window_for(_this, event.window);
        if (!window) {
            continue;
        }
        switch (event.type) {
        case BOS_EVENT_KEY_DOWN:
            SDL_SendKeyboardKey(SDL_PRESSED, scancode(event.a));
            if (event.b >= 32 && event.b < 127 && !(event.modifiers & (BOS_MOD_CTRL | BOS_MOD_ALT)) &&
                SDL_EventState(SDL_TEXTINPUT, SDL_QUERY)) {
                char text[2] = { (char)event.b, 0 };
                SDL_SendKeyboardText(text);
            }
            break;
        case BOS_EVENT_KEY_UP:
            SDL_SendKeyboardKey(SDL_RELEASED, scancode(event.a));
            break;
        case BOS_EVENT_MOUSE_MOVE:
            if (SDL_GetMouse()->relative_mode) {
                if (event.c || event.d) {
                    SDL_SendMouseMotion(window, 0, 1, event.c, event.d);
                }
            } else {
                SDL_SendMouseMotion(window, 0, 0, event.a, event.b);
            }
            break;
        case BOS_EVENT_MOUSE_DOWN:
        case BOS_EVENT_MOUSE_UP:
            SDL_SendMouseButton(window, 0, event.type == BOS_EVENT_MOUSE_DOWN ? SDL_PRESSED : SDL_RELEASED,
                                button(event.a));
            break;
        case BOS_EVENT_WHEEL:
            SDL_SendMouseWheel(window, 0, 0.0f, (float)-event.a, SDL_MOUSEWHEEL_NORMAL);
            break;
        case BOS_EVENT_FOCUS_IN:
            SDL_SetMouseFocus(window);
            SDL_SetKeyboardFocus(window);
            break;
        case BOS_EVENT_FOCUS_OUT:
            if (SDL_GetKeyboardFocus() == window) {
                SDL_SetKeyboardFocus(NULL);
            }
            break;
        case BOS_EVENT_CLOSE:
            SDL_SendWindowEvent(window, SDL_WINDOWEVENT_CLOSE, 0, 0);
            break;
        case BOS_EVENT_RESIZED:
            if ((window->flags & SDL_WINDOW_RESIZABLE) && !(window->flags & SDL_WINDOW_FULLSCREEN)) {
                SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESIZED, event.a, event.b);
            }
            break;
        case BOS_EVENT_HIDDEN:
            SDL_SendWindowEvent(window, SDL_WINDOWEVENT_MINIMIZED, 0, 0);
            break;
        case BOS_EVENT_SHOWN:
            SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESTORED, 0, 0);
            break;
        }
    }
}

/* Cursors: the desktop draws its own pointer; SDL only shows or hides it. */
static SDL_Cursor *new_cursor(void)
{
    SDL_Cursor *cursor = (SDL_Cursor *)SDL_calloc(1, sizeof(*cursor));
    if (!cursor) {
        SDL_OutOfMemory();
    }
    return cursor;
}

static SDL_Cursor *BASEOS_CreateCursor(SDL_Surface *surface, int hot_x, int hot_y)
{
    (void)surface;
    (void)hot_x;
    (void)hot_y;
    return new_cursor();
}

static SDL_Cursor *BASEOS_CreateSystemCursor(SDL_SystemCursor id)
{
    (void)id;
    return new_cursor();
}

static void BASEOS_FreeCursor(SDL_Cursor *cursor)
{
    SDL_free(cursor);
}

static int BASEOS_ShowCursor(SDL_Cursor *cursor)
{
    SDL_VideoDevice *device = SDL_GetVideoDevice();
    cursor_hidden = cursor == NULL;
    if (device) {
        for (SDL_Window *window = device->windows; window; window = window->next) {
            if (window->driverdata) {
                set_flag((BaseosWindow *)window->driverdata, BOS_WINDOW_HIDE_CURSOR, cursor_hidden);
            }
        }
    }
    return 0;
}

static int BASEOS_SetRelativeMouseMode(SDL_bool enabled)
{
    SDL_VideoDevice *device = SDL_GetVideoDevice();
    if (device) {
        for (SDL_Window *window = device->windows; window; window = window->next) {
            if (window->driverdata) {
                set_flag((BaseosWindow *)window->driverdata, BOS_WINDOW_RELATIVE_MOUSE, enabled);
            }
        }
    }
    return 0;
}

static void BASEOS_WarpMouse(SDL_Window *window, int x, int y)
{
    (void)window;
    (void)x;
    (void)y;
}

static int BASEOS_VideoInit(_THIS)
{
    BosDisplayInfo info;
    SDL_DisplayMode mode;
    SDL_zero(info);
    sys(BOS_SYS_DISPLAY_INFO, (long)&info, 0, 0);
    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_RGB888;
    mode.w = info.width ? (int)info.width : 1024;
    mode.h = info.height ? (int)info.height : 768;
    mode.refresh_rate = info.refresh_hz ? (int)info.refresh_hz : 60;
    if (SDL_AddBasicVideoDisplay(&mode) < 0) {
        return -1;
    }
    SDL_AddDisplayMode(&_this->displays[0], &mode);

    SDL_Mouse *mouse = SDL_GetMouse();
    mouse->CreateCursor = BASEOS_CreateCursor;
    mouse->CreateSystemCursor = BASEOS_CreateSystemCursor;
    mouse->ShowCursor = BASEOS_ShowCursor;
    mouse->FreeCursor = BASEOS_FreeCursor;
    mouse->WarpMouse = BASEOS_WarpMouse;
    mouse->SetRelativeMouseMode = BASEOS_SetRelativeMouseMode;
    SDL_SetDefaultCursor(new_cursor());
    return 0;
}

static void BASEOS_VideoQuit(_THIS)
{
    (void)_this;
}

static int BASEOS_GetDisplayUsableBounds(_THIS, SDL_VideoDisplay *display, SDL_Rect *rect)
{
    BosDisplayInfo info;
    (void)_this;
    (void)display;
    SDL_zero(info);
    sys(BOS_SYS_DISPLAY_INFO, (long)&info, 0, 0);
    rect->x = 0;
    rect->y = 0;
    rect->w = (int)info.work_width;
    rect->h = (int)info.work_height;
    return 0;
}

static void BASEOS_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device);
}

static SDL_VideoDevice *BASEOS_CreateDevice(void)
{
    SDL_VideoDevice *device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_OutOfMemory();
        return NULL;
    }
    device->VideoInit = BASEOS_VideoInit;
    device->VideoQuit = BASEOS_VideoQuit;
    device->GetDisplayUsableBounds = BASEOS_GetDisplayUsableBounds;
    device->PumpEvents = BASEOS_PumpEvents;
    device->CreateSDLWindow = BASEOS_CreateWindow;
    device->DestroyWindow = BASEOS_DestroyWindow;
    device->SetWindowTitle = BASEOS_SetWindowTitle;
    device->SetWindowFullscreen = BASEOS_SetWindowFullscreen;
    device->MaximizeWindow = BASEOS_MaximizeWindow;
    device->RestoreWindow = BASEOS_RestoreWindow;
    device->SetWindowResizable = BASEOS_SetWindowResizable;
    device->CreateWindowFramebuffer = BASEOS_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = BASEOS_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = BASEOS_DestroyWindowFramebuffer;
    device->free = BASEOS_DeleteDevice;
    return device;
}

VideoBootStrap BASEOS_bootstrap = {
    "baseos", "BaseOS windows",
    BASEOS_CreateDevice,
    NULL
};

#endif /* SDL_VIDEO_DRIVER_BASEOS */
