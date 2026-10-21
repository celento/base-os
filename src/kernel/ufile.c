#include "kernel/ufile.h"
#include "kernel/uproc.h"
#include "kernel/kheap.h"
#include "kernel/fs.h"
#include "kernel/platform.h"
#include "../sdk/baseos_sys.h"

enum { UFILE_CONSOLE = 1, UFILE_NODE, UFILE_DIRECTORY, UFILE_PIPE_READ, UFILE_PIPE_WRITE };

#define PIPE_BYTES 4096u
/* Seconds from 1970 to 2000, the volume's clock origin. */
#define VOLUME_EPOCH 946684800LL

typedef struct UPipe {
    unsigned readers, writers;
    unsigned head, count;
    char data[PIPE_BYTES];
} UPipe;

typedef struct UFile {
    unsigned refs;
    int kind;
    int flags;                 /* BOS_O_ACCMODE and BOS_O_APPEND bits */
    int node;
    unsigned identity;
    uint32_t offset;
    /* Writable node: the file's contents until written back. */
    char *buffer;
    uint32_t size, capacity;
    int dirty;
    UPipe *pipe;
    struct UFile *next_orphan;
} UFile;

/* Closed files whose contents the volume has not taken yet. */
static UFile *orphans;

/* ---------- volume access ---------- */

static int node_current(const UFile *file) {
    return fs_valid(file->node) && fs_identity(file->node) == file->identity;
}

/* A mutation the volume refused while a snapshot owned it is retried after a
 * short wait; user threads may block, so this never fails for BUSY. */
static void wait_volume(void) {
    thread_wait(0, timer_milliseconds() + 2);
}

static int write_back(UFile *file, int may_wait) {
    if (!file->dirty) return 0;
    for (;;) {
        /* Checked on every attempt: while this waited, the file may have
         * been deleted and its node slot given to another file. */
        if (!node_current(file)) {
            /* The file was deleted or replaced while open: nothing to update. */
            file->dirty = 0;
            return 0;
        }
        int result = fs_write(file->node, file->buffer, (int)file->size);
        if (result == FS_ERR_BUSY) {
            if (!may_wait) return FS_ERR_BUSY;
            wait_volume();
            continue;
        }
        if (result < 0) return -BOS_EIO;
        file->dirty = 0;
        return 0;
    }
}

static void free_file(UFile *file) {
    kfree(file->buffer);
    kfree(file);
}

static void release(UFile *file, int may_wait) {
    if (!file || --file->refs) return;
    if (file->kind == UFILE_PIPE_READ || file->kind == UFILE_PIPE_WRITE) {
        UPipe *pipe = file->pipe;
        if (file->kind == UFILE_PIPE_READ) --pipe->readers;
        else --pipe->writers;
        thread_wake_channel((uintptr_t)pipe, ~0u);
        if (!pipe->readers && !pipe->writers) kfree(pipe);
        kfree(file);
        return;
    }
    if (write_back(file, may_wait) == FS_ERR_BUSY) {
        file->next_orphan = orphans;
        orphans = file;
        return;
    }
    free_file(file);
}

void ufile_poll(void) {
    UFile **link = &orphans;
    while (*link) {
        UFile *file = *link;
        if (write_back(file, 0) == FS_ERR_BUSY) {
            link = &file->next_orphan;
            continue;
        }
        *link = file->next_orphan;
        free_file(file);
    }
}

/* ---------- descriptor table ---------- */

static UFile *new_file(int kind) {
    UFile *file = kmalloc(sizeof *file);
    if (!file) return 0;
    kmemset(file, 0, sizeof *file);
    file->refs = 1;
    file->kind = kind;
    file->node = -1;
    return file;
}

/* Far above any real program's need; it only keeps the table size from
 * overflowing. */
#define UFILE_MAX_DESCRIPTORS (1u << 20)

