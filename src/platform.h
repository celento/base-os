#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdint.h>
#include "layout.h"
#define TIMER_HZ 70u
_Static_assert(E820_BASE + E820_MAX * 24 < 0x7000, "memory map overlaps boot stack");

typedef struct {
    uint32_t magic, lfb;
    uint16_t width, height, pitch;
    uint8_t bpp, flags;
    uint16_t map_count;
    uint8_t boot_drive, sectors_per_track;
    uint8_t red_size, red_pos, green_size, green_pos, blue_size, blue_pos;
} BootInfo;

typedef struct __attribute__((packed)) {
    uint64_t base, length;
    uint32_t type, attributes;
} MemoryRange;

void platform_init(void);
void platform_validate_memory(void);
unsigned platform_memory_mb(void);
/* Exact optional physical range check: rejects gaps, reservations and bad maps.
 * Does not access the queried memory or stop the machine. End is exclusive. */
int platform_memory_range_available(uint32_t base, uint32_t end);
/* Pure E820 predicates shared by boot admission and owned-page initialization.
 * Enabled overflowing descriptors invalidate the whole map. Disabled/empty
 * entries do not contribute; reserved overlaps win over the usable union. */
int platform_memory_map_valid(const MemoryRange *map, unsigned count);
int platform_memory_map_available(const MemoryRange *map, unsigned count,
                                  uint64_t base, uint64_t end);
uint32_t timer_ticks(void);
void timer_delay(unsigned ticks);
/* Collect input while blocking; never dispatch application actions here. */
void platform_poll(void);
int video_info_valid(const BootInfo *bi);
void panic(const char *message) __attribute__((noreturn));
void platform_log(const char *message);
#endif
