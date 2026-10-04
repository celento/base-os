#ifndef FS_H
#define FS_H

#include "layout.h"
#define FS_NAME_LEN  24
#define FS_MAX_SIZE  16384 /* Legacy floppy, scripts and BEX image buffer limit. */
#define FS_FILE_MAX  2097152 /* Data-volume file limit, inclusive. */

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
/* File-data allowance for the current node count; extra IDE nodes cost 40 bytes. */
unsigned fs_capacity(void);
/* Same allowance for a projected total node count, or 0 outside this backend. */
unsigned fs_capacity_for_nodes(unsigned count);
unsigned fs_file_limit(void);
const char *fs_storage_name(void);
unsigned fs_used_bytes(void);
int fs_resolve(int cwd, const char *path);
int fs_destination(int cwd, const char *path, char *name);
void fs_init(void);
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
/* Atomic: writes all bytes, or returns -1 without changing the file. */
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
void fs_empty_dir(int parent);
int fs_copy(int id, int parent);
int fs_rename(int id, const char *name);

int fs_save_disk(void);
#define FS_LOAD_BLANK 1
int fs_load_disk(void);
int fs_needs_sync(void);
int fs_sync(void);
void fs_autosync(void);
const char *fs_storage_status(void);

#endif
