/* Include right before kernel/fs.c in host tests: fs.c mounts BaseFS 2 with
 * kernel/basefs.c and the kernel heap. Tests of the snapshot formats keep
 * marked disks in those formats unless they define FS_CONVERT_TO_BASEFS. */
#ifndef FS_BASEFS_SUPPORT_H
#define FS_BASEFS_SUPPORT_H
#include <stdlib.h>
#ifndef FS_CONVERT_TO_BASEFS
#define FS_CONVERT_TO_BASEFS 0
#endif
void *kmalloc(uint32_t bytes) { return calloc(1, bytes ? bytes : 1); }
void kfree(void *pointer) { free(pointer); }
#include "../src/kernel/basefs.c"
#endif
