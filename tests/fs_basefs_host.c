/* kernel/fs.c on BaseFS 2: formatting a blank marked disk, saves that write
 * only changed files in bounded steps, power cuts during saves, and the
 * one-time conversion of snapshot disks including a cut at every write. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "kernel/platform.h"
static unsigned char node_arena[FS_CAPACITY];
static unsigned char image_arena[FS_IMG_CAPACITY];
static unsigned char pool_arena[FS_POOL_CAPACITY];
#undef FS_BASE
#undef FS_IMG_BASE
#undef FS_POOL_BASE
#define FS_BASE ((uintptr_t)node_arena)
#define FS_IMG_BASE ((uintptr_t)image_arena)
#define FS_POOL_BASE ((uintptr_t)pool_arena)
int platform_memory_range_available(uint32_t base, uint32_t end) { (void)base; (void)end; return 0; }
#define FS_CONVERT_TO_BASEFS 1
static int transport_poll(int operation, unsigned lba, unsigned sectors);
#define TEST_ATA_ASYNC_POLL(operation, lba, sectors) transport_poll(operation, lba, sectors)
#include "fs_basefs_support.h"
#include "../src/kernel/fs.c"
#include "ata_async_stub.h"

static unsigned char data_disk[DATA_DISK_SECTORS * SECTOR_SIZE], saved[sizeof(data_disk)];
static unsigned data_sectors = DATA_DISK_SECTORS;
static long write_budget = -1;      /* sectors before a power cut; <0 unlimited */
static int cut, read_errors_after = -1;
static long fail_read_at = -1;      /* fail only this read (counted from 0) */
static long fail_lba = -1;          /* fail every read of this sector */
static long reads;
static unsigned restored;
static unsigned sectors_written, max_poll;
int ata_probe(void) { return 1; }
unsigned ata_sector_count(void) { return data_sectors; }
int ata_read(unsigned lba, void *buffer, int sectors) {
    if (cut || sectors < 0 || lba > data_sectors || (unsigned)sectors > data_sectors - lba) return -1;
    if (read_errors_after == 0) return -1;
    if (read_errors_after > 0) --read_errors_after;
    if (reads++ == fail_read_at) return -1;
    if (fail_lba >= 0 && (long)lba <= fail_lba && fail_lba < (long)lba + sectors) return -1;
    memcpy(buffer, data_disk + (size_t)lba * SECTOR_SIZE, (size_t)sectors * SECTOR_SIZE);
    return 0;
}
int ata_write(unsigned lba, const void *buffer, int sectors) {
    assert(sectors >= 0 && lba + (unsigned)sectors <= data_sectors);
    for (int i = 0; i < sectors; ++i) {
        if (cut || write_budget == 0) { cut = 1; return -1; }
        if (write_budget > 0) --write_budget;
        memcpy(data_disk + (size_t)(lba + i) * SECTOR_SIZE, (const unsigned char *)buffer + (size_t)i * SECTOR_SIZE,
               SECTOR_SIZE);
        ++sectors_written;
    }
    return 0;
}
int ata_flush(void) { return cut ? -1 : 0; }
static int transport_poll(int operation, unsigned lba, unsigned sectors) {
    (void)operation; (void)lba;
    if (sectors > max_poll) max_poll = sectors;
    return 0;
}
static unsigned char floppy[DISK_SECTORS * SECTOR_SIZE];
static unsigned now;
uint32_t timer_ticks(void) { return now; }
void platform_log(const char *s) {
    if (strstr(s, "snapshot format restored")) ++restored;
}
unsigned disk_sector_count(void) { return DISK_SECTORS; }
int disk_read(unsigned lba, void *buffer, int sectors) {
    memcpy(buffer, floppy + (size_t)lba * SECTOR_SIZE, (size_t)sectors * SECTOR_SIZE);
    return 0;
}
int disk_write(unsigned lba, const void *buffer, int sectors) {
    memcpy(floppy + (size_t)lba * SECTOR_SIZE, buffer, (size_t)sectors * SECTOR_SIZE);
    return 0;
}

