/* Ordinary functional workflows and deterministic I/O errors; no memory faults. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kernel/platform.h"
static unsigned char node_arena[FS_CAPACITY], image_arena[FS_IMG_CAPACITY], pool_arena[FS_POOL_CAPACITY];
#undef FS_BASE
#undef FS_IMG_BASE
#undef FS_POOL_BASE
#define FS_BASE ((uintptr_t)node_arena)
#define FS_IMG_BASE ((uintptr_t)image_arena)
#define FS_POOL_BASE ((uintptr_t)pool_arena)
#include "fs_basefs_support.h"
#include "../src/kernel/fs.c"
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
#include "kernel/display.h"
#include "../src/kernel/input_ingress.c"
static InputIngress device_input;
#include "theme_metrics_host.h"
#include "ui/wallpaper.h"
#define MAX_WIN 8
/* Appearance, window, saver, clock and mouse globals with their production
 * defaults (preference_globals.inc), then the fixtures they act on. */
#include "preference_globals.inc"
static int dirty, prefs_id, saver_enabled = 1, display_pending, display_previous;
static int cursor_on, current_mode, desk_cache_theme, theme_applies, icon_layouts, clamps;
static unsigned display_deadline;
static const char *display_message;
typedef struct { int open, x, y, w, h; } Win;
static Win wins[MAX_WIN], display_saved_windows[MAX_WIN];
int display_current_mode(void) { return current_mode; }
int display_set_mode(int mode) {
    if (mode < 0 || mode >= DISPLAY_MODE_COUNT) return 0;
    current_mode = mode; return 1;
}
static void cursor_restore(void) {}
static void display_refresh_layout(void) { dirty = 1; }
static void theme_apply(void) { ui_theme_install(ui_style, ui_accent_id, ui_desktop_id); ++theme_applies; }
static int wall_palettes;
static void wall_palette(void) { ++wall_palettes; }
static void icons_init(void) { ++icon_layouts; }
static void win_clamp(Win *w) { assert(w->open); ++clamps; }
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
/* Boot defaults of every preference-backed global. */
static void reset_settings(void) {
    memset(preferences, 0, sizeof preferences);
    preference_sync_active = preference_retry_requested = 0;
    preference_retry_after = 0;
    preference_sync_ticket = (FsSyncTicket){0};
    preference_writes = preference_requests = preference_mutations = 0;
    prefs_id = -1; dirty = display_pending = current_mode = 0;
    ui_style = UI_STYLE_CLASSIC; ui_accent_id = ui_desktop_id = ui_pattern = 0;
    ui_wall_kind = ui_gradient = ui_gradient_shape = 0; wall_palettes = 0;
    win_options = WINOPT_DOUBLE_CLICK | WINOPT_SNAP | WINOPT_LINE_NUMBERS; win_snap_distance = 8;
    saver_enabled = 1; saver_delay = 0; clock_flags = 0; dbl_click_speed = 1;
    pointer_speed = INPUT_SPEED_DEFAULT; input_set_speed(&device_input, INPUT_SPEED_UNITY);
    memset(wins, 0, sizeof wins); theme_applies = icon_layouts = clamps = 0;
    ui_theme_install(ui_style, ui_accent_id, ui_desktop_id);
    display_message = ""; now = 0;
}
/* Accent is the appearance choice these workflows change most; it uses the
 * production setter, which applies the ramp and queues its preference. */
