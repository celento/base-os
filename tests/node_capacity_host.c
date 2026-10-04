/* Ordinary valid-volume tests only: capacity, paths, mutation and remount. */
#define main unused_legacy_suite
#include "fs_host.c"
#undef main

static unsigned char pattern[FS_FILE_MAX];
static unsigned char previous_data[sizeof(data_disk)];
static const char longest_name[] = "abcdefghijklmnopqrstuvw";

static void mark_data(void) {
    memset(data_disk, 0, sizeof(data_disk));
    unsigned marker[] = {DATA_MARKER_MAGIC, DATA_MARKER_VERSION,
                        DATA_DISK_SECTORS, DATA_SLOT_SECTORS,
                        DATA_FIRST_LBA, DATA_SECOND_LBA, 0};
    marker[6] = crc32(marker, 24);
    memcpy(data_disk, marker, sizeof(marker));
    data_present = 1;
}
static void empty_data(void) {
    reset(); mark_data(); fs_init();
    assert(fs_load_disk() == FS_LOAD_BLANK && fs_node_limit() == 256);
    fs_empty_dir(0);
    assert(fs_node_count() == 1 && fs_sync() == 0);
}
static void reboot_data(void) {
    fs_init(); assert(fs_load_disk() == 0);
    assert(fs_node_limit() == 256 && !fs_storage_status() && !fs_needs_sync());
}
static int named_file(int parent, const char *prefix, int index) {
    char name[FS_NAME_LEN]; snprintf(name, sizeof(name), "%s%d", prefix, index);
    int id = fs_create(parent, name); assert(id > 0); return id;
}

/* Construct known old-format bytes independently of the current serializer.
 * Full v4 uses exactly 64 records and the complete original byte allowance. */
static void old_snapshot(unsigned version, int full) {
    reset();
    if (version == 4) mark_data();
    unsigned char *image = version == 4 ? data_disk : disk;
    unsigned lba = version == 4 ? DATA_FIRST_LBA : FS_DISK_LBA;
    unsigned char *start = image + lba * SECTOR_SIZE;
    unsigned pos = SECTOR_SIZE;
    for (int id = 0; id < 64; ++id) {
        DiskNode d = {.id = id, .parent = id ? (full ? 0 : id - 1) : -1,
                      .is_dir = full ? !id : id != 63, .modified = 12345u + id};
        if (id) {
            if (full) snprintf(d.name, sizeof(d.name), "old%d", id);
            else strcpy(d.name, longest_name);
        }
        if (full && id > 0 && id <= 4)
            d.size = id < 4 ? FS_FILE_MAX : 8385024u - 3 * FS_FILE_MAX;
        else if (!full && id == 63) d.size = 16383;
        unsigned record = version >= 3 ? 40 : 36;
        memcpy(start + pos, &d, record); pos += record;
        memcpy(start + pos, pattern, d.size); pos += d.size;
    }
    DiskHeader h = {.magic = FS_DISK_MAGIC, .version = version, .count = 64,
                    .bytes = pos - SECTOR_SIZE, .generation = version == 1 ? 0 : 17};
    h.sum = version == 1 ? checksum(start + SECTOR_SIZE, h.bytes)
                        : crc32(start + SECTOR_SIZE, h.bytes);
    h.header_sum = version == 1 ? 0 : crc32(&h, 24);
    memcpy(start, &h, sizeof(h));
}

static void legacy_paths(void) {
    for (unsigned version = 1; version <= 4; ++version) {
        old_snapshot(version, 0);
        memcpy(previous_data, data_disk, sizeof(data_disk));
        memcpy(saved, disk, sizeof(disk));
        fs_init(); assert(fs_load_disk() == 0);
        assert(fs_node_count() == 64 && fs_node_limit() == (version == 4 ? 256 : 64));
        assert(fs_size(63) == 16383 && !memcmp(fs_data(63), pattern, 16383));
        assert(fs_modified(63) == (version >= 3 ? 12408u : 0u));
        char path[FS_PATH_LEN]; fs_path(63, path, sizeof(path));
        assert(strlen(path) == 63 * 24 && fs_resolve(0, path) == 63);
        assert(fs_sync() == 0 && !memcmp(saved, disk, sizeof(disk)));
        assert(!memcmp(previous_data, data_disk, sizeof(data_disk)));
        assert(fs_write(63, fs_data(63), 16383) == 16383 && fs_sync() == 0);
        /* Original slot stays byte-exact after the first changed save. */
        if (version == 4)
            assert(!memcmp(previous_data, data_disk, DATA_SECOND_LBA * SECTOR_SIZE));
        else assert(!memcmp(saved, disk, FS_SECOND_LBA * SECTOR_SIZE));
        fs_init(); assert(fs_load_disk() == 0);
        assert(fs_size(63) == 16383 && !memcmp(fs_data(63), pattern, 16383));
    }
    puts("node capacity: exact v1/v2/v3/v4 63-component trees and bytes survive save/remount");
}

