/* Ordinary file/folder workflows on valid in-memory disks. No malformed
 * snapshots, out-of-bounds inputs or deliberate CPU/memory fault probes. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"

static unsigned char node_arena[FS_CAPACITY];
static unsigned char image_arena[FS_IMG_CAPACITY];
static unsigned char pool_arena[FS_POOL_CAPACITY];
#undef FS_BASE
#undef FS_IMG_BASE
#undef FS_POOL_BASE
#define FS_BASE ((uintptr_t)node_arena)
#define FS_IMG_BASE ((uintptr_t)image_arena)
#define FS_POOL_BASE ((uintptr_t)pool_arena)
#include "../src/fs.c"
#include "file_clipboard.h"

static unsigned char floppy_disk[DISK_SECTORS * SECTOR_SIZE];
static unsigned char ide_disk[DATA_DISK_SECTORS * SECTOR_SIZE];
static char content[FS_FILE_MAX];
static int ide_present, device_unavailable, writes;

uint32_t timer_ticks(void) { return 0; }
void platform_log(const char *text) { (void)text; }
unsigned disk_sector_count(void) { return DISK_SECTORS; }
int disk_read(unsigned lba, void *buffer, int count) {
    assert(count >= 0 && lba + (unsigned)count <= DISK_SECTORS);
    memcpy(buffer, floppy_disk + lba * SECTOR_SIZE, (unsigned)count * SECTOR_SIZE);
    return 0;
}
int disk_write(unsigned lba, const void *buffer, int count) {
    assert(count >= 0 && lba >= FS_DISK_LBA && lba + (unsigned)count <= DISK_SECTORS);
    ++writes;
    if (device_unavailable) return -1;
    memcpy(floppy_disk + lba * SECTOR_SIZE, buffer, (unsigned)count * SECTOR_SIZE);
    return 0;
}
int ata_probe(void) { return ide_present; }
unsigned ata_sector_count(void) { return DATA_DISK_SECTORS; }
int ata_read(unsigned lba, void *buffer, int count) {
    assert(count >= 0 && lba + (unsigned)count <= DATA_DISK_SECTORS);
    memcpy(buffer, ide_disk + lba * SECTOR_SIZE, (unsigned)count * SECTOR_SIZE);
    return 0;
}
int ata_write(unsigned lba, const void *buffer, int count) {
    assert(count >= 0 && lba && lba + (unsigned)count <= DATA_DISK_SECTORS);
    ++writes;
    if (device_unavailable) return -1;
    memcpy(ide_disk + lba * SECTOR_SIZE, buffer, (unsigned)count * SECTOR_SIZE);
    return 0;
}
int ata_flush(void) { return 0; }

static void reset_volume(int ide) {
    memset(floppy_disk, 0, sizeof(floppy_disk));
    memset(ide_disk, 0, sizeof(ide_disk));
    ide_present = ide;
    device_unavailable = writes = 0;
    if (ide) {
        unsigned marker[] = {DATA_MARKER_MAGIC, DATA_MARKER_VERSION,
                            DATA_DISK_SECTORS, DATA_SLOT_SECTORS,
                            DATA_FIRST_LBA, DATA_SECOND_LBA, 0};
        marker[6] = crc32(marker, 24);
        memcpy(ide_disk, marker, sizeof(marker));
    }
    fs_init();
    assert(fs_load_disk() == FS_LOAD_BLANK);
    fs_empty_dir(0);
    assert(fs_node_count() == 1 && fs_sync() == 0);
    file_clipboard_clear();
    assert(file_clipboard_mode() == FILE_CLIPBOARD_NONE);
    assert(!file_clipboard_can_paste(0) && !file_clipboard_pending_sync());
}

static void remount_volume(void) {
    file_clipboard_clear();
    fs_init();
    assert(fs_load_disk() == 0 && !fs_needs_sync() && !fs_storage_status());
}

static int make_file(int parent, const char *name, const char *data, int length) {
    int id = fs_create(parent, name);
    assert(id > 0 && fs_write(id, data, length) == length);
    return id;
}

static void expect_bytes(int id, const char *data, int length) {
    assert(fs_valid(id) && fs_size(id) == length);
    assert(!memcmp(fs_data(id), data, length));
}

static unsigned data_hash(int id) {
    const unsigned char *p = (const unsigned char *)fs_data(id);
    unsigned hash = 2166136261u;
    for (int i = 0; i < fs_size(id); ++i) hash = (hash ^ p[i]) * 16777619u;
    return hash;
}

typedef struct {
    int valid, parent, size, directory, app;
    unsigned identity, modified, hash;
    char name[FS_NAME_LEN];
} NodeSnapshot;
typedef struct {
    int count, dirty, write_count;
    unsigned bytes;
    NodeSnapshot nodes[FS_MAX_NODES];
} Snapshot;

static void snapshot(Snapshot *out) {
    memset(out, 0, sizeof(*out));
    out->count = fs_node_count();
    out->dirty = fs_needs_sync();
    out->write_count = writes;
    out->bytes = fs_used_bytes();
    for (int i = 0; i < fs_node_limit(); ++i) {
        NodeSnapshot *node = &out->nodes[i];
        if (!(node->valid = fs_valid(i))) continue;
        node->parent = fs_parent(i);
        node->size = fs_size(i);
        node->directory = fs_is_dir(i);
        node->app = fs_is_app(i);
        node->identity = fs_identity(i);
        node->modified = fs_modified(i);
        node->hash = data_hash(i);
        strcpy(node->name, fs_name(i));
    }
}

static void expect_unchanged(const Snapshot *before) {
    Snapshot after;
    snapshot(&after);
    assert(!memcmp(before, &after, sizeof(after)));
    assert(strlen(file_clipboard_status()) < FILE_CLIPBOARD_STATUS_LEN);
}

static void copy_files(void) {
    reset_volume(1);
    int folder = fs_mkdir(0, "destination");
    int original = make_file(0, "notes.bin", "A\0B\377C", 5);
    unsigned identity = fs_identity(original);
    assert(file_clipboard_set(original, FILE_CLIPBOARD_COPY) == 0);
    assert(file_clipboard_source() == original && file_clipboard_identity() == identity);
    assert(!strcmp(file_clipboard_name(), "notes.bin"));
    assert(file_clipboard_can_paste(folder));
    char status_before[FILE_CLIPBOARD_STATUS_LEN];
    strcpy(status_before, file_clipboard_status());
    assert(!file_clipboard_can_paste(original));
    assert(!strcmp(status_before, file_clipboard_status()));
    int copy = -1;
    int writes_before = writes;
    assert(file_clipboard_paste(folder, &copy) == FILE_CLIPBOARD_SYNCED);
    assert(copy == fs_find_child(folder, "notes.bin"));
    assert(fs_find_child(folder, "notes.bin copy") < 0);
    assert(writes == writes_before + 2); /* One payload/header snapshot only. */
    assert(fs_identity(copy) != identity && fs_identity(original) == identity);
    expect_bytes(copy, "A\0B\377C", 5);
    assert(file_clipboard_mode() == FILE_CLIPBOARD_COPY && !fs_needs_sync());
    assert(file_clipboard_paste(folder, &copy) == FILE_CLIPBOARD_SYNCED);
    assert(copy == fs_find_child(folder, "notes.bin copy"));
    expect_bytes(copy, "A\0B\377C", 5);
    assert(file_clipboard_paste(0, &copy) == FILE_CLIPBOARD_SYNCED);
    assert(copy == fs_find_child(0, "notes.bin copy"));
    int empty = make_file(0, "empty", "", 0);
    assert(file_clipboard_set(empty, FILE_CLIPBOARD_COPY) == 0);
    assert(file_clipboard_paste(folder, 0) == FILE_CLIPBOARD_SYNCED);
    expect_bytes(fs_find_child(folder, "empty"), "", 0);
    remount_volume();
    folder = fs_resolve(0, "/destination");
    expect_bytes(fs_resolve(0, "/destination/notes.bin"), "A\0B\377C", 5);
    expect_bytes(fs_resolve(0, "/destination/notes.bin copy"), "A\0B\377C", 5);
    expect_bytes(fs_find_child(folder, "empty"), "", 0);
    expect_bytes(fs_resolve(0, "/notes.bin"), "A\0B\377C", 5);
    puts("file clipboard: binary/empty/repeated copies and persisted output passed");
}

