#include "baseos.h"
/* BEX2 memory is zeroed by the loader before entry. No relocation or dynamic
 * setup is required. Code, stack and all C pointers use segment offsets. */
extern int main(void);
__attribute__((section(".text.start"), noreturn)) void _start(void) {
    bos_exit(main());
    __builtin_unreachable();
}