static int grow(UFileTable *table, unsigned needed) {
    if (needed <= table->capacity) return 0;
    if (needed > UFILE_MAX_DESCRIPTORS) return -BOS_EMFILE;
    unsigned capacity = table->capacity ? table->capacity : 16;
    while (capacity < needed) capacity *= 2;
    UFile **slots = kmalloc(capacity * sizeof *slots);
    if (!slots) return -BOS_EMFILE;
    kmemset(slots, 0, (int)(capacity * sizeof *slots));
    if (table->slots) kmemcpy(slots, table->slots, (int)(table->capacity * sizeof *slots));
    kfree(table->slots);
    table->slots = slots;
    table->capacity = capacity;
    return 0;
}

/* The lowest free descriptor at or above from, holding file. */
static int install(UFileTable *table, UFile *file, unsigned from) {
    unsigned fd = from;
    while (fd < table->capacity && table->slots[fd]) ++fd;
    int result = grow(table, fd + 1);
    if (result) return result;
    table->slots[fd] = file;
    return (int)fd;
}

static UFile *lookup(UFileTable *table, int fd) {
    return fd >= 0 && (unsigned)fd < table->capacity ? table->slots[fd] : 0;
}

int ufile_table_init(UFileTable *table) {
    kmemset(table, 0, sizeof *table);
    table->cwd = fs_root();
    table->cwd_identity = fs_identity(table->cwd);
    UFile *console = new_file(UFILE_CONSOLE);
    if (!console || grow(table, 3)) {
        kfree(console);
        kfree(table->slots);
        table->slots = 0;
        return -BOS_ENOMEM;
    }
    console->flags = BOS_O_RDWR;
    console->refs = 3;
    for (int fd = 0; fd < 3; ++fd) table->slots[fd] = console;
    return 0;
}

void ufile_table_flush(UFileTable *table) {
    for (unsigned fd = 0; fd < table->capacity; ++fd) {
        UFile *file = table->slots[fd];
        if (!file || file->kind != UFILE_NODE) continue;
        /* Sibling threads still run while this waits for the volume and may
         * close the descriptor; the extra reference keeps the file alive. */
        ++file->refs;
        write_back(file, 1);
        release(file, 1);
    }
}

void ufile_table_close_all(UFileTable *table) {
    for (unsigned fd = 0; fd < table->capacity; ++fd) {
        release(table->slots[fd], 0);
        table->slots[fd] = 0;
    }
    kfree(table->slots);
    table->slots = 0;
    table->capacity = 0;
}

/* ---------- paths ---------- */

/* Copies a NUL-terminated user path, one page at a time so a path that ends
 * just before an unmapped page is fine. */
static int path_in(Uproc *process, uint32_t address, char *out) {
    uint32_t done = 0;
    while (done < FS_PATH_LEN) {
        uint32_t chunk = 4096u - ((address + done) & 4095u);
        if (chunk > FS_PATH_LEN - done) chunk = FS_PATH_LEN - done;
        int result = uproc_copy_in(process, out + done, address + done, chunk);
        if (result) return result;
        for (uint32_t i = 0; i < chunk; ++i)
            if (!out[done + i]) return out[0] ? 0 : -BOS_ENOENT;
        done += chunk;
    }
    return -BOS_ENAMETOOLONG;
}

static int cwd_of(Uproc *process) {
    UFileTable *table = &process->files;
    if (!fs_is_dir(table->cwd) || fs_identity(table->cwd) != table->cwd_identity) return fs_root();
    return table->cwd;
}

static int resolve(Uproc *process, uint32_t address, int *node) {
    char path[FS_PATH_LEN];
    int result = path_in(process, address, path);
    if (result) return result;
    *node = fs_resolve(cwd_of(process), path);
    return *node < 0 ? -BOS_ENOENT : 0;
}

