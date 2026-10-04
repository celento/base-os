/* Deterministic allocator, migration and persistence checks on simulated media. */
#define main legacy_filesystem_tests
#include "fs_host.c"
#undef main
static char media[FS_FILE_MAX];
static unsigned char old_data[sizeof(data_disk)];
static void marked_disk(void) {
    memset(data_disk, 0, sizeof(data_disk));
    unsigned *m = (unsigned *)data_disk;
    m[0] = DATA_MARKER_MAGIC; m[1] = DATA_MARKER_VERSION;
    m[2] = DATA_DISK_SECTORS; m[3] = DATA_SLOT_SECTORS;
    m[4] = DATA_FIRST_LBA; m[5] = DATA_SECOND_LBA; m[6] = crc32(m, 24);
    data_present = 1;
}
static void data_remount(void) {
    fs_init(); assert(fs_load_disk() == 0);
    assert(!strcmp(fs_storage_name(), "IDE data disk"));
    assert(!fs_storage_status());
}
int main(void) {
    for (unsigned i = 0; i < sizeof(media); ++i) media[i] = (char)(i * 37 + i / 65536);
    reset(); checkpoint("old boot files");
    memcpy(saved, disk, sizeof(disk));
    marked_disk();
    fs_init(); assert(fs_load_disk() == 0);
    expect_value("old boot files");
    assert(fs_needs_sync() && fs_file_limit() == FS_FILE_MAX);
    assert(fs_capacity() == 8385024u && fs_node_count() < FS_MAX_NODES);
    assert(!memcmp(saved, disk, sizeof(disk)) && data_writes == 0);
    assert(fs_sync() == 0);
    assert(!memcmp(saved, disk, sizeof(disk)));
    data_remount(); expect_value("old boot files");
    int id = fs_create(0, "media.bin"); assert(id > 0);
    assert(fs_write(id, media, sizeof(media)) == sizeof(media));
    assert(!memcmp(fs_data(id), media, sizeof(media)));
    assert(fs_data(id)[sizeof(media)] == 0);
    assert(fs_write(id, media, FS_FILE_MAX + 1) == -1);
    assert(fs_size(id) == sizeof(media) && !memcmp(fs_data(id), media, sizeof(media)));
    assert(fs_write(id, 0, 1) == -1 && fs_write(id, media, -1) == -1);
    assert(fs_sync() == 0);
    data_remount(); id = fs_find_child(0, "media.bin");
    assert(fs_size(id) == sizeof(media) && !memcmp(fs_data(id), media, sizeof(media)));
    assert(!memcmp(saved, disk, sizeof(disk)));

    /* Copy and overwrites retain source bytes across arena compaction. */
    int copy = fs_copy(id, 0); assert(copy >= 0);
    assert(fs_size(copy) == sizeof(media) && !memcmp(fs_data(copy), media, sizeof(media)));
    assert(fs_write(id, fs_data(copy) + 17, 70001) == 70001);
    assert(!memcmp(fs_data(id), media + 17, 70001));
    assert(!memcmp(fs_data(copy), media, sizeof(media)));
    assert(fs_write(copy, fs_data(copy) + 123, 120001) == 120001);
    assert(!memcmp(fs_data(copy), media + 123, 120001));
    assert(fs_delete(id) == 0 && !memcmp(fs_data(copy), media + 123, 120001));
    assert(fs_write(copy, 0, 0) == 0 && fs_size(copy) == 0 && !*fs_data(copy));
    assert(fs_sync() == 0); data_remount();
    assert(fs_size(fs_find_child(0, "media.bin copy")) == 0);

    /* Whole-volume capacity is independent of the per-file cap. No giant
     * per-node reservation, and a full volume can overwrite/shrink/reclaim. */
    fs_empty_dir(0);
    int ids[4];
    for (int i = 0; i < 4; ++i) {
        char name[] = "large0"; name[5] += i;
        ids[i] = fs_create(0, name); assert(ids[i] > 0);
        unsigned count = i < 3 ? FS_FILE_MAX : fs_capacity() - 3 * FS_FILE_MAX;
        assert(fs_write(ids[i], media, count) == (int)count);
    }
    assert(fs_used_bytes() == fs_capacity());
    assert(fs_write(ids[0], fs_data(ids[0]), FS_FILE_MAX) == FS_FILE_MAX);
    assert(!memcmp(fs_data(ids[0]), media, FS_FILE_MAX));
    unsigned pool_before = pool_used;
    int empty = fs_create(0, "no-room"); assert(empty > 0);
    assert(fs_write(empty, "x", 1) == -1 && fs_size(empty) == 0 && pool_before == pool_used);
    int count_before = fs_node_count();
    assert(fs_sync() == 0);
    assert(fs_copy(ids[0], 0) == -1 && fs_node_count() == count_before && !fs_needs_sync());
    data_remount(); assert(fs_used_bytes() == fs_capacity());
    for (int i = 0; i < 4; ++i) assert(!memcmp(fs_data(ids[i]), media, fs_size(ids[i])));
    assert(fs_write(ids[1], "small", 5) == 5);
    assert(fs_write(empty, media, FS_FILE_MAX - 5) == FS_FILE_MAX - 5);
    assert(fs_used_bytes() == fs_capacity());
    assert(fs_sync() == 0); data_remount();

    /* Unknown disks and unreadable migration sources are never initialized. */
    reset(); checkpoint("recoverable");
    memset(data_disk, 0, sizeof(data_disk)); data_present = 1;
    memcpy(old_data, data_disk, sizeof(data_disk));
    fs_init(); assert(fs_load_disk() < 0); expect_value("recoverable");
    assert(fs_storage_status());
    fs_write(file(), "RAM only", 8); assert(fs_sync() < 0);
    assert(!memcmp(old_data, data_disk, sizeof(data_disk)) && data_writes == 0);
    marked_disk(); read_fail_slot = 1;
    fs_init(); assert(fs_load_disk() < 0 && fs_storage_status());
    memcpy(old_data, data_disk, sizeof(data_disk));
    fs_write(file(), "pending", 7);
    assert(fs_sync() < 0 && !memcmp(old_data, data_disk, sizeof(data_disk)));
    read_fail_slot = -1;
    data_remount(); assert(fs_needs_sync());
    assert(fs_sync() == 0);

    /* A failed payload write preserves the previous completed generation. */
    checkpoint("data checkpoint");
    fs_write(file(), "pending", 7);
    data_write_error = 1; assert(fs_sync() < 0);
    data_write_error = 0; data_remount(); expect_value("data checkpoint");
    fs_write(file(), "pending", 7);
    data_flush_error = 1; assert(fs_sync() < 0);
    data_flush_error = 0; data_remount(); expect_value("data checkpoint");
    /* If header write completed but its flush failed, do not retry against
     * the peer until a remount resolves the uncertain commit outcome. */
    fs_write(file(), "new checkpoint", 14);
    data_fail_flush_at = data_flush_count + 2;
    assert(fs_sync() < 0 && fs_storage_status());
    int writes_before = data_writes;
    assert(fs_sync() < 0 && data_writes == writes_before);
    data_fail_flush_at = 0; data_remount(); expect_value("new checkpoint");
    data_present = 0; fs_init(); assert(fs_load_disk() == 0);
    expect_value("recoverable"); assert(fs_file_limit() == FS_MAX_SIZE - 1);
    assert(fs_write(file(), media, 20000) < 0); expect_value("recoverable");
    puts("data volume: 2 MiB files, full capacity, compaction aliases, copy rollback, migration, protected media, failure recovery passed");
}
