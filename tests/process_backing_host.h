#ifndef PROCESS_BACKING_HOST_H
#define PROCESS_BACKING_HOST_H
/* Real production allocator metadata/lifecycle, with ordinary allocated host
 * buffers standing in for physical storage. Integer frame addresses are never
 * dereferenced. Only the kernel wrapper and kmemcpy address translation are
 * host adapters; process creation, scatter copying and teardown stay unchanged. */
#include <stdlib.h>
#include "physmem.h"
#ifndef PROCESS_BACKING_RELEASE_HOOK
#define PROCESS_BACKING_RELEASE_HOOK(owner) ((void)(owner))
#endif
char __kernel_end[1];
void panic(const char *message){fprintf(stderr,"%s\n",message);abort();}
static PhysmemCore backing_core;
static uint32_t backing_owners[PHYS_PAGE_COUNT];
static uint8_t backing_kinds[PHYS_PAGE_COUNT];
static unsigned char *backing_contents[PHYS_PAGE_COUNT];
static unsigned backing_zeroes,backing_allocations,backing_releases;
static void host_zero_page(uint32_t frame,void *context){
    assert(context==&backing_core&&backing_core.busy);
    unsigned pfn=frame/PHYS_PAGE_BYTES;
    assert(frame%PHYS_PAGE_BYTES==0&&pfn<PHYS_PAGE_COUNT&&backing_owners[pfn]);
    if(!backing_contents[pfn]){
        backing_contents[pfn]=malloc(PHYS_PAGE_BYTES);assert(backing_contents[pfn]);
        memset(backing_contents[pfn],0xa5,PHYS_PAGE_BYTES);
    }
    memset(backing_contents[pfn],0,PHYS_PAGE_BYTES);backing_zeroes++;
}
static void host_physmem_init(unsigned ram_mib){
    const MemoryRange map={0x100000,((uint64_t)ram_mib<<20)-0x100000,1,1};
    const BootInfo video={.magic=BOOTINFO_MAGIC,.lfb=0xfd000000,.width=1280,.height=720,
        .pitch=5120,.bpp=32,.flags=1,.red_size=8,.red_pos=16,
        .green_size=8,.green_pos=8,.blue_size=8,.blue_pos=0};
    assert(physmem_core_init(&backing_core,backing_owners,backing_kinds,&map,1,
                            &video,host_zero_page,&backing_core)==PHYS_OK);
}
static PhysmemStats host_physmem_stats(void){
    PhysmemStats stats;assert(physmem_core_stats(&backing_core,&stats)==PHYS_OK);
    assert(stats.free+stats.allocated==stats.total);return stats;
}
static int host_physmem_alloc(BosHandle owner,enum PhysPageKind kind,unsigned count,uint32_t *frames){
    int result=physmem_core_alloc(&backing_core,owner,kind,count,frames);
    if(result==PHYS_OK)backing_allocations++;
    return result;
}
static int host_physmem_release(BosHandle owner,enum PhysPageKind kind,const uint32_t *frames,unsigned count){
    PROCESS_BACKING_RELEASE_HOOK(owner);
    int result=physmem_core_release(&backing_core,owner,kind,frames,count);
    if(result==PHYS_OK)backing_releases++;
    return result;
}
static int host_physmem_is_frame(const void *pointer){return (uintptr_t)pointer<PHYS_MANAGED_END;}
static void *host_physmem_pointer(const void *pointer,unsigned bytes){
    uintptr_t address=(uintptr_t)pointer;
    if(!host_physmem_is_frame(pointer))return (void *)pointer;
    unsigned pfn=address/PHYS_PAGE_BYTES,offset=address%PHYS_PAGE_BYTES;
    assert(backing_core.ready&&backing_owners[pfn]&&backing_contents[pfn]);
    assert(bytes<=PHYS_PAGE_BYTES-offset);
    return backing_contents[pfn]+offset;
}
static void host_physmem_copy(void *to,const void *from,unsigned bytes){
    if(bytes)memcpy(host_physmem_pointer(to,bytes),host_physmem_pointer(from,bytes),bytes);
}
static void host_physmem_destroy(void){
    for(unsigned pfn=0;pfn<PHYS_PAGE_COUNT;pfn++)free(backing_contents[pfn]);
}
static uint32_t *space_frame_pointer(uint32_t frame){
    return host_physmem_pointer((const void *)(uintptr_t)frame,PHYS_PAGE_BYTES);
}
static int host_stats(PhysmemStats *out){return physmem_core_stats(&backing_core,out);}
static int host_release_owner(BosHandle owner){return physmem_core_release_owner(&backing_core,owner);}
#define physmem_stats host_stats
#define physmem_release_owner host_release_owner
#define physmem_alloc host_physmem_alloc
#define physmem_release host_physmem_release
#endif
