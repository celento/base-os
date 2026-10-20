#ifndef NATIVE_SYNC_H
#define NATIVE_SYNC_H

/* Kernel-only owner keys and opaque app-visible completion handles. All calls
 * run in serialized process/desktop context, never IRQ or device polling.
 * No function here dispatches applications or performs storage I/O. */
#define NATIVE_SYNC_CAPACITY 32u
#define NATIVE_SYNC_PER_OWNER 4u

int native_sync_available(void);
unsigned native_sync_capacity(void);
unsigned native_sync_per_owner_limit(void);
/* Accept a durability boundary for all FS mutations completed before the call.
 * An existing snapshot can be joined because it leases the live FS unchanged.
 * Returns BOS_OK and a new owned handle, or a BOS_E_* error without touching
 * *handle. A clean volume's handle is immediately complete. */
int native_sync_begin(unsigned owner, unsigned *handle);
/* BOS_PENDING, BOS_OK, or retained BOS_E_IO/BOS_E_STALE. No implicit release. */
int native_sync_poll(unsigned owner, unsigned handle);
/* Discard this owner's interest, even if pending. Never cancels a commit. */
int native_sync_release(unsigned owner, unsigned handle);
void native_sync_owner_release(unsigned owner);
/* Copy final outcome and promptly release the one underlying FS ticket. Runs
 * even with zero app owners, and does not wait for completed handles to close. */
void native_sync_tick(void);

#endif