static void copy_names_and_folders(void) {
    reset_volume(1);
    int original = make_file(0, "abcdefghijklmnopqrstuvw", "long", 4);
    int copies[110];
    assert(file_clipboard_set(original, FILE_CLIPBOARD_COPY) == 0);
    for (int i = 0; i < 110; ++i) {
        assert(file_clipboard_paste(0, &copies[i]) == FILE_CLIPBOARD_SYNCED);
        assert(strlen(fs_name(copies[i])) <= FS_NAME_LEN - 1);
        expect_bytes(copies[i], "long", 4);
        for (int j = 0; j < i; ++j) assert(strcmp(fs_name(copies[i]), fs_name(copies[j])));
    }
    assert(!strcmp(fs_name(copies[0]), "abcdefghijklmnopqr copy"));
    assert(!strcmp(fs_name(copies[109]), "abcdefghijklmn copy 110"));
    int folder = fs_mkdir(0, "project");
    int nested = fs_mkdir(folder, "nested");
    int child = make_file(nested, "data.bin", "nested\0bytes", 12);
    int empty = make_file(folder, "empty", "", 0);
    unsigned folder_identity = fs_identity(folder), child_identity = fs_identity(child);
    assert(file_clipboard_set(folder, FILE_CLIPBOARD_COPY) == 0);
    int copy;
    assert(file_clipboard_paste(0, &copy) == FILE_CLIPBOARD_SYNCED);
    assert(copy == fs_find_child(0, "project copy"));
    int copied_child = fs_resolve(copy, "nested/data.bin");
    expect_bytes(copied_child, "nested\0bytes", 12);
    expect_bytes(fs_find_child(copy, "empty"), "", 0);
    assert(fs_identity(copy) != folder_identity && fs_identity(copied_child) != child_identity);
    assert(fs_identity(folder) == folder_identity && fs_identity(child) == child_identity);
    assert(fs_parent(empty) == folder);
    int destination = fs_mkdir(0, "destination");
    assert(file_clipboard_paste(destination, &copy) == FILE_CLIPBOARD_SYNCED);
    assert(copy == fs_find_child(destination, "project"));
    assert(fs_find_child(destination, "project copy") < 0);
    expect_bytes(fs_resolve(copy, "nested/data.bin"), "nested\0bytes", 12);
    assert(file_clipboard_paste(destination, &copy) == FILE_CLIPBOARD_SYNCED);
    assert(copy == fs_find_child(destination, "project copy"));
    expect_bytes(fs_resolve(copy, "nested/data.bin"), "nested\0bytes", 12);
    /* Generated names can already exist while the original name is free. */
    int unrelated = make_file(destination, "abcdefghijklmnopqr copy", "keep one", 8);
    int unrelated2 = make_file(destination, "abcdefghijklmnop copy 2", "keep two", 8);
    assert(file_clipboard_set(original, FILE_CLIPBOARD_COPY) == 0);
    assert(file_clipboard_paste(destination, &copy) == FILE_CLIPBOARD_SYNCED);
    assert(!strcmp(fs_name(copy), "abcdefghijklmnopqrstuvw"));
    expect_bytes(copy, "long", 4);
    expect_bytes(unrelated, "keep one", 8);
    expect_bytes(unrelated2, "keep two", 8);
    assert(file_clipboard_paste(destination, &copy) == FILE_CLIPBOARD_SYNCED);
    assert(!strcmp(fs_name(copy), "abcdefghijklmnop copy 3"));
    expect_bytes(unrelated, "keep one", 8);
    expect_bytes(unrelated2, "keep two", 8);
    remount_volume();
    expect_bytes(fs_resolve(0, "/project copy/nested/data.bin"), "nested\0bytes", 12);
    expect_bytes(fs_resolve(0, "/destination/project/nested/data.bin"), "nested\0bytes", 12);
    expect_bytes(fs_resolve(0, "/destination/abcdefghijklmnopqrstuvw"), "long", 4);
    expect_bytes(fs_resolve(0, "/destination/abcdefghijklmnop copy 3"), "long", 4);
    puts("file clipboard: long collision-safe names and recursive folder copy passed");
}

