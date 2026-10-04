/* Linked only into disposable images by tools/data_volume_test.py.
 * No desktop/audio startup or autosave: all media writes below are explicit.
 * Modes: 1 large binary roundtrip, 2 v3 migration, 3 floppy, 4 protected IDE,
 *        5 full-capacity save/restart/reclaim/refill.
 */
#include "platform.h"
#include "persist.h"
#include "fs.h"

#ifndef DATA_VOLUME_MODE
#define DATA_VOLUME_MODE 1
#endif

#define LARGE_SIZE 2097152u
#define HOST_SIZE 262177u
#define LEGACY_SIZE 12017u
#define FLOPPY_SIZE 13001u
#define TOTAL_SIZE 8385024u

/* Audio is never started in this fixture. Keep scratch outside the FS arenas. */
#define scratch ((char *)AUDIO_WORK_BASE)
_Static_assert(LARGE_SIZE <= AUDIO_WORK_CAPACITY, "test scratch is too small");

static void require(int condition, const char *message) {
    if (!condition) panic(message);
}

static unsigned char pattern(unsigned i, unsigned seed) {
    return (unsigned char)(i * 37u + (i >> 16) + seed);
}

static void fill(unsigned length, unsigned seed, unsigned mask) {
    for (unsigned i = 0; i < length; ++i)
        scratch[i] = (char)(pattern(i, seed) ^ mask);
}

static void verify(int id, unsigned length, unsigned seed, unsigned mask) {
    require(id > 0, "data test: expected file missing");
    require(fs_size(id) == (int)length, "data test: file length mismatch");
    const unsigned char *bytes = (const unsigned char *)fs_data(id);
    for (unsigned i = 0; i < length; ++i)
        require(bytes[i] == (pattern(i, seed) ^ mask), "data test: binary byte mismatch");
    require(bytes[length] == 0, "data test: convenience NUL missing");
}

static int create_pattern(const char *name, unsigned length, unsigned seed) {
    int id = fs_create(0, name);
    require(id > 0, "data test: create failed");
    fill(length, seed, 0);
    require(fs_write(id, scratch, length) == (int)length, "data test: write failed");
    verify(id, length, seed, 0);
    return id;
}

static void synced(void) {
    require(fs_sync() == 0, "data test: sync failed");
    require(!fs_needs_sync(), "data test: sync left dirty state");
    require(!fs_storage_status(), "data test: unexpected storage warning");
}

#if DATA_VOLUME_MODE == 5
static void verify_full(int refilled) {
    require(fs_capacity() == TOTAL_SIZE && fs_used_bytes() == TOTAL_SIZE,
            "data test: full-volume capacity mismatch");
    for (unsigned i = 0; i < 4; ++i) {
        char name[] = "full0.bin";
        name[4] += i;
        int id = fs_find_child(0, name);
        if (refilled && i == 0) {
            require(id > 0 && fs_size(id) == 5 && !kstrcmp(fs_data(id), "small"),
                    "data test: shrunken file mismatch");
        } else {
            unsigned length = i < 3 ? LARGE_SIZE : TOTAL_SIZE - 3 * LARGE_SIZE;
            verify(id, length, 101 + i * 7, 0);
        }
    }
    int reclaimed = fs_find_child(0, "reclaimed.bin");
    if (refilled) verify(reclaimed, LARGE_SIZE - 5, 211, 0);
    else require(reclaimed > 0 && fs_size(reclaimed) == 0,
                 "data test: unexpected reclaimed data");
}

static void reject_full_writes(void) {
    int empty = fs_find_child(0, "blocked.bin");
    require(empty > 0 && fs_size(empty) == 0, "data test: empty full-volume file missing");
    require(!fs_needs_sync(), "data test: rejection baseline not clean");
    int count = fs_node_count();
    require(fs_write(empty, "!", 1) == -1 && fs_size(empty) == 0,
            "data test: full-volume write did not reject atomically");
    require(fs_copy(fs_find_child(0, "full1.bin"), 0) == -1,
            "data test: full-volume copy did not reject");
    require(fs_node_count() == count && !fs_needs_sync(),
            "data test: rejected full-volume operation changed metadata");
    require(fs_used_bytes() == TOTAL_SIZE, "data test: rejected operation changed usage");
}
#endif

