/* Supported large-volume operations on generated disks; no fault probes. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "platform.h"
static unsigned char node_arena[FS_CAPACITY], image_arena[FS_IMG_CAPACITY];
static unsigned char pool_arena[FS_POOL_CAPACITY];
static unsigned char high_pool[FS_LARGE_POOL_CAPACITY], high_image[FS_LARGE_IMG_CAPACITY];
static int high_available;
int platform_memory_range_available(uint32_t base, uint32_t end) {
    (void)base; assert(end == RAM_LARGE_REQUIRED_END); return high_available;
}
#undef FS_BASE
#undef FS_IMG_BASE
#undef FS_POOL_BASE
#undef FS_LARGE_POOL_BASE
#undef FS_LARGE_IMG_BASE
#define FS_BASE ((uintptr_t)node_arena)
#define FS_IMG_BASE ((uintptr_t)image_arena)
#define FS_POOL_BASE ((uintptr_t)pool_arena)
#define FS_LARGE_POOL_BASE ((uintptr_t)high_pool)
#define FS_LARGE_IMG_BASE ((uintptr_t)high_image)
#include "../src/fs.c"

static unsigned char floppy[DISK_SECTORS * SECTOR_SIZE], old_floppy[sizeof(floppy)];
static unsigned char disk[DATA_LARGE_DISK_SECTORS * SECTOR_SIZE], old_disk[sizeof(disk)];
static char bytes[FS_LARGE_FILE_MAX];
static int ide, writes, reads;
static int ide_readable = 1;
static unsigned ide_sectors = DATA_LARGE_DISK_SECTORS;
extern unsigned large_test_polls;
uint32_t timer_ticks(void) { return 0; }
void platform_log(const char *text) { (void)text; }
unsigned disk_sector_count(void) { return DISK_SECTORS; }
int disk_read(unsigned lba, void *out, int sectors) {
    assert(sectors >= 0 && lba + (unsigned)sectors <= DISK_SECTORS);
    memcpy(out, floppy + lba * SECTOR_SIZE, sectors * SECTOR_SIZE); return 0;
}
int disk_write(unsigned lba, const void *data, int sectors) {
    assert(lba >= FS_DISK_LBA && sectors >= 0 && lba + (unsigned)sectors <= DISK_SECTORS);
    memcpy(floppy + lba * SECTOR_SIZE, data, sectors * SECTOR_SIZE); return 0;
}
int ata_probe(void) { return ide; }
unsigned ata_sector_count(void) { return ide_sectors; }
int ata_read(unsigned lba, void *out, int sectors) {
    assert(sectors >= 0 && lba + (unsigned)sectors <= ide_sectors);
    ++reads; if (!ide_readable) return -1;
    memcpy(out, disk + lba * SECTOR_SIZE, sectors * SECTOR_SIZE); return 0;
}
int ata_write(unsigned lba, const void *data, int sectors) {
    assert(lba && sectors >= 0 && lba + (unsigned)sectors <= ide_sectors);
    ++writes; memcpy(disk + lba * SECTOR_SIZE, data, sectors * SECTOR_SIZE); return 0;
}
int ata_flush(void) { return 0; }
static void mark(void) {
    memset(disk, 0, sizeof(disk));
    unsigned marker[] = { DATA_MARKER_MAGIC, DATA_LARGE_MARKER_VERSION,
        DATA_LARGE_DISK_SECTORS, DATA_LARGE_SLOT_SECTORS,
        DATA_LARGE_FIRST_LBA, DATA_LARGE_SECOND_LBA, 0 };
    marker[6] = crc32(marker, 24); memcpy(disk, marker, sizeof(marker)); ide = 1;
}
static void reboot(void) {
    fs_init(); assert(fs_load_disk() == 0 && fs_large_profile());
    assert(fs_node_limit() == 256 && fs_file_limit() == FS_LARGE_FILE_MAX);
    assert(!fs_storage_status() && !fs_needs_sync());
}
static int create(const char *name, unsigned size) {
    int id = fs_create(0, name); assert(id > 0);
    assert(fs_write(id, bytes, size) == (int)size); return id;
}
static void verify(int id, unsigned length, unsigned offset) {
    assert(fs_size(id) == (int)length && !memcmp(fs_data(id), bytes + offset, length));
    assert(fs_data(id)[length] == 0);
}
int main(int argc, char **argv) {
    _Static_assert(DATA_DISK_SECTORS == 32768 && DATA_SLOT_SECTORS == 16383 &&
                   FS_FILE_MAX == 2097152 && FS_POOL_CAPACITY == 8388608 &&
                   FS_IMG_CAPACITY == 8388608 && RAM_REQUIRED_END == 0x3f00000,
                   "default profile changed");
    for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (char)(i * 37 + (i >> 16) + 19);
    fs_init(); assert(fs_load_disk() == FS_LOAD_BLANK);
    fs_empty_dir(0);
    int legacy = create("legacy", 12345), app = fs_create_app(0, "App");
    nodes[legacy].modified = 777; nodes[app].modified = 999;
    assert(fs_sync() == 0); memcpy(old_floppy, floppy, sizeof(floppy));
    mark(); high_available = 0; reads = writes = 0;
    fs_init(); assert(fs_load_disk() < 0 && !fs_large_profile() && reads == 1 && !writes);
    verify(legacy, 12345, 0);
    assert(strstr(fs_storage_status(), "128 MiB") && pool_base == FS_POOL_BASE);
    assert(fs_write(legacy, "RAM", 3) == 3 && fs_sync() < 0 && !writes);
    assert(!memcmp(old_floppy, floppy, sizeof(floppy)));
    puts("large profile: low-RAM marker-only read, protected floppy view and zero disk writes");
    /* A generic blank disk and an ordinary device-read error remain protected. */
    high_available = 1; memset(disk, 0, sizeof(disk));
    memcpy(old_disk, disk, sizeof(disk));
    fs_init(); assert(fs_load_disk() < 0 && !fs_large_profile());
    verify(legacy, 12345, 0);
    assert(fs_write(legacy, "RAM", 3) == 3 && fs_sync() < 0 && !writes);
    assert(!memcmp(old_disk, disk, sizeof(disk)));
    mark(); ide_readable = 0; memcpy(old_disk, disk, sizeof(disk));
    fs_init(); assert(fs_load_disk() < 0 && !fs_large_profile());
    verify(legacy, 12345, 0);
    assert(fs_write(legacy, "RAM", 3) == 3 && fs_sync() < 0 && !writes);
    assert(!memcmp(old_disk, disk, sizeof(disk)) && !memcmp(old_floppy, floppy, sizeof(floppy)));
    ide_readable = 1;
    puts("large profile: unmarked disk and ordinary read error preserve protected recovery");

    high_available = 1; fs_init(); assert(fs_load_disk() == 0 && fs_large_profile());
    assert(pool_base == FS_LARGE_POOL_BASE && image_base == FS_LARGE_IMG_BASE);
    verify(legacy, 12345, 0);
    assert(fs_modified(legacy) == 777 && fs_is_app(app) && fs_modified(app) == 999);
    assert(fs_needs_sync() && fs_sync() == 0); reboot(); verify(legacy, 12345, 0);
    assert(!memcmp(old_floppy, floppy, sizeof(floppy)));
    puts("large profile: legacy migration keeps high arena offsets, names, bytes, apps and timestamps");
    memset(pool_arena, 0xd3, sizeof(pool_arena));
    memset(image_arena, 0xe4, sizeof(image_arena));

    fs_empty_dir(0);
    int one = create("three-MiB", 3 * 1048576);
    int copy = fs_copy(one, 0); assert(copy > 0); verify(copy, 3 * 1048576, 0);
    assert(fs_write(one, fs_data(copy) + 17, 2 * 1048576 + 123) == 2 * 1048576 + 123);
    verify(one, 2 * 1048576 + 123, 17); verify(copy, 3 * 1048576, 0);
    assert(fs_write(copy, fs_data(copy) + 23, 3 * 1048576 - 23) == 3 * 1048576 - 23);
    verify(copy, 3 * 1048576 - 23, 23);
    assert(fs_delete(one) == 0); verify(copy, 3 * 1048576 - 23, 23);
    assert(fs_sync() == 0); reboot(); verify(copy, 3 * 1048576 - 23, 23);
    puts("large profile: >2 MiB copy, source aliases, resize, delete and remount are byte-exact");

    fs_empty_dir(0);
    unsigned polls = large_test_polls;
    one = create("maximum", FS_LARGE_FILE_MAX);
    assert(large_test_polls - polls >= FS_LARGE_FILE_MAX / 4096);
    verify(one, FS_LARGE_FILE_MAX, 0);
    assert(fs_write(one, bytes, FS_LARGE_FILE_MAX + 1) < 0); verify(one, FS_LARGE_FILE_MAX, 0);
    copy = create("remainder", fs_capacity() - FS_LARGE_FILE_MAX);
    while (fs_node_count() < 64) {
        char name[24]; snprintf(name, sizeof(name), "empty%d", fs_node_count());
        assert(fs_create(0, name) > 0);
    }
    assert(fs_capacity() == 33550848 && fs_used_bytes() == fs_capacity());
    assert(fs_sync() == 0); memcpy(old_disk, disk, sizeof(disk));
    /* A direct large remount also leaves former low staging/pool untouched. */
    assert(fs_load_disk() == 0 && fs_large_profile());
    /* fs_init during an earlier reboot legitimately reseeds low storage, so
     * take this ownership check within one mounted session. */
    memset(pool_arena, 0xd3, sizeof(pool_arena));
    memset(image_arena, 0xe4, sizeof(image_arena));
    assert(fs_write(one, fs_data(one), FS_LARGE_FILE_MAX) == FS_LARGE_FILE_MAX);
    assert(fs_sync() == 0 && fs_load_disk() == 0);
    for (unsigned i = 0; i < sizeof(pool_arena); ++i) assert(pool_arena[i] == 0xd3);
    for (unsigned i = 0; i < sizeof(image_arena); ++i) assert(image_arena[i] == 0xe4);
    memcpy(old_disk, disk, sizeof(disk));
    assert(fs_create(0, "node65") < 0 && fs_copy(one, 0) < 0);
    assert(fs_write(3, "x", 1) < 0 && !fs_needs_sync());
    assert(!memcmp(old_disk, disk, sizeof(disk))); verify(one, FS_LARGE_FILE_MAX, 0);
    reboot(); verify(one, FS_LARGE_FILE_MAX, 0);
    verify(copy, fs_capacity() - FS_LARGE_FILE_MAX, 0);
    unsigned old_size = fs_size(copy);
    assert(fs_write(copy, bytes, old_size - 39) == (int)old_size - 39);
    assert(fs_create(0, "node65") < 0);
    assert(fs_write(copy, bytes, old_size - 40) == (int)old_size - 40);
    assert(fs_create(0, "node65") == 64 && fs_capacity() == 33550808);
    assert(fs_write(copy, bytes, 33543168 - FS_LARGE_FILE_MAX) == 33543168 - FS_LARGE_FILE_MAX);
    while (fs_node_count() < 256) {
        char name[24]; snprintf(name, sizeof(name), "record%d", fs_node_count());
        assert(fs_create(0, name) > 0);
    }
    assert(fs_capacity() == 33543168 && fs_used_bytes() == fs_capacity());
    assert(fs_sync() == 0); reboot();
    verify(one, FS_LARGE_FILE_MAX, 0); verify(copy, 33543168 - FS_LARGE_FILE_MAX, 0);
    assert(fs_delete(255) == 0 && fs_capacity() == 33543208);
    assert(fs_create_app(0, "last-app") == 255 && fs_sync() == 0); reboot();
    assert(fs_is_app(255) && fs_node_count() == 256);
    puts("large profile: 16 MiB bound, full 64/256-node capacities, rejection atomicity and reclaim passed");

    memcpy(old_disk, disk, sizeof(disk)); high_available = 0; reads = writes = 0;
    fs_init(); assert(fs_load_disk() < 0 && !fs_large_profile() && reads == 1 && !writes);
    verify(legacy, 12345, 0);
    assert(!memcmp(old_disk, disk, sizeof(disk)) && !memcmp(old_floppy, floppy, sizeof(floppy)));
    if (argc == 2) {
        FILE *out = fopen(argv[1], "wb"); assert(out);
        assert(fwrite(disk, 1, sizeof(disk), out) == sizeof(disk)); assert(!fclose(out));
    }
    puts("large profile: populated large disk remains byte-exact under low-RAM refusal");
    /* Physical high RAM is not permission to repurpose an active low pool. */
    high_available = 1; ide_sectors = DATA_DISK_SECTORS;
    memset(disk, 0, sizeof(disk));
    unsigned marker[] = { DATA_MARKER_MAGIC, DATA_MARKER_VERSION,
        DATA_DISK_SECTORS, DATA_SLOT_SECTORS, DATA_FIRST_LBA, DATA_SECOND_LBA, 0 };
    marker[6] = crc32(marker, 24); memcpy(disk, marker, sizeof(marker));
    fs_init(); assert(fs_load_disk() == 0);
    assert(fs_large_arenas_available() && !fs_large_profile());
    assert(pool_base == FS_POOL_BASE && image_base == FS_IMG_BASE && fs_file_limit() == FS_FILE_MAX);
    verify(legacy, 12345, 0); assert(fs_sync() == 0);
    puts("large profile: 128 MiB plus default disk retains low arenas and old limits");
}
