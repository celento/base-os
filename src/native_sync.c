#include "native_sync.h"
#include "fs.h"
#include "../sdk/baseos_abi.h"

typedef struct {
    unsigned owner, handle;
    int result;
} NativeSyncResult;
static NativeSyncResult results[NATIVE_SYNC_CAPACITY];
static unsigned next_serial;
static FsSyncTicket active_ticket;
static int ticket_owned;

_Static_assert(sizeof(results) + sizeof(active_ticket) + 2 * sizeof(unsigned) <= 512,
               "native sync control state exceeds bounded budget");

int native_sync_available(void) { return fs_sync_async_supported(); }
unsigned native_sync_capacity(void) { return NATIVE_SYNC_CAPACITY; }
unsigned native_sync_per_owner_limit(void) { return NATIVE_SYNC_PER_OWNER; }

void native_sync_tick(void) {
    if (!ticket_owned) return;
    int result = fs_sync_result(active_ticket);
    if (result == FS_SYNC_PENDING) return;
    /* Completed app records are detached historical outcomes. Reap the FS
     * record before any subsequent request, independently of owner lifetime. */
    int outcome = result == 0 ? BOS_OK :
                  result == FS_SYNC_STALE ? BOS_E_STALE : BOS_E_IO;
    if (result != FS_SYNC_STALE) (void)fs_sync_release(active_ticket);
    ticket_owned = 0;
    for (unsigned i = 0; i < NATIVE_SYNC_CAPACITY; ++i)
        if (results[i].owner && results[i].result == BOS_PENDING)
            results[i].result = outcome;
}

static NativeSyncResult *find(unsigned owner, unsigned handle) {
    if (!owner || !handle) return 0;
    for (unsigned i = 0; i < NATIVE_SYNC_CAPACITY; ++i)
        if (results[i].owner == owner && results[i].handle == handle)
            return &results[i];
    return 0;
}

int native_sync_begin(unsigned owner, unsigned *handle) {
    if (!owner || !handle) return BOS_E_INVALID;
    native_sync_tick();
    if (!native_sync_available()) return BOS_E_UNSUPPORTED;
    unsigned count = 0;
    NativeSyncResult *empty = 0;
    for (unsigned i = 0; i < NATIVE_SYNC_CAPACITY; ++i) {
        if (results[i].owner == owner) ++count;
        if (!results[i].owner && !empty) empty = &results[i];
    }
    if (!empty || count == NATIVE_SYNC_PER_OWNER || next_serial == BOS_HANDLE_SERIAL_MAX)
        return BOS_E_CAPACITY;
    if (!ticket_owned) {
        int status = fs_sync_request_owned(&active_ticket);
        if (status < 0) {
            if (status == FS_ERR_BUSY) return BOS_E_BUSY;
            if (status == FS_SYNC_UNSUPPORTED) return BOS_E_UNSUPPORTED;
            if (status == FS_SYNC_PROTECTED) return BOS_E_PROTECTED;
            if (status == FS_SYNC_EXHAUSTED) return BOS_E_CAPACITY;
            return BOS_E_IO;
        }
        ticket_owned = 1;
    }
    empty->owner = owner;
    empty->handle = BOS_HANDLE_TYPE_OPERATION | ++next_serial;
    empty->result = BOS_PENDING;
    *handle = empty->handle;
    native_sync_tick();
    return BOS_OK;
}

int native_sync_poll(unsigned owner, unsigned handle) {
    native_sync_tick();
    NativeSyncResult *result = find(owner, handle);
    return result ? result->result : BOS_E_STALE;
}

int native_sync_release(unsigned owner, unsigned handle) {
    native_sync_tick();
    NativeSyncResult *result = find(owner, handle);
    if (!result) return BOS_E_STALE;
    result->owner = 0;
    result->handle = 0;
    result->result = 0;
    return BOS_OK;
}

void native_sync_owner_release(unsigned owner) {
    native_sync_tick();
    if (!owner) return;
    for (unsigned i = 0; i < NATIVE_SYNC_CAPACITY; ++i)
        if (results[i].owner == owner) {
            results[i].owner = 0;
            results[i].handle = 0;
            results[i].result = 0;
        }
}