void data_volume_guest(void) {
    platform_validate_memory();
    disk_configure(((BootInfo *)BOOTINFO_ADDR)->sectors_per_track);
    fs_init();
    platform_log("DATA-VOLUME-TEST-START\n");
    int loaded = fs_load_disk();

#if DATA_VOLUME_MODE == 1
    require(!kstrcmp(fs_storage_name(), "IDE data disk"), "data test: IDE not selected");
    require(fs_file_limit() == LARGE_SIZE, "data test: wrong per-file limit");
    require(fs_capacity() == 8385024u, "data test: wrong total capacity");
    if (loaded == FS_LOAD_BLANK) {
        fs_empty_dir(0);
        create_pattern("large.bin", LARGE_SIZE, 17);
        int stage = fs_create(0, "stage");
        require(stage > 0 && fs_write(stage, "W", 1) == 1, "data test: stage write failed");
        synced();
        platform_log("DATA-LARGE-WRITE-PASS\n");
    } else {
        require(loaded == 0, "data test: existing data mount failed");
        verify(fs_find_child(0, "large.bin"), LARGE_SIZE, 17, 0);
        int stage = fs_find_child(0, "stage");
        require(stage > 0 && fs_size(stage) == 1, "data test: stage missing");
        char state = *fs_data(stage);
        if (state == 'W') {
            require(fs_write(stage, "V", 1) == 1, "data test: restart stage failed");
            synced();
            platform_log("DATA-LARGE-RESTART-PASS\n");
        } else if (state == 'V') {
            int imported = fs_find_child(0, "host.bin");
            verify(imported, HOST_SIZE, 93, 0);
            platform_log("DATA-HOST-IMPORT-PASS\n");
            fill(HOST_SIZE, 93, 0xA5);
            require(fs_write(imported, scratch, HOST_SIZE) == (int)HOST_SIZE,
                    "data test: imported overwrite failed");
            int directory = fs_mkdir(0, "Roundtrip");
            require(directory > 0, "data test: mkdir failed");
            require(fs_rename(imported, "edited.bin") == 0, "data test: rename failed");
            require(fs_move(imported, directory) == 0, "data test: move failed");
            int copy = fs_copy(fs_find_child(0, "large.bin"), directory);
            verify(copy, LARGE_SIZE, 17, 0);
            int temporary = fs_create(0, "temporary");
            require(temporary > 0 && fs_write(temporary, "delete me", 9) == 9,
                    "data test: temporary write failed");
            require(fs_delete(temporary) == 0, "data test: delete failed");
            require(fs_write(stage, "E", 1) == 1, "data test: edited stage failed");
            synced();
            platform_log("DATA-HOST-EDIT-PASS\n");
        } else {
            require(state == 'E', "data test: unknown stage");
            int directory = fs_find_child(0, "Roundtrip");
            require(directory > 0, "data test: roundtrip directory missing");
            verify(fs_find_child(directory, "edited.bin"), HOST_SIZE, 93, 0xA5);
            verify(fs_find_child(directory, "large.bin copy"), LARGE_SIZE, 17, 0);
            require(fs_find_child(0, "temporary") < 0, "data test: deletion not persisted");
            require(!fs_needs_sync(), "data test: read-only verification dirtied volume");
            platform_log("DATA-HOST-RESTART-PASS\n");
        }
    }
#elif DATA_VOLUME_MODE == 2
    require(loaded == 0, "data test: migration mount failed");
    require(!kstrcmp(fs_storage_name(), "IDE data disk"), "data test: migration backend wrong");
    require(fs_file_limit() == LARGE_SIZE, "data test: migration limit wrong");
    verify(fs_find_child(0, "old.bin"), LEGACY_SIZE, 41, 0);
    int pending = fs_needs_sync();
    synced();
    platform_log(pending ? "DATA-MIGRATION-WRITE-PASS\n" : "DATA-MIGRATION-RESTART-PASS\n");
#elif DATA_VOLUME_MODE == 3
    require(!kstrcmp(fs_storage_name(), "Boot floppy"), "data test: fallback backend wrong");
    require(fs_file_limit() == FS_MAX_SIZE - 1, "data test: fallback limit wrong");
    if (loaded == FS_LOAD_BLANK) {
        fs_empty_dir(0);
        create_pattern("floppy.bin", FLOPPY_SIZE, 71);
        synced();
        platform_log("DATA-FLOPPY-WRITE-PASS\n");
    } else {
        require(loaded == 0, "data test: fallback mount failed");
        verify(fs_find_child(0, "floppy.bin"), FLOPPY_SIZE, 71, 0);
        require(!fs_needs_sync(), "data test: floppy read dirtied volume");
        platform_log("DATA-FLOPPY-RESTART-PASS\n");
    }
#elif DATA_VOLUME_MODE == 4
    require(loaded < 0, "data test: unmarked IDE was accepted");
    require(!kstrcmp(fs_storage_name(), "Boot floppy"), "data test: protected backend wrong");
    require(fs_storage_status() != 0, "data test: missing protected warning");
    int old = fs_find_child(0, "old.bin");
    verify(old, LEGACY_SIZE, 41, 0);
    require(fs_write(old, "RAM only", 8) == 8, "data test: RAM edit failed");
    require(fs_sync() < 0, "data test: protected sync succeeded");
    platform_log("DATA-UNMARKED-PROTECTED-PASS\n");
#elif DATA_VOLUME_MODE == 5
    require(!kstrcmp(fs_storage_name(), "IDE data disk"), "data test: full-volume IDE not selected");
    require(fs_file_limit() == LARGE_SIZE && fs_capacity() == TOTAL_SIZE,
            "data test: full-volume limits wrong");
    if (loaded == FS_LOAD_BLANK) {
        fs_empty_dir(0);
        require(fs_create(0, "full-written") > 0, "data test: full-volume stage failed");
        require(fs_create(0, "blocked.bin") > 0, "data test: empty file create failed");
        require(fs_create(0, "reclaimed.bin") > 0, "data test: reclaim file create failed");
        for (unsigned i = 0; i < 4; ++i) {
            char name[] = "full0.bin";
            name[4] += i;
            unsigned length = i < 3 ? LARGE_SIZE : TOTAL_SIZE - 3 * LARGE_SIZE;
            create_pattern(name, length, 101 + i * 7);
        }
        /* Equal-size overwrite needs no free capacity, including an aliased
         * source borrowed from the FS arena. Verify every file afterward. */
        fill(LARGE_SIZE, 101, 0);
        require(fs_write(fs_find_child(0, "full0.bin"), scratch, LARGE_SIZE) == (int)LARGE_SIZE,
                "data test: full-volume equal-size overwrite failed");
        int alias = fs_find_child(0, "full1.bin");
        require(fs_write(alias, fs_data(alias), LARGE_SIZE) == (int)LARGE_SIZE,
                "data test: full-volume aliased overwrite failed");
        synced();
        reject_full_writes();
        verify_full(0);
        platform_log("DATA-FULL-WRITE-PASS\n");
    } else {
        require(loaded == 0, "data test: full-volume restart mount failed");
        int stage = fs_find_child(0, "full-written");
        if (stage > 0) {
            reject_full_writes();
            verify_full(0);
            platform_log("DATA-FULL-RESTART-PASS\n");
            int first = fs_find_child(0, "full0.bin");
            require(fs_write(first, "small", 5) == 5, "data test: full-volume shrink failed");
            require(fs_used_bytes() == TOTAL_SIZE - LARGE_SIZE + 5,
                    "data test: shrink did not reclaim capacity");
            fill(LARGE_SIZE - 5, 211, 0);
            require(fs_write(fs_find_child(0, "reclaimed.bin"), scratch, LARGE_SIZE - 5)
                    == (int)LARGE_SIZE - 5, "data test: reclaimed refill failed");
            require(fs_rename(stage, "refilled") == 0, "data test: refill stage failed");
            synced();
            reject_full_writes();
            verify_full(1);
            platform_log("DATA-FULL-REFILL-PASS\n");
        } else {
            require(fs_find_child(0, "refilled") > 0, "data test: full-volume stage missing");
            reject_full_writes();
            verify_full(1);
            platform_log("DATA-FULL-REFILL-RESTART-PASS\n");
        }
    }
#else
#error Unsupported DATA_VOLUME_MODE
#endif
    for (;;) __asm__ volatile("hlt");
}
