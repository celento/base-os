#ifndef BASEOS_INPUT_INGRESS_H
#define BASEOS_INPUT_INGRESS_H
#include <stdint.h>

/* Internal device samples, not a native application ABI. One polled producer
 * and one desktop consumer; callers must serialize if IRQ acquisition is added. */
#define INPUT_CAPACITY 256u
#define INPUT_BATCH 64u
#define INPUT_LEFT 1u
#define INPUT_RIGHT 2u
#define INPUT_LSHIFT (1u << 0)
#define INPUT_RSHIFT (1u << 1)
#define INPUT_LCTRL (1u << 2)
#define INPUT_RCTRL (1u << 3)
#define INPUT_LALT (1u << 4)
#define INPUT_RALT (1u << 5)
#define INPUT_SHIFT (INPUT_LSHIFT | INPUT_RSHIFT)
#define INPUT_CTRL (INPUT_LCTRL | INPUT_RCTRL)
#define INPUT_ALT (INPUT_LALT | INPUT_RALT)
/* Pointer speed levels. Per axis, a packet of at most INPUT_PRECISION counts
 * moves that many pixels at every level, so slow hand motion stays exact.
 * Counts beyond it are scaled by the level's gain in eighths. Level
 * INPUT_SPEED_UNITY is the plain 1:1 PS/2 mapping. */
#define INPUT_SPEED_LEVELS 10u
#define INPUT_SPEED_DEFAULT 6u
#define INPUT_SPEED_UNITY 2u
#define INPUT_PRECISION 2
#define INPUT_SPEED_EIGHTHS {4, 6, 8, 10, 12, 14, 16, 20, 24, 32}
enum { INPUT_KEY = 1, INPUT_MODIFIER, INPUT_POINTER, INPUT_RESET };
enum { INPUT_MAKE = 1, INPUT_EXTENDED = 2 };
enum { INPUT_LOSS_QUEUE = 1, INPUT_LOSS_DEVICE = 2 };
typedef struct {
    uint64_t serial, epoch;
    uint32_t ticks, modifiers, buttons;
    int32_t x, y, wheel;
    uint32_t kind, flags, scancode, character, dropped, reason;
} InputSample;
typedef struct {
    InputSample queue[INPUT_CAPACITY];
    uint64_t serial, epoch;
    uint32_t head, count, modifiers, buttons, ticks, dropped, loss;
    int32_t x, y, width, height;
    uint8_t packet[4], packet_n, packet_bytes, mouse_type, extended, pause_left, speed;
} InputIngress;

void input_init(InputIngress *in, int width, int height, int x, int y);
void input_mouse_type(InputIngress *in, unsigned type);
void input_keyboard_byte(InputIngress *in, uint8_t byte, uint32_t ticks);
void input_mouse_byte(InputIngress *in, uint8_t byte, uint32_t ticks);
void input_device_loss(InputIngress *in, uint32_t ticks);
int input_pending(const InputIngress *in);
int input_pop(InputIngress *in, InputSample *sample);
/* Fence pending pointer samples before autonomous scene/lifetime changes.
 * Keyboard samples keep their order. No dispatch or callbacks occur here. */
void input_pointer_fence(InputIngress *in);
/* End an exclusive synchronous input owner without replaying its backlog. */
void input_discard(InputIngress *in);
void input_resize(InputIngress *in, int width, int height);
/* Out-of-range levels select INPUT_SPEED_DEFAULT. */
void input_set_speed(InputIngress *in, unsigned level);
int input_pointer_scale(unsigned level, int counts);
#endif