/* Parent directory and final name of a path that may not exist yet. */
static int destination(Uproc *process, uint32_t address, int *parent, char *name) {
    char path[FS_PATH_LEN];
    int result = path_in(process, address, path);
    if (result) return result;
    unsigned length = (unsigned)kstrlen(path);
    while (length > 1 && path[length - 1] == '/') path[--length] = 0;
    const char *last = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/') last = p + 1;
    if (kstrlen(last) >= FS_NAME_LEN) return -BOS_ENAMETOOLONG;
    *parent = fs_destination(cwd_of(process), path, name);
    if (*parent >= 0) return 0;
    if (!*last || !kstrcmp(last, ".") || !kstrcmp(last, "..")) return -BOS_EEXIST;
    return -BOS_ENOENT;
}

/* ---------- open and close ---------- */

static int load_contents(UFile *file) {
    uint32_t size = (uint32_t)fs_size(file->node);
    uint32_t capacity = size > 4096u ? size : 4096u;
    file->buffer = kmalloc(capacity);
    if (!file->buffer) return -BOS_ENOMEM;
    if (size) kmemcpy(file->buffer, fs_data(file->node), (int)size);
    file->size = size;
    file->capacity = capacity;
    return 0;
}

int ufile_open(Uproc *process, uint32_t path, int flags, int mode) {
    (void)mode;
    int access = flags & BOS_O_ACCMODE;
    if (access == 3) return -BOS_EINVAL;
    int node, result = resolve(process, path, &node);
    if (result == -BOS_ENOENT && (flags & BOS_O_CREAT)) {
        char name[FS_NAME_LEN];
        int parent;
        if ((result = destination(process, path, &parent, name))) return result;
        for (;;) {
            node = fs_create(parent, name);
            if (node != FS_ERR_BUSY) break;
            wait_volume();
        }
        if (node < 0) return fs_node_count() >= fs_node_limit() ? -BOS_ENOSPC : -BOS_EINVAL;
        result = 0;
    } else if (!result && (flags & BOS_O_CREAT) && (flags & BOS_O_EXCL)) {
        return -BOS_EEXIST;
    }
    if (result) return result;
    int directory = fs_is_dir(node);
    if (directory && access != BOS_O_RDONLY) return -BOS_EISDIR;
    if (!directory && (flags & BOS_O_DIRECTORY)) return -BOS_ENOTDIR;
    if (!directory && fs_is_app(node) && access != BOS_O_RDONLY) return -BOS_EACCES;
    UFile *file = new_file(directory ? UFILE_DIRECTORY : UFILE_NODE);
    if (!file) return -BOS_ENOMEM;
    file->flags = flags & (BOS_O_ACCMODE | BOS_O_APPEND);
    file->node = node;
    file->identity = fs_identity(node);
    if (!directory && access != BOS_O_RDONLY) {
        if ((result = load_contents(file))) {
            free_file(file);
            return result;
        }
        if ((flags & BOS_O_TRUNC) && file->size) {
            file->size = 0;
            file->dirty = 1;
        }
    }
    int fd = install(&process->files, file, 0);
    if (fd < 0) release(file, 1);
    return fd;
}

int ufile_close(Uproc *process, int fd) {
    UFile *file = lookup(&process->files, fd);
    if (!file) return -BOS_EBADF;
    process->files.slots[fd] = 0;
    /* Report a failed write-back to the closer, as POSIX close may. */
    int result = file->refs == 1 ? write_back(file, 1) : 0;
    release(file, 1);
    return result;
}

/* ---------- data ---------- */

static int pipe_read(Uproc *process, UPipe *pipe, uint32_t buffer, uint32_t length) {
    while (!pipe->count) {
        if (!pipe->writers) return 0;
        thread_wait((uintptr_t)pipe, 0);
        if (process->state != UPROC_RUNNING) return -BOS_EIO;
    }
    uint32_t done = 0;
    while (done < length && pipe->count) {
        uint32_t run = PIPE_BYTES - pipe->head;
        if (run > pipe->count) run = pipe->count;
        if (run > length - done) run = length - done;
        int result = uproc_copy_out(process, buffer + done, pipe->data + pipe->head, run);
        if (result) return done ? (int)done : result;
        pipe->head = (pipe->head + run) % PIPE_BYTES;
        pipe->count -= run;
        done += run;
    }
    thread_wake_channel((uintptr_t)pipe, ~0u);
    return (int)done;
}