static void full_old_volume(void) {
    old_snapshot(4, 1);
    memcpy(previous_data, data_disk, sizeof(data_disk));
    reboot_data();
    assert(fs_node_count() == 64 && fs_used_bytes() == 8385024u && fs_capacity() == 8385024u);
    assert(fs_sync() == 0 && !memcmp(previous_data, data_disk, sizeof(data_disk)));
    unsigned identity_before = next_identity, pool_before = pool_used;
    assert(fs_create(0, "node65") == -1 && fs_node_count() == 64);
    assert(next_identity == identity_before && pool_used == pool_before && !fs_needs_sync());
    assert(fs_write(4, fs_data(4), fs_size(4) - 39) > 0 && fs_sync() == 0);
    assert(fs_capacity() - fs_used_bytes() == 39);
    assert(fs_mkdir(0, "node65") == -1 && !fs_needs_sync());
    assert(fs_node_count() == 64 && fs_used_bytes() == 8385024u - 39);
    assert(fs_write(4, fs_data(4), fs_size(4) - 1) > 0);
    assert(fs_create(0, "node65") == 64 && fs_capacity() == 8385024u - 40);
    assert(fs_used_bytes() == fs_capacity() && fs_sync() == 0);
    reboot_data();
    assert(fs_node_count() == 65 && fs_size(1) == FS_FILE_MAX);
    for (int id = 1; id <= 4; ++id) assert(!memcmp(fs_data(id), pattern, fs_size(id)));
    assert(fs_write(1, (const char *)pattern, FS_FILE_MAX - 10000) == FS_FILE_MAX - 10000);
    for (int n = 65; n < 256; ++n) assert(named_file(0, "more", n) == n);
    assert(fs_node_limit() == 256 && fs_node_count() == 256);
    assert(fs_capacity() == 8377344u);
    assert(fs_create(0, "no-free-nodes") == -1);
    int list[256]; assert(fs_list(0, list, 256) == 255 && list[254] == 255);
    assert(fs_list_files(list, 256) == 255 && list[254] == 255);
    assert(fs_write(255, "last\0node", 9) == 9 && fs_sync() == 0);
    reboot_data();
    assert(fs_node_count() == 256 && !memcmp(fs_data(255), "last\0node", 9));
    assert(fs_delete(254) == 0 && fs_capacity() == 8377384u);
    assert(fs_create_app(0, "last-app") == 254 && fs_is_app(254));
    assert(fs_rename(255, "renamed") == 0 && fs_sync() == 0);
    reboot_data(); assert(fs_find_child(0, "renamed") == 255);
    assert(fs_is_app(254) && !memcmp(fs_data(255), "last\0node", 9));
    puts("node capacity: full old 8,385,024-byte/64-record v4, 40-byte admission and node 255 passed");
}

