#include "kernel/ufile.h"
#include "kernel/uproc.h"
#include "kernel/kheap.h"
#include "kernel/fs.h"
#include "kernel/fatvol.h"
#include "kernel/platform.h"
#include "../sdk/baseos_sys.h"

enum { UFILE_CONSOLE = 1, UFILE_NODE, UFILE_DIRECTORY, UFILE_PIPE_READ, UFILE_PIPE_WRITE,
       UFILE_FAT, UFILE_FAT_DIRECTORY };

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
    /* Writable node past the volume's file limit: no buffer, reads and
     * writes go to the volume as they happen. */
    int direct;
    UPipe *pipe;
    /* FAT volume file or directory. */
    int volume;
    struct FatOpen *fat;       /* files: the node, shared by every open of it */
    FatNode *fat_directory;    /* directories: this open's own copy */
    uint32_t fat_cursor;       /* directories: where readdir goes on */
    struct UFile *next_orphan;
} UFile;

/* One FatNode per open FAT file, so every descriptor sees one size and
 * chain, and renames through a path keep open files current. */
typedef struct FatOpen {
    unsigned refs;
    int volume;
    FatNode node;
    struct FatOpen *next;
} FatOpen;
static FatOpen *fat_opens;

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
    if (!file->dirty || file->direct) return 0;
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

/* Volume access in pieces through a kernel bounce buffer, for files that
 * never load whole. A mutation refused while a save runs is retried. */
#define DIRECT_PIECE 65536u
static int direct_read(Uproc *process, UFile *file, uint32_t buffer, uint32_t length) {
    char *bounce = kmalloc(length < DIRECT_PIECE ? length : DIRECT_PIECE);
    if (!bounce) return -BOS_ENOMEM;
    uint32_t done = 0;
    int result = 0;
    while (done < length) {
        uint32_t n = length - done < DIRECT_PIECE ? length - done : DIRECT_PIECE;
        int got = fs_read_at(file->node, file->offset, bounce, n);
        if (got < 0) { result = -BOS_EIO; break; }
        if (got && (result = uproc_copy_out(process, buffer + done, bounce, (uint32_t)got))) break;
        file->offset += (uint32_t)got;
        done += (uint32_t)got;
        if ((uint32_t)got < n) break;
    }
    kfree(bounce);
    return done ? (int)done : result;
}
static int direct_result(int result, uint32_t end) {
    if (result >= 0) return 0;
    return end > fs_file_limit() && !fs_streams() ? -BOS_EFBIG : -BOS_ENOSPC;
}
static int direct_write(Uproc *process, UFile *file, uint32_t buffer, uint32_t length) {
    char *bounce = kmalloc(length < DIRECT_PIECE ? length : DIRECT_PIECE);
    if (!bounce) return -BOS_ENOMEM;
    uint32_t done = 0;
    int result = 0;
    while (done < length) {
        uint32_t n = length - done < DIRECT_PIECE ? length - done : DIRECT_PIECE;
        if ((result = uproc_copy_in(process, bounce, buffer + done, n))) break;
        if (!node_current(file)) { result = -BOS_EIO; break; }
        int written = fs_write_at(file->node, file->offset, bounce, n);
        if (written == FS_ERR_BUSY) { wait_volume(); continue; }
        if ((result = direct_result(written, file->offset + n))) break;
        file->offset += n;
        done += n;
    }
    kfree(bounce);
    return done ? (int)done : result;
}
static int direct_truncate(UFile *file, uint32_t length) {
    for (;;) {
        if (!node_current(file)) return -BOS_EIO;
        int result = fs_truncate(file->node, length);
        if (result != FS_ERR_BUSY) return direct_result(result, length);
        wait_volume();
    }
}
/* A buffered file about to pass the volume's file limit goes direct. */
static int go_direct(UFile *file) {
    if (file->direct) return 0;
    if (!fs_streams()) return -BOS_EFBIG;
    int result = write_back(file, 1);
    if (result) return result;
    kfree(file->buffer);
    file->buffer = 0;
    file->size = file->capacity = 0;
    file->direct = 1;
    return 0;
}

