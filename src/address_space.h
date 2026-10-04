#ifndef ADDRESS_SPACE_H
#define ADDRESS_SPACE_H
#include <stdint.h>
#include "executable.h"

/* Pure construction/inspection helpers; no hardware access or allocation.
 * Offsets are relative to the native aperture, never physical addresses. */
enum UserAccess { USER_READ, USER_WRITE };
#define ADDRESS_PAGE_BYTES 4096u
#define ADDRESS_TABLE_ENTRIES 1024u
#define ADDRESS_PRESENT 1u
#define ADDRESS_WRITABLE 2u
#define ADDRESS_USER 4u
static inline int address_space_span(const uint32_t *table,uint32_t extent,
                                     uint32_t offset,uint32_t bytes,enum UserAccess access) {
    if(access!=USER_READ&&access!=USER_WRITE)return 0;
    if(!table&&extent!=65536u)return 0;
    if(offset>extent||bytes>extent-offset)return 0;
    /* Empty spans never dereference a pointer. Call-specific stricter legacy
     * behavior (notably write's offset<extent) remains at the call site. */
    if(!bytes)return 1;
    if(!table)return 1; /* Exact BEX1 contiguous compatibility extent only. */
    if(extent>ADDRESS_PAGE_BYTES*ADDRESS_TABLE_ENTRIES)return 0;
    unsigned first=offset/ADDRESS_PAGE_BYTES,last=(offset+bytes-1)/ADDRESS_PAGE_BYTES;
    uint32_t required=ADDRESS_PRESENT|ADDRESS_USER;
    if(access==USER_WRITE)required|=ADDRESS_WRITABLE;
    for(unsigned page=first;page<=last;page++)
        if((table[page]&required)!=required)return 0;
    return 1;
}
/* plan is parser-validated and frames contains exactly mapped_pages uniquely
 * owned, zeroed physical pages. Never use this for a partially validated plan. */
static inline void address_space_tables(uint32_t directory[1024],uint32_t table[1024],
                                        uint32_t aperture_index,uint32_t table_frame,
                                        const ExecutablePlan *plan,const uint32_t *frames) {
    for(unsigned i=0;i<1024;i++){directory[i]=(i<<22)|0x83;table[i]=0;}
    directory[aperture_index]=table_frame|7u;
    const ExecutableRegion *regions[4]={&plan->text,&plan->data,&plan->workspace,&plan->stack};
    unsigned frame=0;
    for(unsigned r=0;r<4;r++){
        unsigned first=regions[r]->offset/ADDRESS_PAGE_BYTES;
        for(unsigned page=0;page<regions[r]->pages;page++)
            table[first+page]=frames[frame++]|(r?7u:5u);
    }
}
#endif
