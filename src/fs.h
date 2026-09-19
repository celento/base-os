#ifndef FS_H
#define FS_H

#define FS_MAX_NODES 64
#define FS_NAME_LEN  24
#define FS_MAX_SIZE  16384
#define FS_PATH_LEN  1536

int kstrlen(const char *s);
int kstrcmp(const char *a, const char *b);
void kstrcpy(char *d, const char *s);
void kmemcpy(void *d, const void *s, int n);
void kmemset(void *d, int v, int n);

unsigned fs_clock(void);
unsigned fs_modified(int id);
unsigned fs_identity(int id);
unsigned fs_capacity(void);
unsigned fs_used_bytes(void);
int fs_resolve(int cwd, const char *path);
int fs_destination(int cwd, const char *path, char *name);
void fs_init(void);
int fs_root(void);
int fs_valid(int id);
int fs_is_dir(int id);
int fs_is_app(int id);
int fs_node_count(void);
int fs_parent(int id);
int fs_size(int id);
const char *fs_name(int id);
const char *fs_data(int id);

int fs_find_child(int parent, const char *name);
int fs_mkdir(int parent, const char *name);
int fs_create(int parent, const char *name);
int fs_create_app(int parent, const char *name);
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
