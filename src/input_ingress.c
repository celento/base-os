#include "input_ingress.h"

static const char keymap[0x40] = {
    0,   0,   '1', '2', '3', '4', '5', '6',
    '7', '8', '9', '0', '-', '=', 0,   0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    'o', 'p', '[', ']', 0,   0,   'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*',
    0,   ' ', 0,   0,   0,   0,   0,   0};

static const char keymap_shift[0x40] = {
    0,   0,   '!', '@', '#', '$', '%', '^',
    '&', '*', '(', ')', '_', '+', 0,   0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{', '}', 0,   0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~', 0,  '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*',
    0,   ' ', 0,   0,   0,   0,   0,   0};

static int clamp(int value, int limit) {
    return value < 0 ? 0 : value >= limit ? limit - 1 : value;
}
static void add_dropped(InputIngress *in, uint32_t n) {
    if (UINT32_MAX - in->dropped < n) in->dropped = UINT32_MAX;
    else in->dropped += n;
}
static InputSample snapshot(const InputIngress *in) {
    InputSample s = {0};
    s.serial = in->serial; s.epoch = in->epoch; s.ticks = in->ticks;
    s.modifiers = in->modifiers; s.buttons = in->buttons;
    s.x = in->x; s.y = in->y;
    return s;
}
static void lose(InputIngress *in, uint32_t reason) {
    add_dropped(in, in->count);
    in->head = in->count = 0;
    in->loss |= reason;
}
static void push(InputIngress *in, InputSample s) {
    s.serial = ++in->serial;
    if (in->loss) { add_dropped(in, 1); return; }
    if (in->count == INPUT_CAPACITY) {
        lose(in, INPUT_LOSS_QUEUE); add_dropped(in, 1); return;
    }
    in->queue[(in->head + in->count) % INPUT_CAPACITY] = s;
    ++in->count;
}
void input_init(InputIngress *in, int width, int height, int x, int y) {
    /* Do not put the 16 KiB FIFO on the kernel stack as a compound temporary. */
    in->serial = 0; in->epoch = 1;
    in->head = in->count = in->modifiers = in->buttons = in->ticks = 0;
    in->dropped = in->loss = 0;
    in->packet_n = in->extended = in->pause_left = 0;
    input_mouse_type(in, 0);
    in->x = x; in->y = y; input_resize(in, width, height);
}
void input_resize(InputIngress *in, int width, int height) {
    in->width = width > 0 ? width : 1; in->height = height > 0 ? height : 1;
    in->x = clamp(in->x, in->width); in->y = clamp(in->y, in->height);
}
void input_mouse_type(InputIngress *in, unsigned type) {
    in->mouse_type = (uint8_t)type;
    in->packet_bytes = type == 3 || type == 4 ? 4 : 3;
    in->packet_n = 0;
}
void input_keyboard_byte(InputIngress *in, uint8_t byte, uint32_t ticks) {
    in->ticks = ticks;
    if (in->pause_left) { --in->pause_left; return; }
    if (byte == 0xe1) { in->pause_left = 5; in->extended = 0; return; }
    if (byte == 0xe0) { in->extended = 1; return; }
    unsigned ext = in->extended, sc = byte & 0x7f, make = !(byte & 0x80), mod = 0;
    in->extended = 0;
    /* Print Screen's extended fake shifts are not physical modifier latches. */
    if (ext && (sc == 0x2a || sc == 0x36)) return;
    if (!ext && sc == 0x2a) mod = INPUT_LSHIFT;
    if (!ext && sc == 0x36) mod = INPUT_RSHIFT;
    if (sc == 0x1d) mod = ext ? INPUT_RCTRL : INPUT_LCTRL;
    if (sc == 0x38) mod = ext ? INPUT_RALT : INPUT_LALT;
    if (mod) {
        if (make) in->modifiers |= mod;
        else in->modifiers &= ~mod;
    }
    InputSample s = snapshot(in);
    s.kind = mod ? INPUT_MODIFIER : INPUT_KEY;
    s.flags = (make ? INPUT_MAKE : 0) | (ext ? INPUT_EXTENDED : 0);
    s.scancode = sc;
    if (!mod && make && sc < 0x40)
        s.character = (unsigned char)((in->modifiers & INPUT_SHIFT) ? keymap_shift[sc] : keymap[sc]);
    push(in, s);
}
void input_device_loss(InputIngress *in, uint32_t ticks) {
    in->ticks = ticks;
    in->packet_n = in->extended = in->pause_left = 0;
    /* A missing keyboard break is unknowable. Reset the modifier latches,
     * rather than leaving a permanently asserted shortcut modifier. */
    in->modifiers = 0;
    lose(in, INPUT_LOSS_DEVICE);
    add_dropped(in, 1);
}
void input_mouse_byte(InputIngress *in, uint8_t byte, uint32_t ticks) {
    if (!in->packet_n && !(byte & 0x08)) {
        input_device_loss(in, ticks); return;
    }
    in->packet[in->packet_n++] = byte;
    if (in->packet_n < in->packet_bytes) return;
    in->packet_n = 0; in->ticks = ticks;
    uint8_t flags = in->packet[0];
    in->buttons = flags & (INPUT_LEFT | INPUT_RIGHT);
    if (flags & 0xc0) {
        /* Valid button snapshot, but movement was lost. Never infer an edge. */
        lose(in, INPUT_LOSS_DEVICE); add_dropped(in, 1); return;
    }
    int dx = in->packet[1] - ((flags & 0x10) ? 256 : 0);
    int dy = in->packet[2] - ((flags & 0x20) ? 256 : 0);
    in->x = clamp(in->x + dx, in->width); in->y = clamp(in->y - dy, in->height);
    InputSample s = snapshot(in); s.kind = INPUT_POINTER;
    if (in->packet_bytes == 4) {
        s.wheel = in->mouse_type == 4 ? in->packet[3] & 15 : (int8_t)in->packet[3];
        if (in->mouse_type == 4 && s.wheel >= 8) s.wheel -= 16;
    }
    push(in, s);
}
int input_pending(const InputIngress *in) { return in->loss || in->count; }
int input_pop(InputIngress *in, InputSample *s) {
    if (in->loss) {
        *s = snapshot(in); s->kind = INPUT_RESET;
        s->reason = in->loss; s->dropped = in->dropped;
        in->loss = in->dropped = 0;
        return 1;
    }
    if (!in->count) return 0;
    *s = in->queue[in->head]; in->head = (in->head + 1) % INPUT_CAPACITY; --in->count;
    return 1;
}
void input_pointer_fence(InputIngress *in) { ++in->epoch; }
