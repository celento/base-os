/* Normal valid-volume expansion checks; linked into disposable images only. */
#include "platform.h"
#include "persist.h"
#include "fs.h"
#ifndef NODE_TEST_MODE
#define NODE_TEST_MODE 1
#endif
#define scratch ((char *)AUDIO_WORK_BASE)
static void require(int condition, const char *message) { if (!condition) panic(message); }
static void fill(unsigned length, unsigned seed) {
    for (unsigned i = 0; i < length; ++i) scratch[i] = (char)(i * 37u + (i >> 16) + seed);
}
static void verify(int id, unsigned length, unsigned seed) {
    require(id > 0 && fs_size(id) == (int)length, "node test: missing/wrong file size");
    const unsigned char *data = (const unsigned char *)fs_data(id);
    for (unsigned i = 0; i < length; ++i)
        require(data[i] == (unsigned char)(i * 37u + (i >> 16) + seed), "node test: file byte mismatch");
}
static void synced(void) {
    require(fs_sync() == 0 && !fs_needs_sync() && !fs_storage_status(), "node test: sync failed");
}
static void name_for(char *name, unsigned number) {
    kstrcpy(name, "doc000.bin");
    name[3] += number / 100; name[4] += number / 10 % 10; name[5] += number % 10;
}

void node_capacity_guest(void) {
    platform_validate_memory();
    disk_configure(((BootInfo *)BOOTINFO_ADDR)->sectors_per_track);
    fs_init(); int loaded = fs_load_disk();
    require(fs_node_limit() == 256 && fs_file_limit() == 2097152u, "node test: wrong IDE limits");
#if NODE_TEST_MODE == 1
    if (loaded == FS_LOAD_BLANK) {
        fs_empty_dir(0);
        int stage = fs_create(0, "stage"); require(stage > 0, "node test: stage create failed");
        require(fs_write(stage, "W", 1) == 1, "node test: stage write failed");
        int parent = 0;
        for (int i = 0; i < 60; ++i) {
            parent = fs_mkdir(parent, "abcdefghijklmnopqrstuvw");
            require(parent > 0, "node test: tree create failed");
        }
        for (unsigned i = 0; i < 190; ++i) {
            char name[FS_NAME_LEN]; name_for(name, i);
            int id = fs_create(0, name); require(id > 0, "node test: file create failed");
            fill(1025 + i, i);
            require(fs_write(id, scratch, 1025 + i) == (int)(1025 + i), "node test: file write failed");
        }
        int moved = fs_find_child(0, "doc189.bin");
        require(fs_move(moved, parent) == 0 && fs_rename(moved, "moved.bin") == 0,
                "node test: move/rename failed");
        int copy = fs_copy(fs_find_child(0, "doc188.bin"), parent);
        verify(copy, 1213, 188);
        require(fs_create_app(0, "Test App") > 0, "node test: app create failed");
        require(fs_create(0, "empty1") > 0 && fs_create(0, "empty2") > 0, "node test: fill slots failed");
        require(fs_node_count() == 256 && fs_capacity() == 8377344u, "node test: 256 count/capacity wrong");
        synced();
        require(fs_create(0, "overflow") == -1 && !fs_needs_sync(), "node test: full slots changed state");
        platform_log("NODE-256-WRITE-PASS\n");
    } else {
        require(loaded == 0 && fs_node_count() == 256, "node test: 256-node remount failed");
        int parent = 0;
        for (int i = 0; i < 60; ++i) {
            parent = fs_find_child(parent, "abcdefghijklmnopqrstuvw");
            require(parent > 0, "node test: nested folder missing");
        }
        int stage = fs_find_child(0, "stage");
        char state = *fs_data(stage);
        for (unsigned i = 0; i < 189; ++i) {
            char name[FS_NAME_LEN]; name_for(name, i);
            verify(fs_find_child(0, name), 1025 + i, i == 0 && state == 'H' ? 211 : i);
        }
        verify(fs_find_child(parent, "moved.bin"), 1214, 189);
        verify(fs_find_child(parent, "doc188.bin copy"), 1213, 188);
        require(fs_is_app(fs_find_child(0, "Test App")), "node test: app missing");
        char path[FS_PATH_LEN]; int moved = fs_find_child(parent, "moved.bin");
        fs_path(moved, path, sizeof(path));
        require(fs_resolve(0, path) == moved, "node test: full path mismatch");
        require(!fs_needs_sync(), "node test: read-only verification dirtied state");
        platform_log(state == 'H' ? "NODE-HOST-EXCHANGE-RESTART-PASS\n" : "NODE-256-RESTART-PASS\n");
    }
#elif NODE_TEST_MODE == 2
    require(loaded == 0, "node test: old full v4 mount failed");
    int grew = fs_find_child(0, "expanded");
    if (grew < 0) {
        require(fs_node_count() == 64 && fs_capacity() == 8385024u && fs_used_bytes() == 8385024u,
                "node test: old full v4 capacity changed");
        for (unsigned i = 0; i < 4; ++i) {
            char name[] = "large0"; name[5] += i;
            verify(fs_find_child(0, name), i < 3 ? FS_FILE_MAX : 8385024u - 3 * FS_FILE_MAX, i);
        }
        require(fs_create(0, "expanded") == -1 && !fs_needs_sync(), "node test: full v4 admitted metadata");
        int last = fs_find_child(0, "large3");
        int length = fs_size(last);
        require(fs_write(last, fs_data(last), length - 39) == length - 39, "node test: 39-byte shrink failed");
        synced();
        require(fs_create(0, "expanded") == -1 && !fs_needs_sync(), "node test: metadata fit in 39 bytes");
        require(fs_write(last, fs_data(last), length - 40) == length - 40, "node test: 40-byte shrink failed");
        require(fs_create(0, "expanded") > 0 && fs_node_count() == 65 && fs_used_bytes() == fs_capacity(),
                "node test: exact metadata reservation failed");
        synced();
        platform_log("NODE-OLD-FULL-EXPAND-PASS\n");
    } else {
        require(fs_node_count() == 65 && fs_capacity() == 8384984u && fs_used_bytes() == fs_capacity(),
                "node test: expanded full-volume remount wrong");
        for (unsigned i = 0; i < 4; ++i) {
            char name[] = "large0"; name[5] += i;
            verify(fs_find_child(0, name), i < 3 ? FS_FILE_MAX : 8385024u - 3 * FS_FILE_MAX - 40, i);
        }
        require(fs_delete(grew) == 0 && fs_capacity() == 8385024u, "node test: metadata reclaim failed");
        int last = fs_find_child(0, "large3"); unsigned length = fs_size(last) + 40;
        fill(length, 3);
        require(fs_write(last, scratch, length) == (int)length && fs_used_bytes() == fs_capacity(),
                "node test: reclaimed bytes refill failed");
        synced();
        platform_log("NODE-OLD-FULL-RESTART-RECLAIM-PASS\n");
    }
#endif
    for (;;) __asm__ volatile("hlt");
}
