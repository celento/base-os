#ifndef ELF_H
#define ELF_H
#include <stdint.h>
#include "kernel/vm.h"

/* Static ELF32 i386 executables (ET_EXEC) for ELF processes. */
typedef struct {
    uint32_t entry;
    uint32_t phdr, phent, phnum;   /* program headers as mapped, for auxv */
} ElfImage;

/* 1 if the bytes start with the ELF magic, whatever else they contain. */
int elf_is_elf(const void *image, uint32_t size);
/* Map and copy every PT_LOAD segment of image into space, which must not be
 * active. The image must stay readable while the space is active, so it must
 * live below PHYS_KERNEL_LIMIT. Returns 0 or a negated BOS_E* error; on
 * failure the space holds partial mappings that vm_destroy releases. */
int elf_load(VmSpace *space, const void *image, uint32_t size, ElfImage *out);
#endif
