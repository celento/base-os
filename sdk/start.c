#include "baseos.h"
extern int main(void);
__attribute__((section(".text.start"), noreturn)) void _start(void) {
    bos_exit(main());
    __builtin_unreachable();
}
