#ifndef BASEOS_ABI_H
#define BASEOS_ABI_H
/* Shared freestanding wire contract. All fields are little-endian 32-bit values.
 * BEX1 retains its exact 16-byte header and 64 KiB offset-pointer interpretation.
 * A handle is opaque, owner-bound and valid only for this boot and its lifetime.
 * Never persist, inspect, calculate with, or exchange handles between processes. */
typedef unsigned int bos_u32;
typedef int bos_i32;
typedef bos_u32 BosHandle;
_Static_assert(sizeof(bos_u32)==4 && sizeof(bos_i32)==4,"BaseOS ABI requires 32-bit integers");
#define BOS_ABI_MAJOR 1u
#define BOS_ABI_MINOR 0u
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
    BOS_CALL_FILE_REPLACE=26, BOS_CALL_FILE_CLOSE=27
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
    bos_u32 user_bytes, image_bytes, stack_min_bytes, path_bytes;
    bos_u32 file_chunk_bytes, replace_bytes, file_bytes;
    bos_u32 files_per_process, files_total;
    bos_u32 operations_per_process, operations_total;
    bos_u32 wait_milliseconds, ticks_per_second, processes_total;
    bos_u32 reserved[4];
} BosAbiInfo;
_Static_assert(sizeof(BosAbiInfo)==96,"ABI query wire size");

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
