#ifndef PHYSMEM_H
#define PHYSMEM_H
#include "platform.h"
#include "../sdk/baseos_abi.h"

/* Kernel-only owned physical frames. These addresses are never app handles or
 * an application heap. Call only in serialized desktop/process context, never
 * from IRQs, device polling, or a zero-page callback. No interrupt masking is
 * needed: those paths cannot allocate, release, or switch owners. */
#define PHYS_PAGE_COUNT (PHYS_MANAGED_END / PHYS_PAGE_BYTES)
#define PHYS_BATCH_MAX 1024u
enum PhysPageKind {
    PHYS_UNAVAILABLE, PHYS_FREE, PHYS_BEX1_BACKING, PHYS_USER_IMAGE,
    PHYS_PAGE_TABLE, PHYS_PAGE_DIRECTORY, PHYS_KIND_LIMIT
};
enum PhysResult {
    PHYS_OK = 0, PHYS_INVALID = -1, PHYS_CAPACITY = -2,
    PHYS_NOT_OWNED = -3, PHYS_NOT_READY = -4, PHYS_BUSY = -5
};
typedef struct {
    uint32_t total, free, allocated, high_water;
    uint32_t by_kind[PHYS_KIND_LIMIT];
} PhysmemStats;
typedef struct {
    const char *name;
    uint32_t base, end;
} PhysReservation;

/* The production core is also used unchanged by ordinary host fixtures with
 * real metadata arrays and a host-backed zero callback. All pointers remain
 * kernel-private. The caller supplies exactly PHYS_PAGE_COUNT entries in each
 * metadata array and initially zeroes the small core object. A core cannot be
 * reinitialized once ready, even when empty. No reset can drop live ownership.
 * zero_page must synchronously clear all 4096 bytes and cannot fail or recurse. */
typedef void (*PhysZeroPage)(uint32_t frame, void *context);
typedef struct {
    uint32_t *owners;
    uint8_t *kinds;
    PhysZeroPage zero_page;
    void *zero_context;
    PhysmemStats stats;
    unsigned ready, busy;
} PhysmemCore;

const PhysReservation *physmem_reservations(unsigned *count);
int physmem_core_init(PhysmemCore *core, uint32_t *owners, uint8_t *kinds,
                      const MemoryRange *map, unsigned count,
                      const BootInfo *video, PhysZeroPage zero_page, void *context);
/* Owners must be live, nonreused process handles issued by process.c; this
 * service checks the process domain but does not create a second owner table.
 * Lists contain 1..PHYS_BATCH_MAX individually scattered frames. All argument,
 * ownership, and capacity failures leave output, metadata and counts unchanged.
 * Allocation claims and clears every frame before publishing any output.
 * Release preflights the whole list, including duplicate frames, before writes.
 * An already released list returns PHYS_NOT_OWNED. Caller buffers must be valid
 * kernel memory, distinct from metadata and the frames being allocated. */
int physmem_core_alloc(PhysmemCore *core, BosHandle owner, enum PhysPageKind kind,
                       unsigned count, uint32_t *frames);
int physmem_core_release(PhysmemCore *core, BosHandle owner, enum PhysPageKind kind,
                         const uint32_t *frames, unsigned count);
int physmem_core_release_owner(PhysmemCore *core, BosHandle owner);
int physmem_core_stats(const PhysmemCore *core, PhysmemStats *stats);
unsigned physmem_core_owner_pages(const PhysmemCore *core, BosHandle owner);

/* Late boot wrapper: only after platform_validate_memory and video_init. A
 * successful zero-capacity initialization is possible and is not a boot fault.
 * Desktop BEX1 creation claims 16 zeroed backing frames; legacy exec does not. */
int physmem_init(void);
int physmem_alloc(BosHandle owner, enum PhysPageKind kind, unsigned count, uint32_t *frames);
int physmem_release(BosHandle owner, enum PhysPageKind kind, const uint32_t *frames, unsigned count);
int physmem_release_owner(BosHandle owner);
int physmem_stats(PhysmemStats *stats);
unsigned physmem_owner_pages(BosHandle owner);
#endif
