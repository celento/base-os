/* Ordinary deterministic map, ownership and finite-capacity sequences. Real
 * physmem.c/bootinfo.c are linked unchanged. No physical address is dereferenced
 * and no privileged instruction or guest memory-fault scenario runs here. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "physmem.h"

char __kernel_end[1];
void panic(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
void kmemset(void *dst, int value, int bytes) { memset(dst, value, (size_t)bytes); }
static uint32_t owners[PHYS_PAGE_COUNT], saved_owners[PHYS_PAGE_COUNT];
static uint8_t kinds[PHYS_PAGE_COUNT], saved_kinds[PHYS_PAGE_COUNT];
static unsigned char *contents[PHYS_PAGE_COUNT];
static PhysmemCore core;
static unsigned zero_calls;
static uint32_t *unpublished;
static unsigned unpublished_count;
static const BosHandle alice = BOS_HANDLE_TYPE_PROCESS | 1;
static const BosHandle bob = BOS_HANDLE_TYPE_PROCESS | 2;
static const BosHandle carol = BOS_HANDLE_TYPE_PROCESS | 3;
static const BootInfo normal_video = {
    .magic = BOOTINFO_MAGIC, .lfb = 0xfd000000, .width = 1280, .height = 720,
    .pitch = 5120, .bpp = 32, .flags = 1, .red_size = 8, .red_pos = 16,
    .green_size = 8, .green_pos = 8, .blue_size = 8, .blue_pos = 0
};
static void zero_page(uint32_t frame, void *context) {
    assert(context == &zero_calls && frame % PHYS_PAGE_BYTES == 0 && frame < PHYS_MANAGED_END);
    unsigned pfn = frame / PHYS_PAGE_BYTES;
    assert(core.busy && owners[pfn] && kinds[pfn] != PHYS_FREE);
    for (unsigned i = 0; i < unpublished_count; ++i) assert(unpublished[i] == 0xdeadbeef);
    uint32_t unused;
    assert(physmem_core_alloc(&core, bob, PHYS_BEX1_BACKING, 1, &unused) == PHYS_BUSY);
    assert(physmem_core_release_owner(&core, alice) == PHYS_BUSY);
    PhysmemStats stats;
    assert(physmem_core_stats(&core, &stats) == PHYS_BUSY);
    if (!contents[pfn]) {
        contents[pfn] = malloc(PHYS_PAGE_BYTES);
        assert(contents[pfn]);
        memset(contents[pfn], 0xa5, PHYS_PAGE_BYTES);
    }
    memset(contents[pfn], 0, PHYS_PAGE_BYTES);
    ++zero_calls;
}
static void fresh(void) {
    for (unsigned pfn = 0; pfn < PHYS_PAGE_COUNT; ++pfn) {
        free(contents[pfn]);
        contents[pfn] = 0;
    }
    memset(&core, 0, sizeof core);
    memset(owners, 0x55, sizeof owners);
    memset(kinds, 0x55, sizeof kinds);
    zero_calls = unpublished_count = 0;
    unpublished = 0;
}
static void initialize(const MemoryRange *map, unsigned count, const BootInfo *video) {
    fresh();
    assert(physmem_core_init(&core, owners, kinds, map, count, video, zero_page, &zero_calls) == PHYS_OK);
    assert(zero_calls == 0); /* Initialization touches metadata only. */
}
static PhysmemStats stats(void) {
    PhysmemStats result;
    assert(physmem_core_stats(&core, &result) == PHYS_OK);
    assert(result.free + result.allocated == result.total);
    unsigned used = 0;
    for (unsigned kind = PHYS_BEX1_BACKING; kind < PHYS_KIND_LIMIT; ++kind) used += result.by_kind[kind];
    assert(used == result.allocated && result.by_kind[PHYS_FREE] == 0 && result.by_kind[PHYS_UNAVAILABLE] == 0);
    return result;
}
static int candidate(uint32_t frame) {
    return (frame >= 0x00510000 && frame < 0x00600000) ||
           (frame >= 0x00680000 && frame < 0x00700000) ||
           (frame >= 0x00f03000 && frame < 0x01000000) ||
           (frame >= 0x01650000 && frame < 0x01700000) ||
           (frame >= 0x07f00000 && frame < PHYS_MANAGED_END);
}
static void check_page_set(unsigned expected, uint32_t ram_end) {
    unsigned count = 0;
    for (unsigned pfn = 0; pfn < PHYS_PAGE_COUNT; ++pfn) {
        assert(owners[pfn] == 0);
        uint32_t frame = pfn * PHYS_PAGE_BYTES;
        int eligible = candidate(frame) && frame < ram_end;
        assert((kinds[pfn] == PHYS_FREE) == eligible);
        count += eligible;
    }
    assert(count == expected && stats().total == expected && stats().free == expected);
}
static void check_zeroed(const uint32_t *frames, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        assert(frames[i] % PHYS_PAGE_BYTES == 0 && candidate(frames[i]));
        unsigned char *bytes = contents[frames[i] / PHYS_PAGE_BYTES];
        assert(bytes);
        for (unsigned j = 0; j < PHYS_PAGE_BYTES; ++j) assert(bytes[j] == 0);
        for (unsigned j = 0; j < i; ++j) assert(frames[j] != frames[i]);
    }
}
static void allocate(BosHandle owner, enum PhysPageKind kind, unsigned count, uint32_t *frames) {
    for (unsigned i = 0; i < count; ++i) frames[i] = 0xdeadbeef;
    unpublished = frames; unpublished_count = count;
    unsigned before = zero_calls;
    assert(physmem_core_alloc(&core, owner, kind, count, frames) == PHYS_OK);
    unpublished = 0; unpublished_count = 0;
    assert(zero_calls - before == count);
    check_zeroed(frames, count);
}
static PhysmemStats save_state(void) {
    memcpy(saved_owners, owners, sizeof owners);
    memcpy(saved_kinds, kinds, sizeof kinds);
    return stats();
}
static void unchanged(PhysmemStats before) {
    PhysmemStats now = stats();
    assert(memcmp(&before, &now, sizeof now) == 0);
    assert(memcmp(saved_owners, owners, sizeof owners) == 0);
    assert(memcmp(saved_kinds, kinds, sizeof kinds) == 0);
}
static void profile_maps(void) {
    const unsigned ends[] = {64u << 20, 128u << 20, 256u << 20, 512u << 20};
    const unsigned totals[] = {797, 1053, 33821, 33821};
    for (unsigned i = 0; i < 4; ++i) {
        MemoryRange map[] = {{0, 0x9fc00, 1, 1}, {0x9fc00, 0x60400, 2, 1},
                             {0x100000, ends[i] - 0x100000, 1, 1},
                             {0xfffc0000, 0x40000, 2, 1}};
        initialize(map, 4, &normal_video);
        check_page_set(totals[i], ends[i]);
    }
    unsigned count;
    const PhysReservation *reserved = physmem_reservations(&count);
    assert(count == 27);
    for (unsigned i = 0; i < count; ++i) {
        assert(reserved[i].name && reserved[i].base < reserved[i].end);
        for (uint32_t p = reserved[i].base / PHYS_PAGE_BYTES;
             p < (reserved[i].end + PHYS_PAGE_BYTES - 1) / PHYS_PAGE_BYTES; ++p)
            assert(kinds[p] == PHYS_UNAVAILABLE);
    }
    /* There is deliberately no profile flag to toggle: default, large,
     * fallback and migrating mounts all retain this identical exclusion union. */
    assert(kinds[AUDIO_WORK_BASE / PHYS_PAGE_BYTES] == PHYS_UNAVAILABLE);
    assert(kinds[AUDIO_LARGE_WORK_BASE / PHYS_PAGE_BYTES] == PHYS_UNAVAILABLE);
    assert(kinds[FS_POOL_BASE / PHYS_PAGE_BYTES] == PHYS_UNAVAILABLE);
    assert(kinds[FS_LARGE_POOL_BASE / PHYS_PAGE_BYTES] == PHYS_UNAVAILABLE);
    assert(kinds[FS_LARGE_IMG_BASE / PHYS_PAGE_BYTES] == PHYS_UNAVAILABLE);
    puts("owned pages: 64/128/256 MiB counts 797/1053/33821; 512 MiB remains ceiling-bounded");
}
static void map_unions(void) {
    /* Unsorted, overlapping and adjacent descriptors jointly cover full pages;
     * an enabled reserved sub-page removes the entire intersected page. */
    MemoryRange map[] = {
        {0x00511800, 0x3800, 1, 1}, {0x00510000, 0x1800, 1, 1},
        {0x00512000, 0x1000, 1, 1}, {0x00513100, 1, 2, 1},
        {0x00510000, 0x5000, 2, 0}, {0x00515000, 0, 2, 1}
    };
    initialize(map, 6, &normal_video);
    assert(stats().total == 4);
    assert(kinds[0x510000 / PHYS_PAGE_BYTES] == PHYS_FREE);
    assert(kinds[0x511000 / PHYS_PAGE_BYTES] == PHYS_FREE);
    assert(kinds[0x512000 / PHYS_PAGE_BYTES] == PHYS_FREE);
    assert(kinds[0x513000 / PHYS_PAGE_BYTES] == PHYS_UNAVAILABLE);
    assert(kinds[0x514000 / PHYS_PAGE_BYTES] == PHYS_FREE);
    map[1].base++; map[1].length--;
    initialize(map, 6, &normal_video);
    assert(stats().total == 3); /* First page has a one-byte coverage gap. */
    map[0].attributes = 0;
    initialize(map, 6, &normal_video);
    assert(stats().total == 1 && kinds[0x512000 / PHYS_PAGE_BYTES] == PHYS_FREE);
    MemoryRange edges[] = {{0x00510001, 3 * PHYS_PAGE_BYTES - 2, 1, 1}};
    initialize(edges, 1, &normal_video);
    assert(stats().total == 1 && kinds[0x511000 / PHYS_PAGE_BYTES] == PHYS_FREE);
    MemoryRange none[] = {{0, 256u << 20, 2, 1}};
    initialize(none, 1, &normal_video);
    assert(stats().total == 0);
    uint32_t output = 0xcafebabe;
    assert(physmem_core_alloc(&core, alice, PHYS_BEX1_BACKING, 1, &output) == PHYS_CAPACITY);
    assert(output == 0xcafebabe && stats().allocated == 0);
    puts("owned pages: exact usable union, descriptor ordering, page edges and reservation precedence passed");
}
static void descriptor_bounds(void) {
    MemoryRange map[E820_MAX] = {{0x100000, (64u << 20) - 0x100000, 1, 1}};
    initialize(map, E820_MAX, &normal_video);
    assert(stats().total == 797);
    fresh();
    assert(physmem_core_init(&core, owners, kinds, map, 0, &normal_video, zero_page, &zero_calls) == PHYS_INVALID);
    assert(physmem_core_init(&core, owners, kinds, map, E820_MAX + 1, &normal_video, zero_page, &zero_calls) == PHYS_INVALID);
    map[1] = (MemoryRange){UINT64_MAX - 5, 16, 1, 1};
    assert(physmem_core_init(&core, owners, kinds, map, 2, &normal_video, zero_page, &zero_calls) == PHYS_INVALID);
    for (unsigned pfn = 0; pfn < PHYS_PAGE_COUNT; ++pfn)
        assert(owners[pfn] == 0x55555555 && kinds[pfn] == 0x55);
    map[1].attributes = 0;
    assert(physmem_core_init(&core, owners, kinds, map, 2, &normal_video, zero_page, &zero_calls) == PHYS_OK);
    assert(stats().total == 797);
    puts("owned pages: descriptor count/overflow bounds fail without metadata writes");
}
static void framebuffer_exclusion(void) {
    MemoryRange map = {0x100000, (256u << 20) - 0x100000, 1, 1};
    BootInfo video = normal_video;
    video.lfb = 0x08000001;
    initialize(&map, 1, &video);
    assert(stats().total == 33821 - 1025); /* Unaligned 4 MiB, rounded outward. */
    assert(kinds[0x08000000 / PHYS_PAGE_BYTES] == PHYS_UNAVAILABLE);
    assert(kinds[0x08400000 / PHYS_PAGE_BYTES] == PHYS_UNAVAILABLE);
    assert(kinds[0x08401000 / PHYS_PAGE_BYTES] == PHYS_FREE);
    video.pitch = 8192; video.height = 1024;
    initialize(&map, 1, &video);
    assert(stats().total == 33821 - 2049);
    video = normal_video; video.lfb = 0xffc00000;
    initialize(&map, 1, &video); /* Exact 4 GiB exclusive end is representable. */
    assert(stats().total == 33821);
    fresh(); video.lfb = 0xffc00001;
    assert(physmem_core_init(&core, owners, kinds, &map, 1, &video, zero_page, &zero_calls) == PHYS_INVALID);
    assert(!core.ready && owners[0] == 0x55555555);
    puts("owned pages: dynamic framebuffer covers all supported mode bytes and uses widened arithmetic");
}
static void ownership_and_capacity(void) {
    MemoryRange map = {0x100000, (64u << 20) - 0x100000, 1, 1};
    initialize(&map, 1, &normal_video);
    uint32_t a[797], b[16], more[16];
    allocate(alice, PHYS_BEX1_BACKING, 300, a);
    assert(a[239] == 0x005ff000 && a[240] == 0x00680000);
    allocate(bob, PHYS_PAGE_TABLE, 16, b);
    allocate(alice, PHYS_BEX1_BACKING, 16, more); /* Same owner/kind may grow. */
    assert(more[0] != a[0] && more[0] != b[0]);
    allocate(alice, PHYS_USER_IMAGE, 4, a + 300);
    assert(physmem_core_owner_pages(&core, alice) == 320);
    PhysmemStats before = save_state();
    assert(physmem_core_release(&core, alice, PHYS_PAGE_TABLE, b, 16) == PHYS_NOT_OWNED);
    unchanged(before);
    uint32_t mixed[] = {a[0], b[0]};
    assert(physmem_core_release(&core, alice, PHYS_BEX1_BACKING, mixed, 2) == PHYS_NOT_OWNED);
    unchanged(before);
    uint32_t duplicate[] = {a[0], a[0]};
    assert(physmem_core_release(&core, alice, PHYS_BEX1_BACKING, duplicate, 2) == PHYS_INVALID);
    unchanged(before);
    assert(physmem_core_release(&core, bob, PHYS_BEX1_BACKING, b, 16) == PHYS_NOT_OWNED);
    unchanged(before);
    assert(physmem_core_init(&core, owners, kinds, &map, 1, &normal_video, zero_page, &zero_calls) == PHYS_BUSY);
    unchanged(before);
    assert(physmem_core_release(&core, bob, PHYS_PAGE_TABLE, b, 16) == PHYS_OK);
    before = save_state();
    assert(physmem_core_release(&core, bob, PHYS_PAGE_TABLE, b, 16) == PHYS_NOT_OWNED);
    unchanged(before);
    assert(physmem_core_release_owner(&core, alice) == PHYS_OK);
    assert(physmem_core_release_owner(&core, alice) == PHYS_OK); /* Cleanup is idempotent. */
    assert(stats().free == 797 && stats().high_water == 336);
    /* Used contents survive release but are cleared for the next fresh owner. */
    memset(contents[a[0] / PHYS_PAGE_BYTES], 0xb7, PHYS_PAGE_BYTES);
    allocate(carol, PHYS_BEX1_BACKING, 790, a);
    assert(stats().free == 7 && stats().allocated == 790);
    before = save_state();
    uint32_t output[16];
    for (unsigned i = 0; i < 16; ++i) output[i] = 0xcafebabe;
    unsigned before_zero = zero_calls;
    assert(physmem_core_alloc(&core, bob, PHYS_BEX1_BACKING, 16, output) == PHYS_CAPACITY);
    unchanged(before);
    assert(zero_calls == before_zero);
    for (unsigned i = 0; i < 16; ++i) assert(output[i] == 0xcafebabe);
    allocate(carol, PHYS_BEX1_BACKING, 7, a + 790);
    assert(stats().free == 0 && stats().allocated == 797 && stats().high_water == 797);
    before = save_state();
    assert(physmem_core_alloc(&core, bob, PHYS_BEX1_BACKING, 1, output) == PHYS_CAPACITY);
    assert(physmem_core_release(&core, bob, PHYS_PAGE_TABLE, b, 16) == PHYS_NOT_OWNED);
    unchanged(before); /* A former owner's stale list cannot free reused frames. */
    assert(physmem_core_release(&core, carol, PHYS_BEX1_BACKING, a, 797) == PHYS_OK);
    assert(stats().free == 797 && stats().allocated == 0);
    assert(physmem_core_owner_pages(&core, alice) == 0 && physmem_core_owner_pages(&core, carol) == 0);
    allocate(bob, PHYS_PAGE_DIRECTORY, 16, output);
    assert(physmem_core_release_owner(&core, bob) == PHYS_OK);
    assert(stats().free == 797 && stats().allocated == 0);
    puts("owned pages: disjoint allocation, deferred output, zero/reuse, owner isolation and exact capacity cleanup passed");
}
static void high_pages_and_batch_limit(void) {
    MemoryRange map = {0x100000, (128u << 20) - 0x100000, 1, 1};
    initialize(&map, 1, &normal_video);
    uint32_t large[PHYS_BATCH_MAX], tail[29];
    allocate(alice, PHYS_PAGE_DIRECTORY, PHYS_BATCH_MAX, large);
    assert(large[796] == 0x016ff000 && large[797] == 0x07f00000);
    assert(stats().free == 29 && stats().allocated == PHYS_BATCH_MAX);
    allocate(bob, PHYS_PAGE_TABLE, 29, tail);
    assert(stats().free == 0 && stats().allocated == 1053);
    assert(physmem_core_release_owner(&core, alice) == PHYS_OK);
    assert(physmem_core_owner_pages(&core, bob) == 29);
    assert(physmem_core_release(&core, bob, PHYS_PAGE_TABLE, tail, 29) == PHYS_OK);
    assert(stats().free == 1053 && stats().allocated == 0 && stats().high_water == 1053);
    puts("owned pages: maximum 1024-frame batch and optional high frames release to exact baseline");
}
static void argument_contract(void) {
    MemoryRange map = {0x100000, (64u << 20) - 0x100000, 1, 1};
    fresh(); uint32_t output = 0xdeadbeef;
    assert(physmem_core_alloc(&core, alice, PHYS_BEX1_BACKING, 1, &output) == PHYS_NOT_READY);
    assert(physmem_alloc(alice, PHYS_BEX1_BACKING, 1, &output) == PHYS_NOT_READY);
    initialize(&map, 1, &normal_video);
    PhysmemStats before = save_state();
    const BosHandle invalid[] = {0, BOS_HANDLE_TYPE_PROCESS, BOS_HANDLE_TYPE_FILE | 1, BOS_HANDLE_TYPE_OPERATION | 1};
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        assert(physmem_core_alloc(&core, invalid[i], PHYS_BEX1_BACKING, 1, &output) == PHYS_INVALID);
        assert(physmem_core_release_owner(&core, invalid[i]) == PHYS_INVALID);
    }
    assert(physmem_core_alloc(&core, alice, PHYS_FREE, 1, &output) == PHYS_INVALID);
    assert(physmem_core_alloc(&core, alice, PHYS_KIND_LIMIT, 1, &output) == PHYS_INVALID);
    assert(physmem_core_alloc(&core, alice, PHYS_BEX1_BACKING, 0, &output) == PHYS_INVALID);
    assert(physmem_core_alloc(&core, alice, PHYS_BEX1_BACKING, PHYS_BATCH_MAX + 1, &output) == PHYS_INVALID);
    assert(physmem_core_alloc(&core, alice, PHYS_BEX1_BACKING, 1, 0) == PHYS_INVALID);
    unchanged(before); assert(output == 0xdeadbeef);
}

