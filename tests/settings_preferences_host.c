/* Ordinary functional workflows and deterministic I/O errors; no memory faults. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
static unsigned char node_arena[FS_CAPACITY], image_arena[FS_IMG_CAPACITY], pool_arena[FS_POOL_CAPACITY];
#undef FS_BASE
#undef FS_IMG_BASE
#undef FS_POOL_BASE
#define FS_BASE ((uintptr_t)node_arena)
#define FS_IMG_BASE ((uintptr_t)image_arena)
#define FS_POOL_BASE ((uintptr_t)pool_arena)
#include "../src/fs.c"
#include "ata_async_stub.h"
static unsigned char floppy[DISK_SECTORS * SECTOR_SIZE], ide[DATA_DISK_SECTORS * SECTOR_SIZE];
static int ide_present, fail_write, transport_writes;
static unsigned now;
int platform_memory_range_available(uint32_t base, uint32_t end) { (void)base; (void)end; return 0; }
void platform_log(const char *text) { (void)text; }
uint32_t timer_ticks(void) { return now; }
unsigned disk_sector_count(void) { return DISK_SECTORS; }
int disk_read(unsigned lba, void *buf, int count) {
    assert(count >= 0 && lba + count <= DISK_SECTORS);
    memcpy(buf, floppy + lba * SECTOR_SIZE, count * SECTOR_SIZE); return 0;
}
int disk_write(unsigned lba, const void *buf, int count) {
    assert(count >= 0 && lba + count <= DISK_SECTORS); ++transport_writes;
    if (fail_write) return -1;
    memcpy(floppy + lba * SECTOR_SIZE, buf, count * SECTOR_SIZE); return 0;
}
int ata_probe(void) { return ide_present; }
unsigned ata_sector_count(void) { return DATA_DISK_SECTORS; }
int ata_read(unsigned lba, void *buf, int count) {
    assert(count >= 0 && lba + count <= DATA_DISK_SECTORS);
    memcpy(buf, ide + lba * SECTOR_SIZE, count * SECTOR_SIZE); return 0;
}
int ata_write(unsigned lba, const void *buf, int count) {
    assert(count >= 0 && lba + count <= DATA_DISK_SECTORS); ++transport_writes;
    if (fail_write) return -1;
    memcpy(ide + lba * SECTOR_SIZE, buf, count * SECTOR_SIZE); return 0;
}
int ata_flush(void) { return 0; }
#define THEME_N 8
#define DISPLAY_MODE_COUNT 4
static int dirty, prefs_id, theme_id, saver_enabled, display_pending, display_previous;
static int cursor_on, current_mode;
static unsigned display_deadline;
static const char *display_message;
static int wins[8], display_saved_windows[8];
static int display_current_mode(void) { return current_mode; }
static int display_set_mode(int mode) {
    if (mode < 0 || mode >= DISPLAY_MODE_COUNT) return 0;
    current_mode = mode; return 1;
}
static void cursor_restore(void) {}
static void display_refresh_layout(void) { dirty = 1; }
static unsigned preference_writes, preference_requests, preference_mutations;
static int preference_test_write(int id, const char *bytes, int count) {
    ++preference_writes; ++preference_mutations; return fs_write(id, bytes, count);
}
static int preference_test_mkdir(int dir, const char *name) {
    ++preference_mutations; return fs_mkdir(dir, name);
}
static int preference_test_create(int dir, const char *name) {
    ++preference_mutations; return fs_create(dir, name);
}
static int preference_test_delete(int id) { ++preference_mutations; return fs_delete(id); }
static int preference_test_request(FsSyncTicket *ticket) { ++preference_requests; return fs_sync_request(ticket); }
#define fs_write preference_test_write
#define fs_mkdir preference_test_mkdir
#define fs_create preference_test_create
#define fs_delete preference_test_delete
#define fs_sync_request preference_test_request
#include "settings_kernel.inc"
#undef fs_write
#undef fs_mkdir
#undef fs_create
#undef fs_delete
#undef fs_sync_request
static void reset_settings(void) {
    memset(preferences, 0, sizeof preferences);
    preference_sync_active = preference_retry_requested = 0;
    preference_retry_after = 0;
    preference_sync_ticket = (FsSyncTicket){0};
    preference_writes = preference_requests = preference_mutations = 0;
    prefs_id = -1; dirty = theme_id = display_pending = current_mode = 0;
    saver_enabled = 1; display_message = ""; now = 0;
}
static void reset_volume(int marked) {
    assert(!fs_sync_busy());
    memset(floppy, 0, sizeof floppy); memset(ide, 0, sizeof ide);
    ide_present = marked; fail_write = transport_writes = 0;
    if (marked) {
        unsigned marker[] = {DATA_MARKER_MAGIC, DATA_MARKER_VERSION, DATA_DISK_SECTORS,
            DATA_SLOT_SECTORS, DATA_FIRST_LBA, DATA_SECOND_LBA, 0};
        marker[6] = crc32(marker, 24); memcpy(ide, marker, sizeof marker);
    }
    assert(!fs_init() && fs_load_disk() == FS_LOAD_BLANK);
    assert(!fs_empty_dir(fs_root()) && !fs_sync());
    reset_settings();
}
static void drain(void) {
    unsigned count = 0;
    while (fs_sync_busy()) { assert(fs_sync_step() != FS_SYNC_IDLE); assert(++count < 100000); }
}
static void assert_value(int which, char value) {
    int id = preference_file(which);
    assert(id >= 0 && fs_size(id) == 1 && fs_data(id)[0] == value);
}
static void assert_status(int which, int state, const char *text) {
    assert(preferences[which].state == state);
    assert(strstr(preference_message(which), text));
}
static void ordinary_and_coalesced(void) {
    reset_volume(1);
    theme_id = 2; theme_save(); saver_enabled = 0; saver_save();
    assert(display_request(1) && display_pending && display_deadline == now + 15 * TIMER_HZ);
    display_keep(); assert(!display_pending && current_mode == 1);
    assert_status(PREF_DISPLAY, PREF_RAM, "RAM");
    assert_value(PREF_THEME, '2'); assert_value(PREF_SAVER, '0'); assert_value(PREF_DISPLAY, '1');
    assert(preference_writes == 3);
    fs_autosync(); assert(fs_sync_busy());
    theme_id = 3; theme_save(); theme_id = 7; theme_save();
    saver_enabled = 1; saver_save(); saver_enabled = 0; saver_save();
    assert(display_request(2)); display_keep();
    assert(display_request(3)); display_revert(); assert(current_mode == 2);
    assert_status(PREF_THEME, PREF_QUEUED, "queued");
    assert_status(PREF_DISPLAY, PREF_QUEUED, "queued");
    for (int i = 0; i < 1000; ++i) preferences_tick();
    assert(preference_writes == 3 && !preference_requests);
    assert_value(PREF_THEME, '2'); assert_value(PREF_DISPLAY, '1');
    drain(); preferences_tick();
    assert_value(PREF_THEME, '7'); assert_value(PREF_SAVER, '0'); assert_value(PREF_DISPLAY, '2');
    assert(preference_writes == 5); /* No rewrite for the unchanged saver. */
    assert_status(PREF_DISPLAY, PREF_RAM, "RAM");
    fs_autosync(); drain(); preferences_tick();
    for (int i = 0; i < PREF_COUNT; ++i) assert_status(i, PREF_SAVED, "saved");
    unsigned calls = preference_mutations;
    theme_save(); saver_save(); for (int i = 0; i < 1000; ++i) preferences_tick();
    assert(preference_mutations == calls && !fs_needs_sync());
    assert(!fs_init() && !fs_load_disk()); reset_settings();
    theme_load(); saver_load(); display_load();
    assert(theme_id == 7 && !saver_enabled && current_mode == 2);
    puts("Settings: busy coalescing, one-byte/no-op writes, Keep/Revert, durable reboot passed");
}
static void fixed_paths_and_rejections(void) {
    reset_volume(1);
    int dir = fs_mkdir(0, "prefs"), theme = fs_create(dir, "theme");
    assert(fs_write(theme, "0", 1) == 1); prefs_id = dir;
    assert(!fs_rename(dir, "Unrelated"));
    theme_id = 4; theme_save();
    assert(preference_file(PREF_THEME) != theme && fs_data(theme)[0] == '0');
    assert_value(PREF_THEME, '4');
    assert(!fs_sync()); preferences_tick();
    /* A queued choice resolves only after release, never through a stale cache. */
    fs_write(theme, "1", 1); FsSyncTicket ticket; assert(!fs_sync_request(&ticket));
    theme_id = 5; theme_save(); drain();
    int latest_dir = fs_find_child(0, "prefs");
    assert(!fs_rename(latest_dir, "Old prefs"));
    int occupied = fs_create(0, "prefs"); assert(fs_write(occupied, "personal", 8) == 8);
    preferences_tick();
    assert_status(PREF_THEME, PREF_PATH_FAILED, "occupied");
    assert(!strcmp(fs_data(occupied), "personal"));
    unsigned mutations = preference_mutations;
    for (int i = 0; i < 1000; ++i) preferences_tick();
    assert(preference_mutations == mutations && preferences[PREF_THEME].choice == '5');
    assert(!fs_sync_release(ticket));
    assert(!fs_rename(occupied, "personal")); preferences_retry(); drain(); preferences_tick();
    assert_value(PREF_THEME, '5'); assert_status(PREF_THEME, PREF_SAVED, "saved");

    for (int kind = 0; kind < 4; ++kind) {
        reset_volume(1); dir = fs_mkdir(0, "prefs");
        int id = kind == 0 ? fs_mkdir(dir, "display") : kind == 1 ? fs_create_app(dir, "display") : fs_create(dir, "display");
        assert(id > 0);
        if (kind == 2) assert(fs_write(id, "my file", 7) == 7);
        if (kind == 3) assert(fs_write(id, "9", 1) == 1);
        int size = fs_size(id), count = fs_node_count(); unsigned bytes = fs_used_bytes();
        display_pending = 1; current_mode = 3; display_keep();
        assert_status(PREF_DISPLAY, PREF_PATH_FAILED, "occupied");
        assert(!preference_mutations && fs_size(id) == size && fs_node_count() == count && fs_used_bytes() == bytes);
        preferences_retry(); assert(!preference_mutations);
    }
    puts("Settings: fixed paths, stale IDs, occupied directory/app/unknown file rejection passed");
}
static void full_volume_and_node_limit(void) {
    reset_volume(1);
    char name[24];
    while (fs_node_count() < fs_node_limit() - 1) {
        snprintf(name, sizeof name, "entry-%d", fs_node_count()); assert(fs_create(0, name) > 0);
    }
    int before = fs_node_count(); theme_id = 3; theme_save();
    assert_status(PREF_THEME, PREF_FULL_FAILED, "No room");
    assert(fs_node_count() == before && !preference_mutations);
    assert(preferences_prepare_shutdown() < 0 && fs_node_count() == before && !preference_mutations);
    int remove = fs_find_child(0, "entry-1"); assert(remove > 0 && !fs_delete(remove));
    preferences_retry(); drain(); preferences_tick();
    assert_value(PREF_THEME, '3'); assert_status(PREF_THEME, PREF_SAVED, "saved");

    reset_volume(1);
    static char payload[FS_FILE_MAX]; memset(payload, 'x', sizeof payload);
    unsigned remaining = fs_capacity(); int index = 0;
    while (remaining) {
        snprintf(name, sizeof name, "fill-%d", index++); int id = fs_create(0, name); assert(id > 0);
        unsigned n = remaining > sizeof payload ? sizeof payload : remaining;
        assert(fs_write(id, payload, (int)n) == (int)n); remaining -= n;
    }
    before = fs_node_count(); unsigned used = fs_used_bytes();
    display_pending = 1; current_mode = 2; display_keep();
    assert_status(PREF_DISPLAY, PREF_FULL_FAILED, "No room");
    assert(fs_node_count() == before && fs_used_bytes() == used && !preference_mutations);
    assert(preferences_prepare_shutdown() < 0 && fs_node_count() == before && !preference_mutations);
    assert(!fs_delete(fs_find_child(0, "fill-0")));
    preferences_retry(); drain(); preferences_tick();
    assert_value(PREF_DISPLAY, '2'); assert_status(PREF_DISPLAY, PREF_SAVED, "saved");
    puts("Settings: full bytes/node preflight, retained choice and explicit retry passed");
}
static void errors_and_retry(void) {
    reset_volume(1); theme_id = 6; theme_save(); fail_write = 1;
    for (int i = 0; i < 3; ++i) {
        fs_autosync(); assert(fs_sync_busy()); drain(); preferences_tick();
        assert_status(PREF_THEME, PREF_SYNC_FAILED, "RAM only"); now += 5 * TIMER_HZ;
    }
    unsigned writes = preference_writes; int disk_writes = transport_writes;
    for (int i = 0; i < 1000; ++i) { fs_autosync(); preferences_tick(); }
    assert(!fs_sync_busy() && preference_writes == writes && transport_writes == disk_writes);
    fail_write = 0; preferences_retry(); assert(preference_sync_active && fs_sync_busy());
    assert(preference_writes == writes); drain(); preferences_tick();
    assert(!preference_sync_active && !preference_retry_requested);
    assert_status(PREF_THEME, PREF_SAVED, "saved");
    assert(!fs_init() && !fs_load_disk()); reset_settings(); theme_load(); assert(theme_id == 6);

    /* Read-only is an ordinary mount fallback: an unknown IDE marker is never reformatted. */
    reset_volume(0); ide_present = 1; memcpy(ide, "unknown disk", 12);
    assert(!fs_init() && fs_load_disk() < 0); reset_settings();
    theme_id = 1; theme_save(); assert_value(PREF_THEME, '1');
    assert_status(PREF_THEME, PREF_SYNC_FAILED, "RAM only");
    disk_writes = transport_writes; preferences_retry(); preferences_tick();
    assert_status(PREF_THEME, PREF_SYNC_FAILED, "RAM only");
    assert(transport_writes == disk_writes && !memcmp(ide, "unknown disk", 12));

    /* A foreign completed ticket occupies the result slot, not the mutation lease. */
    reset_volume(1); FsSyncTicket foreign; assert(!fs_sync_request(&foreign));
    saver_enabled = 0; saver_save(); preferences_retry();
    assert(preference_requests == 1 && preference_retry_requested && !preference_sync_active);
    for (int i = 0; i < 1000; ++i) preferences_tick();
    assert(preference_requests == 1); now += TIMER_HZ; preferences_tick();
    assert(preference_requests == 2); assert(!fs_sync_release(foreign));
    now += TIMER_HZ; preferences_tick(); assert(preference_sync_active);
    drain(); preferences_tick(); assert_status(PREF_SAVER, PREF_SAVED, "saved");

    /* A replacement before durability is never mistaken for our saved value. */
    theme_id = 3; theme_save(); int theme = preference_file(PREF_THEME);
    assert(fs_write(theme, "4", 1) == 1 && !fs_sync()); preferences_tick();
    assert_status(PREF_THEME, PREF_WRITE_FAILED, "changed");
    puts("Settings: autosync backoff/exhaustion, explicit async retry, readonly/unknown disk and replacement checks passed");
}
static void floppy_and_clean_observation(void) {
    reset_volume(0); theme_id = 7; theme_save();
    assert_status(PREF_THEME, PREF_RAM, "RAM"); fs_autosync(); preferences_tick();
    assert_status(PREF_THEME, PREF_SAVED, "saved");
    reset_volume(1); theme_id = 2; theme_save(); fs_autosync();
    saver_enabled = 0; saver_save(); drain(); preferences_tick();
    assert_status(PREF_THEME, PREF_SAVED, "saved");
    assert_status(PREF_SAVER, PREF_RAM, "RAM");
    fs_autosync(); drain(); preferences_tick(); assert_status(PREF_SAVER, PREF_SAVED, "saved");
    puts("Settings: legacy floppy and clean-before-queued-mutation observation passed");
}
static int finish_shutdown(void) {
    if (preferences_prepare_shutdown() < 0) return -1;
    int marker = fs_find_child(0, "session-marker");
    if (marker < 0) marker = fs_create(0, "session-marker");
    assert(marker >= 0 && fs_write(marker, "session", 7) == 7);
    int result = fs_sync(); preferences_tick(); return result;
}
static void shutdown_boundaries(void) {
    reset_volume(1); theme_id = 2; theme_save(); fs_autosync();
    theme_id = 4; theme_save(); display_pending = 1; current_mode = 3; display_keep();
    assert_status(PREF_DISPLAY, PREF_QUEUED, "queued");
    assert(!finish_shutdown());
    assert_status(PREF_THEME, PREF_SAVED, "saved"); assert_status(PREF_DISPLAY, PREF_SAVED, "saved");
    assert(!fs_init() && !fs_load_disk()); reset_settings(); theme_load(); display_load();
    assert(theme_id == 4 && current_mode == 3);

    reset_volume(1); saver_enabled = 0; saver_save(); preferences_retry();
    assert(preference_sync_active && fs_sync_busy());
    theme_id = 6; theme_save(); preferences_retry();
    assert(!finish_shutdown() && !preference_sync_active && !preference_retry_requested);
    assert_value(PREF_THEME, '6'); assert_value(PREF_SAVER, '0');

    reset_volume(1); theme_id = 2; theme_save(); fs_autosync();
    theme_id = 3; theme_save(); fail_write = 1;
    assert(finish_shutdown() < 0 && !fs_sync_busy());
    assert(preferences[PREF_THEME].choice == '3');
    assert_status(PREF_THEME, PREF_SYNC_FAILED, "RAM only");
    fail_write = 0; assert(!finish_shutdown()); assert_value(PREF_THEME, '3');

    reset_volume(1); int bad = fs_create(0, "prefs"); assert(bad > 0);
    theme_id = 7; theme_save(); assert(finish_shutdown() < 0);
    assert_status(PREF_THEME, PREF_PATH_FAILED, "occupied");
    assert(!fs_rename(bad, "personal")); assert(!finish_shutdown()); assert_value(PREF_THEME, '7');
    reset_volume(0); ide_present = 1; memcpy(ide, "unknown disk", 12);
    assert(!fs_init() && fs_load_disk() < 0); reset_settings();
    display_pending = 1; current_mode = 2; display_keep();
    int disk_writes = transport_writes;
    assert(finish_shutdown() < 0 && transport_writes == disk_writes);
    assert_status(PREF_DISPLAY, PREF_SYNC_FAILED, "RAM only");
    puts("Settings: shutdown drains queued choices and retry tickets; failures block completion passed");
}
int main(void) {
    ordinary_and_coalesced(); fixed_paths_and_rejections(); full_volume_and_node_limit();
    errors_and_retry(); floppy_and_clean_observation(); shutdown_boundaries(); return 0;
}
