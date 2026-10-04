/* Ordinary deterministic keyboard/mouse samples against the production FIFO. */
#include <assert.h>
#include <stdio.h>
#include "input_ingress.h"
static InputIngress in;
static InputSample sample;
static void fresh(unsigned type) { input_init(&in, 800, 600, 400, 300); input_mouse_type(&in, type); }
static void key(unsigned byte, unsigned ticks) { input_keyboard_byte(&in, (uint8_t)byte, ticks); }
static void pointer(int dx, int dy, unsigned buttons, int wheel, unsigned ticks) {
    input_mouse_byte(&in, (uint8_t)(8 | buttons | (dx < 0 ? 16 : 0) | (dy < 0 ? 32 : 0)), ticks);
    input_mouse_byte(&in, (uint8_t)dx, ticks);
    input_mouse_byte(&in, (uint8_t)dy, ticks);
    if (in.packet_bytes == 4) input_mouse_byte(&in, (uint8_t)wheel, ticks);
}
static InputSample pop(unsigned kind) { assert(input_pop(&in, &sample)); assert(sample.kind == kind); return sample; }
int main(void) {
    fresh(3);
    pointer(2, -3, INPUT_LEFT, 0, 10); key(0x2a, 11);
    pointer(4, 5, INPUT_LEFT | INPUT_RIGHT, -2, 12); key(0x1e, 13);
    pointer(1, 0, 0, 1, 14); key(0xaa, 15);
    InputSample s = pop(INPUT_POINTER); assert(s.serial == 1 && s.ticks == 10 && s.x == 402 && s.y == 303 && s.buttons == INPUT_LEFT && !s.modifiers);
    s = pop(INPUT_MODIFIER); assert(s.modifiers == INPUT_LSHIFT);
    s = pop(INPUT_POINTER); assert(s.ticks == 12 && s.x == 406 && s.y == 298 && s.buttons == 3 && s.wheel == -2 && s.modifiers == INPUT_LSHIFT);
    s = pop(INPUT_KEY); assert(s.character == 'A' && s.flags == INPUT_MAKE);
    s = pop(INPUT_POINTER); assert(!s.buttons && s.wheel == 1 && s.modifiers == INPUT_LSHIFT);
    assert(!pop(INPUT_MODIFIER).modifiers && !input_pending(&in));
    puts("Ingress ordering: rapid transitions, cross-device modifiers, coordinates and ticks passed.");

    fresh(0);
    /* A packet is one sample at completion, not at its first byte. */
    input_mouse_byte(&in, 9, 1); key(0x1d, 2); input_mouse_byte(&in, 5, 3);
    key(0xe0, 4); key(0x38, 4); input_mouse_byte(&in, 0, 5);
    assert(pop(INPUT_MODIFIER).modifiers == INPUT_LCTRL);
    assert(pop(INPUT_MODIFIER).modifiers == (INPUT_LCTRL | INPUT_RALT));
    s = pop(INPUT_POINTER); assert(s.ticks == 5 && s.modifiers == (INPUT_LCTRL | INPUT_RALT));
    /* All six modifier latches are independent on both make and break. */
    key(0x2a, 6); key(0x36, 7); key(0x38, 8); key(0xe0, 9); key(0x1d, 9);
    assert(pop(INPUT_MODIFIER).modifiers == (INPUT_LCTRL | INPUT_RALT | INPUT_LSHIFT));
    pop(INPUT_MODIFIER); pop(INPUT_MODIFIER); assert(pop(INPUT_MODIFIER).modifiers == 63);
    key(0xaa, 10); key(0xb8, 11); key(0x9d, 12); key(0x1e, 13);
    pop(INPUT_MODIFIER); pop(INPUT_MODIFIER); pop(INPUT_MODIFIER);
    s = pop(INPUT_KEY); assert(s.character == 'A' && s.modifiers == (INPUT_RSHIFT | INPUT_RCTRL | INPUT_RALT));
    key(0xb6, 14); key(0xe0, 15); key(0x9d, 15); key(0xe0, 16); key(0xb8, 16);
    pop(INPUT_MODIFIER); pop(INPUT_MODIFIER); assert(!pop(INPUT_MODIFIER).modifiers);
    key(0xe0, 17); key(0x48, 17); s = pop(INPUT_KEY); assert(s.scancode == 0x48 && !s.character && s.flags == (INPUT_MAKE | INPUT_EXTENDED));
    key(0xe0, 18); key(0xc8, 18); assert(!(pop(INPUT_KEY).flags & INPUT_MAKE));
    key(0xe0, 19); key(0x2a, 19); assert(!input_pending(&in));
    const uint8_t pause[] = {0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5};
    for (unsigned i = 0; i < sizeof pause; ++i) key(pause[i], 20);
    assert(!input_pending(&in) && !in.modifiers);
    puts("Ingress keyboard: independent left/right modifiers, legacy characters and extended keys passed.");

    fresh(4); pointer(255, 255, 0, 15, 1); s = pop(INPUT_POINTER); assert(s.x == 655 && s.y == 45 && s.wheel == -1);
    pointer(255, 255, 0, 1, 2); s = pop(INPUT_POINTER); assert(s.x == 799 && s.y == 0 && s.wheel == 1);
    input_resize(&in, 320, 200); assert(in.x == 319 && in.y == 0);
    pointer(-255, -255, 0, 0, 3); s = pop(INPUT_POINTER); assert(s.x == 64 && s.y == 199);
    pointer(-255, 0, 0, 0, 4); assert(pop(INPUT_POINTER).x == 0);

    fresh(0);
    for (unsigned i = 0; i < INPUT_CAPACITY; ++i) key(0x1e, i);
    assert(in.count == INPUT_CAPACITY && !in.loss);
    key(0x2a, 300); pointer(1, 1, INPUT_LEFT, 0, 301); key(0xaa, 302);
    assert(!in.count && in.loss == INPUT_LOSS_QUEUE);
    s = pop(INPUT_RESET); assert(s.dropped == INPUT_CAPACITY + 3 && s.buttons == INPUT_LEFT && !s.modifiers && s.ticks == 302 && s.x == 401);
    assert(!input_pending(&in)); pointer(0, 0, 0, 0, 303); assert(!pop(INPUT_POINTER).buttons);
    /* Refill/drain rings through multiple normal wrap-arounds. */
    for (unsigned turn = 0; turn < 20; ++turn) {
        for (unsigned i = 0; i < INPUT_BATCH; ++i) pointer(0, 0, i & 1, 0, i);
        for (unsigned i = 0; i < INPUT_BATCH; ++i) assert(pop(INPUT_POINTER).buttons == (i & 1));
    }
    puts("Ingress finite loss: exact capacity, reset snapshot and queue reuse passed.");

    fresh(0); pointer(0, 0, 1, 0, 1); key(0x1e, 2); input_pointer_fence(&in);
    pointer(0, 0, 0, 0, 3);
    assert(pop(INPUT_POINTER).epoch != in.epoch); assert(pop(INPUT_KEY).character == 'a');
    assert(pop(INPUT_POINTER).epoch == in.epoch);
    puts("Ingress fences: pointer epochs preserve keyboard ordering passed.");
    puts("All ordered ingress checks passed.");
    return 0;
}