static FatOpen *fat_open_of(int volume, const FatNode *node) {
    for (FatOpen *open = fat_opens; open; open = open->next)
        if (open->volume == volume && open->node.parent == node->parent && open->node.slot == node->slot)
            return open;
    return 0;
}
static FatOpen *fat_hold(int volume, const FatNode *node) {
    FatOpen *open = fat_open_of(volume, node);
    if (open) { ++open->refs; return open; }
    if (!(open = kmalloc(sizeof *open))) return 0;
    open->refs = 1;
    open->volume = volume;
    open->node = *node;
    open->next = fat_opens;
    fat_opens = open;
    return open;
}
static void fat_drop(FatOpen *open) {
    if (!open || --open->refs) return;
    for (FatOpen **link = &fat_opens; *link; link = &(*link)->next)
        if (*link == open) { *link = open->next; break; }
    kfree(open);
}

static void free_file(UFile *file) {
    fat_drop(file->fat);
    kfree(file->fat_directory);
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
    kfree(table->cwd_path);
    table->cwd_path = 0;
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

static int resolve(Uproc *process, const char *path, int *node) {
    *node = fs_resolve(cwd_of(process), path);
    return *node < 0 ? -BOS_ENOENT : 0;
}

/* Parent directory and final name of a path that may not exist yet. The
 * path may lose trailing slashes. */
static int destination(Uproc *process, char *path, int *parent, char *name) {
    unsigned length = (unsigned)kstrlen(path);
    while (length > 1 && path[length - 1] == '/') path[--length] = 0;
    const char *last = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/') last = p + 1;
    if (kstrlen(last) > fs_name_max()) return -BOS_ENAMETOOLONG;
    *parent = fs_destination(cwd_of(process), path, name);
    if (*parent >= 0) return 0;
    if (!*last || !kstrcmp(last, ".") || !kstrcmp(last, "..")) return -BOS_EEXIST;
    return -BOS_ENOENT;
}

/* ---------- FAT volumes ---------- */

static int fat_errno(int result) {
    switch (result) {
    case FAT_ERR_NOSPACE: return -BOS_ENOSPC;
    case FAT_ERR_NOMEM: return -BOS_ENOMEM;
    case FAT_ERR_INVALID: return -BOS_EINVAL;
    case FAT_ERR_NOTFOUND: return -BOS_ENOENT;
    case FAT_ERR_EXISTS: return -BOS_EEXIST;
    case FAT_ERR_NOTDIR: return -BOS_ENOTDIR;
    case FAT_ERR_ISDIR: return -BOS_EISDIR;
    case FAT_ERR_NOTEMPTY: return -BOS_ENOTEMPTY;
    case FAT_ERR_NAMETOOLONG: return -BOS_ENAMETOOLONG;
    case FAT_ERR_FBIG: return -BOS_EFBIG;
    case FAT_ERR_READONLY: return -BOS_EROFS;
    default: return result < 0 ? -BOS_EIO : result;
    }
}

/* Copies a user path and finds the volume it is on. *volume is -1 for the
 * native volume: path then works with resolve and destination. Otherwise
 * path is absolute and normalized and *rest is the part within the FAT
 * volume ("" for its root). */
static int locate(Uproc *process, uint32_t address, char *path, int *volume, const char **rest) {
    int result = path_in(process, address, path);
    if (result) return result;
    const char *cwd = process->files.cwd_path;
    int any = 0;
    for (int i = 0; i < FATVOL_MAX; ++i) any |= fatvol_get(i) != 0;
    if (!cwd && !any) { *volume = -1; return 0; }
    char *joined = kmalloc(2u * FS_PATH_LEN);
    if (!joined) return -BOS_ENOMEM;
    unsigned length = 0;
    if (*path != '/') {
        if (cwd) {
            while (cwd[length]) { joined[length] = cwd[length]; ++length; }
        } else {
            fs_path(cwd_of(process), joined, FS_PATH_LEN);
            length = (unsigned)kstrlen(joined);
        }
        joined[length++] = '/';
    }
    for (unsigned i = 0; path[i]; ++i) joined[length++] = path[i];
    joined[length] = 0;
    /* "." and ".." are resolved here, by name. */
    char *out = joined + FS_PATH_LEN;
    unsigned size = 0;
    for (const char *p = joined; *p;) {
        while (*p == '/') ++p;
        if (!*p) break;
        const char *part = p;
        while (*p && *p != '/') ++p;
        unsigned n = (unsigned)(p - part);
        if (n == 1 && part[0] == '.') continue;
        if (n == 2 && part[0] == '.' && part[1] == '.') {
            while (size && out[size - 1] != '/') --size;
            if (size) --size;
            continue;
        }
        if (size + 1 + n >= FS_PATH_LEN) { kfree(joined); return -BOS_ENAMETOOLONG; }
        out[size++] = '/';
        kmemcpy(out + size, part, (int)n);
        size += n;
    }
    if (!size) out[size++] = '/';
    out[size] = 0;
    const char *within;
    *volume = fatvol_route(out, &within);
    if (*volume >= 0 || cwd) {
        kmemcpy(path, out, (int)size + 1);
        if (*volume >= 0) *rest = path + (within - out);
    }
    kfree(joined);
    return 0;
}

/* Parent directory node and final name of a path within a FAT volume. */
static int fat_parent(FatVolume *v, const char *rest, FatNode *parent, const char **name) {
    const char *slash = rest;
    for (const char *p = rest; *p; ++p)
        if (*p == '/') slash = p;
    if (!*rest || !slash[1]) return -BOS_EEXIST;      /* the volume's root */
    *name = slash + 1;
    char *head = kmalloc((uint32_t)(slash - rest) + 1);
    if (!head) return -BOS_ENOMEM;
    kmemcpy(head, rest, (int)(slash - rest));
    head[slash - rest] = 0;
    int result = fat_resolve(v, head, parent);
    kfree(head);
    if (!result && !(parent->attributes & FAT_ATTR_DIRECTORY)) result = FAT_ERR_NOTDIR;
    return fat_errno(result);
}

static void stat_fat(int volume, const FatNode *node, BosStat *out) {
    kmemset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    int directory = (node->attributes & FAT_ATTR_DIRECTORY) != 0;
    out->mode = directory ? BOS_S_IFDIR | 0755
              : BOS_S_IFREG | (node->attributes & FAT_ATTR_READ_ONLY ? 0444 : 0644);
    out->node = node->is_root ? 0xF0000000u + (unsigned)volume
              : 0x80000000u ^ ((unsigned)volume << 28) ^ (node->parent << 12) ^ node->slot;
    out->links = 1;
    out->size = directory ? 0 : node->size;
    out->modified = node->modified ? VOLUME_EPOCH + node->modified : 0;
}

/* Reads into user memory through a kernel bounce buffer. */
static int fat_read_user(Uproc *process, UFile *file, uint32_t buffer, uint32_t length) {
    FatVolume *v = fatvol_get(file->volume);
    if (!v) return -BOS_EIO;
    FatNode *node = &file->fat->node;
    if (file->offset >= node->size) return 0;
    if (length > node->size - file->offset) length = node->size - file->offset;
    char *bounce = kmalloc(length < DIRECT_PIECE ? length : DIRECT_PIECE);
    if (!bounce) return -BOS_ENOMEM;
    uint32_t done = 0;
    int result = 0;
    while (done < length) {
        uint32_t n = length - done < DIRECT_PIECE ? length - done : DIRECT_PIECE;
        int got = fat_read(v, node, file->offset, bounce, n);
        if (got < 0) { result = fat_errno(got); break; }
        if (got && (result = uproc_copy_out(process, buffer + done, bounce, (uint32_t)got))) break;
        file->offset += (uint32_t)got;
        done += (uint32_t)got;
        if ((uint32_t)got < n) break;
    }
    kfree(bounce);
    return done ? (int)done : result;
}

static int fat_write_user(Uproc *process, UFile *file, uint32_t buffer, uint32_t length) {
    FatVolume *v = fatvol_get(file->volume);
    if (!v) return -BOS_EIO;
    FatNode *node = &file->fat->node;
    if (!length) return 0;
    if (file->flags & BOS_O_APPEND) file->offset = node->size;
    if (file->offset + length < file->offset) return -BOS_EFBIG;
    char *bounce = kmalloc(length < DIRECT_PIECE ? length : DIRECT_PIECE);
    if (!bounce) return -BOS_ENOMEM;
    uint32_t done = 0;
    int result = 0;
    while (done < length) {
        uint32_t n = length - done < DIRECT_PIECE ? length - done : DIRECT_PIECE;
        if ((result = uproc_copy_in(process, bounce, buffer + done, n))) break;
        if ((result = fat_errno(fat_write(v, node, file->offset, bounce, n)))) break;
        file->offset += n;
        done += n;
    }
    kfree(bounce);
    fatvol_changed(file->volume);
    return done ? (int)done : result;
}

static int fat_open_path(Uproc *process, int volume, const char *rest, int flags) {
    FatVolume *v = fatvol_get(volume);
    int access = flags & BOS_O_ACCMODE;
    FatNode *node = kmalloc(sizeof *node);
    if (!node) return -BOS_ENOMEM;
    int result = fat_resolve(v, rest, node);
    if (result == FAT_ERR_NOTFOUND && (flags & BOS_O_CREAT)) {
        FatNode *parent = kmalloc(sizeof *parent);
        const char *name;
        result = parent ? fat_parent(v, rest, parent, &name) : -BOS_ENOMEM;
        if (!result) result = fat_errno(fat_create(v, parent, name, 0, node));
        kfree(parent);
        if (!result) fatvol_changed(volume);
    } else if (!result && (flags & BOS_O_CREAT) && (flags & BOS_O_EXCL)) {
        result = -BOS_EEXIST;
    } else {
        result = fat_errno(result);
    }
    int directory = !result && (node->attributes & FAT_ATTR_DIRECTORY);
    if (!result && directory && access != BOS_O_RDONLY) result = -BOS_EISDIR;
    if (!result && !directory && (flags & BOS_O_DIRECTORY)) result = -BOS_ENOTDIR;
    UFile *file = result ? 0 : new_file(directory ? UFILE_FAT_DIRECTORY : UFILE_FAT);
    if (!result && !file) result = -BOS_ENOMEM;
    if (result) { kfree(node); return result; }
    file->flags = flags & (BOS_O_ACCMODE | BOS_O_APPEND);
    file->volume = volume;
    if (directory) {
        file->fat_directory = node;
    } else {
        file->fat = fat_hold(volume, node);
        kfree(node);
        if (!file->fat) { free_file(file); return -BOS_ENOMEM; }
        if ((flags & BOS_O_TRUNC) && access != BOS_O_RDONLY && file->fat->node.size) {
            if ((result = fat_errno(fat_truncate(v, &file->fat->node, 0)))) { free_file(file); return result; }
            fatvol_changed(volume);
        }
    }
    int fd = install(&process->files, file, 0);
    if (fd < 0) release(file, 1);
    return fd;
}

/* Records for ".", ".." and then the directory's entries. */
static int fat_readdir_user(Uproc *process, UFile *file, uint32_t buffer, uint32_t length) {
    FatVolume *v = fatvol_get(file->volume);
    if (!v) return -BOS_EIO;
    FatNode *entry = kmalloc(sizeof *entry);
    if (!entry) return -BOS_ENOMEM;
    uint32_t done = 0;
    int result = 0;
    for (;;) {
        const char *name;
        uint32_t cursor = file->fat_cursor;
        int type = BOS_DT_DIR;
        unsigned node = 0;
        if (file->offset == 0) name = ".";
        else if (file->offset == 1) name = "..";
        else {
            int found = fat_next(v, file->fat_directory, &cursor, entry);
            if (found <= 0) { result = fat_errno(found); break; }
            name = entry->name;
            BosStat stat;
            stat_fat(file->volume, entry, &stat);
            node = stat.node;
            type = entry->attributes & FAT_ATTR_DIRECTORY ? BOS_DT_DIR : BOS_DT_REG;
        }
        unsigned name_length = (unsigned)kstrlen(name);
        unsigned record = (8u + name_length + 1u + 3u) & ~3u;
        if (done + record > length) {
            if (!done) result = -BOS_EINVAL;
            break;
        }
        char *bytes = kmalloc(record);
        if (!bytes) { result = -BOS_ENOMEM; break; }
        kmemset(bytes, 0, (int)record);
        BosDirent *dirent = (BosDirent *)bytes;
        dirent->node = node ? node : 0xF0000000u + (unsigned)file->volume + file->offset;
        dirent->length = (unsigned short)record;
        dirent->type = (unsigned char)type;
        kmemcpy(dirent->name, name, (int)name_length + 1);
        result = uproc_copy_out(process, buffer + done, bytes, record);
        kfree(bytes);
        if (result) break;
        done += record;
        file->fat_cursor = cursor;
        ++file->offset;
    }
    kfree(entry);
    return done ? (int)done : result;
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
    char text[FS_PATH_LEN];
    const char *rest;
    int volume, node, result = locate(process, path, text, &volume, &rest);
    if (result) return result;
    if (volume >= 0) return fat_open_path(process, volume, rest, flags);
    result = resolve(process, text, &node);
    if (result == -BOS_ENOENT && (flags & BOS_O_CREAT)) {
        char name[FS_NAME_LEN];
        int parent;
        if ((result = destination(process, text, &parent, name))) return result;
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
    if (!directory && access != BOS_O_RDONLY && fs_streamed(node)) {
        file->direct = 1;
        if ((flags & BOS_O_TRUNC) && (result = direct_truncate(file, 0))) {
            free_file(file);
            return result;
        }
    } else if (!directory && access != BOS_O_RDONLY) {
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
    case UFILE_FAT_DIRECTORY:
        return -BOS_EISDIR;
    case UFILE_FAT:
        return fat_read_user(process, file, buffer, length);
    case UFILE_NODE: {
        const char *data;
        uint32_t size;
        if (file->buffer) {
            data = file->buffer;
            size = file->size;
        } else {
            if (!node_current(file)) return -BOS_EIO;
            if (fs_streamed(file->node)) return direct_read(process, file, buffer, length);
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
    case UFILE_FAT:
        return fat_write_user(process, file, buffer, length);
    case UFILE_NODE: {
        if (!length) return 0;
        if (file->flags & BOS_O_APPEND)
            file->offset = file->direct ? (node_current(file) ? (uint32_t)fs_size(file->node) : 0) : file->size;
        uint32_t end = file->offset + length;
        if (end < file->offset || end > FS_STREAM_MAX) return -BOS_EFBIG;
        int result = end > fs_file_limit() ? go_direct(file) : 0;
        if (result) return result;
        if (file->direct) return direct_write(process, file, buffer, length);
        result = reserve(file, end);
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
        base = file->kind == UFILE_FAT ? (long long)file->fat->node.size
             : file->buffer && !file->direct ? (long long)file->size
             : (file->kind == UFILE_NODE && node_current(file) ? fs_size(file->node) : 0);
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
    if (file && file->kind == UFILE_FAT && (file->flags & BOS_O_ACCMODE) != BOS_O_RDONLY) {
        FatVolume *v = fatvol_get(file->volume);
        int result = v ? fat_errno(fat_truncate(v, &file->fat->node, length)) : -BOS_EIO;
        fatvol_changed(file->volume);
        return result;
    }
    if (!file || file->kind != UFILE_NODE || (!file->buffer && !file->direct)) return -BOS_EINVAL;
    if (length > FS_STREAM_MAX) return -BOS_EFBIG;
    int result = length > fs_file_limit() ? go_direct(file) : 0;
    if (result) return result;
    if (file->direct) return direct_truncate(file, length);
    result = reserve(file, length);
    if (result) return result;
    if (length > file->size) kmemset(file->buffer + file->size, 0, (int)(length - file->size));
    file->size = length;
    file->dirty = 1;
    return 0;
}

int ufile_fsync(Uproc *process, int fd) {
    UFile *file = lookup(&process->files, fd);
    if (!file) return -BOS_EBADF;
    if (file->kind == UFILE_FAT) return fat_errno(fatvol_sync(file->volume));
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
    char text[FS_PATH_LEN];
    const char *rest;
    int volume, node, result = locate(process, path, text, &volume, &rest);
    if (result) return result;
    BosStat stat;
    if (volume >= 0) {
        FatNode *fat = kmalloc(sizeof *fat);
        if (!fat) return -BOS_ENOMEM;
        result = fat_errno(fat_resolve(fatvol_get(volume), rest, fat));
        if (!result) {
            FatOpen *open = fat_open_of(volume, fat);
            stat_fat(volume, open ? &open->node : fat, &stat);
        }
        kfree(fat);
        return result ? result : uproc_copy_out(process, out, &stat, sizeof stat);
    }
    if ((result = resolve(process, text, &node))) return result;
    stat_node(node, &stat);
    return uproc_copy_out(process, out, &stat, sizeof stat);
}

int ufile_fstat(Uproc *process, int fd, uint32_t out) {
    UFile *file = lookup(&process->files, fd);
    if (!file) return -BOS_EBADF;
    BosStat stat;
    if (file->kind == UFILE_FAT || file->kind == UFILE_FAT_DIRECTORY) {
        stat_fat(file->volume, file->kind == UFILE_FAT ? &file->fat->node : file->fat_directory, &stat);
    } else if (file->kind == UFILE_NODE || file->kind == UFILE_DIRECTORY) {
        if (!node_current(file)) return -BOS_EIO;
        stat_node(file->node, &stat);
        if (file->buffer && !file->direct) stat.size = file->size;
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
    if (file->kind == UFILE_FAT_DIRECTORY) return fat_readdir_user(process, file, buffer, length);
    if (file->kind != UFILE_DIRECTORY) return -BOS_ENOTDIR;
    if (!node_current(file)) return -BOS_EIO;
    int limit = fs_child_count(file->node);
    int *children = kmalloc((uint32_t)(limit ? limit : 1) * sizeof *children);
    if (!children) return -BOS_ENOMEM;
    int count = fs_list(file->node, children, limit);
    /* The root also lists the mounted FAT volumes, which hide native
     * folders of the same name. */
    int mounts[FATVOL_MAX], mounted = 0;
    if (file->node == fs_root()) {
        for (int i = 0; i < FATVOL_MAX; ++i)
            if (fatvol_get(i)) mounts[mounted++] = i;
        int kept = 0;
        for (int i = 0; i < count; ++i) {
            const char *rest;
            char path[FS_NAME_LEN + 2];
            path[0] = '/';
            kmemcpy(path + 1, fs_name(children[i]), kstrlen(fs_name(children[i])) + 1);
            if (fatvol_route(path, &rest) < 0) children[kept++] = children[i];
        }
        count = kept;
    }
    uint32_t done = 0;
    int result = 0;
    while (file->offset < (uint32_t)count + 2 + (uint32_t)mounted) {
        unsigned index = file->offset;
        if (index >= (unsigned)count + 2) {
            int volume = mounts[index - (unsigned)count - 2];
            const char *name = fatvol_name(volume);
            unsigned name_length = (unsigned)kstrlen(name);
            unsigned record = (8u + name_length + 1u + 3u) & ~3u;
            if (done + record > length) {
                if (!done) result = -BOS_EINVAL;
                break;
            }
            char entry[32];
            kmemset(entry, 0, (int)record);
            BosDirent *dirent = (BosDirent *)entry;
            dirent->node = 0xF0000000u + (unsigned)volume;
            dirent->length = (unsigned short)record;
            dirent->type = BOS_DT_DIR;
            kmemcpy(dirent->name, name, (int)name_length + 1);
            if ((result = uproc_copy_out(process, buffer + done, entry, record))) break;
            done += record;
            ++file->offset;
            continue;
        }
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
    char text[FS_PATH_LEN];
    const char *rest;
    int volume, result = locate(process, path, text, &volume, &rest);
    if (result) return result;
    if (volume >= 0) {
        FatVolume *v = fatvol_get(volume);
        FatNode *nodes = kmalloc(2 * sizeof *nodes);
        const char *name;
        if (!nodes) return -BOS_ENOMEM;
        if (!(result = fat_parent(v, rest, &nodes[0], &name)))
            result = fat_errno(fat_create(v, &nodes[0], name, 1, &nodes[1]));
        kfree(nodes);
        if (!result) fatvol_changed(volume);
        return result;
    }
    int parent;
    char name[FS_NAME_LEN];
    if ((result = destination(process, text, &parent, name))) return result;
    if (fs_find_child(parent, name) >= 0) return -BOS_EEXIST;
    int node = MUTATE(fs_mkdir(parent, name));
    if (node < 0) return fs_node_count() >= fs_node_limit() ? -BOS_ENOSPC : -BOS_EINVAL;
    return 0;
}

/* Removes a FAT file or empty directory; an open file stays (EBUSY), as on
 * other systems that use FAT. */
static int fat_unlink(int volume, const FatNode *node, int flags) {
    if (node->is_root) return -BOS_EBUSY;
    int directory = (node->attributes & FAT_ATTR_DIRECTORY) != 0;
    if ((flags & BOS_AT_REMOVEDIR) && !directory) return -BOS_ENOTDIR;
    if (!(flags & BOS_AT_REMOVEDIR) && directory) return -BOS_EISDIR;
    if (fat_open_of(volume, node)) return -BOS_EBUSY;
    int result = fat_errno(fat_remove(fatvol_get(volume), node));
    fatvol_changed(volume);
    return result;
}

int ufile_unlink(Uproc *process, uint32_t path, int flags) {
    char text[FS_PATH_LEN];
    const char *rest;
    int volume, node, result = locate(process, path, text, &volume, &rest);
    if (result) return result;
    if (volume >= 0) {
        FatNode *fat = kmalloc(sizeof *fat);
        if (!fat) return -BOS_ENOMEM;
        result = fat_errno(fat_resolve(fatvol_get(volume), rest, fat));
        if (!result) result = fat_unlink(volume, fat, flags);
        kfree(fat);
        return result;
    }
    if ((result = resolve(process, text, &node))) return result;
    if (!node) return -BOS_EBUSY;
    if (flags & BOS_AT_REMOVEDIR) {
        if (!fs_is_dir(node)) return -BOS_ENOTDIR;
        if (fs_child_count(node)) return -BOS_ENOTEMPTY;
    } else if (fs_is_dir(node)) {
        return -BOS_EISDIR;
    }
    return MUTATE(fs_delete(node)) < 0 ? -BOS_EIO : 0;
}

/* Rename within one FAT volume, replacing a file (or empty directory) at
 * the target as POSIX rename does. */
static int fat_move(int volume, const char *from, const char *to) {
    FatVolume *v = fatvol_get(volume);
    FatNode *n = kmalloc(3 * sizeof *n);
    if (!n) return -BOS_ENOMEM;
    FatNode *node = &n[0], *parent = &n[1], *existing = &n[2];
    const char *name;
    int result = fat_errno(fat_resolve(v, from, node));
    if (!result && node->is_root) result = -BOS_EBUSY;
    if (!result) result = fat_parent(v, to, parent, &name);
    if (!result) {
        result = fat_lookup(v, parent, name, existing);
        if (!result && !(existing->parent == node->parent && existing->slot == node->slot)) {
            int dir = (node->attributes & FAT_ATTR_DIRECTORY) != 0;
            int existing_dir = (existing->attributes & FAT_ATTR_DIRECTORY) != 0;
            if (dir != existing_dir) result = existing_dir ? -BOS_EISDIR : -BOS_ENOTDIR;
            else result = fat_unlink(volume, existing, dir ? BOS_AT_REMOVEDIR : 0);
            /* fat_remove of a non-empty directory reports it. */
        } else {
            result = result == FAT_ERR_NOTFOUND ? 0 : fat_errno(result);
        }
    }
    if (!result) {
        FatOpen *open = fat_open_of(volume, node);
        result = fat_errno(fat_rename(v, open ? &open->node : node, parent, name));
        fatvol_changed(volume);
    }
    kfree(n);
    return result;
}

int ufile_rename(Uproc *process, uint32_t from, uint32_t to) {
    char text[FS_PATH_LEN], target[FS_PATH_LEN];
    const char *rest, *target_rest;
    int volume, target_volume, node, result = locate(process, from, text, &volume, &rest);
    if (result) return result;
    if ((result = locate(process, to, target, &target_volume, &target_rest))) return result;
    if (volume != target_volume) return -BOS_EXDEV;
    if (volume >= 0) return fat_move(volume, rest, target_rest);
    if ((result = resolve(process, text, &node))) return result;
    if (!node) return -BOS_EBUSY;
    int parent;
    char name[FS_NAME_LEN];
    if ((result = destination(process, target, &parent, name))) return result;
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
    char text[FS_PATH_LEN];
    const char *rest;
    int volume, node, result = locate(process, path, text, &volume, &rest);
    if (result) return result;
    if (volume >= 0) {
        FatNode *fat = kmalloc(sizeof *fat);
        if (!fat) return -BOS_ENOMEM;
        result = fat_errno(fat_resolve(fatvol_get(volume), rest, fat));
        if (!result && !(fat->attributes & FAT_ATTR_DIRECTORY)) result = -BOS_ENOTDIR;
        kfree(fat);
        if (result) return result;
        unsigned size = (unsigned)kstrlen(text) + 1;
        char *copy = kmalloc(size);
        if (!copy) return -BOS_ENOMEM;
        kmemcpy(copy, text, (int)size);
        kfree(process->files.cwd_path);
        process->files.cwd_path = copy;
        return 0;
    }
    if ((result = resolve(process, text, &node))) return result;
    if (!fs_is_dir(node)) return -BOS_ENOTDIR;
    kfree(process->files.cwd_path);
    process->files.cwd_path = 0;
    process->files.cwd = node;
    process->files.cwd_identity = fs_identity(node);
    return 0;
}

int ufile_getcwd(Uproc *process, uint32_t buffer, uint32_t length) {
    char path[FS_PATH_LEN];
    if (process->files.cwd_path) kmemcpy(path, process->files.cwd_path, kstrlen(process->files.cwd_path) + 1);
    else fs_path(cwd_of(process), path, sizeof path);
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