static void power_on(void) { write_budget = -1; cut = 0; read_errors_after = -1; fail_read_at = -1; fail_lba = -1; }
static void marked_blank(void) {
    memset(data_disk, 0, sizeof(data_disk));
    unsigned *m = (unsigned *)data_disk;
    m[0] = DATA_MARKER_MAGIC; m[1] = DATA_MARKER_VERSION;
    m[2] = DATA_DISK_SECTORS; m[3] = DATA_SLOT_SECTORS;
    m[4] = DATA_FIRST_LBA; m[5] = DATA_SECOND_LBA; m[6] = crc32(m, 24);
}
static int mount(void) { power_on(); fs_init(); return fs_load_disk(); }
static int put(const char *name, const char *text, int length) {
    int id = fs_find_child(0, name);
    if (id < 0) id = fs_create(0, name);
    assert(id >= 0);
    return fs_write(id, text, length);
}
static void expect(const char *name, const char *text, int length) {
    int id = fs_find_child(0, name);
    assert(id >= 0 && fs_size(id) == length && !memcmp(fs_data(id), text, (size_t)length));
}
static int is_basefs(void) {
    BfsDevice device = bfs_device();
    return bfs_probe(&device);
}

static char big[1u << 20], other[300000], copy[300000];

int main(void) {
    for (unsigned i = 0; i < sizeof big; ++i) big[i] = (char)(i * 7 + i / 4096);
    for (unsigned i = 0; i < sizeof other; ++i) other[i] = (char)(i * 13);

    /* A blank marked disk becomes BaseFS 2 and keeps the boot files. */
    marked_blank();
    fs_init();
    int seeded = fs_node_count();
    assert(fs_load_disk() >= 0 && is_basefs() && fs_writes_allowed() && fs_needs_sync());
    assert(!strcmp(fs_storage_name(), "BaseFS 2 data disk") && fs_node_count() == seeded);
    assert(fs_sync() == 0 && !fs_needs_sync());
    assert(put("big.bin", big, sizeof big) == (int)sizeof big);
    assert(put("other.bin", other, sizeof other) == (int)sizeof other);
    int folder = fs_mkdir(0, "folder");
    assert(folder >= 0 && fs_create(folder, "inner.txt") >= 0);
    assert(fs_sync() == 0);
    assert(mount() == 0 && fs_node_count() == seeded + 4);
    expect("big.bin", big, sizeof big);
    expect("other.bin", other, sizeof other);
    assert(fs_find_child(fs_find_child(0, "folder"), "inner.txt") >= 0);

    /* A change to one small file rewrites that file and the node table only,
     * in steps of at most eight sectors; files stay locked meanwhile. */
    sectors_written = max_poll = 0;
    assert(put("note.txt", "hello", 5) == 5);
    FsSyncTicket ticket;
    assert(fs_sync_request(&ticket) == 0 && fs_sync_busy());
    assert(fs_write(fs_find_child(0, "note.txt"), "x", 1) == FS_ERR_BUSY);
    unsigned steps = 0;
    while (fs_sync_busy()) {
        assert(fs_sync_step() != FS_SYNC_IDLE);
        assert(++steps < 10000);
    }
    assert(fs_sync_result(ticket) == 0 && fs_sync_release(ticket) == 0);
    assert(max_poll <= 8 && sectors_written < 64);
    assert(mount() == 0);
    expect("note.txt", "hello", 5);
    expect("big.bin", big, sizeof big);

    /* Rewriting the large file many times reuses space. */
    for (int round = 0; round < 12; ++round) {
        big[round] ^= 1;
        assert(put("big.bin", big, sizeof big) == (int)sizeof big);
        assert(fs_sync() == 0);
    }
    assert(mount() == 0);
    expect("big.bin", big, sizeof big);

    /* A power cut at any write of a save leaves the old or the new state. */
    memcpy(saved, data_disk, sizeof data_disk);
    unsigned outcomes[2] = {0, 0};
    for (long budget = 0;; budget += 3) {
        memcpy(data_disk, saved, sizeof data_disk);
        assert(mount() == 0);
        assert(put("note.txt", "changed", 7) == 7);
        assert(put("other.bin", big, 70000) == 70000);
        write_budget = budget;
        int result = fs_sync();
        assert(mount() == 0);
        int changed = fs_size(fs_find_child(0, "note.txt")) == 7;
        if (changed) {
            expect("note.txt", "changed", 7);
            expect("other.bin", big, 70000);
        } else {
            expect("note.txt", "hello", 5);
            expect("other.bin", other, sizeof other);
        }
        expect("big.bin", big, sizeof big);
        ++outcomes[changed];
        if (result == 0) { assert(changed); break; }
    }
    assert(outcomes[0] > 5 && outcomes[1] >= 1);

    /* A save that fails before its superblock gives its blocks back. */
    assert(mount() == 0);
    uint64_t free_before = bfs_free_bytes(&bfs);
    assert(put("note.txt", "again", 5) == 5);
    read_errors_after = 0;
    assert(fs_sync() < 0 && fs_needs_sync() && fs_storage_status());
    read_errors_after = -1;
    assert(bfs_free_bytes(&bfs) == free_before);
    assert(fs_sync() == 0);
    assert(mount() == 0);
    expect("note.txt", "again", 5);

    /* Conversion of a snapshot disk, whichever slot is current. */
    for (int slot = 0; slot < 2; ++slot) {
        marked_blank();
        power_on();
        fs_init();
        data_backend = 1; writable = 1; active_slot = -1;
        assert(put("big.bin", big, sizeof big) == (int)sizeof big);
        assert(put("other.bin", other, sizeof other) == (int)sizeof other);
        for (int k = 0; k <= slot; ++k) { fs_touched = 1; assert(save_snapshot() == 0); }
        assert(active_slot == slot && !is_basefs());
        memcpy(saved, data_disk, sizeof data_disk);
        assert(mount() == 0 && is_basefs() && data_backend == 3 && !fs_needs_sync());
        expect("big.bin", big, sizeof big);
        expect("other.bin", other, sizeof other);
        assert(put("after.txt", "converted", 9) == 9 && fs_sync() == 0);
        assert(mount() == 0);
        expect("after.txt", "converted", 9);
    }

    /* A power cut at any write of a conversion: the next boot finds the
     * files either way, and finishes the conversion. */
    unsigned converted = 0, kept = 0;
    for (long budget = 0;; budget += 7) {
        memcpy(data_disk, saved, sizeof data_disk);
        write_budget = budget;
        cut = 0;
        fs_init();
        (void)fs_load_disk();
        int finished = !cut;
        if (is_basefs()) ++converted; else ++kept;
        assert(mount() == 0 && is_basefs());
        expect("big.bin", big, sizeof big);
        expect("other.bin", other, sizeof other);
        if (finished) break;
    }
    assert(kept > 5 && converted >= 1);

    /* One failed read anywhere in a conversion: when it hits the readback of
     * the new volume, the old marker goes back and the snapshot format stays;
     * either way the next boot has every file. */
    unsigned attempts = 0;
    for (long at = 0; at < 400 && !restored; at += 3, ++attempts) {
        memcpy(data_disk, saved, sizeof data_disk);
        power_on();
        fs_init();
        reads = 0;
        fail_read_at = at;
        (void)fs_load_disk();
        if (restored) assert(data_backend == 1 && !is_basefs());
        assert(mount() == 0);
        expect("big.bin", big, sizeof big);
        expect("other.bin", other, sizeof other);
    }
    assert(restored);

    /* A snapshot disk too full to convert is left alone without a write. */
    marked_blank();
    power_on();
    fs_init();
    data_backend = 1; writable = 1; active_slot = -1;
    static char fill[FS_FILE_MAX];
    for (int i = 0; i < 4; ++i) {
        char name[] = "full0"; name[4] = (char)('0' + i);
        int length = i < 3 ? FS_FILE_MAX : (int)fs_capacity() - (int)fs_used_bytes();
        assert(put(name, fill, length) == length);
    }
    fs_touched = 1; assert(save_snapshot() == 0);
    sectors_written = 0;
    assert(mount() == 0 && !is_basefs() && data_backend == 1 && sectors_written == 0);
    expect("full3", fill, fs_size(fs_find_child(0, "full3")));

    /* BaseFS 2 disks need no fixed size. */
    data_sectors = DATA_DISK_SECTORS - 8 * 77;
    memset(data_disk, 0, sizeof data_disk);
    BfsDevice device = bfs_device();
    assert(bfs_format(&device, 5) == BFS_OK);
    assert(mount() == 0 && data_backend == 3 && fs_node_count() == 1);
    assert(put("odd.txt", "size", 4) == 4 && fs_sync() == 0);
    assert(mount() == 0);
    expect("odd.txt", "size", 4);

    /* Changes are admitted only while their save still fits: a small disk
     * fills well past half and every accepted change saves. Room to rewrite
     * the largest file is held back, so rewrites of a full disk still save,
     * and a refused write leaves the file as it was. */
    data_sectors = 8192;
    memset(data_disk, 0, sizeof data_disk);
    device = bfs_device();
    assert(bfs_format(&device, 5) == BFS_OK);
    assert(mount() == 0);
    char name[] = "fillA";
    int files = 0, refused = 0;
    for (;; ++files) {
        name[4] = (char)('A' + files);
        if (put(name, other, sizeof other) != (int)sizeof other) break;
        assert(fs_used_bytes() <= fs_capacity() && fs_sync() == 0);
    }
    assert(files >= 10 && fs_used_bytes() > 3u * 1024 * 1024);
    name[4] = (char)('A' + files);
    int last = fs_find_child(0, name);
    assert(last > 0 && fs_size(last) == 0 && fs_write(last, other, sizeof other) == -1 && fs_size(last) == 0);
    for (int round = 0; round < 24; ++round) {
        name[4] = (char)('A' + round % files);
        int length = round % 3 ? 4096 * (round % 5) + 7 : (int)sizeof other;
        int id = fs_find_child(0, name), before = fs_size(id);
        memcpy(copy, fs_data(id), (size_t)before);
        if (fs_write(id, big + round, length) == length) assert(fs_sync() == 0);
        else { ++refused; assert(fs_size(id) == before && !memcmp(fs_data(id), copy, (size_t)before)); }
    }
    assert(refused == 0);
    assert(mount() == 0);

    /* A volume too small for a larger node table refuses the new node
     * instead of accepting a change that cannot be saved. */
    data_sectors = 5 * BFS_SECTORS_PER_BLOCK;
    memset(data_disk, 0, sizeof data_disk);
    device = bfs_device();
    assert(bfs_format(&device, 5) == BFS_OK);
    assert(mount() == 0);
    char folder_name[] = "d000";
    int folders = 0;
    for (;; ++folders) {
        folder_name[1] = (char)('0' + folders / 100);
        folder_name[2] = (char)('0' + folders / 10 % 10);
        folder_name[3] = (char)('0' + folders % 10);
        if (fs_mkdir(0, folder_name) < 0) break;
        assert(fs_sync() == 0);
    }
    assert(folders > 10 && folders < FS_MAX_NODES - 1 && !fs_needs_sync() && mount() == 0);
    assert(fs_node_count() == folders + 1);
    data_sectors = DATA_DISK_SECTORS;

    /* An unreadable superblock may hold the newest commit: the volume does
     * not mount as BaseFS 2 (so nothing writes over it), and it mounts with
     * the newest files once the sector reads again. */
    memset(data_disk, 0, sizeof data_disk);
    device = bfs_device();
    assert(bfs_format(&device, 5) == BFS_OK);
    assert(mount() == 0 && put("first.txt", "one", 3) == 3 && fs_sync() == 0);
    assert(put("first.txt", "two", 3) == 3 && fs_sync() == 0);
    int newest = bfs.slot;
    power_on();
    fs_init();
    fail_lba = (1 + newest) * BFS_SECTORS_PER_BLOCK;
    (void)fs_load_disk();
    assert(data_backend != 3 && !fs_writes_allowed());
    assert(mount() == 0 && data_backend == 3 && bfs.slot == newest);
    expect("first.txt", "two", 3);

    /* Files load on demand: a volume holding more than the RAM arena
     * mounts, every file reads back, and the arena never overflows. Writes
     * past the changed-data limit save first instead of failing. */
    memset(data_disk, 0, sizeof data_disk);
    device = bfs_device();
    assert(bfs_format(&device, 5) == BFS_OK);
    assert(mount() == 0);
    enum { CACHED_FILES = 5 };
    char cached[] = "cache0";
    unsigned saves_before = sectors_written;
    for (int i = 0; i < CACHED_FILES; ++i) {
        for (unsigned k = 0; k < FS_FILE_MAX; ++k) fill[k] = (char)(k * 31u + (unsigned)i * 101u + k / 8192u);
        cached[5] = (char)('0' + i);
        assert(put(cached, fill, FS_FILE_MAX) == FS_FILE_MAX);
        assert(pool_used <= pool_capacity && dirty_bytes(-1) <= dirty_limit());
    }
    assert(sectors_written > saves_before && fs_used_bytes() > FS_POOL_CAPACITY);
    assert(fs_sync() == 0 && mount() == 0 && pool_used == 0);
    for (int round = 0; round < 3; ++round)
        for (int i = 0; i < CACHED_FILES; ++i) {
            int at = round == 1 ? CACHED_FILES - 1 - i : i;
            for (unsigned k = 0; k < FS_FILE_MAX; ++k) fill[k] = (char)(k * 31u + (unsigned)at * 101u + k / 8192u);
            cached[5] = (char)('0' + at);
            expect(cached, fill, FS_FILE_MAX);
            assert(pool_used <= pool_capacity);
        }
    /* A copy from one file to another reads the source on demand. */
    int first = fs_find_child(0, "cache0");
    int twin = fs_copy(first, 0);
    assert(twin >= 0 && fs_size(twin) == FS_FILE_MAX && pool_used <= pool_capacity);
    for (unsigned k = 0; k < FS_FILE_MAX; ++k) fill[k] = (char)(k * 31u + k / 8192u);
    assert(!memcmp(fs_data(twin), fill, FS_FILE_MAX) && fs_delete(twin) == 0 && fs_sync() == 0);
    /* A read during a save finishes the save first. */
    assert(put("pending.txt", "pending", 7) == 7);
    assert(fs_sync_request(&ticket) == 0 && fs_sync_busy());
    (void)fs_sync_step();
    for (int i = 0; i < CACHED_FILES; ++i) {
        cached[5] = (char)('0' + i);
        (void)fs_data(fs_find_child(0, cached));
    }
    assert(!fs_sync_busy() && fs_sync_result(ticket) == 0 && fs_sync_release(ticket) == 0);
    assert(mount() == 0);
    expect("pending.txt", "pending", 7);
    /* A file that cannot be read shows zeros at its size, stays unchanged
     * on disk, and reads correctly once the disk answers again. */
    int victim = fs_find_child(0, "cache3");
    read_errors_after = 0;
    const char *zeros = fs_data(victim);
    for (unsigned k = 0; k < FS_FILE_MAX; ++k) assert(!zeros[k]);
    read_errors_after = -1;
    for (int i = 0; i < CACHED_FILES; ++i) {
        cached[5] = (char)('0' + i);
        (void)fs_data(fs_find_child(0, cached));
    }
    assert(!fs_needs_sync() && mount() == 0);
    for (unsigned k = 0; k < FS_FILE_MAX; ++k) fill[k] = (char)(k * 31u + 3u * 101u + k / 8192u);
    expect("cache3", fill, FS_FILE_MAX);

    /* A folder copy that fails after saving part of itself on the way
     * leaves the removal of that part to be saved. */
    memset(data_disk, 0, sizeof data_disk);
    device = bfs_device();
    assert(bfs_format(&device, 5) == BFS_OK);
    assert(mount() == 0);
    int batch = fs_mkdir(0, "Batch");
    assert(batch > 0);
    for (int i = 0; i < 4; ++i) {
        cached[5] = (char)('0' + i);
        int id = fs_create(batch, cached);
        assert(id > 0 && fs_write(id, fill, FS_FILE_MAX) == FS_FILE_MAX);
    }
    assert(fs_sync() == 0);
    int nodes_before = fs_node_count();
    unsigned saves = sectors_written;
    assert(fs_copy(batch, 0) == -1 && fs_node_count() == nodes_before);
    assert(sectors_written > saves && fs_needs_sync() && fs_sync() == 0);
    assert(mount() == 0 && fs_node_count() == nodes_before);

    printf("basefs volume: format, %u-step incremental save, reuse, %u+%u save cuts, conversion from "
           "both slots, %u+%u conversion cuts, restore after %u read faults, odd sizes, %d full-disk files with %d "
           "refused rewrites, a %d-folder small volume, an unreadable superblock and on-demand loading of %d files "
           "passed\n", steps, outcomes[0], outcomes[1], kept, converted, attempts, files, refused, folders,
           (int)CACHED_FILES);
    return 0;
}
