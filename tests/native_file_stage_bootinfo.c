/* FS fixture supplies its own backend arena-availability query. Keep the real
 * pure E820/core helpers while giving the unrelated boot-global wrapper a name
 * that cannot collide. Production physmem_core_init is linked unchanged. */
#define platform_memory_range_available stage_bootinfo_memory_range_available
#include "../src/bootinfo.c"