static void move_and_noop(void) {
    reset_volume(1);
    int source_folder = fs_mkdir(0, "source");
    int destination = fs_mkdir(0, "destination");
    int document = make_file(source_folder, "document", "move me", 7);
    unsigned identity = fs_identity(document);
    assert(file_clipboard_set(document, FILE_CLIPBOARD_CUT) == 0);
    int result;
    assert(file_clipboard_paste(destination, &result) == FILE_CLIPBOARD_SYNCED);
    assert(result == document && fs_identity(document) == identity);
    assert(fs_parent(document) == destination && fs_find_child(source_folder, "document") < 0);
    expect_bytes(document, "move me", 7);
    assert(file_clipboard_mode() == FILE_CLIPBOARD_NONE && !file_clipboard_can_paste(0));
    Snapshot before;
    snapshot(&before);
    assert(file_clipboard_paste(0, &result) == FILE_CLIPBOARD_ERROR && result == -1);
    expect_unchanged(&before);
    assert(file_clipboard_set(destination, FILE_CLIPBOARD_CUT) == 0);
    unsigned folder_identity = fs_identity(destination);
    assert(file_clipboard_paste(source_folder, &result) == FILE_CLIPBOARD_SYNCED);
    assert(result == destination && fs_parent(destination) == source_folder);
    assert(fs_identity(destination) == folder_identity && fs_identity(document) == identity);
    assert(file_clipboard_set(document, FILE_CLIPBOARD_CUT) == 0);
    snapshot(&before);
    assert(file_clipboard_paste(destination, &result) == FILE_CLIPBOARD_NOOP);
    assert(result == document && file_clipboard_mode() == FILE_CLIPBOARD_NONE);
    expect_unchanged(&before);
    remount_volume();
    expect_bytes(fs_resolve(0, "/source/destination/document"), "move me", 7);
    puts("file clipboard: identity-preserving file/folder moves and same-folder no-op passed");
}

