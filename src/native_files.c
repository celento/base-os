#include "native_files.h"
#include "fs.h"
#include "physmem.h"

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

static void transactions_reset(void);
static void transactions_release_owner(uint32_t owner);
static void transactions_close_file(uint32_t owner, uint32_t handle);

void native_files_init(void) {
    transactions_reset();
    kmemset(native_files, 0, sizeof(native_files));
}
void native_files_release_owner(uint32_t owner) {
    transactions_release_owner(owner);
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
    transactions_close_file(owner, handle);
    /* A changed/remounted file must still be closable to release its slot. */
    kmemset(file, 0, sizeof(*file));
    return BOS_OK;
}

/* Stages occupy individually owned allocator frames, never the shared FS image
 * arena or an application mapping. This metadata is bounded kernel storage. */
typedef struct {
    uint32_t owner, handle, mode, total, received, state, incarnation;
    uint32_t source_handle, identity, revision, page_count;
    int node;
    char path[NATIVE_FILE_PATH_MAX + 1], name[FS_NAME_LEN];
    uint32_t frames[NATIVE_FILE_TRANSACTION_PAGES];
} NativeFileTransaction;
static NativeFileTransaction transactions[NATIVE_FILE_TRANSACTION_CAPACITY];
static uint32_t transaction_serial;
static unsigned transaction_pages;
_Static_assert(NATIVE_FILE_TRANSACTION_MAX == NATIVE_FILE_TRANSACTION_PAGES * PHYS_PAGE_BYTES,
               "stage bound must match complete page reservation");
_Static_assert(NATIVE_FILE_TRANSACTION_CHUNK == PHYS_PAGE_BYTES, "review append span bound");

