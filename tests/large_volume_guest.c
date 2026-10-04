/* Ordinary data-volume entry for disposable 128 MiB (or protected 64 MiB) boots. */
#include "platform.h"
#include "persist.h"
#include "fs.h"
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
void large_volume_guest(void) {
    platform_validate_memory();
    disk_configure(((BootInfo *)BOOTINFO_ADDR)->sectors_per_track);
    fs_init(); int loaded = fs_load_disk();
    platform_log("LARGE-VOLUME-TEST-START\n");
#if LARGE_VOLUME_MODE == 2
    check(loaded < 0 && !fs_large_profile() && fs_storage_status(), "large: low RAM did not protect disk");
    check(fs_file_limit() == FS_MAX_SIZE - 1 && !fs_large_arenas_available(), "large: low RAM selected high arenas");
    verify(fs_find_child(0, "legacy"), 12345, 0);
    check(fs_write(fs_find_child(0, "legacy"), "RAM only", 8) == 8 && fs_sync() < 0,
          "large: protected disk accepted sync");
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
    check(fs_file_limit() == FS_LARGE_FILE_MAX && fs_node_limit() == 256, "large: limits wrong");
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
        create("remainder", 33543168 - FS_LARGE_FILE_MAX);
        while (fs_node_count() < 256) {
            char name[FS_NAME_LEN]; check(fs_unique_file(0, name) == 0 && fs_create(0, name) > 0,
                                          "large: node creation failed");
        }
        check(fs_used_bytes() == 33543168 && fs_capacity() == 33543168, "large: full capacity mismatch");
        sync_disk(); platform_log("LARGE-FULL-WRITE-PASS\n");
    } else {
        int remainder = fs_find_child(0, "remainder");
        verify(maximum, FS_LARGE_FILE_MAX, 0);
        verify(remainder, 33543168 - FS_LARGE_FILE_MAX, 0);
        check(fs_node_count() == 256 && fs_used_bytes() == fs_capacity(), "large: persisted capacity mismatch");
        check(fs_write(3, "x", 1) < 0 && fs_copy(maximum, 0) < 0 && !fs_needs_sync(),
              "large: rejected operation mutated state");
        if (!fs_is_app(255)) {
            check(fs_delete(255) == 0 && fs_capacity() == 33543208, "large: metadata reclaim failed");
            check(fs_create_app(0, "last-app") == 255, "large: slot reclaim failed");
            fill(FS_LARGE_FILE_MAX);
            check(fs_write(maximum, scratch, FS_LARGE_FILE_MAX) == FS_LARGE_FILE_MAX,
                  "large: full volume overwrite failed");
            sync_disk(); platform_log("LARGE-FULL-REBOOT-SAVE-PASS\n");
        } else {
            sync_disk(); platform_log("LARGE-FULL-READONLY-REBOOT-PASS\n");
        }
    }
#endif
    for (;;) __asm__ volatile("hlt");
}
