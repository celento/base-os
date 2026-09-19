#include "platform.h"

static const MemoryRange *memory_map = (const MemoryRange *)E820_BASE;
static const BootInfo *boot_info = (const BootInfo *)BOOTINFO_ADDR;

static int available(uint32_t base, uint32_t end) {
    uint64_t cursor = base;
    /* Reject overlap with firmware reservations even if a usable descriptor
     * also covers it. Then allow adjacent usable descriptors to form a range. */
    for (unsigned i = 0; i < boot_info->map_count; ++i) {
        const MemoryRange *r = &memory_map[i];
        if (!(r->attributes & 1) || !r->length) continue;
        uint64_t top = r->base + r->length;
        if (top < r->base) return 0;
        if (r->type != 1 && r->base < end && top > base) return 0;
    }
    while (cursor < end) {
        uint64_t next = cursor;
        for (unsigned i = 0; i < boot_info->map_count; ++i) {
            const MemoryRange *r = &memory_map[i];
            if (r->type == 1 && (r->attributes & 1) && r->base <= cursor &&
                r->base + r->length > next) next = r->base + r->length;
        }
        if (next == cursor) return 0;
        cursor = next;
    }
    return 1;
}
void platform_validate_memory(void) {
    extern char __kernel_end[];
    if (!boot_info->map_count || boot_info->map_count > E820_MAX)
        panic("BIOS memory map unavailable");
    if (!available(KERNEL_LOAD_ADDR, (uintptr_t)__kernel_end) ||
        !available(STACK_BOTTOM, STACK_TOP) ||
        !available(FB_BASE, RAM_REQUIRED_END))
        panic("required RAM is unavailable or reserved");
    if (boot_info->boot_drive != 0 ||
        (boot_info->sectors_per_track != 18 && boot_info->sectors_per_track != 36))
        panic("unsupported boot floppy geometry");
}
unsigned platform_memory_mb(void) {
    uint64_t bytes = 0;
    for (unsigned i = 0; i < boot_info->map_count; ++i) {
        const MemoryRange *r = &memory_map[i];
        if (r->type == 1 && (r->attributes & 1) && r->base < 0x100000000ULL) {
            uint64_t end = r->base + r->length;
            if (end > 0x100000000ULL) end = 0x100000000ULL;
            bytes += end - r->base;
        }
    }
    return (unsigned)(bytes >> 20);
}
int video_info_valid(const BootInfo *b) {
    if (b->magic != BOOTINFO_MAGIC || b->flags != 1 || !b->lfb ||
        b->width < 640 || b->height < 400 ||
        (uint32_t)b->width * b->height > FB_CAPACITY ||
        (b->bpp != 16 && b->bpp != 24 && b->bpp != 32)) return 0;
    uint32_t bytes = b->bpp / 8;
    uint32_t span = (uint32_t)b->pitch * b->height;
    if (b->pitch < b->width * bytes || b->lfb < RAM_REQUIRED_END ||
        span > UINT32_MAX - b->lfb) return 0;
    for (unsigned i = 0; i < boot_info->map_count && i < E820_MAX; ++i) {
        const MemoryRange *r = &memory_map[i];
        if (r->type == 1 && (r->attributes & 1) &&
            r->base < (uint64_t)b->lfb + span && r->base + r->length > b->lfb)
            return 0; /* A framebuffer must not alias ordinary usable RAM. */
    }
    if (b->bpp == 16)
        return b->red_size == 5 && b->red_pos == 11 &&
               b->green_size == 6 && b->green_pos == 5 &&
               b->blue_size == 5 && b->blue_pos == 0;
    return b->red_size == 8 && b->red_pos == 16 &&
           b->green_size == 8 && b->green_pos == 8 &&
           b->blue_size == 8 && b->blue_pos == 0;
}

_Static_assert(FB_BASE + FB_CAPACITY <= FS_BASE, "backbuffer overlaps FS");
_Static_assert(FS_BASE + FS_CAPACITY <= PAINT_MEM, "FS overlaps paint");
_Static_assert(PAINT_MEM + PAINT_CAPACITY <= DMA_BASE, "paint overlaps DMA");
_Static_assert(DMA_BASE % 65536 == 0 && DMA_CAPACITY <= 65536 &&
               DMA_BASE + DMA_CAPACITY <= 0x1000000, "invalid ISA DMA arena");
_Static_assert(DMA_BASE + DMA_CAPACITY <= FS_IMG_BASE, "DMA overlaps image");
_Static_assert(FS_IMG_BASE + FS_IMG_CAPACITY <= DESK_CACHE, "image overlaps cache");
_Static_assert(DESK_CACHE + DESK_CAPACITY <= APPS_BASE, "cache overlaps apps");
_Static_assert(APPS_BASE+APPS_CAPACITY<=PAGING_BASE,"apps overlap paging");
_Static_assert(PAGING_BASE+PAGING_CAPACITY<=USER_BASE,"paging overlaps user");
_Static_assert(USER_BASE+USER_CAPACITY<=RAM_REQUIRED_END,"user beyond RAM");

_Static_assert(sizeof(MemoryRange) == 24 && __builtin_offsetof(BootInfo, map_count) == 16 &&
               __builtin_offsetof(BootInfo, red_size) == 20, "assembly boot ABI mismatch");

_Static_assert(USER_BASE+USER_CAPACITY<=DRAG_CACHE,"user overlaps compositor");
_Static_assert(DRAG_CACHE+DRAG_CAPACITY<=PRESENT_BASE,"drag overlaps presentation");
_Static_assert(PRESENT_BASE+PRESENT_CAPACITY<=RAM_REQUIRED_END,"presentation beyond RAM");