static void rename_delete_and_text(void) {
    reset_volume(1);
    int destination = fs_mkdir(0, "destination");
    int source_node = make_file(0, "before", "old", 3);
    unsigned identity = fs_identity(source_node);
    assert(file_clipboard_set(source_node, FILE_CLIPBOARD_COPY) == 0);
    assert(fs_rename(source_node, "renamed") == 0 && fs_write(source_node, "latest", 6) == 6);
    assert(file_clipboard_source() == source_node && file_clipboard_identity() == identity);
    assert(!strcmp(file_clipboard_name(), "renamed"));
    int result;
    assert(file_clipboard_paste(destination, &result) == FILE_CLIPBOARD_SYNCED);
    assert(result == fs_find_child(destination, "renamed"));
    expect_bytes(result, "latest", 6);
    assert(file_clipboard_set(source_node, FILE_CLIPBOARD_CUT) == 0);
    assert(fs_rename(source_node, "latest name") == 0);
    assert(file_clipboard_paste(destination, &result) == FILE_CLIPBOARD_SYNCED);
    assert(result == source_node && !strcmp(fs_name(source_node), "latest name"));
    for (int operation = FILE_CLIPBOARD_COPY; operation <= FILE_CLIPBOARD_CUT; ++operation) {
        assert(file_clipboard_set(source_node, operation) == 0);
        assert(fs_delete(source_node) == 0);
        assert(file_clipboard_source() == -1 && !file_clipboard_can_paste(0));
        int replacement = make_file(destination, "latest name", "replacement", 11);
        assert(replacement == source_node && fs_identity(replacement) != identity);
        assert(fs_sync() == 0);
        Snapshot before;
        snapshot(&before);
        assert(file_clipboard_paste(0, &result) == FILE_CLIPBOARD_ERROR && result == -1);
        expect_unchanged(&before);
        expect_bytes(replacement, "replacement", 11);
        source_node = replacement;
        identity = fs_identity(source_node);
    }
    assert(file_clipboard_set(source_node, FILE_CLIPBOARD_CUT) == 0);
    file_clipboard_clear(); /* The shared text clipboard's write hook. */
    assert(file_clipboard_source() == -1 && file_clipboard_identity() == 0);
    assert(file_clipboard_mode() == FILE_CLIPBOARD_NONE && !*file_clipboard_name());
    assert(!file_clipboard_can_paste(0));
    expect_bytes(source_node, "replacement", 11);
    puts("file clipboard: live renames/edits, deleted/reused identities and text-clear passed");
}

