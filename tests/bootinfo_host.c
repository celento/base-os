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
void panic(const char *s) { fprintf(stderr, "%s\n", s); __builtin_trap(); }
int main(void) {
    BootInfo good = { .magic = BOOTINFO_MAGIC, .lfb = 0xfd000000,
        .width = 1280, .height = 720, .pitch = 5120, .bpp = 32, .flags = 1,
        .red_size = 8, .red_pos = 16, .green_size = 8, .green_pos = 8,
        .blue_size = 8, .blue_pos = 0 };
    assert(video_info_valid(&good));
    BootInfo bad = good;
    bad.width = bad.height = 2048; assert(!video_info_valid(&bad));
    bad = good; bad.pitch = 1280; assert(!video_info_valid(&bad));
    bad = good; bad.bpp = 15; assert(!video_info_valid(&bad));
    bad = good; bad.lfb = 0xfffffff0; assert(!video_info_valid(&bad));
    bad = good; bad.lfb = 0; assert(!video_info_valid(&bad));
    bad = good; bad.red_pos = 0; assert(!video_info_valid(&bad));
    good.bpp = 24; good.pitch = 3840; assert(video_info_valid(&good));
    good.bpp = 16; good.pitch = 2560;
    good.red_size = 5; good.red_pos = 11;
    good.green_size = 6; good.green_pos = 5; good.blue_size = 5;
    assert(video_info_valid(&good));

    test_boot.map_count = 2;
    test_map[0] = (MemoryRange){0x100000, 0x400000, 1, 1};
    test_map[1] = (MemoryRange){0x500000, RAM_REQUIRED_END-0x500000, 1, 1};
    assert(available(FB_BASE, RAM_REQUIRED_END));
    test_map[1].base++;
    assert(!available(FB_BASE, RAM_REQUIRED_END));
    test_map[1].base--;
    test_map[1].attributes = 0; assert(!available(FB_BASE, RAM_REQUIRED_END));
    test_map[1].attributes = 1;
    test_boot.map_count = 3;
    test_map[2] = (MemoryRange){0x700000, 0x1000, 2, 1};
    assert(!available(FB_BASE, RAM_REQUIRED_END));
    test_map[2] = (MemoryRange){0xfffffffffffffff0ULL, 0x1000, 1, 1};
    assert(!available(FB_BASE, RAM_REQUIRED_END));
    puts("boot info: oversized buffers, pitch, format, framebuffer overflow, RAM gaps/reservations/overflow passed");
}
