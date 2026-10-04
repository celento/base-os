#include "executable.h"

/* Decode explicitly: input alignment and host byte order are not assumptions. */
static uint32_t word(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint64_t page_up(uint64_t bytes) {
    return (bytes+BOS_BEX2_PAGE_BYTES-1u)&~(uint64_t)(BOS_BEX2_PAGE_BYTES-1u);
}
static ExecutableRegion region(uint32_t offset,uint32_t bytes) {
    ExecutableRegion r={offset,bytes,bytes/BOS_BEX2_PAGE_BYTES};return r;
}
int executable_plan_bex2(const void *image,uint32_t bytes,
                         const ExecutablePolicy *policy,ExecutablePlan *out) {
    if(!image||!policy||!out||bytes<BOS_BEX2_HEADER_BYTES)return EXECUTABLE_FORMAT;
    const unsigned char *p=image;
    if(word(p)!=BOS_BEX2_MAGIC)return EXECUTABLE_FORMAT;
    if(word(p+4)!=BOS_BEX2_HEADER_BYTES||word(p+8)!=BOS_BEX2_FORMAT_VERSION||
       word(p+12)||word(p+56)||word(p+60))return EXECUTABLE_UNSUPPORTED;
    if(word(p+48)!=policy->abi_major||word(p+52)>policy->abi_minor)
        return EXECUTABLE_UNSUPPORTED;
    uint32_t file=word(p+16),entry=word(p+20),text=word(p+24),data=word(p+28);
    uint32_t data_file=word(p+32),data_mem=word(p+36),workspace=word(p+40),stack=word(p+44);
    if(file!=bytes||!text||data_file>data_mem)return EXECUTABLE_FORMAT;
    /* All sums/alignments are widened before narrowing, including values from
     * the header which may exceed this format's virtual or file policy. */
    uint64_t text_end=(uint64_t)BOS_BEX2_PAGE_BYTES+text;
    uint64_t text_page_end=page_up(text_end);
    uint64_t data_end=(uint64_t)data+data_mem;
    uint64_t workspace_start=page_up(data_end);
    uint64_t workspace_end=workspace_start+workspace;
    if(entry<BOS_BEX2_PAGE_BYTES||(uint64_t)entry>=text_end||
       text_page_end!=data||(uint64_t)data+data_file!=file||
       workspace%BOS_BEX2_PAGE_BYTES||stack%BOS_BEX2_PAGE_BYTES)
        return EXECUTABLE_FORMAT;
    if(stack<BOS_BEX2_STACK_MIN||stack>BOS_BEX2_STACK_MAX)
        return EXECUTABLE_CAPACITY;
    uint32_t stack_start=BOS_BEX2_VIRTUAL_BYTES-stack;
    uint32_t guard=stack_start-BOS_BEX2_PAGE_BYTES;
    if(file>BOS_BEX2_FILE_MAX||file>policy->file_bytes_max||workspace_end>guard)
        return EXECUTABLE_CAPACITY;
    ExecutablePlan result={0};
    result.kind=BOS_EXECUTABLE_BEX2;result.file_bytes=file;result.entry_offset=entry;
    result.virtual_bytes=BOS_BEX2_VIRTUAL_BYTES;result.initial_sp=BOS_BEX2_VIRTUAL_BYTES-16u;
    result.text=region(BOS_BEX2_PAGE_BYTES,(uint32_t)text_page_end-BOS_BEX2_PAGE_BYTES);
    result.data=region(data,(uint32_t)workspace_start-data);
    result.workspace=region((uint32_t)workspace_start,workspace);
    result.stack=region(stack_start,stack);
    result.text_file_bytes=text;result.data_file_bytes=data_file;result.guard_offset=guard;
    result.mapped_pages=result.text.pages+result.data.pages+result.workspace.pages+result.stack.pages;
    result.table_pages=BOS_BEX2_TABLE_PAGES;
    result.owned_pages=result.mapped_pages+result.table_pages;
    if(result.owned_pages>policy->owned_pages_max)return EXECUTABLE_CAPACITY;
    result.code_limit=(uint32_t)text_page_end-1u;
    result.data_limit=BOS_BEX2_VIRTUAL_BYTES-1u;
    *out=result;return EXECUTABLE_OK;
}