static void rejected_destinations(void) {
    reset_volume(1);
    int folder = fs_mkdir(0, "folder");
    int child = fs_mkdir(folder, "child");
    int leaf = make_file(child, "leaf", "original", 8);
    int destination = fs_mkdir(0, "destination");
    int collision = make_file(destination, "folder", "unrelated", 9);
    int app = fs_create_app(0, "Calculator");
    assert(fs_sync() == 0);
    for (int operation = FILE_CLIPBOARD_COPY; operation <= FILE_CLIPBOARD_CUT; ++operation) {
        assert(file_clipboard_set(folder, operation) == 0);
        Snapshot before;
        snapshot(&before);
        assert(file_clipboard_set(0, operation) == -1);
        assert(file_clipboard_set(app, operation) == -1);
        assert(file_clipboard_source() == folder && file_clipboard_mode() == operation);
        int result;
        assert(!file_clipboard_can_paste(folder) && !file_clipboard_can_paste(child));
        assert(file_clipboard_paste(folder, &result) == FILE_CLIPBOARD_ERROR && result == -1);
        assert(file_clipboard_paste(child, &result) == FILE_CLIPBOARD_ERROR && result == -1);
        assert(!file_clipboard_can_paste(leaf));
        assert(file_clipboard_paste(leaf, &result) == FILE_CLIPBOARD_ERROR && result == -1);
        expect_unchanged(&before);
    }
    Snapshot before;
    snapshot(&before);
    assert(!file_clipboard_can_paste(destination));
    assert(file_clipboard_paste(destination, 0) == FILE_CLIPBOARD_ERROR);
    assert(file_clipboard_mode() == FILE_CLIPBOARD_CUT);
    expect_unchanged(&before);
    expect_bytes(collision, "unrelated", 9);
    expect_bytes(leaf, "original", 8);
    assert(fs_rename(folder, "renamed folder") == 0);
    assert(file_clipboard_can_paste(destination));
    assert(file_clipboard_paste(destination, 0) == FILE_CLIPBOARD_SYNCED);
    expect_bytes(collision, "unrelated", 9);
    /* Ordinary folders remain eligible until an app is placed inside. */
    assert(file_clipboard_set(folder, FILE_CLIPBOARD_COPY) == 0);
    assert(fs_move(app, child) == 0 && fs_sync() == 0);
    snapshot(&before);
    assert(!file_clipboard_can_paste(0));
    assert(file_clipboard_paste(0, 0) == FILE_CLIPBOARD_ERROR);
    assert(file_clipboard_set(folder, FILE_CLIPBOARD_CUT) == -1);
    expect_unchanged(&before);
    puts("file clipboard: root/apps, nested destinations and unrelated name collisions preserved");
}