static int pipe_write(Uproc *process, UPipe *pipe, uint32_t buffer, uint32_t length) {
    uint32_t done = 0;
    while (done < length) {
        if (!pipe->readers) return done ? (int)done : -BOS_EPIPE;
        if (pipe->count == PIPE_BYTES) {
            thread_wait((uintptr_t)pipe, 0);
            if (process->state != UPROC_RUNNING) return -BOS_EIO;
            continue;
        }
        uint32_t tail = (pipe->head + pipe->count) % PIPE_BYTES;
        /* Free space runs to the end of the ring, or up to the head. */
        uint32_t run = tail >= pipe->head ? PIPE_BYTES - tail : pipe->head - tail;
        if (run > length - done) run = length - done;
        int result = uproc_copy_in(process, pipe->data + tail, buffer + done, run);
        if (result) return done ? (int)done : result;
        pipe->count += run;
        done += run;
        thread_wake_channel((uintptr_t)pipe, ~0u);
    }
    return (int)done;
}

int ufile_read(Uproc *process, int fd, uint32_t buffer, uint32_t length) {
    UFile *file = lookup(&process->files, fd);
    if (!file || (file->flags & BOS_O_ACCMODE) == BOS_O_WRONLY) return -BOS_EBADF;
    if (!length) return 0;
    switch (file->kind) {
    case UFILE_CONSOLE:
        return 0;                    /* no console input yet: end of file */
    case UFILE_PIPE_READ: {
        /* Another thread may close the descriptor while this one waits. */
        ++file->refs;
        int result = pipe_read(process, file->pipe, buffer, length);
        release(file, 1);
        return result;
    }
    case UFILE_DIRECTORY:
        return -BOS_EISDIR;
    case UFILE_NODE: {
        const char *data;
        uint32_t size;
        if (file->buffer) {
            data = file->buffer;
            size = file->size;
        } else {
            if (!node_current(file)) return -BOS_EIO;
            data = fs_data(file->node);
            size = (uint32_t)fs_size(file->node);
        }
        if (file->offset >= size) return 0;
        if (length > size - file->offset) length = size - file->offset;
        int result = uproc_copy_out(process, buffer, data + file->offset, length);
        if (result) return result;
        file->offset += length;
        return (int)length;
    }
    }
    return -BOS_EBADF;
}

/* Room for size bytes in a writable file's buffer, within the volume's
 * file limit. */
static int reserve(UFile *file, uint32_t size) {
    if (size > fs_file_limit()) return -BOS_EFBIG;
    if (size <= file->capacity) return 0;
    uint32_t capacity = file->capacity;
    while (capacity < size) capacity = capacity > 0x40000000u ? size : capacity * 2;
    if (capacity > fs_file_limit()) capacity = fs_file_limit();
    char *buffer = kmalloc(capacity);
    if (!buffer) return -BOS_ENOMEM;
    if (file->size) kmemcpy(buffer, file->buffer, (int)file->size);
    kfree(file->buffer);
    file->buffer = buffer;
    file->capacity = capacity;
    return 0;
}

