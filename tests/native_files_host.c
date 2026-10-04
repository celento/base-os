/* Ordinary API lifecycle tests over the real FS and native file service.
 * Explicit finite-counter boundaries are direct unit cases, not fault/fuzz tests. */
#define main legacy_fs_main
#include "fs_host.c"
#undef main
#include "../src/native_files.c"

#define OWNER_A (BOS_HANDLE_TYPE_PROCESS | 1u)
#define OWNER_B (BOS_HANDLE_TYPE_PROCESS | 2u)
#define RW (BOS_FILE_OPEN_READ | BOS_FILE_OPEN_WRITE)
#define CREATE (RW | BOS_FILE_OPEN_CREATE)
static unsigned char replacement[NATIVE_FILE_REPLACE_MAX], large_file[FS_FILE_MAX];
static void setup_files(void) {
    reset();
    memset(data_disk, 0, sizeof(data_disk));
    unsigned *m = (unsigned *)data_disk;
    m[0] = DATA_MARKER_MAGIC; m[1] = DATA_MARKER_VERSION;
    m[2] = DATA_DISK_SECTORS; m[3] = DATA_SLOT_SECTORS;
    m[4] = DATA_FIRST_LBA; m[5] = DATA_SECOND_LBA; m[6] = crc32(m, 24);
    data_present = 1;
    assert(fs_init() == 0 && fs_load_disk() == FS_LOAD_BLANK);
    assert(fs_empty_dir(0) == 0 && fs_mkdir(0, "Documents") > 0);
    native_files_init();
}
static int add_file(const char *name, const char *bytes, unsigned length) {
    int id = fs_create(fs_resolve(0, "/Documents"), name);
    assert(id > 0 && fs_write(id, bytes, length) == (int)length);
    return id;
}
static BosFileInfo open_file(uint32_t owner, const char *path, unsigned flags) {
    BosFileInfo info;
    memset(&info, 0xaa, sizeof(info));
    assert(native_file_open(owner, path, flags, &info) == BOS_OK);
    assert(info.struct_size == sizeof(info) && info.handle && info.revision);
    assert(info.flags == (flags & RW) && !info.reserved[0] && !info.reserved[1] && !info.reserved[2]);
    return info;
}
static void unchanged_error(int result, int expected, const BosFileInfo *info,
                            const BosFileInfo *before) {
    assert(result == expected && !memcmp(info, before, sizeof(*info)));
}
static void coherence(void) {
    setup_files();
    memset(replacement, 'a', sizeof(replacement));
    int id = add_file("shared.txt", (const char *)replacement, 8192);
    BosFileInfo a = open_file(OWNER_A, "/Documents/shared.txt", RW);
    BosFileInfo b = open_file(OWNER_B, "/Documents/shared.txt", RW);
    BosFileInfo second = open_file(OWNER_A, "/Documents/shared.txt", BOS_FILE_OPEN_READ);
    char bytes[4096];
    assert(native_file_read_at(OWNER_A, a.handle, 0, bytes, sizeof(bytes)) == 4096);
    assert(!memcmp(bytes, replacement, sizeof(bytes)));
    assert(native_file_read_at(OWNER_B, b.handle, 4096, bytes, sizeof(bytes)) == 4096);
    assert(!memcmp(bytes, replacement, sizeof(bytes)));
    assert(native_file_read_at(OWNER_A, second.handle, 0, bytes, sizeof(bytes)) == 4096);
    unsigned old_revision = b.revision;
    memset(replacement, 'b', 8192);
    assert(native_file_replace(OWNER_B, b.handle, replacement, 8192, &b) == BOS_OK);
    assert(b.revision != old_revision && b.size == 8192 && fs_content_revision(id) == b.revision);
    memset(bytes, 0x55, sizeof(bytes));
    char untouched[sizeof(bytes)]; memcpy(untouched, bytes, sizeof(bytes));
    assert(native_file_read_at(OWNER_A, a.handle, 4096, bytes, sizeof(bytes)) == BOS_E_CHANGED);
    assert(!memcmp(bytes, untouched, sizeof(bytes)));
    BosFileInfo before = a;
    unchanged_error(native_file_info(OWNER_A, a.handle, &a), BOS_E_CHANGED, &a, &before);
    unchanged_error(native_file_replace(OWNER_A, a.handle, "old writer", 10, &a), BOS_E_CHANGED, &a, &before);
    assert(!memcmp(fs_data(id), replacement, 8192));
    /* A same-length identical rewrite is still a fresh content version. */
    assert(fs_write(id, fs_data(id), fs_size(id)) == 8192);
    assert(native_file_read_at(OWNER_B, b.handle, 0, bytes, 1) == BOS_E_CHANGED);
    assert(native_file_close(OWNER_A, a.handle) == BOS_OK);
    assert(native_file_close(OWNER_A, a.handle) == BOS_E_STALE);
    assert(native_file_close(OWNER_B, b.handle) == BOS_OK);
    native_files_release_owner(OWNER_A);
    puts("versioned files: coherent concurrent readers, changed mid-stream, competing and identical writes passed");
}
static void lifecycle(void) {
    setup_files(); int id = add_file("source.txt", "original", 8);
    BosFileInfo a = open_file(OWNER_A, "/Documents/source.txt", RW);
    char bytes[16];
    assert(fs_rename(id, "renamed.txt") == 0);
    int folder = fs_mkdir(fs_resolve(0, "/Documents"), "Folder"); assert(folder > 0);
    assert(fs_move(id, folder) == 0);
    assert(native_file_read_at(OWNER_A, a.handle, 0, bytes, sizeof(bytes)) == 8);
    assert(!memcmp(bytes, "original", 8));
    assert(native_file_replace(OWNER_A, a.handle, "updated", 7, &a) == BOS_OK);
    assert(fs_move(id, 0) == 0);
    BosFileInfo before = a;
    unchanged_error(native_file_replace(OWNER_A, a.handle, "blocked", 7, &a), BOS_E_PROTECTED, &a, &before);
    assert(native_file_read_at(OWNER_A, a.handle, 0, bytes, sizeof(bytes)) == 7);
    assert(!memcmp(bytes, "updated", 7));
    assert(fs_move(id, folder) == 0);
    /* Renaming the Documents ancestor rechecks confinement, even though the
     * descendant's identity/content are unchanged. */
    int docs = fs_resolve(0, "/Documents");
    assert(fs_rename(docs, "OldDocuments") == 0 && fs_mkdir(0, "Documents") > 0);
    unchanged_error(native_file_replace(OWNER_A, a.handle, "blocked", 7, &a), BOS_E_PROTECTED, &a, &before);
    assert(fs_delete(id) == 0);
    int recreated = fs_create(folder, "renamed.txt"); assert(recreated == id);
    assert(fs_write(recreated, "new file", 8) == 8);
    memset(bytes, 0x55, sizeof(bytes));
    assert(native_file_read_at(OWNER_A, a.handle, 0, bytes, sizeof(bytes)) == BOS_E_CHANGED);
    for (unsigned i = 0; i < sizeof(bytes); ++i) assert(bytes[i] == 0x55);
    unchanged_error(native_file_replace(OWNER_A, a.handle, "blocked", 7, &a), BOS_E_CHANGED, &a, &before);
    assert(!memcmp(fs_data(id), "new file", 8));
    assert(native_file_close(OWNER_B, a.handle) == BOS_E_STALE);
    assert(native_file_close(OWNER_A, a.handle) == BOS_OK);
    a = open_file(OWNER_A, "/OldDocuments/Folder/renamed.txt", BOS_FILE_OPEN_READ);
    before = a;
    assert(fs_sync() == 0 && fs_load_disk() == 0);
    unchanged_error(native_file_info(OWNER_A, a.handle, &a), BOS_E_CHANGED, &a, &before);
    assert(native_file_close(OWNER_A, a.handle) == BOS_OK);
    a = open_file(OWNER_A, "/OldDocuments/Folder/renamed.txt", BOS_FILE_OPEN_READ);
    native_files_release_owner(OWNER_A);
    assert(native_file_read_at(OWNER_A, a.handle, 0, bytes, 1) == BOS_E_STALE);
    BosFileInfo b = open_file(OWNER_B, "/OldDocuments/Folder/renamed.txt", BOS_FILE_OPEN_READ);
    assert(b.handle != a.handle);
    assert(native_file_read_at(OWNER_B, a.handle, 0, bytes, 1) == BOS_E_STALE);
    native_files_init();
    assert(native_file_read_at(OWNER_B, b.handle, 0, bytes, 1) == BOS_E_STALE);
    a = open_file(OWNER_A, "/OldDocuments/Folder/renamed.txt", BOS_FILE_OPEN_READ);
    assert(a.handle != b.handle);
    puts("versioned files: rename, move, confinement, deletion/recreation, remount and owner cleanup passed");
}
static void bounds_and_busy(void) {
    setup_files();
    BosFileInfo a = open_file(OWNER_A, "/Documents/new.txt", CREATE);
    assert(a.size == 0);
    BosFileInfo out = a, before = out;
    unchanged_error(native_file_open(OWNER_B, "/Documents/new.txt", CREATE, &out), BOS_E_CHANGED, &out, &before);
    memset(replacement, 'x', sizeof(replacement));
    assert(native_file_replace(OWNER_A, a.handle, replacement, sizeof(replacement), &a) == BOS_OK);
    assert(a.size == sizeof(replacement));
    char bytes[4096]; memset(bytes, 0x55, sizeof(bytes));
    assert(native_file_read_at(OWNER_A, a.handle, 32760, bytes, sizeof(bytes)) == 8);
    assert(!memcmp(bytes, replacement, 8) && bytes[8] == 0x55);
    assert(native_file_read_at(OWNER_A, a.handle, ~0u, bytes, sizeof(bytes)) == 0);
    assert(native_file_read_at(OWNER_A, a.handle, 0, 0, 0) == 0);
    assert(native_file_replace(OWNER_A, a.handle, 0, 0, &a) == BOS_OK && a.size == 0);
    before = a;
    unchanged_error(native_file_replace(OWNER_A, a.handle, replacement, sizeof(replacement) + 1, &a), BOS_E_INVALID, &a, &before);
    assert(native_file_read_at(OWNER_A, a.handle, 0, bytes, sizeof(bytes) + 1) == BOS_E_INVALID);
    FsSyncTicket ticket;
    assert(fs_sync_request(&ticket) == 0 && fs_sync_busy());
    unchanged_error(native_file_replace(OWNER_A, a.handle, "busy", 4, &a), BOS_E_BUSY, &a, &before);
    out = before;
    unchanged_error(native_file_open(OWNER_B, "/Documents/busy.txt", CREATE, &out), BOS_E_BUSY, &out, &before);
    assert(fs_resolve(0, "/Documents/busy.txt") < 0);
    assert(native_file_read_at(OWNER_A, a.handle, 0, bytes, sizeof(bytes)) == 0);
    while (fs_sync_busy()) fs_sync_step();
    assert(fs_sync_result(ticket) == 0 && fs_sync_release(ticket) == 0);
    writable = 0;
    unchanged_error(native_file_replace(OWNER_A, a.handle, "protected", 9, &a), BOS_E_PROTECTED, &a, &before);
    unchanged_error(native_file_open(OWNER_B, "/Documents/protected.txt", CREATE, &out), BOS_E_PROTECTED, &out, &before);
    BosFileInfo read = open_file(OWNER_B, "/Documents/new.txt", BOS_FILE_OPEN_READ);
    assert(native_file_read_at(OWNER_B, read.handle, 0, bytes, sizeof(bytes)) == 0);
    assert(fs_resolve(0, "/Documents/protected.txt") < 0);
    writable = 1;
    assert(native_file_replace(OWNER_A, a.handle, "retry", 5, &a) == BOS_OK);
    /* Full printable ASCII path bound, including nested directories. */
    char path[130] = "/Documents/";
    int parent = fs_resolve(0, "/Documents");
    for (unsigned i = 0; i < 4; ++i) {
        parent = fs_mkdir(parent, "12345678901234567890123"); assert(parent > 0);
        strcat(path, "12345678901234567890123/");
    }
    strcat(path, "123456789012345678901"); assert(strlen(path) == 128);
    BosFileInfo maximum = open_file(OWNER_B, path, CREATE);
    assert(maximum.size == 0);
    path[128] = '2'; path[129] = 0;
    out = before;
    unchanged_error(native_file_open(OWNER_B, path, CREATE, &out), BOS_E_INVALID, &out, &before);
    /* Floppy keeps its smaller ordinary per-file bound. */
    reset(); native_files_init();
    a = open_file(OWNER_A, "/Documents/floppy.txt", CREATE); before = a;
    unchanged_error(native_file_replace(OWNER_A, a.handle, replacement, FS_MAX_SIZE, &a), BOS_E_CAPACITY, &a, &before);
    assert(native_file_replace(OWNER_A, a.handle, replacement, FS_MAX_SIZE - 1, &a) == BOS_OK);
    puts("versioned files: exclusive create, full transfer/path/floppy limits, lease busy and protected retries passed");
}
static void capacity_and_owners(void) {
    setup_files(); int id = add_file("capacity.txt", "old", 3);
    BosFileInfo handles[NATIVE_FILE_CAPACITY], out, before;
    memset(&out, 0x55, sizeof(out)); before = out;
    for (unsigned i = 0; i < NATIVE_FILE_CAPACITY; ++i)
        handles[i] = open_file(BOS_HANDLE_TYPE_PROCESS | (i / NATIVE_FILE_PER_OWNER + 1),
                               "/Documents/capacity.txt", RW);
    unchanged_error(native_file_open(OWNER_A, "/Documents/new.txt", CREATE, &out), BOS_E_CAPACITY, &out, &before);
    assert(fs_resolve(0, "/Documents/new.txt") < 0);
    native_files_release_owner(OWNER_A);
    BosFileInfo fresh = open_file(BOS_HANDLE_TYPE_PROCESS | 9u, "/Documents/capacity.txt", RW);
    assert(fresh.handle != handles[0].handle);
    assert(native_file_close(BOS_HANDLE_TYPE_PROCESS | 9u, handles[0].handle) == BOS_E_STALE);
    assert(native_file_close(OWNER_B, handles[0].handle) == BOS_E_STALE);
    native_files_init();
    BosFileInfo a = open_file(OWNER_A, "/Documents/capacity.txt", RW);
    int docs = fs_resolve(0, "/Documents");
    /* Consume the entire normal byte capacity without touching the target. */
    memset(large_file, 'z', sizeof(large_file));
    for (unsigned i = 0; fs_used_bytes() < fs_capacity(); ++i) {
        char name[24]; snprintf(name, sizeof(name), "fill%u", i);
        int fill = fs_create(docs, name); assert(fill > 0);
        unsigned remaining = fs_capacity() - fs_used_bytes();
        unsigned size = remaining > sizeof(large_file) ? sizeof(large_file) : remaining;
        assert(fs_write(fill, (const char *)large_file, size) == (int)size);
    }
    before = a;
    unchanged_error(native_file_replace(OWNER_A, a.handle, "does not fit", 12, &a), BOS_E_CAPACITY, &a, &before);
    assert(fs_size(id) == 3 && !memcmp(fs_data(id), "old", 3));
    setup_files();
    for (unsigned i = 0; fs_node_count() < fs_node_limit(); ++i) {
        char name[24]; snprintf(name, sizeof(name), "node%u", i);
        assert(fs_create(fs_resolve(0, "/Documents"), name) > 0);
    }
    out = before;
    unchanged_error(native_file_open(OWNER_A, "/Documents/overflow", CREATE, &out), BOS_E_CAPACITY, &out, &before);
    assert(fs_node_count() == fs_node_limit());
    puts("versioned files: bounded per-owner/global tables, full byte/node capacity and untouched output passed");
}
static void default_stream(void) {
    setup_files();
    for (unsigned i = 0; i < sizeof(large_file); ++i) large_file[i] = (unsigned char)(i * 17 + i / 4096);
    int id = add_file("maximum.bin", (const char *)large_file, sizeof(large_file));
    BosFileInfo a = open_file(OWNER_A, "/Documents/maximum.bin", RW);
    BosFileInfo b = open_file(OWNER_B, "/Documents/maximum.bin", BOS_FILE_OPEN_READ);
    assert(a.size == FS_FILE_MAX && fs_file_limit() == FS_FILE_MAX);
    unsigned char chunk[NATIVE_FILE_READ_MAX];
    for (unsigned offset = 0; offset < a.size; offset += sizeof(chunk)) {
        assert(native_file_read_at(OWNER_A, a.handle, offset, chunk, sizeof(chunk)) == (int)sizeof(chunk));
        assert(!memcmp(chunk, large_file + offset, sizeof(chunk)));
        assert(native_file_read_at(OWNER_B, b.handle, offset, chunk, sizeof(chunk)) == (int)sizeof(chunk));
        assert(!memcmp(chunk, large_file + offset, sizeof(chunk)));
    }
    assert(native_file_read_at(OWNER_A, a.handle, a.size, chunk, sizeof(chunk)) == 0);
    /* The bounded whole-document replace limit does not grow with read limit. */
    BosFileInfo before = a;
    unchanged_error(native_file_replace(OWNER_A, a.handle, large_file, sizeof(large_file), &a), BOS_E_INVALID, &a, &before);
    assert(fs_size(id) == FS_FILE_MAX && !memcmp(fs_data(id), large_file, sizeof(large_file)));
    puts("versioned files: two exact full 2 MiB default-profile streams and unchanged replace bound passed");
}
static void revision_exhaustion(void) {
    setup_files(); int id = add_file("limit.txt", "old", 3);
    BosFileInfo a = open_file(OWNER_A, "/Documents/limit.txt", RW);
    next_content_revision = ~0u - 1;
    assert(native_file_replace(OWNER_A, a.handle, "last", 4, &a) == BOS_OK && a.revision == ~0u);
    BosFileInfo before = a, out = a;
    unchanged_error(native_file_replace(OWNER_A, a.handle, "too late", 8, &a), BOS_E_CAPACITY, &a, &before);
    unchanged_error(native_file_open(OWNER_A, "/Documents/never.txt", CREATE, &out), BOS_E_CAPACITY, &out, &before);
    assert(!memcmp(fs_data(id), "last", 4) && fs_resolve(0, "/Documents/never.txt") < 0);
    char bytes[4]; assert(native_file_read_at(OWNER_A, a.handle, 0, bytes, sizeof(bytes)) == 4);
    assert(fs_write(id, "legacy", 6) == 6 && !fs_content_revision(id));
    unchanged_error(native_file_open(OWNER_B, "/Documents/limit.txt", RW, &out), BOS_E_CAPACITY, &out, &before);
    assert(native_file_read_at(OWNER_A, a.handle, 0, bytes, sizeof(bytes)) == BOS_E_CHANGED);
    puts("versioned files: content revision exhaustion fails before mutation and legacy writes remain available");
}
static void identity_exhaustion(void) {
    setup_files();
    next_identity = ~0u - 1;
    BosFileInfo a = open_file(OWNER_A, "/Documents/last.txt", CREATE);
    int id = fs_resolve(0, "/Documents/last.txt"); assert(fs_identity(id) == ~0u);
    BosFileInfo before = a, out = a;
    unchanged_error(native_file_open(OWNER_A, "/Documents/never.txt", CREATE, &out), BOS_E_CAPACITY, &out, &before);
    assert(fs_resolve(0, "/Documents/never.txt") < 0);
    assert(native_file_replace(OWNER_A, a.handle, "valid", 5, &a) == BOS_OK);
    int unknown = add_file("legacy.txt", "legacy", 6); assert(!fs_identity(unknown));
    unchanged_error(native_file_open(OWNER_B, "/Documents/legacy.txt", RW, &out), BOS_E_CAPACITY, &out, &before);
    puts("versioned files: node identity exhaustion never wraps and existing bound files remain usable");
}
static void handle_exhaustion(void) {
    setup_files(); add_file("limit.txt", "old", 3);
    native_file_serial = FILE_SERIAL_MAX - 1;
    BosFileInfo a = open_file(OWNER_A, "/Documents/limit.txt", RW);
    assert(a.handle == (FILE_HANDLE_TAG | FILE_SERIAL_MAX));
    assert(native_file_close(OWNER_A, a.handle) == BOS_OK);
    native_files_init();
    BosFileInfo before = a;
    unchanged_error(native_file_open(OWNER_A, "/Documents/never.txt", CREATE, &a), BOS_E_CAPACITY, &a, &before);
    assert(fs_resolve(0, "/Documents/never.txt") < 0);
    assert(native_file_close(OWNER_A, before.handle) == BOS_E_STALE);
    puts("versioned files: handle generation exhaustion remains closed across init and never aliases");
}
int main(int argc, char **argv) {
    if (argc == 1) { coherence(); lifecycle(); bounds_and_busy(); capacity_and_owners(); default_stream(); }
    else if (!strcmp(argv[1], "revision")) revision_exhaustion();
    else if (!strcmp(argv[1], "identity")) identity_exhaustion();
    else if (!strcmp(argv[1], "handle")) handle_exhaustion();
    else return 1;
    return 0;
}