static void full_nodes_and_depth(void) {
    for (int ide = 0; ide <= 1; ++ide) {
        reset_volume(ide);
        int folder = fs_mkdir(0, "folder");
        make_file(folder, "one", "one", 3);
        make_file(folder, "two", "two", 3);
        while (fs_node_count() < fs_node_limit() - 2) {
            char name[FS_NAME_LEN];
            snprintf(name, sizeof(name), "filler%d", fs_node_count());
            assert(fs_create(0, name) > 0);
        }
        assert(fs_sync() == 0);
        assert(file_clipboard_set(folder, FILE_CLIPBOARD_COPY) == 0);
        Snapshot before;
        snapshot(&before);
        assert(file_clipboard_paste(0, 0) == FILE_CLIPBOARD_ERROR);
        expect_unchanged(&before);
        assert(fs_create(0, "last one") > 0 && fs_create(0, "last two") > 0);
        assert(fs_sync() == 0);
        snapshot(&before);
        assert(file_clipboard_paste(0, 0) == FILE_CLIPBOARD_ERROR);
        expect_unchanged(&before);
        assert(file_clipboard_mode() == FILE_CLIPBOARD_COPY);
    }
    reset_volume(1);
    int deepest = 0, penultimate = 0;
    for (int i = 1; i <= FS_MAX_DEPTH; ++i) {
        penultimate = deepest;
        deepest = fs_mkdir(deepest, "abcdefghijklmnopqrstuvw");
        assert(deepest > 0);
    }
    int folder = fs_mkdir(0, "source");
    make_file(folder, "abcdefghijklmnopqrstuvw", "keep", 4);
    assert(fs_sync() == 0);
    for (int operation = FILE_CLIPBOARD_COPY; operation <= FILE_CLIPBOARD_CUT; ++operation) {
        assert(file_clipboard_set(folder, operation) == 0);
        Snapshot before;
        snapshot(&before);
        assert(file_clipboard_paste(deepest, 0) == FILE_CLIPBOARD_ERROR);
        assert(file_clipboard_paste(penultimate, 0) == FILE_CLIPBOARD_ERROR);
        expect_unchanged(&before);
        assert(file_clipboard_mode() == operation && file_clipboard_source() == folder);
    }
    /* Original-name restoration also fits the longest legal directory path.
     * Use a distinct 23-character name beside the destination's existing child. */
    assert(fs_rename(folder, "bcdefghijklmnopqrstuvwx") == 0);
    assert(file_clipboard_set(folder, FILE_CLIPBOARD_COPY) == 0);
    int copy;
    assert(file_clipboard_paste(fs_parent(penultimate), &copy) == FILE_CLIPBOARD_SYNCED);
    assert(!strcmp(fs_name(copy), "bcdefghijklmnopqrstuvwx"));
    int copied_child = fs_find_child(copy, "abcdefghijklmnopqrstuvw");
    char path[FS_PATH_LEN];
    fs_path(copied_child, path, sizeof(path));
    assert(strlen(path) == 1512);
    expect_bytes(copied_child, "keep", 4);
    remount_volume();
    expect_bytes(fs_resolve(0, path), "keep", 4);
    puts("file clipboard: full 64/256-node capacity, partial-copy rollback and path depth passed");
}

static void full_data_capacity(void) {
    reset_volume(1);
    int destination = fs_mkdir(0, "destination");
    int large[4];
    for (int i = 0; i < 4; ++i) {
        char name[FS_NAME_LEN];
        snprintf(name, sizeof(name), "large%d", i);
        unsigned length = i < 3 ? FS_FILE_MAX : fs_capacity() - 3 * FS_FILE_MAX;
        large[i] = make_file(0, name, content, length);
    }
    assert(fs_used_bytes() == fs_capacity() && fs_sync() == 0);
    Snapshot before;
    snapshot(&before);
    assert(file_clipboard_set(large[0], FILE_CLIPBOARD_COPY) == 0);
    assert(file_clipboard_paste(destination, 0) == FILE_CLIPBOARD_ERROR);
    expect_unchanged(&before);
    /* A move needs no data allocation, even on a volume with no free bytes. */
    assert(file_clipboard_set(large[0], FILE_CLIPBOARD_CUT) == 0);
    assert(file_clipboard_paste(destination, 0) == FILE_CLIPBOARD_SYNCED);
    expect_bytes(large[0], content, FS_FILE_MAX);
    remount_volume();
    assert(fs_used_bytes() == fs_capacity());
    expect_bytes(fs_resolve(0, "/destination/large0"), content, FS_FILE_MAX);
    puts("file clipboard: full data-capacity rejection preserves bytes; allocation-free move persists");
}