/* Execute only the original table-construction prefix from protect_memory.
 * Tests provide host tables; CR0/CR3/CR4 and all guest addresses remain unused. */
static uint32_t host_tables[3072] __attribute__((aligned(PHYS_PAGE_BYTES)));
static int paging_ready;
#undef PAGING_BASE
#define PAGING_BASE ((uintptr_t)host_tables)
#include "physmem_paging_construction.inc"
static void identity_reachability(void) {
    MemoryRange map = {0x100000, PHYS_MANAGED_END - 0x100000, 1, 1};
    initialize(&map, 1, &normal_video);
    protect_memory();
    for(unsigned i=0;i<1024;i++)assert(host_tables[2048+i]==((i<<22)|0x83));
    for (unsigned pfn = 0; pfn < PHYS_PAGE_COUNT; ++pfn) {
        if (kinds[pfn] != PHYS_FREE) continue;
        uint32_t frame = pfn * PHYS_PAGE_BYTES;
        uint32_t pde = host_tables[frame >> 22];
        assert((pde & 0x87) == 0x83); /* Present, writable, PSE, supervisor. */
        assert((pde & 0xffc00000) == (frame & 0xffc00000));
    }
    assert((host_tables[USER_BASE >> 22] & 0x87) == 7);
    for (unsigned i = 0; i < 1024; ++i) {
        assert(host_tables[1024 + i] == (i < USER_CAPACITY / PHYS_PAGE_BYTES ?
               (USER_BASE + i * PHYS_PAGE_BYTES) | 7u : 0));
        assert(kinds[USER_BASE / PHYS_PAGE_BYTES + i] == PHYS_UNAVAILABLE);
    }
    puts("owned pages: every candidate is supervisor identity-reachable in the existing compatibility tables");
}
int main(void) {
    profile_maps(); map_unions(); descriptor_bounds(); framebuffer_exclusion();
    ownership_and_capacity(); high_pages_and_batch_limit(); argument_contract(); identity_reachability(); fresh();
    puts("owned physical-page core: all deterministic functional checks passed");
}
