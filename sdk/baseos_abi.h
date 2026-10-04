#ifndef BASEOS_ABI_H
#define BASEOS_ABI_H
#include "baseos_executable.h"
/* Shared freestanding wire contract. All fields are little-endian 32-bit values.
 * BEX1 retains its exact 16-byte header and 64 KiB offset-pointer interpretation.
 * A handle is opaque, owner-bound and valid only for this boot and its lifetime.
 * Never persist, inspect, calculate with, or exchange handles between processes. */
typedef unsigned int bos_u32;
typedef int bos_i32;
typedef bos_u32 BosHandle;
_Static_assert(sizeof(bos_u32)==4 && sizeof(bos_i32)==4,"BaseOS ABI requires 32-bit integers");
#define BOS_ABI_MAJOR 1u
#define BOS_ABI_MINOR 1u
#define BOS_ABI_QUERY_MIN_SIZE 16u
#define BOS_HANDLE_INVALID 0u

/* Legacy IDs and result values are frozen. Extended results below apply only
 * to calls >=18; legacy unknown calls still return -1, writes may return -2. */
enum BosSyscall {
    BOS_CALL_EXIT=0, BOS_CALL_WRITE=1, BOS_CALL_PLOT=2, BOS_CALL_TICKS=3,
    BOS_CALL_KEY=4, BOS_CALL_PRESENT=5, BOS_CALL_READ_FILE=6,
    BOS_CALL_WRITE_FILE=7, BOS_CALL_FILE_SIZE=8, BOS_CALL_RECT=9,
    BOS_CALL_YIELD=10, BOS_CALL_SLEEP=11, BOS_CALL_TASK_ID=12,
    BOS_CALL_READ_FILE_AT=13, BOS_CALL_CANVAS_SIZE=14,
    BOS_CALL_REPLACE_FILE=15, BOS_CALL_SYNC=16, BOS_CALL_ARGUMENT=17,
    BOS_CALL_ABI_QUERY=18, BOS_CALL_SYNC_BEGIN=19, BOS_CALL_SYNC_POLL=20,
    BOS_CALL_SYNC_WAIT=21, BOS_CALL_SYNC_RELEASE=22, BOS_CALL_FILE_OPEN=23,
    BOS_CALL_FILE_INFO=24, BOS_CALL_FILE_READ_AT=25,
    BOS_CALL_FILE_REPLACE=26, BOS_CALL_FILE_CLOSE=27,
    BOS_CALL_MEMORY_INFO=28 /* 29 reserved for a later UI service. */
};
enum BosResult {
    BOS_OK=0, BOS_PENDING=1,
    BOS_E_UNSUPPORTED=-1000, BOS_E_INVALID=-1001, BOS_E_NOT_FOUND=-1002,
    BOS_E_CAPACITY=-1003, BOS_E_BUSY=-1004, BOS_E_STALE=-1005,
    BOS_E_CHANGED=-1006, BOS_E_CANCELLED=-1007, BOS_E_PROTECTED=-1008,
    BOS_E_IO=-1009, BOS_E_TIMEOUT=-1010
};
#define BOS_FEATURE_VERSIONED_FILES (1u<<0)
#define BOS_FEATURE_OWNED_SYNC      (1u<<1)
#define BOS_FEATURE_OPERATION_WAIT (1u<<2)
#define BOS_FEATURE_PROCESS_ID     (1u<<3)
#define BOS_FEATURE_MEMORY_INFO    (1u<<4)
/* This execution context uses BEX2 sparse offsets; not a global exec promise. */
#define BOS_FEATURE_BEX2           (1u<<5)
#define BOS_CONTEXT_LEGACY_EXEC 1u
#define BOS_CONTEXT_DESKTOP_TASK 2u

/* Query is output-only. Pass capacity>=16 and requested major=1. The kernel
 * copies min(capacity,struct_size); the tail is untouched. The first four words
 * are always present. Unknown feature bits and output reserved words are ignored.
 * Unsupported major/invalid arguments leave the entire output unchanged.
 * Limits describe this execution context/backend, not implementation addresses. */
