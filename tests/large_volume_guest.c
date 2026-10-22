/* Ordinary data-volume entry for disposable 128 MiB (or protected 64 MiB) boots. */
#include "kernel/platform.h"
#include "kernel/persist.h"
#include "kernel/physmem.h"
#include "kernel/fs.h"
#ifndef LARGE_VOLUME_MODE
#define LARGE_VOLUME_MODE 1
#endif
/* Test-only scratch borrows the former low FS arenas after large selection.
 * No desktop/audio work starts here. Production consumers retain their limits. */
#define scratch ((char *)FS_POOL_BASE)
_Static_assert(FS_POOL_BASE + FS_LARGE_FILE_MAX <= TASK_BASE, "guest scratch overlap");
static void check(int ok, const char *why) { if (!ok) panic(why); }
static unsigned char pattern(unsigned i) { return (unsigned char)(i * 37 + (i >> 16) + 19); }
static void fill(unsigned size) { for (unsigned i = 0; i < size; ++i) scratch[i] = pattern(i); }
static void verify(int id, unsigned length, unsigned offset) {
    check(id > 0 && fs_size(id) == (int)length, "large: file length mismatch");
    const unsigned char *data = (const unsigned char *)fs_data(id);
    for (unsigned i = 0; i < length; ++i) check(data[i] == pattern(i + offset), "large: byte mismatch");
    check(data[length] == 0, "large: missing convenience NUL");
}
static int create(const char *name, unsigned length) {
    int id = fs_create(0, name); check(id > 0, "large: create failed");
    fill(length); check(fs_write(id, scratch, length) == (int)length, "large: write failed"); return id;
}
static void sync_disk(void) {
    check(fs_sync() == 0 && !fs_needs_sync() && !fs_storage_status(), "large: sync failed");
}
/* Fills empty files from node id on, stepping by step, until no byte is
 * left. A save can free checkpoint room, so the volume is full once saved. */