static void accent_choose(int accent) { accent_set(accent); assert(ui_accent_id == accent && ui_theme_accent() == accent); }
static void accent_resave(void) { preference_save(PREF_ACCENT, ui_accent_id); }
static void boot_load(void) { theme_load(); saver_load(); display_load(); }
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
    accent_choose(2); saver_enabled = 0; saver_save();
    assert(display_request(1) && display_pending && display_deadline == now + 15 * TIMER_HZ);
    display_keep(); assert(!display_pending && current_mode == 1);
    assert_status(PREF_DISPLAY, PREF_RAM, "RAM");
    assert_value(PREF_ACCENT, '2'); assert_value(PREF_SAVER, '0'); assert_value(PREF_DISPLAY, '1');
    assert(preference_writes == 3);
    fs_autosync(); assert(fs_sync_busy());
    accent_choose(3); accent_choose(5);
    saver_enabled = 1; saver_save(); saver_enabled = 0; saver_save();
    assert(display_request(2)); display_keep();
    assert(display_request(3)); display_revert(); assert(current_mode == 2);
    assert_status(PREF_ACCENT, PREF_QUEUED, "queued");
    assert_status(PREF_DISPLAY, PREF_QUEUED, "queued");
    for (int i = 0; i < 1000; ++i) preferences_tick();
    assert(preference_writes == 3 && !preference_requests);
    assert_value(PREF_ACCENT, '2'); assert_value(PREF_DISPLAY, '1');
    drain(); preferences_tick();
    assert_value(PREF_ACCENT, '5'); assert_value(PREF_SAVER, '0'); assert_value(PREF_DISPLAY, '2');
    assert(preference_writes == 5); /* No rewrite for the unchanged saver. */
    assert_status(PREF_DISPLAY, PREF_RAM, "RAM");
    fs_autosync(); drain(); preferences_tick();
    assert_status(PREF_ACCENT, PREF_SAVED, "saved"); assert_status(PREF_SAVER, PREF_SAVED, "saved");
    assert_status(PREF_DISPLAY, PREF_SAVED, "Resolution saved");
    for (int i = 0; i < PREF_COUNT; ++i)
        if (i != PREF_ACCENT && i != PREF_SAVER && i != PREF_DISPLAY) assert(preferences[i].state == PREF_IDLE);
    unsigned calls = preference_mutations;
    accent_resave(); saver_save(); for (int i = 0; i < 1000; ++i) preferences_tick();
    assert(preference_mutations == calls && !fs_needs_sync());
    assert(!fs_init() && !fs_load_disk()); reset_settings();
    boot_load();
    assert(ui_accent_id == 5 && !saver_enabled && current_mode == 2);
    puts("Settings: busy coalescing, one-byte/no-op writes, Keep/Revert, durable reboot passed");
}
/* Every one-byte preference: its fixed name, its production setter or
 * Settings action, its full value range, a durable reboot and rejection of
 * bytes outside the range. */
