#ifndef ADDRESS_SPACE_H
#define ADDRESS_SPACE_H
#include <stdint.h>

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
#endif