int ufile_write(Uproc *process, int fd, uint32_t buffer, uint32_t length) {
    UFile *file = lookup(&process->files, fd);
    if (!file || (file->flags & BOS_O_ACCMODE) == BOS_O_RDONLY) return -BOS_EBADF;
    switch (file->kind) {
    case UFILE_CONSOLE:
        return uproc_console_write(process, buffer, length);
    case UFILE_PIPE_WRITE: {
        ++file->refs;
        int result = pipe_write(process, file->pipe, buffer, length);
        release(file, 1);
        return result;
    }
    case UFILE_NODE: {
        if (!length) return 0;
        if (file->flags & BOS_O_APPEND) file->offset = file->size;
        uint32_t end = file->offset + length;
        if (end < file->offset) return -BOS_EFBIG;
        int result = reserve(file, end);
        if (result) return result;
        if (file->offset > file->size)
            kmemset(file->buffer + file->size, 0, (int)(file->offset - file->size));
        if ((result = uproc_copy_in(process, file->buffer + file->offset, buffer, length))) return result;
        if (end > file->size) file->size = end;
        file->offset = end;
        file->dirty = 1;
        return (int)length;
    }
    }
    return -BOS_EBADF;
}

int ufile_seek(Uproc *process, int fd, uint32_t low, uint32_t high, int whence, uint32_t result) {
    UFile *file = lookup(&process->files, fd);
    if (!file) return -BOS_EBADF;
    if (file->kind == UFILE_CONSOLE || file->kind == UFILE_PIPE_READ || file->kind == UFILE_PIPE_WRITE)
        return -BOS_ESPIPE;
    long long offset = (long long)(((unsigned long long)high << 32) | low);
    long long base = 0;
    if (whence == BOS_SEEK_CUR) base = file->offset;
    else if (whence == BOS_SEEK_END)
        base = file->buffer ? (long long)file->size : (file->kind == UFILE_NODE && node_current(file) ? fs_size(file->node) : 0);
    else if (whence != BOS_SEEK_SET) return -BOS_EINVAL;
    long long position = base + offset;
    if (position < 0) return -BOS_EINVAL;
    if (position > 0xFFFFFFFFLL) return -BOS_EFBIG;
    file->offset = (uint32_t)position;
    if (!result) return 0;
    return uproc_copy_out(process, result, &position, sizeof position);
}

int ufile_truncate(Uproc *process, int fd, uint32_t length) {
    UFile *file = lookup(&process->files, fd);
    if (!file || file->kind != UFILE_NODE || !file->buffer) return -BOS_EINVAL;
    int result = reserve(file, length);
    if (result) return result;
    if (length > file->size) kmemset(file->buffer + file->size, 0, (int)(length - file->size));
    file->size = length;
    file->dirty = 1;
    return 0;
}

int ufile_fsync(Uproc *process, int fd) {
    UFile *file = lookup(&process->files, fd);
    if (!file) return -BOS_EBADF;
    if (file->kind != UFILE_NODE) return 0;
    ++file->refs;
    int result = write_back(file, 1);
    release(file, 1);
    return result;
}

/* ---------- metadata ---------- */

static void stat_node(int node, BosStat *out) {
    kmemset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->mode = fs_is_dir(node) ? BOS_S_IFDIR | 0755 : BOS_S_IFREG | (fs_is_app(node) ? 0555 : 0644);
    out->node = fs_identity(node) ? fs_identity(node) : (unsigned)node + 1;
    out->links = 1;
    out->size = fs_is_dir(node) ? 0 : fs_size(node);
    unsigned modified = fs_modified(node);
    out->modified = modified ? VOLUME_EPOCH + modified : 0;
}

int ufile_stat(Uproc *process, uint32_t path, uint32_t out) {
    int node, result = resolve(process, path, &node);
    if (result) return result;
    BosStat stat;
    stat_node(node, &stat);
    return uproc_copy_out(process, out, &stat, sizeof stat);
}