static int transaction_owner(uint32_t owner) {
    return (owner & ~BOS_HANDLE_SERIAL_MAX) == BOS_HANDLE_TYPE_PROCESS &&
           (owner & BOS_HANDLE_SERIAL_MAX);
}
static NativeFileTransaction *transaction_lookup(uint32_t owner, uint32_t handle) {
    if (!transaction_owner(owner) ||
        (handle & ~BOS_HANDLE_SERIAL_MAX) != BOS_HANDLE_TYPE_FILE_TRANSACTION) return 0;
    for (unsigned i = 0; i < NATIVE_FILE_TRANSACTION_CAPACITY; ++i)
        if (transactions[i].owner == owner && transactions[i].handle == handle) return transactions + i;
    return 0;
}
static void transaction_drop_pages(NativeFileTransaction *stage) {
    if (stage->page_count) {
        if (physmem_release(stage->owner, PHYS_FILE_STAGE, stage->frames, stage->page_count) != PHYS_OK)
            panic("file stage ownership");
        transaction_pages -= stage->page_count;
        stage->page_count = 0;
        kmemset(stage->frames, 0, sizeof stage->frames);
    }
}
static void transaction_discard(NativeFileTransaction *stage) {
    transaction_drop_pages(stage);
    kmemset(stage, 0, sizeof *stage);
}
static void transactions_reset(void) {
    for (unsigned i = 0; i < NATIVE_FILE_TRANSACTION_CAPACITY; ++i) transaction_discard(transactions + i);
}
static void transactions_release_owner(uint32_t owner) {
    if (!owner) return;
    for (unsigned i = 0; i < NATIVE_FILE_TRANSACTION_CAPACITY; ++i)
        if (transactions[i].owner == owner) transaction_discard(transactions + i);
}
static void transactions_close_file(uint32_t owner, uint32_t handle) {
    for (unsigned i = 0; i < NATIVE_FILE_TRANSACTION_CAPACITY; ++i)
        if (transactions[i].owner == owner && transactions[i].source_handle == handle)
            transaction_discard(transactions + i);
}
void native_file_transactions_tick(void) {
    unsigned incarnation = fs_incarnation();
    for (unsigned i = 0; i < NATIVE_FILE_TRANSACTION_CAPACITY; ++i) {
        NativeFileTransaction *stage = transactions + i;
        if (!stage->owner || stage->incarnation == incarnation ||
            stage->state == BOS_FILE_TRANSACTION_INVALIDATED) continue;
        transaction_drop_pages(stage);
        stage->received = 0;
        stage->state = BOS_FILE_TRANSACTION_INVALIDATED;
    }
}
int native_file_transactions_available(void) { return fs_sync_async_supported(); }
void native_file_transactions_query(BosFileTransactionInfoV1 *out) {
    native_file_transactions_tick();
    BosFileTransactionInfoV1 info = {0};
    info.struct_size = sizeof info; info.major = BOS_FILE_TRANSACTION_MAJOR;
    info.minor = BOS_FILE_TRANSACTION_MINOR;
    info.capabilities = BOS_FILE_TRANSACTION_CAP_REPLACE | BOS_FILE_TRANSACTION_CAP_CREATE;
    info.context = BOS_CONTEXT_DESKTOP_TASK;
    info.chunk_bytes = NATIVE_FILE_TRANSACTION_CHUNK; info.total_bytes = NATIVE_FILE_TRANSACTION_MAX;
    info.transactions_per_process = NATIVE_FILE_TRANSACTION_PER_OWNER;
    info.transactions_total = NATIVE_FILE_TRANSACTION_CAPACITY;
    info.page_bytes = PHYS_PAGE_BYTES; info.pages_per_transaction = NATIVE_FILE_TRANSACTION_PAGES;
    info.pages_total = NATIVE_FILE_TRANSACTION_TOTAL_PAGES; info.pages_used = transaction_pages;
    for (unsigned i = 0; i < NATIVE_FILE_TRANSACTION_CAPACITY; ++i) info.transactions_used += transactions[i].owner != 0;
    info.file_bytes = fs_file_limit(); info.path_bytes = NATIVE_FILE_PATH_MAX;
    info.file_info_bytes = sizeof(BosFileInfo); info.begin_bytes = sizeof(BosFileTransactionBeginV1);
    info.status_bytes = sizeof(BosFileTransactionStatusV1);
    *out = info;
}
static void transaction_status(const NativeFileTransaction *stage, BosFileTransactionStatusV1 *out) {
    BosFileTransactionStatusV1 info = {0};
    info.struct_size = sizeof info; info.version = BOS_FILE_TRANSACTION_VERSION;
    info.handle = stage->handle; info.mode = stage->mode; info.total_bytes = stage->total;
    info.received_bytes = stage->received; info.state = stage->state;
    *out = info;
}
/* Separate malformed path components from a missing/non-directory parent. */
static int transaction_path(const char *path, unsigned bytes) {
    if (!valid_path(path) || !bytes || bytes > NATIVE_FILE_PATH_MAX || (unsigned)kstrlen(path) != bytes) return 0;
    unsigned component = 0, last = 1;
    for (unsigned i = 1; i < bytes; ++i) {
        if (path[i] == '/') { component = 0; last = i + 1; }
        else if (++component >= FS_NAME_LEN) return 0;
    }
    if (!component || !kstrcmp(path + last, ".") || !kstrcmp(path + last, "..")) return 0;
    return 1;
}
int native_file_transaction_begin(uint32_t owner, unsigned mode,
    const BosFileTransactionBeginV1 *input, const char *path, BosFileTransactionStatusV1 *out) {
    if (!input || !out) return BOS_E_INVALID;
    if (input->version != BOS_FILE_TRANSACTION_VERSION) return BOS_E_UNSUPPORTED;
    if (!transaction_owner(owner) || input->struct_size != sizeof *input || input->flags ||
        (mode != BOS_FILE_TRANSACTION_REPLACE && mode != BOS_FILE_TRANSACTION_CREATE)) return BOS_E_INVALID;
    for (unsigned i = 0; i < 8; ++i) if (input->reserved[i]) return BOS_E_INVALID;
    int creating = mode == BOS_FILE_TRANSACTION_CREATE;
    if (creating ? (input->source_handle || input->expected_revision ||
                   !transaction_path(path, input->path_bytes)) :
                  (!input->source_handle || !input->expected_revision || input->path_offset || input->path_bytes))
        return BOS_E_INVALID;
    native_file_transactions_tick();
    if (!native_file_transactions_available()) return BOS_E_UNSUPPORTED;
    NativeFile *file = 0;
    int node;
    char name[FS_NAME_LEN] = {0};
    if (creating) {
        node = fs_destination(fs_root(), path, name);
        if (node < 0) return BOS_E_NOT_FOUND;
        if (fs_find_child(node, name) >= 0) return BOS_E_CHANGED;
        if (!document_parent(node) || !fs_writes_allowed()) return BOS_E_PROTECTED;
        if (!fs_identity(node)) return BOS_E_CAPACITY;
    } else {
        file = file_lookup(owner, input->source_handle);
        if (!file) return BOS_E_STALE;
        if (!file_current(file) || file->revision != input->expected_revision) return BOS_E_CHANGED;
        if (!(file->flags & BOS_FILE_OPEN_WRITE) ||
            !document_parent(fs_parent(file->node)) || !fs_writes_allowed()) return BOS_E_PROTECTED;
        node = file->node;
    }
    if (input->total_bytes > NATIVE_FILE_TRANSACTION_MAX || input->total_bytes > fs_file_limit()) return BOS_E_CAPACITY;
    NativeFileTransaction *slot = 0;
    for (unsigned i = 0; i < NATIVE_FILE_TRANSACTION_CAPACITY; ++i) {
        if (transactions[i].owner == owner) return BOS_E_CAPACITY;
        if (!transactions[i].owner && !slot) slot = transactions + i;
    }
    unsigned pages = (input->total_bytes + PHYS_PAGE_BYTES - 1) / PHYS_PAGE_BYTES;
    if (!slot || transaction_serial == BOS_HANDLE_SERIAL_MAX ||
        pages > NATIVE_FILE_TRANSACTION_TOTAL_PAGES - transaction_pages) return BOS_E_CAPACITY;
    /* Allocator output remains kernel-private until every reservation succeeds.
     * The vacant record is otherwise zero; failure never publishes owner/handle. */
    if (pages && physmem_alloc(owner, PHYS_FILE_STAGE, pages, slot->frames) != PHYS_OK) return BOS_E_CAPACITY;
    slot->owner = owner; slot->handle = BOS_HANDLE_TYPE_FILE_TRANSACTION | ++transaction_serial;
    slot->mode = mode; slot->total = input->total_bytes; slot->page_count = pages;
    slot->state = input->total_bytes ? BOS_FILE_TRANSACTION_UPLOADING : BOS_FILE_TRANSACTION_COMPLETE;
    slot->incarnation = fs_incarnation(); slot->node = node; slot->identity = fs_identity(node);
    slot->source_handle = input->source_handle; slot->revision = input->expected_revision;
    if (creating) { kstrcpy(slot->path, path); kstrcpy(slot->name, name); }
    transaction_pages += pages;
    transaction_status(slot, out);
    return BOS_OK;
}
static void transaction_check_frames(const NativeFileTransaction *stage) {
    unsigned pages = (stage->total + PHYS_PAGE_BYTES - 1) / PHYS_PAGE_BYTES;
    if (stage->total > NATIVE_FILE_TRANSACTION_MAX || stage->received > stage->total || stage->page_count != pages ||
        (pages && physmem_validate(stage->owner, PHYS_FILE_STAGE, stage->frames, pages) != PHYS_OK))
        panic("file stage preflight");
}
int native_file_transaction_append(uint32_t owner, uint32_t handle, const void *data,
                                   unsigned length, unsigned offset) {
    if (!data || !length || length > NATIVE_FILE_TRANSACTION_CHUNK) return BOS_E_INVALID;
    native_file_transactions_tick();
    NativeFileTransaction *stage = transaction_lookup(owner, handle);
    if (!stage) return BOS_E_STALE;
    if (stage->state == BOS_FILE_TRANSACTION_INVALIDATED) return BOS_E_CHANGED;
    if (offset != stage->received || length > stage->total - stage->received) return BOS_E_INVALID;
    transaction_check_frames(stage);
    const char *source = data;
    for (unsigned copied = 0; copied < length;) {
        unsigned position = offset + copied, in_page = position % PHYS_PAGE_BYTES;
        unsigned bytes = PHYS_PAGE_BYTES - in_page;
        if (bytes > length - copied) bytes = length - copied;
        kmemcpy((void *)(uintptr_t)(stage->frames[position / PHYS_PAGE_BYTES] + in_page), source + copied, bytes);
        copied += bytes;
    }
    stage->received += length;
    if (stage->received == stage->total) stage->state = BOS_FILE_TRANSACTION_COMPLETE;
    return (int)length;
}
int native_file_transaction_info(uint32_t owner, uint32_t handle, BosFileTransactionStatusV1 *out) {
    if (!out) return BOS_E_INVALID;
    native_file_transactions_tick();
    NativeFileTransaction *stage = transaction_lookup(owner, handle);
    if (!stage) return BOS_E_STALE;
    transaction_status(stage, out);
    return BOS_OK;
}
static void transaction_source(void *context, unsigned offset, void *out, unsigned length) {
    const NativeFileTransaction *stage = context;
    /* FS requests aligned 4 KiB chunks with one short tail. All frames are
     * already checked, stable and private for the entire publication. */
    kmemcpy(out, (const void *)(uintptr_t)stage->frames[offset / PHYS_PAGE_BYTES], length);
}
int native_file_transaction_accept(uint32_t owner, uint32_t handle, BosFileInfo *out) {
    if (!out) return BOS_E_INVALID;
    native_file_transactions_tick();
    NativeFileTransaction *stage = transaction_lookup(owner, handle);
    if (!stage) return BOS_E_STALE;
    if (stage->state == BOS_FILE_TRANSACTION_INVALIDATED) return BOS_E_CHANGED;
    if (stage->state != BOS_FILE_TRANSACTION_COMPLETE) return BOS_E_INVALID;
    NativeFile *file = 0;
    int creating = stage->mode == BOS_FILE_TRANSACTION_CREATE;
    int node = stage->node;
    if (creating) {
        char name[FS_NAME_LEN];
        if (!fs_is_dir(node) || stage->identity != fs_identity(node) ||
            fs_destination(fs_root(), stage->path, name) != node || kstrcmp(name, stage->name) ||
            fs_find_child(node, stage->name) >= 0) return BOS_E_CHANGED;
        if (!document_parent(node) || !fs_writes_allowed()) return BOS_E_PROTECTED;
    } else {
        file = file_lookup(owner, stage->source_handle);
        if (!file) return BOS_E_STALE;
        if (!fs_valid(node) || fs_is_dir(node) || fs_is_app(node) ||
            !stage->identity || stage->identity != fs_identity(node) ||
            !stage->revision || stage->revision != fs_content_revision(node)) return BOS_E_CHANGED;
        if (!(file->flags & BOS_FILE_OPEN_WRITE) ||
            !document_parent(fs_parent(node)) || !fs_writes_allowed()) return BOS_E_PROTECTED;
    }
    if (fs_sync_busy()) return BOS_E_BUSY;
    if (stage->total > fs_file_limit() || !fs_version_available(creating)) return BOS_E_CAPACITY;
    if (creating) {
        unsigned count = 0;
        for (unsigned i = 0; i < NATIVE_FILE_CAPACITY; ++i) {
            if (native_files[i].owner == owner) ++count;
            if (!native_files[i].owner && !file) file = native_files + i;
        }
        if (!file || count >= NATIVE_FILE_PER_OWNER || native_file_serial == FILE_SERIAL_MAX) return BOS_E_CAPACITY;
    }
    transaction_check_frames(stage);
    int result = fs_publish_private(creating ? -1 : node, creating ? node : 0,
                                    creating ? stage->name : 0, stage->total, transaction_source, stage);
    if (result == FS_ERR_BUSY) return BOS_E_BUSY;
    if (result < 0) return BOS_E_CAPACITY;
    /* All resource preflight has passed. Only infallible local assignments,
     * known-span output, and release of this exact checked list remain. */
    if (creating) *file = (NativeFile){ .owner = owner, .handle = FILE_HANDLE_TAG | ++native_file_serial,
        .flags = BOS_FILE_OPEN_READ | BOS_FILE_OPEN_WRITE, .incarnation = fs_incarnation(),
        .identity = fs_identity(result), .node = result };
    file->revision = fs_content_revision(result);
    file_info(file, out);
    transaction_discard(stage);
    return BOS_OK;
}
int native_file_transaction_abort(uint32_t owner, uint32_t handle) {
    native_file_transactions_tick();
    NativeFileTransaction *stage = transaction_lookup(owner, handle);
    if (!stage) return BOS_E_STALE;
    transaction_discard(stage);
    return BOS_OK;
}