static void fill_volume(int id, int step) {
    for (;; id += step) {
        sync_disk();
        if (fs_used_bytes() == fs_capacity()) break;
        while (id > 3 && id < 256 && (fs_is_app(id) || fs_size(id))) id += step;
        check(id > 3 && id < 256, "large: ran out of nodes to fill");
        unsigned room = fs_capacity() - fs_used_bytes();
        if (room > FS_LARGE_FILE_MAX) room = FS_LARGE_FILE_MAX;
        fill(room); check(fs_write(id, scratch, room) == (int)room, "large: fill write failed");
    }
}
void large_volume_guest(void) {
    platform_validate_memory();
    disk_configure(((BootInfo *)BOOTINFO_ADDR)->sectors_per_track);
    /* BaseFS 2 volumes allocate from the kernel heap, as under kmain. */
    if (physmem_init() != PHYS_OK) platform_log("PANIC: physmem\n");
    fs_init(); int loaded = fs_load_disk();
    platform_log("LARGE-VOLUME-TEST-START\n");
#if LARGE_VOLUME_MODE == 2
    /* Without the large arenas, files past the 2 MiB limit are read in
     * pieces, so the volume still mounts. Nothing here writes to it. */
    check(loaded >= 0 && !fs_large_profile() && !fs_large_arenas_available(), "large: low RAM did not mount");
    check(fs_file_limit() == FS_FILE_MAX, "large: low RAM file limit");
    int maximum = fs_find_child(0, "maximum");
    check(maximum > 0 && fs_streamed(maximum) && !fs_data(maximum) && fs_size(maximum) == FS_LARGE_FILE_MAX,
          "large: low RAM did not stream the largest file");
    static char piece[65536];
    for (unsigned at = 0; at < FS_LARGE_FILE_MAX; at += sizeof piece) {
        check(fs_read_at(maximum, at, piece, sizeof piece) == (int)sizeof piece, "large: streamed read failed");
        for (unsigned i = 0; i < sizeof piece; ++i)
            check((unsigned char)piece[i] == pattern(at + i), "large: streamed byte mismatch");
    }
    check(!fs_needs_sync(), "large: reading changed the volume");
    platform_log("LARGE-LOW-RAM-PASS\n");
#elif LARGE_VOLUME_MODE == 3
    check(loaded >= 0 && fs_large_profile(), "large: migration failed");
    verify(fs_find_child(0, "legacy"), 12345, 0);
    check(fs_modified(fs_find_child(0, "legacy")) == 777, "large: migration timestamp changed");
    check(fs_is_app(fs_find_child(0, "App")) && fs_modified(fs_find_child(0, "App")) == 999,
          "large: migration app changed");
    sync_disk(); platform_log("LARGE-MIGRATION-PASS\n");
#else
    check(loaded >= 0 && fs_large_profile() && fs_large_arenas_available(), "large: high arenas unavailable");
    check(fs_file_limit() == FS_LARGE_FILE_MAX && fs_node_limit() >= 256, "large: limits wrong");
    int maximum = fs_find_child(0, "maximum");
    if (maximum < 0) {
        fs_empty_dir(0);
        int file = create("three-MiB", 3 * 1048576);
        int copied = fs_copy(file, 0); check(copied > 0, "large: copy failed");
        check(fs_write(file, fs_data(copied) + 17, 2 * 1048576 + 123) == 2 * 1048576 + 123,
              "large: alias resize failed");
        verify(file, 2 * 1048576 + 123, 17); verify(copied, 3 * 1048576, 0);
        check(fs_delete(file) == 0, "large: delete failed"); verify(copied, 3 * 1048576, 0);
        fs_empty_dir(0);
        maximum = create("maximum", FS_LARGE_FILE_MAX);
        check(fs_write(maximum, scratch, FS_LARGE_FILE_MAX + 1) < 0, "large: oversized file admitted");
        verify(maximum, FS_LARGE_FILE_MAX, 0);
        /* The volume holds at least the snapshot format's 33,543,168 bytes. */
        check(fs_capacity_for_nodes(256) >= 33543168u, "large: capacity below the snapshot format");
        while (fs_node_count() < 256) {
            char name[FS_NAME_LEN]; check(fs_unique_file(0, name) == 0 && fs_create(0, name) > 0,
                                          "large: node creation failed");
        }
        fill_volume(255, -1);
        check(fs_used_bytes() > 33543168u, "large: full volume below the snapshot format");
        sync_disk(); platform_log("LARGE-FULL-WRITE-PASS\n");
    } else {
        verify(maximum, FS_LARGE_FILE_MAX, 0);
        for (int id = 2; id < 256; ++id)
            if (!fs_is_app(id) && fs_size(id) > 0) verify(id, fs_size(id), 0);
        check(fs_node_count() == 256 && fs_used_bytes() == fs_capacity(), "large: persisted capacity mismatch");
        check(fs_write(3, "x", 1) < 0 && fs_copy(maximum, 0) < 0 && !fs_needs_sync(),
              "large: rejected operation mutated state");
        if (!fs_is_app(255)) {
            /* A deleted file's blocks are free once the delete is saved. */
            check(fs_delete(255) == 0, "large: delete failed"); sync_disk();
            check(fs_capacity() > fs_used_bytes(), "large: delete reclaimed no room");
            check(fs_create_app(0, "last-app") == 255, "large: slot reclaim failed");
            fill(FS_LARGE_FILE_MAX);
            check(fs_write(maximum, scratch, FS_LARGE_FILE_MAX) == FS_LARGE_FILE_MAX,
                  "large: full volume overwrite failed");
            fill_volume(4, 1); platform_log("LARGE-FULL-REBOOT-SAVE-PASS\n");
        } else {
            sync_disk(); platform_log("LARGE-FULL-READONLY-REBOOT-PASS\n");
        }
    }
#endif
    for (;;) __asm__ volatile("hlt");
}