int ufile_fstat(Uproc *process, int fd, uint32_t out) {
    UFile *file = lookup(&process->files, fd);
    if (!file) return -BOS_EBADF;
    BosStat stat;
    if (file->kind == UFILE_NODE || file->kind == UFILE_DIRECTORY) {
        if (!node_current(file)) return -BOS_EIO;
        stat_node(file->node, &stat);
        if (file->buffer) stat.size = file->size;
    } else {
        kmemset(&stat, 0, sizeof stat);
        stat.struct_size = sizeof stat;
        stat.mode = file->kind == UFILE_CONSOLE ? BOS_S_IFCHR | 0620 : BOS_S_IFIFO | 0600;
        stat.links = 1;
    }
    return uproc_copy_out(process, out, &stat, sizeof stat);
}

int ufile_isatty(Uproc *process, int fd) {
    UFile *file = lookup(&process->files, fd);
    if (!file) return -BOS_EBADF;
    return file->kind == UFILE_CONSOLE ? 0 : -BOS_ENOTTY;
}

/* Records for ".", ".." and then the children; the offset counts records. */
int ufile_readdir(Uproc *process, int fd, uint32_t buffer, uint32_t length) {
    UFile *file = lookup(&process->files, fd);
    if (!file) return -BOS_EBADF;
    if (file->kind != UFILE_DIRECTORY) return -BOS_ENOTDIR;
    if (!node_current(file)) return -BOS_EIO;
    int limit = fs_node_limit();
    int *children = kmalloc((uint32_t)limit * sizeof *children);
    if (!children) return -BOS_ENOMEM;
    int count = fs_list(file->node, children, limit);
    uint32_t done = 0;
    int result = 0;
    while (file->offset < (uint32_t)count + 2) {
        unsigned index = file->offset;
        int node = index == 0 ? file->node : index == 1 ? (file->node ? fs_parent(file->node) : 0) : children[index - 2];
        const char *name = index == 0 ? "." : index == 1 ? ".." : fs_name(node);
        unsigned name_length = (unsigned)kstrlen(name);
        unsigned record = (8u + name_length + 1u + 3u) & ~3u;
        if (done + record > length) {
            if (!done) result = -BOS_EINVAL;
            break;
        }
        char entry[8 + FS_NAME_LEN + 4];
        kmemset(entry, 0, (int)record);
        BosDirent *dirent = (BosDirent *)entry;
        dirent->node = fs_identity(node) ? fs_identity(node) : (unsigned)node + 1;
        dirent->length = (unsigned short)record;
        dirent->type = fs_is_dir(node) ? BOS_DT_DIR : BOS_DT_REG;
        kmemcpy(dirent->name, name, (int)name_length + 1);
        if ((result = uproc_copy_out(process, buffer + done, entry, record))) break;
        done += record;
        ++file->offset;
    }
    kfree(children);
    return done ? (int)done : result;
}

/* ---------- names ---------- */

/* Runs a volume mutation, waiting while a snapshot owns the volume. */
#define MUTATE(call) ({ int mutate_result_; \
    for (;;) { mutate_result_ = (call); if (mutate_result_ != FS_ERR_BUSY) break; wait_volume(); } \
    mutate_result_; })

int ufile_mkdir(Uproc *process, uint32_t path) {
    int parent;
    char name[FS_NAME_LEN];
    int result = destination(process, path, &parent, name);
    if (result) return result;
    if (fs_find_child(parent, name) >= 0) return -BOS_EEXIST;
    int node = MUTATE(fs_mkdir(parent, name));
    if (node < 0) return fs_node_count() >= fs_node_limit() ? -BOS_ENOSPC : -BOS_EINVAL;
    return 0;
}

int ufile_unlink(Uproc *process, uint32_t path, int flags) {
    int node, result = resolve(process, path, &node);
    if (result) return result;
    if (!node) return -BOS_EBUSY;
    if (flags & BOS_AT_REMOVEDIR) {
        if (!fs_is_dir(node)) return -BOS_ENOTDIR;
        if (fs_child_count(node)) return -BOS_ENOTEMPTY;
    } else if (fs_is_dir(node)) {
        return -BOS_EISDIR;
    }
    return MUTATE(fs_delete(node)) < 0 ? -BOS_EIO : 0;
}

