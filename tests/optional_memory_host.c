/* Exact optional E820 range queries. No physical-memory accesses or probes. */
#include <assert.h>
#include <stdio.h>
#include "platform.h"
static BootInfo test_boot;
static MemoryRange test_map[E820_MAX];
#undef BOOTINFO_ADDR
#undef E820_BASE
#define BOOTINFO_ADDR ((uintptr_t)&test_boot)
#define E820_BASE ((uintptr_t)test_map)
#include "../src/bootinfo.c"
char __kernel_end[1];
void panic(const char *message) { fprintf(stderr, "%s\n", message); __builtin_trap(); }
int main(void) {
    assert(!platform_memory_range_available(FS_LARGE_POOL_BASE, RAM_LARGE_REQUIRED_END));
    test_boot.map_count = 1;
    test_map[0] = (MemoryRange){0x100000, 0x3f00000, 1, 1};
    assert(!platform_memory_range_available(FS_LARGE_POOL_BASE, RAM_LARGE_REQUIRED_END));
    test_map[0].length = 0x7f00000;
    assert(platform_memory_range_available(FS_LARGE_POOL_BASE, RAM_LARGE_REQUIRED_END));
    test_boot.map_count = 2;
    test_map[0].length = 0x4000000 - 0x100000;
    test_map[1] = (MemoryRange){0x4000000, 0x4000000, 1, 1};
    assert(platform_memory_range_available(FS_LARGE_POOL_BASE, RAM_LARGE_REQUIRED_END));
    test_map[1].base += 4096;
    assert(!platform_memory_range_available(FS_LARGE_POOL_BASE, RAM_LARGE_REQUIRED_END));
    test_map[1].base -= 4096;
    test_boot.map_count = 3;
    test_map[2] = (MemoryRange){0x6000000, 4096, 2, 1};
    assert(!platform_memory_range_available(FS_LARGE_POOL_BASE, RAM_LARGE_REQUIRED_END));
    test_map[2].attributes = 0;
    assert(platform_memory_range_available(FS_LARGE_POOL_BASE, RAM_LARGE_REQUIRED_END));
    assert(!platform_memory_range_available(RAM_LARGE_REQUIRED_END, FS_LARGE_POOL_BASE));
    test_boot.map_count = E820_MAX + 1;
    assert(!platform_memory_range_available(FS_LARGE_POOL_BASE, RAM_LARGE_REQUIRED_END));
    puts("optional memory: 64/128 MiB, adjacent ranges, gaps, reserved overlap and disabled ranges passed");
}
