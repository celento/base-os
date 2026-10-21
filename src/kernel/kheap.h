#ifndef KHEAP_H
#define KHEAP_H
#include <stdint.h>

/* Kernel heap for objects whose number is not known at build time (ELF
 * processes, threads, address-space areas, file handles). Frames come from
 * physmem below PHYS_KERNEL_LIMIT, so every allocation is identity-mapped in
 * every root. Small objects use per-size pages; larger ones take physically
 * contiguous runs. Serialized kernel context only, never interrupt handlers.
 * Memory is zeroed. kfree(0) is a no-op; freeing anything else that kmalloc
 * did not return panics. */
void *kmalloc(uint32_t bytes);
void kfree(void *pointer);

typedef struct {
    uint32_t pages, small_pages, large_runs, live_objects, live_bytes;
} KheapStats;
void kheap_stats(KheapStats *out);
#endif