int ufile_rename(Uproc *process, uint32_t from, uint32_t to) {
    int node, result = resolve(process, from, &node);
    if (result) return result;
    if (!node) return -BOS_EBUSY;
    int parent;
    char name[FS_NAME_LEN];
    if ((result = destination(process, to, &parent, name))) return result;
    for (int walk = parent; walk > 0; walk = fs_parent(walk))
        if (walk == node) return -BOS_EINVAL;
    int existing = fs_find_child(parent, name);
    if (existing == node) return 0;
    if (existing >= 0) {
        if (fs_is_dir(node) != fs_is_dir(existing)) return fs_is_dir(existing) ? -BOS_EISDIR : -BOS_ENOTDIR;
        if (fs_is_dir(existing) && fs_child_count(existing)) return -BOS_ENOTEMPTY;
    }
    /* Check the whole placement before the destination is deleted. */
    if (!fs_can_place(node, parent, name)) return -BOS_EINVAL;
    if (existing >= 0 && MUTATE(fs_delete(existing)) < 0) return -BOS_EIO;
    return MUTATE(fs_place(node, parent, name)) < 0 ? -BOS_EIO : 0;
}

int ufile_chdir(Uproc *process, uint32_t path) {
    int node, result = resolve(process, path, &node);
    if (result) return result;
    if (!fs_is_dir(node)) return -BOS_ENOTDIR;
    process->files.cwd = node;
    process->files.cwd_identity = fs_identity(node);
    return 0;
}

int ufile_getcwd(Uproc *process, uint32_t buffer, uint32_t length) {
    char path[FS_PATH_LEN];
    fs_path(cwd_of(process), path, sizeof path);
    uint32_t size = (uint32_t)kstrlen(path) + 1;
    if (size > length) return -BOS_ERANGE;
    int result = uproc_copy_out(process, buffer, path, size);
    return result ? result : (int)size;
}

/* ---------- descriptors ---------- */

int ufile_dup(Uproc *process, int fd) {
    UFile *file = lookup(&process->files, fd);
    if (!file) return -BOS_EBADF;
    int copy = install(&process->files, file, 0);
    if (copy >= 0) ++file->refs;
    return copy;
}

int ufile_dup2(Uproc *process, int fd, int target) {
    UFile *file = lookup(&process->files, fd);
    if (!file || target < 0 || (unsigned)target >= UFILE_MAX_DESCRIPTORS) return -BOS_EBADF;
    if (fd == target) return target;
    int result = grow(&process->files, (unsigned)target + 1);
    if (result) return result;
    UFile *previous = process->files.slots[target];
    ++file->refs;
    process->files.slots[target] = file;
    release(previous, 1);
    return target;
}

int ufile_pipe(Uproc *process, uint32_t fds) {
    UPipe *pipe = kmalloc(sizeof *pipe);
    UFile *reader = new_file(UFILE_PIPE_READ), *writer = new_file(UFILE_PIPE_WRITE);
    if (!pipe || !reader || !writer) {
        kfree(pipe);
        kfree(reader);
        kfree(writer);
        return -BOS_ENOMEM;
    }
    kmemset(pipe, 0, sizeof *pipe);
    pipe->readers = pipe->writers = 1;
    reader->pipe = writer->pipe = pipe;
    reader->flags = BOS_O_RDONLY;
    writer->flags = BOS_O_WRONLY;
    int ends[2];
    ends[0] = install(&process->files, reader, 0);
    ends[1] = ends[0] < 0 ? ends[0] : install(&process->files, writer, 0);
    int result = ends[1] < 0 ? ends[1] : uproc_copy_out(process, fds, ends, sizeof ends);
    if (result) {
        if (ends[0] >= 0) process->files.slots[ends[0]] = 0;
        if (ends[1] >= 0) process->files.slots[ends[1]] = 0;
        release(reader, 1);
        release(writer, 1);
    }
    return result;
}