typedef struct {
    bos_u32 struct_size, abi_major, abi_minor, features;
    bos_u32 context, process;
    /* BEX1 user_bytes is the contiguous 64 KiB extent. For BEX2 it is a sparse
     * 4 MiB virtual extent, NOT permission to access every byte; query memory
     * regions. BEX1 stack is a linker reservation, BEX2 stack is committed. */
    bos_u32 user_bytes, image_bytes, stack_reserved_bytes, path_bytes;
    bos_u32 file_chunk_bytes, replace_bytes, file_bytes;
    bos_u32 files_per_process, files_total;
    bos_u32 operations_per_process, operations_total;
    bos_u32 wait_milliseconds, ticks_per_second, processes_total;
    bos_u32 reserved[4];
} BosAbiInfo;
_Static_assert(sizeof(BosAbiInfo)==96,"ABI query wire size");

#define BOS_MEMORY_INFO_VERSION 1u
#define BOS_MEMORY_INFO_MIN_SIZE 16u
#define BOS_MEMORY_READ 1u
#define BOS_MEMORY_WRITE 2u
/* EXEC describes the code-segment range; x86 paging here does not implement NX. */
#define BOS_MEMORY_EXEC 4u
#define BOS_MEMORY_LEGACY 1u
#define BOS_MEMORY_TEXT 2u
#define BOS_MEMORY_DATA 3u
#define BOS_MEMORY_WORKSPACE 4u
#define BOS_MEMORY_STACK 5u
typedef struct { bos_u32 offset,bytes,protection,purpose; } BosMemoryRegion;
/* Output-only, versioned like ABI query; capacity>=16, version=1, unused args0.
 * Validate the full supplied capacity; copy min(capacity,128), leave tail alone.
 * Every error leaves output unchanged. Offsets/lengths are virtual, never PFNs.
 * owned_pages includes table_pages (directory+PT) and committed user storage.
 * policy_pages is the context's maximum owned-page policy, including tables.
 * Legacy exec: mapped16/owned0/table0/policy0; BEX1 task:16/16/0/16.
 * Pool values are momentary non-reserving capacity snapshots. Nonempty regions
 * are ordered text/data/workspace/stack (BEX1: single legacy RWX region).
 * Unused regions and reserved words are zero; ignore unknown output flags. */
typedef struct {
    bos_u32 struct_size,version,format,page_bytes;
    bos_u32 virtual_bytes,mapped_pages,owned_pages,table_pages;
    bos_u32 policy_pages,pool_total_pages,pool_free_pages,region_count;
    bos_u32 reserved[4];
    BosMemoryRegion regions[4];
} BosMemoryInfo;
_Static_assert(sizeof(BosMemoryInfo)==128,"memory info wire size");

#define BOS_FILE_OPEN_READ 1u
#define BOS_FILE_OPEN_WRITE 2u
/* Exclusive creation of an absent /Documents file. Requires READ|WRITE.
 * An existing path returns CHANGED, never an implicit overwrite. */
#define BOS_FILE_OPEN_CREATE 4u
/* Output-only metadata. Revision is an opaque equality token, not a timestamp
 * or an integer an application may increment. Successful replacement refreshes
 * this handle's bound revision. Other handles retain their previous revision. */
typedef struct {
    bos_u32 struct_size, handle, size, flags, revision, reserved[3];
} BosFileInfo;
_Static_assert(sizeof(BosFileInfo)==32,"file info wire size");

/* Internal allocation domains, shared to prevent cross-service collisions.
 * Their encoding is not an application permission or a public parsing API. */
#define BOS_HANDLE_TYPE_PROCESS 0x10000000u
#define BOS_HANDLE_TYPE_FILE 0x20000000u
#define BOS_HANDLE_TYPE_OPERATION 0x30000000u
#define BOS_HANDLE_SERIAL_MAX 0x0fffffffu
#endif
