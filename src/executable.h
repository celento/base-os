#ifndef BASEOS_EXECUTABLE_PLAN_H
#define BASEOS_EXECUTABLE_PLAN_H
#include <stdint.h>
#include "../sdk/baseos_executable.h"

/* Pure, allocation-free validation. There are no live memory or ABI globals.
 * bytes must be the exact available file length, not a buffer capacity. Both
 * policy caps are inclusive. A zero cap permits no file/pages; use the explicit
 * format constants or UINT32_MAX when no smaller caller policy is required.
 * Every failure leaves *out unchanged. The caller must keep the input stable
 * until it has copied the validated payload; this API never retains pointers. */
typedef struct { uint32_t offset, bytes, pages; } ExecutableRegion;
typedef struct {
    uint32_t kind, file_bytes, entry_offset, virtual_bytes, initial_sp;
    /* Region bytes are committed, rounded page bytes. Zero-size regions are
     * present in the plan but require no pages. Text has read-only user pages;
     * data/workspace/stack have writable user pages. No other user pages exist. */
    ExecutableRegion text, data, workspace, stack;
    uint32_t text_file_bytes, data_file_bytes, guard_offset;
    uint32_t mapped_pages, table_pages, owned_pages, code_limit, data_limit;
} ExecutablePlan;
typedef struct {
    uint32_t abi_major, abi_minor, file_bytes_max, owned_pages_max;
} ExecutablePolicy;
enum ExecutableResult {
    EXECUTABLE_OK=0, EXECUTABLE_FORMAT=-1,
    EXECUTABLE_UNSUPPORTED=-2, EXECUTABLE_CAPACITY=-3
};
int executable_plan_bex2(const void *image, uint32_t bytes,
                         const ExecutablePolicy *policy, ExecutablePlan *out);
#endif
