#include "kernel_owner.h"
static unsigned kernel_owner_serial;
unsigned kernel_owner_allocate(void) {
    if (kernel_owner_serial == 0x0fffffffu) return 0;
    return 0xf0000000u | ++kernel_owner_serial;
}
