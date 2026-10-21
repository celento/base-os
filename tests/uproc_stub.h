/* Host fixtures link Terminal without the ELF runtime: no program starts. */
#ifndef TEST_UPROC_STUB_H
#define TEST_UPROC_STUB_H
#include "kernel/uproc.h"
#include "../sdk/baseos_sys.h"
int uproc_spawn(const void *image, uint32_t size, const char *const *argv, unsigned argc,
                uint32_t host, uint32_t *pid) {
    (void)image; (void)size; (void)argv; (void)argc; (void)host; (void)pid;
    return -BOS_ENOSYS;
}
int uproc_status(uint32_t pid, int *exit_code) { (void)pid; (void)exit_code; return UPROC_UNKNOWN; }
unsigned uproc_take_output(uint32_t pid, char *out, unsigned capacity) {
    (void)pid; (void)out; (void)capacity;
    return 0;
}
int uproc_kill(uint32_t pid) { (void)pid; return 0; }
int uproc_release(uint32_t pid) { (void)pid; return 0; }
#endif