static void sync_retry_guard(void) {
    reset_volume(1);
    int destination = fs_mkdir(0, "destination");
    int original = make_file(0, "original", "content", 7);
    assert(fs_sync() == 0);
    assert(file_clipboard_set(original, FILE_CLIPBOARD_COPY) == 0);
    device_unavailable = 1;
    int copy;
    assert(file_clipboard_paste(destination, &copy) == FILE_CLIPBOARD_RAM_ONLY);
    assert(copy == fs_find_child(destination, "original") && file_clipboard_pending_sync());
    assert(fs_needs_sync() && file_clipboard_mode() == FILE_CLIPBOARD_COPY);
    int count = fs_node_count(), repeated;
    assert(file_clipboard_paste(destination, &repeated) == FILE_CLIPBOARD_RAM_ONLY);
    assert(repeated == copy && fs_node_count() == count);
    assert(fs_find_child(destination, "original copy") < 0);
    device_unavailable = 0;
    assert(file_clipboard_paste(destination, &repeated) == FILE_CLIPBOARD_SYNCED);
    assert(repeated == copy && fs_node_count() == count && !file_clipboard_pending_sync());
    assert(!fs_needs_sync());
    assert(file_clipboard_paste(destination, &repeated) == FILE_CLIPBOARD_SYNCED);
    assert(repeated == fs_find_child(destination, "original copy"));
    /* Autosync success still cannot make the first retry duplicate the copy. */
    device_unavailable = 1;
    assert(file_clipboard_paste(destination, &copy) == FILE_CLIPBOARD_RAM_ONLY);
    count = fs_node_count();
    device_unavailable = 0;
    assert(fs_sync() == 0);
    assert(file_clipboard_paste(destination, &repeated) == FILE_CLIPBOARD_SYNCED);
    assert(repeated == copy && fs_node_count() == count);
    /* Clearing for a new text copy never removes already completed RAM data. */
    assert(file_clipboard_set(original, FILE_CLIPBOARD_COPY) == 0);
    device_unavailable = 1;
    assert(file_clipboard_paste(destination, &copy) == FILE_CLIPBOARD_RAM_ONLY);
    count = fs_node_count();
    file_clipboard_clear();
    assert(!file_clipboard_pending_sync() && !file_clipboard_can_paste(destination));
    assert(fs_node_count() == count && fs_needs_sync());
    expect_bytes(copy, "content", 7);
    device_unavailable = 0;
    assert(fs_sync() == 0);
    /* Cut is consumed on RAM completion. A failed disk save is never retried
     * by moving the source again, even to another folder. */
    assert(fs_rename(original, "move original") == 0);
    unsigned identity = fs_identity(original);
    assert(file_clipboard_set(original, FILE_CLIPBOARD_CUT) == 0);
    device_unavailable = 1;
    assert(file_clipboard_paste(destination, &repeated) == FILE_CLIPBOARD_RAM_ONLY);
    assert(repeated == original && fs_parent(original) == destination);
    assert(fs_identity(original) == identity && file_clipboard_mode() == FILE_CLIPBOARD_NONE);
    assert(file_clipboard_source() == -1 && !file_clipboard_can_paste(0));
    assert(file_clipboard_paste(0, &repeated) == FILE_CLIPBOARD_ERROR && repeated == -1);
    assert(fs_parent(original) == destination && fs_identity(original) == identity);
    device_unavailable = 0;
    assert(fs_sync() == 0);
    remount_volume();
    expect_bytes(fs_resolve(0, "/destination/move original"), "content", 7);
    expect_bytes(fs_resolve(0, "/destination/original"), "content", 7);
    expect_bytes(fs_resolve(0, "/destination/original copy 3"), "content", 7);
    puts("file clipboard: completed RAM operations, save retries, autosync and consumed cuts passed");
}

static void pending_result_identity(void) {
    reset_volume(1);
    int destination = fs_mkdir(0, "destination");
    int original = make_file(0, "original", "source", 6);
    assert(fs_sync() == 0 && file_clipboard_set(original, FILE_CLIPBOARD_COPY) == 0);
    device_unavailable = 1;
    int copy;
    assert(file_clipboard_paste(destination, &copy) == FILE_CLIPBOARD_RAM_ONLY);
    unsigned identity = fs_identity(copy);
    assert(fs_delete(copy) == 0);
    int replacement = make_file(destination, "replacement", "keep", 4);
    assert(replacement == copy && fs_identity(replacement) != identity);
    assert(fs_delete(original) == 0);
    int count = fs_node_count();
    device_unavailable = 0;
    int result;
    assert(file_clipboard_paste(destination, &result) == FILE_CLIPBOARD_SYNCED);
    assert(result == -1 && fs_node_count() == count);
    expect_bytes(replacement, "keep", 4);
    assert(!file_clipboard_can_paste(destination));
    remount_volume();
    expect_bytes(fs_resolve(0, "/destination/replacement"), "keep", 4);
    puts("file clipboard: retry cannot select a reused result or recopy a deleted source");
}

int main(void) {
    for (unsigned i = 0; i < sizeof(content); ++i) content[i] = (char)(i * 37 + i / 65536);
    copy_files();
    copy_names_and_folders();
    move_and_noop();
    rename_delete_and_text();
    rejected_destinations();
    full_nodes_and_depth();
    full_data_capacity();
    sync_retry_guard();
    pending_result_identity();
    puts("file clipboard: all ordinary host workflows passed");
    return 0;
}