static const char *const expected_names[PREF_COUNT] = {
    "style", "accent", "desktop", "pattern", "windows", "snap",
    "saver", "saverdelay", "display", "clock", "mouse",
    "wallpaper", "gradient", "shape", "pointer"
};
static const int expected_counts[PREF_COUNT] = {
    UI_STYLE_COUNT, UI_ACCENT_N, UI_DESKTOP_N, UI_PATTERN_N, 16, SNAP_DISTANCE_MAX + 1,
    2, SAVER_DELAY_N, DISPLAY_MODE_COUNT, 4, 3,
    UI_WALL_KIND_N, UI_GRADIENT_N, UI_SHAPE_N, INPUT_SPEED_LEVELS
};
static void all_preferences(void) {
    assert(PREF_COUNT == 15);
    for (int i = 0; i < PREF_COUNT; ++i) {
        assert(!strcmp(preference_names[i], expected_names[i]) && preference_counts[i] == expected_counts[i]);
        assert(preference_valid(i, '0') && preference_valid(i, (char)('0' + expected_counts[i] - 1)));
        assert(!preference_valid(i, (char)('0' + expected_counts[i])) && !preference_valid(i, '0' - 1));
    }
    reset_volume(1);
    for (int i = 0; i < PREF_COUNT; ++i) assert(preference_read(i) < 0);
    /* Nothing on disk keeps every default. */
    boot_load();
    assert(ui_style == UI_STYLE_CLASSIC && !ui_accent_id && !ui_desktop_id && !ui_pattern && saver_enabled);
    assert(win_options == (WINOPT_DOUBLE_CLICK | WINOPT_SNAP | WINOPT_LINE_NUMBERS) && win_snap_distance == 8);
    assert(!saver_delay && !clock_flags && dbl_click_speed == 1 && !current_mode);
    assert(pointer_speed == INPUT_SPEED_DEFAULT && device_input.speed == INPUT_SPEED_DEFAULT);
    /* Production setters apply the choice at once and queue its byte. */
    wins[2].open = 1;
    style_set(UI_STYLE_MODERN);
    assert(ui_style == UI_STYLE_MODERN && ui_theme.style == UI_STYLE_MODERN && theme_applies == 1);
    assert(icon_layouts == 1 && clamps == 1 && dirty);
    style_set(UI_STYLE_MODERN); style_set(UI_STYLE_COUNT); assert(theme_applies == 1); /* no-op, invalid */
    accent_set(UI_ACCENT_N - 1); assert(ui_theme_accent() == UI_ACCENT_N - 1);
    accent_set(UI_ACCENT_N); assert(ui_accent_id == UI_ACCENT_N - 1);
    desk_cache_theme = 7; desktop_color_set(3); assert(ui_theme_desktop() == 3 && desk_cache_theme == -1);
    desktop_color_set(-1); assert(ui_desktop_id == 3);
    desk_cache_theme = 7; pattern_set(UI_PATTERN_GRID); assert(ui_pattern == UI_PATTERN_GRID && desk_cache_theme == -1);
    pattern_set(UI_PATTERN_N); assert(ui_pattern == UI_PATTERN_GRID);
    /* Wallpaper kind, gradient and shape; the gradient band is reloaded on
     * every change that rewrites the palette. */
    int palettes = wall_palettes;
    desk_cache_theme = 7; wall_kind_set(UI_WALL_PICTURE);
    assert(ui_wall_kind == UI_WALL_PICTURE && desk_cache_theme == -1 && wall_palettes == palettes + 1);
    wall_kind_set(UI_WALL_KIND_N); assert(ui_wall_kind == UI_WALL_PICTURE);
    gradient_set(UI_GRADIENT_N - 1); assert(ui_gradient == UI_GRADIENT_N - 1 && wall_palettes == palettes + 2);
    gradient_set(UI_GRADIENT_N); assert(ui_gradient == UI_GRADIENT_N - 1);
    desk_cache_theme = 7; gradient_shape_set(UI_SHAPE_RADIAL);
    assert(ui_gradient_shape == UI_SHAPE_RADIAL && desk_cache_theme == -1);
    gradient_shape_set(UI_SHAPE_N); assert(ui_gradient_shape == UI_SHAPE_RADIAL);
    win_options = WINOPT_DRAG_CONTENTS | WINOPT_LINE_NUMBERS; windows_save();
    win_snap_distance = SNAP_DISTANCE_MAX; preference_save(PREF_SNAP, win_snap_distance);
    saver_enabled = 0; saver_save();
    saver_delay = SAVER_DELAY_N - 1; preference_save(PREF_SAVER_DELAY, saver_delay);
    assert(display_request(DISPLAY_MODE_COUNT - 1)); display_keep();
    clock_flags = CLOCK_12H | CLOCK_NO_DATE; preference_save(PREF_CLOCK, clock_flags);
    dbl_click_speed = 2; preference_save(PREF_MOUSE, dbl_click_speed);
    pointer_speed = INPUT_SPEED_LEVELS - 1; preference_save(PREF_POINTER, pointer_speed);
    const char bytes[PREF_COUNT] = {'1', '5', '3', '2', '9', 'P', '0', '4', '3', '3', '2', '2', '7', '2', '9'};
    for (int i = 0; i < PREF_COUNT; ++i) {
        assert_value(i, bytes[i]); assert_status(i, PREF_RAM, "RAM");
        assert(preference_read(i) == bytes[i] - '0');
        int dir = fs_find_child(fs_root(), "prefs");
        assert(fs_is_dir(dir) && prefs_id == dir && fs_find_child(dir, expected_names[i]) == preference_file(i));
    }
    assert(preference_writes == PREF_COUNT && fs_child_count(prefs_id) == PREF_COUNT);
    assert(!fs_sync()); preferences_tick();
    for (int i = 0; i < PREF_COUNT; ++i) assert_status(i, PREF_SAVED, "saved");
    assert(!fs_init() && !fs_load_disk()); reset_settings(); boot_load();
    assert(ui_style == UI_STYLE_MODERN && ui_accent_id == UI_ACCENT_N - 1 && ui_desktop_id == 3);
    assert(ui_pattern == UI_PATTERN_GRID && win_options == (WINOPT_DRAG_CONTENTS | WINOPT_LINE_NUMBERS));
    assert(win_snap_distance == SNAP_DISTANCE_MAX && !saver_enabled && saver_delay == SAVER_DELAY_N - 1);
    assert(current_mode == DISPLAY_MODE_COUNT - 1 && clock_flags == (CLOCK_12H | CLOCK_NO_DATE) && dbl_click_speed == 2);
    assert(ui_wall_kind == UI_WALL_PICTURE && ui_gradient == UI_GRADIENT_N - 1 && ui_gradient_shape == UI_SHAPE_RADIAL);
    assert(pointer_speed == INPUT_SPEED_LEVELS - 1 && device_input.speed == INPUT_SPEED_LEVELS - 1);
    /* An out-of-range or malformed byte keeps the default and is never
     * overwritten: Settings reports the path as occupied. */
    const char bad[PREF_COUNT] = {'2', '6', '6', '3', '@', 'Q', '2', '5', '4', '4', '3', '3', '8', '3', ':'};
    for (int i = 0; i < PREF_COUNT; ++i) {
        int id = preference_file(i); assert(fs_write(id, bad + i, 1) == 1);
        assert(preference_read(i) < 0);
    }
    assert(fs_write(preference_file(PREF_MOUSE), "12", 2) == 2); assert(preference_read(PREF_MOUSE) < 0);
    reset_settings(); boot_load();
    assert(ui_style == UI_STYLE_CLASSIC && !ui_accent_id && !ui_desktop_id && !ui_pattern && saver_enabled);
    assert(win_snap_distance == 8 && !saver_delay && !clock_flags && dbl_click_speed == 1 && !current_mode);
    assert(!ui_wall_kind && !ui_gradient && !ui_gradient_shape);
    assert(pointer_speed == INPUT_SPEED_DEFAULT && device_input.speed == INPUT_SPEED_DEFAULT);
    style_set(UI_STYLE_MODERN); assert(ui_style == UI_STYLE_MODERN);
    assert_status(PREF_STYLE, PREF_PATH_FAILED, "occupied");
    assert(fs_data(preference_file(PREF_STYLE))[0] == '2' && !preference_mutations);
    puts("Settings: fifteen one-byte preferences, fixed names, setters, value ranges, reboot and malformed bytes passed");
}
static void fixed_paths_and_rejections(void) {
    reset_volume(1);
    int dir = fs_mkdir(0, "prefs"), accent = fs_create(dir, "accent");
    assert(fs_write(accent, "0", 1) == 1); prefs_id = dir;
    assert(!fs_rename(dir, "Unrelated"));
    accent_choose(4);
    assert(preference_file(PREF_ACCENT) != accent && fs_data(accent)[0] == '0');
    assert_value(PREF_ACCENT, '4');
    assert(!fs_sync()); preferences_tick();
    /* A queued choice resolves only after release, never through a stale cache. */
    fs_write(accent, "1", 1); FsSyncTicket ticket; assert(!fs_sync_request(&ticket));
    accent_choose(5); drain();
    int latest_dir = fs_find_child(0, "prefs");
    assert(!fs_rename(latest_dir, "Old prefs"));
    int occupied = fs_create(0, "prefs"); assert(fs_write(occupied, "personal", 8) == 8);
    preferences_tick();
    assert_status(PREF_ACCENT, PREF_PATH_FAILED, "occupied");
    assert(!strcmp(fs_data(occupied), "personal"));
    unsigned mutations = preference_mutations;
    for (int i = 0; i < 1000; ++i) preferences_tick();
    assert(preference_mutations == mutations && preferences[PREF_ACCENT].choice == '5');
    assert(!fs_sync_release(ticket));
    assert(!fs_rename(occupied, "personal")); preferences_retry(); drain(); preferences_tick();
    assert_value(PREF_ACCENT, '5'); assert_status(PREF_ACCENT, PREF_SAVED, "saved");

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
    int before = fs_node_count(); accent_choose(3);
    assert_status(PREF_ACCENT, PREF_FULL_FAILED, "No room");
    assert(fs_node_count() == before && !preference_mutations);
    assert(preferences_prepare_shutdown() < 0 && fs_node_count() == before && !preference_mutations);
    int remove = fs_find_child(0, "entry-1"); assert(remove > 0 && !fs_delete(remove));
    preferences_retry(); drain(); preferences_tick();
    assert_value(PREF_ACCENT, '3'); assert_status(PREF_ACCENT, PREF_SAVED, "saved");

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
    reset_volume(1); accent_choose(4); fail_write = 1;
    for (int i = 0; i < 3; ++i) {
        fs_autosync(); assert(fs_sync_busy()); drain(); preferences_tick();
        assert_status(PREF_ACCENT, PREF_SYNC_FAILED, "RAM only"); now += 5 * TIMER_HZ;
    }
    unsigned writes = preference_writes; int disk_writes = transport_writes;
    for (int i = 0; i < 1000; ++i) { fs_autosync(); preferences_tick(); }
    assert(!fs_sync_busy() && preference_writes == writes && transport_writes == disk_writes);
    fail_write = 0; preferences_retry(); assert(preference_sync_active && fs_sync_busy());
    assert(preference_writes == writes); drain(); preferences_tick();
    assert(!preference_sync_active && !preference_retry_requested);
    assert_status(PREF_ACCENT, PREF_SAVED, "saved");
    assert(!fs_init() && !fs_load_disk()); reset_settings(); boot_load(); assert(ui_accent_id == 4);

    /* Read-only is an ordinary mount fallback: an unknown IDE marker is never reformatted. */
    reset_volume(0); ide_present = 1; memcpy(ide, "unknown disk", 12);
    assert(!fs_init() && fs_load_disk() < 0); reset_settings();
    accent_choose(1); assert_value(PREF_ACCENT, '1');
    assert_status(PREF_ACCENT, PREF_SYNC_FAILED, "RAM only");
    disk_writes = transport_writes; preferences_retry(); preferences_tick();
    assert_status(PREF_ACCENT, PREF_SYNC_FAILED, "RAM only");
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
    accent_choose(3); int accent = preference_file(PREF_ACCENT);
    assert(fs_write(accent, "4", 1) == 1 && !fs_sync()); preferences_tick();
    assert_status(PREF_ACCENT, PREF_WRITE_FAILED, "changed");
    puts("Settings: autosync backoff/exhaustion, explicit async retry, readonly/unknown disk and replacement checks passed");
}
static void floppy_and_clean_observation(void) {
    reset_volume(0); accent_choose(5);
    assert_status(PREF_ACCENT, PREF_RAM, "RAM"); fs_autosync(); preferences_tick();
    assert_status(PREF_ACCENT, PREF_SAVED, "saved");
    reset_volume(1); accent_choose(2); fs_autosync();
    saver_enabled = 0; saver_save(); drain(); preferences_tick();
    assert_status(PREF_ACCENT, PREF_SAVED, "saved");
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
    reset_volume(1); accent_choose(2); fs_autosync();
    accent_choose(4); display_pending = 1; current_mode = 3; display_keep();
    assert_status(PREF_DISPLAY, PREF_QUEUED, "queued");
    assert(!finish_shutdown());
    assert_status(PREF_ACCENT, PREF_SAVED, "saved"); assert_status(PREF_DISPLAY, PREF_SAVED, "saved");
    assert(!fs_init() && !fs_load_disk()); reset_settings(); boot_load();
    assert(ui_accent_id == 4 && current_mode == 3);

    reset_volume(1); saver_enabled = 0; saver_save(); preferences_retry();
    assert(preference_sync_active && fs_sync_busy());
    accent_choose(4); preferences_retry();
    assert(!finish_shutdown() && !preference_sync_active && !preference_retry_requested);
    assert_value(PREF_ACCENT, '4'); assert_value(PREF_SAVER, '0');

    reset_volume(1); accent_choose(2); fs_autosync();
    accent_choose(3); fail_write = 1;
    assert(finish_shutdown() < 0 && !fs_sync_busy());
    assert(preferences[PREF_ACCENT].choice == '3');
    assert_status(PREF_ACCENT, PREF_SYNC_FAILED, "RAM only");
    fail_write = 0; assert(!finish_shutdown()); assert_value(PREF_ACCENT, '3');

    reset_volume(1); int bad = fs_create(0, "prefs"); assert(bad > 0);
    accent_choose(5); assert(finish_shutdown() < 0);
    assert_status(PREF_ACCENT, PREF_PATH_FAILED, "occupied");
    assert(!fs_rename(bad, "personal")); assert(!finish_shutdown()); assert_value(PREF_ACCENT, '5');
    reset_volume(0); ide_present = 1; memcpy(ide, "unknown disk", 12);
    assert(!fs_init() && fs_load_disk() < 0); reset_settings();
    display_pending = 1; current_mode = 2; display_keep();
    int disk_writes = transport_writes;
    assert(finish_shutdown() < 0 && transport_writes == disk_writes);
    assert_status(PREF_DISPLAY, PREF_SYNC_FAILED, "RAM only");
    puts("Settings: shutdown drains queued choices and retry tickets; failures block completion passed");
}
static void display_confirmation(void) {
    /* Request saves the window layout and arms a 15 second deadline; Revert
     * restores mode and windows without a preference; Keep queues the mode. */
    reset_volume(1); now = 100;
    wins[1] = (Win){1, 10, 20, 300, 200}; current_mode = 0;
    assert(!display_request(0) && !display_pending); /* already current */
    assert(display_request(2) && display_pending && display_previous == 0 && current_mode == 2);
    assert(display_deadline == 100 + 15 * TIMER_HZ && !strcmp(display_message, ""));
    assert(!display_request(1) && current_mode == 2); /* one confirmation at a time */
    wins[1].x = 600; display_revert();
    assert(!display_pending && current_mode == 0 && wins[1].x == 10 && !preference_mutations);
    assert(preferences[PREF_DISPLAY].state == PREF_IDLE);
    display_revert(); display_keep(); assert(!preference_mutations); /* nothing pending */
    assert(!display_request(DISPLAY_MODE_COUNT) && !display_pending && current_mode == 0);
    assert(strstr(display_message, "requires"));
    assert(display_request(3)); display_keep();
    assert(!display_pending && current_mode == 3 && !strcmp(display_message, ""));
    assert_value(PREF_DISPLAY, '3'); assert_status(PREF_DISPLAY, PREF_RAM, "RAM");
    assert(!fs_sync()); preferences_tick(); assert_status(PREF_DISPLAY, PREF_SAVED, "Resolution saved");
    assert(!fs_init() && !fs_load_disk()); reset_settings(); display_load(); assert(current_mode == 3);
    puts("Settings: display request, single confirmation, revert restores windows, keep saves the mode passed");
}
int main(void) {
    ordinary_and_coalesced(); all_preferences(); fixed_paths_and_rejections(); full_volume_and_node_limit();
    errors_and_retry(); floppy_and_clean_observation(); shutdown_boundaries(); display_confirmation();
    (void)saver_delay_secs; (void)dbl_click_ticks;
    return 0;
}