static void nested_mutation(void) {
    empty_data();
    int directories[64] = {0};
    for (int n = 1; n <= 63; ++n) {
        directories[n] = fs_mkdir(directories[n - 1], longest_name);
        assert(directories[n] > 0);
    }
    char path[FS_PATH_LEN]; fs_path(directories[63], path, sizeof(path));
    assert(strlen(path) == 1512 && fs_resolve(0, path) == directories[63]);
    assert(fs_create(directories[63], "too-deep") == -1);
    int folder = fs_mkdir(0, "folder"), child = fs_create(folder, "binary");
    assert(fs_write(child, (const char *)pattern, 1027) == 1027);
    assert(fs_create(directories[62], "folder") > 0); /* Name collision at limit. */
    assert(fs_sync() == 0);
    int before = fs_node_count();
    assert(fs_move(folder, directories[62]) == -1 && fs_parent(folder) == 0);
    assert(!strcmp(fs_name(folder), "folder") && !fs_needs_sync());
    assert(fs_copy(folder, directories[62]) == -1 && fs_node_count() == before && !fs_needs_sync());
    assert(fs_move(folder, directories[61]) == 0);
    fs_path(child, path, sizeof(path)); assert(fs_resolve(0, path) == child);
    assert(fs_rename(folder, longest_name) == -1); /* Existing sibling. */
    assert(fs_rename(folder, "new folder") == 0);
    int copy = fs_copy(folder, 0); assert(copy > 0);
    int copied_child = fs_find_child(copy, "binary");
    assert(copied_child > 0 && !memcmp(fs_data(copied_child), pattern, 1027));
    assert(fs_sync() == 0); reboot_data();
    assert(fs_parent(folder) == directories[61] && fs_resolve(0, path) == -1);
    assert(!memcmp(fs_data(child), pattern, 1027));
    assert(fs_delete(directories[1]) == 0 && fs_node_count() == 3);
    assert(fs_parent(copy) == 0 && !memcmp(fs_data(copied_child), pattern, 1027));
    /* A partial recursive copy must roll back both nodes and file bytes. */
    assert(fs_create(copy, "second") > 0);
    while (fs_node_count() < 254) named_file(0, "filler", fs_node_count());
    assert(fs_sync() == 0);
    unsigned used = fs_used_bytes(), pool = pool_used;
    assert(fs_copy(copy, 0) == -1 && fs_node_count() == 254 && !fs_needs_sync());
    assert(fs_used_bytes() == used && pool_used == pool);
    assert(!memcmp(fs_data(copied_child), pattern, 1027));
    assert(fs_create(0, "space1") > 0 && fs_create(0, "space2") > 0);
    assert(fs_sync() == 0); reboot_data(); assert(fs_node_count() == 256);
    fs_empty_dir(0); assert(fs_node_count() == 1 && fs_used_bytes() == 0 && pool_used == 0);
    assert(fs_sync() == 0); reboot_data(); assert(fs_node_count() == 1);
    puts("node capacity: bounded create/move/copy/rename paths, rollback, recursive delete and reclaim passed");
}

static void generated_names(void) {
    empty_data();
    char name[FS_NAME_LEN];
    for (int i = 1; i <= 220; ++i) {
        assert(fs_unique_file(0, name) == 0);
        assert(fs_create(0, name) > 0);
    }
    assert(!strcmp(name, "new220.txt"));
    fs_empty_dir(0);
    for (int i = 1; i <= 120; ++i) {
        assert(fs_unique_dir(0, name) == 0);
        assert(fs_mkdir(0, name) > 0);
    }
    assert(!strcmp(name, "untitled folder 120"));
    fs_empty_dir(0);
    int source = fs_create(0, longest_name); assert(source > 0);
    for (int i = 1; i <= 120; ++i) assert(fs_copy(source, 0) > 0);
    assert(fs_unique_copy(0, longest_name, name) == 0 && strstr(name, "copy 121"));
    fs_empty_dir(0);
    int destination = fs_mkdir(0, "destination");
    for (int i = 1; i <= 120; ++i) {
        int id = fs_create(0, longest_name); assert(id > 0);
        assert(fs_move(id, destination) == 0);
    }
    assert(fs_child_count(destination) == 120);
    assert(fs_sync() == 0); reboot_data();
    assert(fs_child_count(destination) == 120);
    puts("node capacity: automatic new/copy/move names exceed 99 without collisions");
}

static void legacy_floppy_limit(void) {
    reset(); fs_empty_dir(0);
    assert(fs_node_limit() == 64 && fs_capacity() == 63u * 16383u);
    for (int i = 1; i < 64; ++i) {
        int id = named_file(0, "full", i);
        assert(fs_write(id, (const char *)pattern, 16383) == 16383);
    }
    assert(fs_create(0, "node65") == -1 && fs_node_count() == 64);
    assert(fs_sync() == 0); remount();
    assert(fs_node_limit() == 64 && fs_node_count() == 64 && fs_used_bytes() == fs_capacity());
    puts("node capacity: floppy retains all 64 nodes and original full payload capacity");
}

int main(void) {
    for (unsigned i = 0; i < sizeof(pattern); ++i) pattern[i] = (i * 37 + (i >> 16)) & 255;
    legacy_paths(); full_old_volume(); nested_mutation(); generated_names(); legacy_floppy_limit();
    return 0;
}
