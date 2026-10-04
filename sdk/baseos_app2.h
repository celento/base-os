#ifndef BASEOS_APP2_H
#define BASEOS_APP2_H
#include "baseos.h"
#include "baseos_executable.h"
/* Defined only by app2.ld. This fixed, launch-declared committed workspace is
 * zero on entry and local to the app; it is not an allocation/resize service.
 * There are unmapped gaps in the 4 MiB extent. Retain ordinary syscall transfer
 * caps (4 KiB reads, 32 KiB replacement) when using these offset pointers. */
extern unsigned char __workspace_start[], __workspace_end[];
extern unsigned char __stack_bottom[], __stack_top[];
extern unsigned char __data_memory_end[];
static inline void *bos_workspace(void) { return __workspace_start; }
static inline unsigned bos_workspace_bytes(void) {
    return (unsigned)__workspace_end-(unsigned)__workspace_start;
}
#endif
