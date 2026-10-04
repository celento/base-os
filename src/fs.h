#ifndef FS_H
#define FS_H

#include "layout.h"
#define FS_NAME_LEN  24
#define FS_MAX_SIZE  16384 /* Legacy floppy, command scripts and Tiny BASIC buffer limit. */
#define FS_FILE_MAX  2097152 /* Default data-volume file limit, inclusive. */
#define FS_LARGE_FILE_MAX 16777216 /* Explicit large-profile volume only. */

int kstrlen(const char *s);
int kstrcmp(const char *a, const char *b);
void kstrcpy(char *d, const char *s);
void kmemcpy(void *d, const void *s, int n);
void kmemset(void *d, int v, int n);

unsigned fs_clock(void);
/* Device-only servicing during long copies/checksums; must not mutate files. */
void fs_background_poll(void);
unsigned fs_modified(int id);
unsigned fs_identity(int id);
/* Runtime content token, separate from identity and the wall-clock timestamp.
 * Create/import and every successful write (even identical bytes) get a fresh
 * token. Rename/move/sync leave it unchanged. Tokens never wrap or reset across
 * remount/init; 0 means invalid/unknown after exhaustion, never a match. */
unsigned fs_content_revision(int id);
/* File-data allowance for the current node count; extra IDE nodes cost 40 bytes. */
unsigned fs_capacity(void);
/* Same allowance for a projected total node count, or 0 outside this backend. */
unsigned fs_capacity_for_nodes(unsigned count);
unsigned fs_file_limit(void);
/* True only while the marked large backend is selected and the FS owns high
 * arenas. False for default IDE, floppy and protected fallback views.
 * Query only after mount; fs_init/reconfiguration ends that arena ownership. */
int fs_large_profile(void);
/* Physical E820 availability only, even on a default disk using low arenas.
 * This query alone NEVER means the old 32-48 MiB FS arenas are unused. No
 * ownership is granted to media; those consumers retain existing limits. */
int fs_large_arenas_available(void);
const char *fs_storage_name(void);
unsigned fs_used_bytes(void);
int fs_resolve(int cwd, const char *path);
int fs_destination(int cwd, const char *path, char *name);
/* Reconfiguration refuses an in-flight snapshot without touching its arenas. */
int fs_init(void);
int fs_root(void);
int fs_valid(int id);
int fs_is_dir(int id);
int fs_is_app(int id);
int fs_node_count(void);
/* Runtime limit: 64 on floppy, 256 on the IDE data volume. */
int fs_node_limit(void);
int fs_parent(int id);
int fs_size(int id);
const char *fs_name(int id);
/* Borrowed bytes, followed by a convenience NUL; any mutation can move them. */
const char *fs_data(int id);

int fs_find_child(int parent, const char *name);
int fs_mkdir(int parent, const char *name);
int fs_create(int parent, const char *name);
int fs_create_app(int parent, const char *name);
/* Mutators return FS_ERR_BUSY without side effects while a snapshot owns RAM. */
#define FS_ERR_BUSY (-2)
/* Atomic: writes all bytes, or returns a negative error without changing it. */
int fs_write(int id, const char *data, int len);
int fs_read(int id, char *out, int max);
int fs_list(int parent, int *ids, int max);
int fs_list_files(int *ids, int max);
void fs_path(int id, char *out, int max);
int fs_unique_file(int parent, char *out);
int fs_unique_dir(int parent, char *out);
int fs_unique_copy(int parent, const char *src, char *out);
int fs_child_count(int parent);
int fs_move(int id, int new_parent);
int fs_delete(int id);
int fs_empty_dir(int parent);
int fs_copy(int id, int parent);
int fs_rename(int id, const char *name);

int fs_save_disk(void);
#define FS_LOAD_BLANK 1
int fs_load_disk(void);
int fs_needs_sync(void);
/* One async owner plus an independent blocking join/result slot. A retained
 * async result never prevents a later blocking sync. Tickets survive completion
 * until explicitly released. Remount/reinitialization invalidates them; counters
 * never wrap/reuse.
 * request: 0 accepted, -1 unavailable, FS_ERR_BUSY during a job or while the
 * sole async result slot is retained. Rejected requests leave *ticket unchanged.
 * It does no IDE serialization/I/O. Floppy requests retain synchronous behavior.
 * result: FS_SYNC_PENDING, 0 fully verified durable, -1 failed, or STALE.
 * release: 0 released, BUSY while running, STALE for another owner.
 * A clean request still has an immediately durable, releasable ticket. */
typedef struct { unsigned incarnation, serial; } FsSyncTicket;
#define FS_SYNC_PENDING 1
#define FS_SYNC_STALE (-3)
int fs_sync_request(FsSyncTicket *ticket);
int fs_sync_result(FsSyncTicket ticket);
int fs_sync_release(FsSyncTicket ticket);
int fs_sync_busy(void);
/* One bounded CPU quantum or ATA poll (at most 8 sectors). MORE means that
 * callers should schedule another quantum, WAIT means hardware is pending.
 * Neither step nor device polling dispatches applications or retains a stack. */
enum FsSyncProgress { FS_SYNC_IDLE, FS_SYNC_WAIT, FS_SYNC_MORE, FS_SYNC_FINISHED };
enum FsSyncProgress fs_sync_step(void);
/* Blocking compatibility: joins an existing snapshot, never queued-success.
 * It does not consume an explicit async owner's terminal result. */
int fs_sync(void);
/* IDE: request only, progressed with fs_sync_step; owns/reaps its own result.
 * Floppy: blocking compatibility path. Existing failure backoff is preserved. */
void fs_autosync(void);
const char *fs_storage_status(void);

#endif
