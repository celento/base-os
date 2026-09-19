#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "platform.h"
static unsigned char node_arena[FS_CAPACITY];
static unsigned char image_arena[FS_IMG_CAPACITY];
#undef FS_BASE
#undef FS_IMG_BASE
#define FS_BASE ((uintptr_t)node_arena)
#define FS_IMG_BASE ((uintptr_t)image_arena)
#include "../src/fs.c"

static unsigned char disk[DISK_SECTORS * SECTOR_SIZE];
static unsigned char saved[sizeof(disk)];
static unsigned now, capacity = DISK_SECTORS;
static int write_budget = -1, read_fail_slot = -1, write_calls;
uint32_t timer_ticks(void) { return now; }
void platform_log(const char *s) { (void)s; }
unsigned disk_sector_count(void) { return capacity; }
int disk_read(unsigned lba, void *buf, int sectors) {
    if (sectors < 0 || lba > capacity || (unsigned)sectors > capacity - lba) return -1;
    int slot = lba >= FS_SECOND_LBA;
    if (slot == read_fail_slot) return -1;
    memcpy(buf, disk + lba * SECTOR_SIZE, sectors * SECTOR_SIZE);
    return 0;
}
int disk_write(unsigned lba, const void *buf, int sectors) {
    assert(lba >= FS_DISK_LBA);
    assert(lba + sectors <= capacity);
    int bytes = sectors * SECTOR_SIZE;
    write_calls++;
    if (write_budget >= 0 && write_budget < bytes) {
        memcpy(disk + lba * SECTOR_SIZE, buf, write_budget);
        write_budget = 0;
        return -1;
    }
    memcpy(disk + lba * SECTOR_SIZE, buf, bytes);
    if (write_budget >= 0) write_budget -= bytes;
    return 0;
}
static void reset(void) {
    memset(disk, 0, sizeof(disk));
    now = 0; write_calls = 0; capacity = DISK_SECTORS;
    write_budget = read_fail_slot = -1;
    fs_init();
    assert(fs_load_disk() == FS_LOAD_BLANK);
}
static void remount(void) {
    fs_init();
    assert(fs_load_disk() == 0);
}
static int file(void) { return fs_find_child(0, "checkpoint"); }
static void checkpoint(const char *value) {
    int id = file();
    if (id < 0) id = fs_create(0, "checkpoint");
    assert(id >= 0);
    assert(fs_write(id, value, strlen(value)) == (int)strlen(value));
    assert(fs_sync() == 0);
}
static void expect_value(const char *value) {
    assert(file() >= 0);
    if (strcmp(fs_data(file()), value)) fprintf(stderr, "expected %s, got %s\n", value, fs_data(file()));
    assert(!strcmp(fs_data(file()), value));
}
int main(void) {
    reset();
    checkpoint("old");
    assert(!fs_needs_sync());
    remount(); expect_value("old");
    checkpoint("new"); remount(); expect_value("new");
    /* CRC failure in latest payload rolls back to previous valid snapshot. */
    disk[FS_SECOND_LBA * SECTOR_SIZE + SECTOR_SIZE + 5] ^= 0x10;
    remount(); expect_value("old");
    assert(!fs_storage_status());

    /* Power loss after every sector boundary and within the commit header. */
    reset(); checkpoint("old");
    memcpy(saved, disk, sizeof(disk));
    unsigned payload_bytes = ((DiskHeader *)(disk + FS_DISK_LBA * SECTOR_SIZE))->bytes;
    unsigned payload_span = (payload_bytes + 512 - 1) / 512 * 512;
    for (unsigned cut = 0; cut < payload_span + 512; cut += 31) {
        memcpy(disk, saved, sizeof(disk));
        write_budget = -1; remount();
        fs_write(file(), "new", 3);
        write_budget = cut;
        assert(fs_sync() < 0);
        assert(fs_needs_sync());
        write_budget = -1; remount();
        /* A partial sector write can contain the entire commit record. An
         * uncertain write must recover one complete generation, never a mix. */
        assert(!strcmp(fs_data(file()), "old") || !strcmp(fs_data(file()), "new"));
    }
    /* Read failure never permits overwriting the unreadable snapshot. */
    memcpy(disk, saved, sizeof(disk));
    read_fail_slot = 1; remount(); expect_value("old");
    int calls = write_calls;
    fs_write(file(), "new", 3); fs_autosync();
    assert(fs_sync() < 0 && write_calls == calls);
    read_fail_slot = -1;

    /* Damaged headers are not mistaken for a fresh disk, even if erased. */
    memcpy(disk, saved, sizeof(disk));
    memset(disk + FS_DISK_LBA * SECTOR_SIZE, 0, SECTOR_SIZE);
    fs_init(); assert(fs_load_disk() < 0);
    calls = write_calls; fs_autosync(); assert(write_calls == calls);

    /* A v1 snapshot migrates by writing slot 1, preserving all legacy bytes. */
    memcpy(disk, saved, sizeof(disk));
    DiskHeader *h = (DiskHeader *)(disk + FS_DISK_LBA * SECTOR_SIZE);
    unsigned char *old_payload=(unsigned char *)h+SECTOR_SIZE;
    unsigned read_pos=0,write_pos=0;
    for(unsigned i=0;i<h->count;i++){
        DiskNode node;memcpy(&node,old_payload+read_pos,sizeof node);read_pos+=sizeof node;
        memmove(old_payload+write_pos,&node,36);write_pos+=36;
        memmove(old_payload+write_pos,old_payload+read_pos,node.size);write_pos+=node.size;read_pos+=node.size;
    }
    h->bytes=write_pos;
    h->version=2;h->generation=7;h->sum=crc32(old_payload,h->bytes);h->header_sum=crc32(h,24);
    remount();expect_value("old");assert(fs_modified(file())==0);
    h->version = 1;
    h->sum = checksum((unsigned char *)h + SECTOR_SIZE, h->bytes);
    h->generation = h->header_sum = 0;
    memcpy(saved, disk, sizeof(disk));
    remount(); expect_value("old");
    checkpoint("new");
    assert(!memcmp(saved, disk, FS_SECOND_LBA * SECTOR_SIZE));
    remount(); expect_value("new");
    capacity = 2880;
    remount(); expect_value("old"); assert(fs_storage_status());
    capacity = DISK_SECTORS;

    /* A checksum-valid cycle is rejected before touching live state. */
    reset(); checkpoint("old");
    h = (DiskHeader *)(disk + FS_DISK_LBA * SECTOR_SIZE);
    unsigned char *payload = (unsigned char *)h + SECTOR_SIZE;
    DiskNode *root = (DiskNode *)payload;
    DiskNode *child = (DiskNode *)(payload + sizeof(*root));
    assert(child->is_dir); child->parent = child->id;
    h->sum = crc32(payload, h->bytes); h->header_sum = crc32(h, 24);
    fs_init(); int seeded = fs_node_count();
    assert(fs_load_disk() < 0 && fs_node_count() == seeded);

    /* Autosync retries are spaced and stop after three failures. */
    reset(); checkpoint("old"); fs_write(file(), "new", 3);
    write_budget = 0; calls = write_calls;
    fs_autosync(); assert(write_calls == calls + 1);
    for (int i = 0; i < 100; ++i) fs_autosync();
    assert(write_calls == calls + 1);
    now += 5 * TIMER_HZ; fs_autosync();
    now += 5 * TIMER_HZ; fs_autosync();
    assert(write_calls == calls + 3);
    now += 100 * TIMER_HZ; fs_autosync(); assert(write_calls == calls + 3);
    write_budget = -1; assert(fs_sync() == 0);

    /* Directory copy rolls back allocations when children cannot all fit. */
    reset();
    int folder = fs_mkdir(0, "source");
    fs_create(folder, "one"); fs_create(folder, "two");
    for (int i = 0; fs_node_count() < FS_MAX_NODES - 2; ++i) {
        char name[24]; snprintf(name, sizeof(name), "f%d", i);
        assert(fs_create(0, name) >= 0);
    }
    int before = fs_node_count();
    assert(fs_sync() == 0);
    assert(fs_copy(folder, 0) < 0);
    assert(fs_node_count() == before && !fs_needs_sync());
    assert(fs_find_child(0, "source copy") < 0);
    assert(fs_child_count(folder) == 2);

    /* Maximum file capacity still fits in each snapshot and round trips. */
    reset();
    fs_empty_dir(0);
    static char content[FS_MAX_SIZE - 1]; memset(content, 'Z', sizeof(content));
    for (int i = 1; i < FS_MAX_NODES; ++i) {
        char name[24]; snprintf(name, sizeof(name), "full%d", i);
        int id = fs_create(0, name); assert(id >= 0);
        assert(fs_write(id, content, sizeof(content)) == sizeof(content));
    }
    assert(fs_sync() == 0); remount(); assert(fs_node_count() == FS_MAX_NODES);
    assert(fs_size(fs_find_child(0, "full63")) == FS_MAX_SIZE - 1);
    assert(fs_create(0, "overflow") < 0);
    assert(fs_create(0, "../bad") < 0);
    puts("filesystem: round trips, interrupted commits, CRC/graph corruption, migration, read errors, retries, rollback, full capacity passed");
    return 0;
}
