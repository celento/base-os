#include "native_files.h"
#include "fs.h"

/* Handles are opaque, tagged, and never reused within a kernel lifetime.
 * Keeping the node index private does not pin FS data in the compacting arena. */
typedef struct {
    uint32_t owner, handle, flags, incarnation, identity, revision;
    int node;
} NativeFile;
static NativeFile native_files[NATIVE_FILE_CAPACITY];
static uint32_t native_file_serial;
#define FILE_HANDLE_TAG BOS_HANDLE_TYPE_FILE
#define FILE_SERIAL_MAX BOS_HANDLE_SERIAL_MAX

void native_files_init(void) {
    kmemset(native_files, 0, sizeof(native_files));
}
void native_files_release_owner(uint32_t owner) {
    for (unsigned i = 0; i < NATIVE_FILE_CAPACITY; ++i)
        if (native_files[i].owner == owner)
            kmemset(&native_files[i], 0, sizeof(native_files[i]));
}
static NativeFile *file_lookup(uint32_t owner, uint32_t handle) {
    if (!owner || (handle & ~FILE_SERIAL_MAX) != FILE_HANDLE_TAG) return 0;
    for (unsigned i = 0; i < NATIVE_FILE_CAPACITY; ++i)
        if (native_files[i].owner == owner && native_files[i].handle == handle)
            return &native_files[i];
    return 0;
}
static int file_current(const NativeFile *file) {
    return file->incarnation == fs_incarnation() && fs_valid(file->node) &&
           !fs_is_dir(file->node) && !fs_is_app(file->node) &&
           file->identity && file->identity == fs_identity(file->node) &&
           file->revision && file->revision == fs_content_revision(file->node);
}
static void file_info(const NativeFile *file, BosFileInfo *out) {
    BosFileInfo info = {0};
    info.struct_size = sizeof(info);
    info.handle = file->handle;
    info.size = (uint32_t)fs_size(file->node);
    info.flags = file->flags;
    info.revision = file->revision;
    *out = info;
}
static int document_parent(int parent) {
    int documents = fs_find_child(fs_root(), "Documents");
    if (documents < 0 || !fs_is_dir(documents)) return 0;
    for (unsigned i = 0; i < FS_MAX_NODES && parent >= 0; ++i) {
        if (parent == documents) return 1;
        parent = fs_parent(parent);
    }
    return 0;
}
static int valid_path(const char *path) {
    if (!path || path[0] != '/') return 0;
    for (unsigned i = 0; i <= NATIVE_FILE_PATH_MAX; ++i) {
        if (!path[i]) return i != 0;
        if ((unsigned char)path[i] < 32 || (unsigned char)path[i] > 126) return 0;
    }
    return 0;
}
int native_file_open(uint32_t owner, const char *path, uint32_t flags, BosFileInfo *out) {
    const unsigned rights = BOS_FILE_OPEN_READ | BOS_FILE_OPEN_WRITE;
    if (!owner || !out || !valid_path(path) || !(flags & rights) ||
        (flags & ~(rights | BOS_FILE_OPEN_CREATE)) ||
        ((flags & BOS_FILE_OPEN_CREATE) && (flags & rights) != rights))
        return BOS_E_INVALID;
    NativeFile *slot = 0;
    unsigned owner_count = 0;
    for (unsigned i = 0; i < NATIVE_FILE_CAPACITY; ++i) {
        if (native_files[i].owner == owner) ++owner_count;
        if (!native_files[i].owner && !slot) slot = &native_files[i];
    }
    if (!slot || owner_count >= NATIVE_FILE_PER_OWNER || native_file_serial == FILE_SERIAL_MAX)
        return BOS_E_CAPACITY;
    int id = fs_resolve(fs_root(), path);
    if (flags & BOS_FILE_OPEN_CREATE) {
        if (id >= 0) return BOS_E_CHANGED;
        char name[FS_NAME_LEN];
        int parent = fs_destination(fs_root(), path, name);
        if (parent < 0) return BOS_E_NOT_FOUND;
        if (!document_parent(parent) || !fs_writes_allowed()) return BOS_E_PROTECTED;
        if (fs_sync_busy()) return BOS_E_BUSY;
        if (!fs_version_available(1)) return BOS_E_CAPACITY;
        /* Empty exclusive create cannot fail after allocating its node; all
         * later handle publication is bounded in-memory assignment. */
        id = fs_create(parent, name);
        if (id == FS_ERR_BUSY) return BOS_E_BUSY;
        if (id < 0) return BOS_E_CAPACITY;
    } else {
        if (id < 0) return BOS_E_NOT_FOUND;
        if (fs_is_dir(id) || fs_is_app(id)) return BOS_E_INVALID;
        if ((flags & BOS_FILE_OPEN_WRITE) &&
            (!document_parent(fs_parent(id)) || !fs_writes_allowed())) return BOS_E_PROTECTED;
        if (!fs_identity(id) || !fs_content_revision(id)) return BOS_E_CAPACITY;
    }
    *slot = (NativeFile){ .owner = owner, .handle = FILE_HANDLE_TAG | ++native_file_serial,
        .flags = flags & rights, .incarnation = fs_incarnation(), .identity = fs_identity(id),
        .revision = fs_content_revision(id), .node = id };
    file_info(slot, out);
    return BOS_OK;
}
int native_file_info(uint32_t owner, uint32_t handle, BosFileInfo *out) {
    if (!out) return BOS_E_INVALID;
    NativeFile *file = file_lookup(owner, handle);
    if (!file) return BOS_E_STALE;
    if (!file_current(file)) return BOS_E_CHANGED;
    file_info(file, out);
    return BOS_OK;
}
int native_file_read_at(uint32_t owner, uint32_t handle, uint32_t offset,
                        void *out, uint32_t capacity) {
    if (capacity > NATIVE_FILE_READ_MAX || (capacity && !out)) return BOS_E_INVALID;
    NativeFile *file = file_lookup(owner, handle);
    if (!file) return BOS_E_STALE;
    if (!(file->flags & BOS_FILE_OPEN_READ)) return BOS_E_PROTECTED;
    if (!file_current(file)) return BOS_E_CHANGED;
    uint32_t size = (uint32_t)fs_size(file->node);
    if (offset >= size || !capacity) return 0;
    size -= offset;
    if (size > capacity) size = capacity;
    /* No app dispatch or file mutation can occur between the version check
     * and this bounded copy. On any error, output is completely untouched. */
    kmemcpy(out, fs_data(file->node) + offset, (int)size);
    return (int)size;
}
int native_file_replace(uint32_t owner, uint32_t handle, const void *data,
                        uint32_t length, BosFileInfo *out) {
    if (!out || length > NATIVE_FILE_REPLACE_MAX || (length && !data)) return BOS_E_INVALID;
    NativeFile *file = file_lookup(owner, handle);
    if (!file) return BOS_E_STALE;
    if (!(file->flags & BOS_FILE_OPEN_WRITE)) return BOS_E_PROTECTED;
    if (!file_current(file)) return BOS_E_CHANGED;
    /* Rights do not preserve access after a node is moved outside Documents. */
    if (!document_parent(fs_parent(file->node)) || !fs_writes_allowed()) return BOS_E_PROTECTED;
    if (fs_sync_busy()) return BOS_E_BUSY;
    if (length > fs_file_limit() || !fs_version_available(0)) return BOS_E_CAPACITY;
    int result = fs_write(file->node, data, (int)length);
    if (result == FS_ERR_BUSY) return BOS_E_BUSY;
    if (result < 0) return BOS_E_CAPACITY;
    file->revision = fs_content_revision(file->node);
    file_info(file, out);
    return BOS_OK;
}
int native_file_close(uint32_t owner, uint32_t handle) {
    NativeFile *file = file_lookup(owner, handle);
    if (!file) return BOS_E_STALE;
    /* A changed/remounted file must still be closable to release its slot. */
    kmemset(file, 0, sizeof(*file));
    return BOS_OK;
}
