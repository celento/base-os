#ifndef NATIVE_FILE_STAGE_HOST_H
#define NATIVE_FILE_STAGE_HOST_H
/* Ordinary host adapter: real production allocator core, separate host bytes
 * for its scattered physical frames. No guest physical address is accessed. */
#include <stdlib.h>
#include "physmem.h"
char __kernel_end[1];
void panic(const char *message) { fprintf(stderr,"%s\n",message); abort(); }
static PhysmemCore stage_core;
static uint32_t stage_owners[PHYS_PAGE_COUNT];
static uint8_t stage_kinds[PHYS_PAGE_COUNT];
static unsigned char *stage_contents[PHYS_PAGE_COUNT];
static void stage_zero(uint32_t frame, void *context) {
    assert(context == &stage_core && stage_core.busy);
    unsigned pfn = frame / PHYS_PAGE_BYTES;
    if (!stage_contents[pfn]) { stage_contents[pfn] = malloc(PHYS_PAGE_BYTES); assert(stage_contents[pfn]); }
    memset(stage_contents[pfn],0,PHYS_PAGE_BYTES);
}
static inline void stage_allocator_init(unsigned ram_mib) {
    MemoryRange map = {0x100000,((uint64_t)ram_mib<<20)-0x100000,1,1};
    BootInfo video = {.magic=BOOTINFO_MAGIC,.lfb=0xfd000000,.width=1280,.height=720,
        .pitch=5120,.bpp=32,.flags=1,.red_size=8,.red_pos=16,.green_size=8,.green_pos=8,.blue_size=8};
    assert(physmem_core_init(&stage_core,stage_owners,stage_kinds,&map,1,&video,stage_zero,&stage_core)==PHYS_OK);
}
static inline PhysmemStats stage_stats(void) {
    PhysmemStats stats;
    assert(physmem_core_stats(&stage_core,&stats)==PHYS_OK);
    assert(stats.free+stats.allocated==stats.total);
    return stats;
}
static inline void stage_allocator_destroy(void) {
    assert(stage_stats().allocated==0);
    for(unsigned pfn=0;pfn<PHYS_PAGE_COUNT;++pfn)free(stage_contents[pfn]);
}
static int stage_alloc(BosHandle owner,enum PhysPageKind kind,unsigned count,uint32_t *frames) {
    return physmem_core_alloc(&stage_core,owner,kind,count,frames);
}
static int stage_release(BosHandle owner,enum PhysPageKind kind,const uint32_t *frames,unsigned count) {
    return physmem_core_release(&stage_core,owner,kind,frames,count);
}
static int stage_validate(BosHandle owner,enum PhysPageKind kind,const uint32_t *frames,unsigned count) {
    return physmem_core_validate(&stage_core,owner,kind,frames,count);
}
static void *stage_pointer(const void *pointer,unsigned bytes) {
    uintptr_t address=(uintptr_t)pointer;
    if(address>=PHYS_MANAGED_END)return (void *)pointer;
    unsigned pfn=address/PHYS_PAGE_BYTES,offset=address%PHYS_PAGE_BYTES;
    assert(stage_core.ready&&stage_owners[pfn]&&stage_contents[pfn]&&bytes<=PHYS_PAGE_BYTES-offset);
    return stage_contents[pfn]+offset;
}
static void stage_copy(void *to,const void *from,int bytes) {
    if(bytes)memcpy(stage_pointer(to,(unsigned)bytes),stage_pointer(from,(unsigned)bytes),(unsigned)bytes);
}
#define physmem_alloc stage_alloc
#define physmem_release stage_release
#define physmem_validate stage_validate
#define kmemcpy stage_copy
#endif
