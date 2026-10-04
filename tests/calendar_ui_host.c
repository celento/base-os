/* Ordinary Calendar mouse/keyboard flows with real model, codec and renderer.
 * Storage is a bounded mock. No fault probes, sanitizer, debugger or QEMU. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fs.h"
#include "layout.h"
static unsigned char presented[FB_CAPACITY];
#undef PRESENT_BASE
#define PRESENT_BASE ((uintptr_t)presented)
#define GFX_HOST_TEST
#include "../src/gfx.c"
#include "../src/calendar_agenda.c"
#include "../src/calendar.c"

static unsigned char back[800 * 600], linear[800 * 600 * 4];
static char store[CAL_AGENDA_FILE_MAX], durable[CAL_AGENDA_FILE_MAX];
static int directory, file, size, durable_size, busy, pending, failed;
static int writes, syncs;
uint8_t app_accent = COLOR_BLUE, app_accent_dk = COLOR_NAVY;
uint8_t app_text = COLOR_BLACK, app_text_dim = PAL_GRAY + 16;
uint8_t app_chrome = PAL_GRAY + 28, app_chrome_dk = PAL_GRAY + 20;

void kmemset(void *d, int v, int n) { memset(d, v, (size_t)n); }
void kmemcpy(void *d, const void *s, int n) { memcpy(d, s, (size_t)n); }
int kstrlen(const char *s) { return (int)strlen(s); }
int kstrcmp(const char *a, const char *b) { return strcmp(a, b); }
void kstrcpy(char *d, const char *s) { strcpy(d, s); }
void platform_log(const char *s) { (void)s; }
uint8_t rtc_test_read(uint8_t reg) {
    switch (reg) { case 0xb: return 6; case 4: return 12; case 7: return 4;
        case 8: return 10; case 9: return 26; case 6: return 1; default: return 0; }
}
int fs_root(void) { return 0; }
int fs_find_child(int p, const char *s) {
    return p == 0 && !strcmp(s, "prefs") ? directory ? 1 : -1 :
        p == 1 && !strcmp(s, "calendar.v1") ? file ? 2 : -1 : -1;
}
int fs_is_dir(int id) { return id == 0 || (id == 1 && directory); }
int fs_is_app(int id) { (void)id; return 0; }
int fs_size(int id) { return id == 2 && file ? size : 0; }
unsigned fs_identity(int id) { return id == 2 && file ? 20 : 0; }
const char *fs_data(int id) { return id == 2 && file ? store : 0; }
int fs_sync_busy(void) { return busy; }
const char *fs_storage_status(void) { return failed ? "Mock disk save failure" : 0; }
int fs_needs_sync(void) { return pending; }
int fs_node_count(void) { return 1 + directory + file; }
int fs_node_limit(void) { return 64; }
unsigned fs_capacity_for_nodes(unsigned nodes) { (void)nodes; return 100000; }
unsigned fs_used_bytes(void) { return (unsigned)size; }
unsigned fs_file_limit(void) { return sizeof store; }
int fs_mkdir(int parent, const char *name) {
    assert(!busy && parent == 0 && !strcmp(name, "prefs")); directory = pending = 1; return 1;
}
int fs_create(int parent, const char *name) {
    assert(!busy && parent == 1 && !strcmp(name, "calendar.v1")); file = pending = 1; return 2;
}
int fs_write(int id, const char *s, int n) {
    assert(!busy && id == 2 && n >= 0 && (unsigned)n <= sizeof store);
    memcpy(store, s, (unsigned)n); size = n; pending = 1; ++writes; return n;
}
int fs_delete(int id) { assert(!busy); if (id == 2) file = size = 0; else directory = 0; return 0; }
int fs_read(int id, char *s, int cap) { assert(id == 2 && cap >= size); memcpy(s, store, (unsigned)size); return size; }
int fs_sync(void) {
    ++syncs; busy = 0;
    if (failed) return -1;
    memcpy(durable, store, (unsigned)size); durable_size = size; pending = 0; return 0;
}
static void key(int sc) { assert(cal_key(sc, 0, 0)); }
static void type(const char *s) { while (*s) assert(cal_key(0, *s++, 0)); }
static void replace(const char *s) { assert(cal_key(0x1e, 0, CAL_MOD_CTRL)); type(s); }
static void click(int x, int y) { assert(cal_click(40, 40, CAL_W, 40 + x, 40 + y)); }
static void render(const char *path) {
    draw_rect(0, 0, 800, 600, COLOR_WHITE); cal_draw(40, 40, CAL_W, CAL_H);
    if (!path) return;
    FILE *f = fopen(path, "wb"); assert(f); fprintf(f, "P6\n800 600\n255\n");
    for (int i = 0; i < 800 * 600; ++i) {
        unsigned rgb = pal32[back[i]];
        fputc((int)(rgb >> 16) & 255, f); fputc((int)(rgb >> 8) & 255, f); fputc((int)rgb & 255, f);
    }
    assert(!fclose(f));
}
static void reboot(void) {
    memcpy(store, durable, (unsigned)durable_size); size = durable_size; pending = 0;
    ca_loaded = ca_pending = ca_state = 0; ca_count = 0; memset(&ca_draft, 0, sizeof ca_draft);
    inited = 0; selected = 0; first_row = 0; cal_reset();
}
int main(int argc, char **argv) {
    /* Match the smallest production desktop: 36px menu + 44px taskbar,
     * 32px title, 8px layout margin, and one client separator pixel. */
    _Static_assert(CAL_H + 32 <= 600 - 36 - 44 - 8, "Calendar must fit 800x600");
    gfx_init(back, linear, 800, 600, 32, 800 * 4);
    cal_reset(); assert(view_year == 2026 && view_month == 10 && view_day == 4);
    assert(!cal_agenda_count());
    assert(!cal_key(0, 'n', CAL_MOD_ALT) && !cal_agenda_draft()->active);
    click(350, 266); type("Project review");
    assert(!cal_key(0, 'x', CAL_MOD_ALT));
    assert(!strcmp(cal_agenda_draft()->title, "Project review"));
    click(198, 356); type("09:30");
    click(475, 306); assert(cal_agenda_count() == 1 && !cal_agenda_draft()->active);
    unsigned first = selected; assert(cal_agenda_find(first)->minute == 570);
    assert(writes == 0); cal_agenda_tick(); assert(writes == 1);
    assert(cal_agenda_retry_save() == 0 && syncs == 1);
    printf("Calendar creation: mouse fields, optional time, explicit disk save passed.\n");

    key(KEY_ENTER); replace("Edited review");
    key(KEY_TAB); replace("2028-02-29"); key(KEY_TAB); replace("14:45");
    assert(cal_key(KEY_ESC, 0, 0) == CAL_CLOSE);
    cal_reset(); assert(!strcmp(cal_agenda_draft()->title, "Edited review"));
    assert(cal_agenda_prepare_shutdown() == 0); assert(fs_sync() == 0); reboot();
    assert(view_year == 2028 && view_month == 2 && view_day == 29);
    assert(cal_agenda_draft()->edit_id == first && !strcmp(cal_agenda_draft()->time, "14:45"));
    key(KEY_ENTER); assert(cal_agenda_count() == 1 && !cal_agenda_draft()->active);
    assert(cal_agenda_find(first)->year == 2028 && cal_agenda_find(first)->minute == 885);
    printf("Calendar recovery: unfinished edits survive close, shutdown staging and remount.\n");

    key(KEY_PGDN); assert(view_month == 3 && view_day == 29);
    key(KEY_PGUP); assert(view_month == 2 && view_day == 29);
    key(KEY_RIGHT); assert(view_month == 3 && view_day == 1);
    key(KEY_LEFT); assert(view_month == 2 && view_day == 29);
    cal_new(); type("Another item"); key(KEY_TAB); replace("2027-02-29");
    key(KEY_ENTER); assert(cal_agenda_draft()->active && strstr(notice, "real date"));
    replace("2028-02-29"); key(KEY_TAB); replace("24:01");
    key(KEY_ENTER); assert(cal_agenda_draft()->active && strstr(notice, "24-hour"));
    click(316, 358); assert(!cal_agenda_draft()->time[0]); key(KEY_ENTER);
    assert(cal_agenda_count() == 2 && cal_agenda_find(selected)->minute == -1);
    assert(cal_agenda_get(0)->id == selected);
    printf("Calendar validation: leap days, month rollover, invalid dates/times and all-day passed.\n");

    unsigned second = selected; key(KEY_DELETE); assert(confirm == CONFIRM_DELETE);
    key(KEY_DELETE); assert(cal_agenda_count() == 2); key(KEY_ESC); assert(!confirm);
    key(KEY_DELETE); key(KEY_ENTER); assert(cal_agenda_count() == 1 && !cal_agenda_find(second));
    cal_new(); type("Keep partial"); key(KEY_TAB); replace("2028-0");
    cal_new(); assert(!strcmp(cal_agenda_draft()->title, "Keep partial"));
    click(548, 306); assert(confirm == CONFIRM_CANCEL); key(KEY_ESC);
    assert(cal_agenda_draft()->active); click(548, 306); key(KEY_ENTER);
    assert(!cal_agenda_draft()->active && cal_agenda_count() == 1);
    printf("Calendar safety: repeated delete needs confirmation, new never replaces entry, cancel is explicit.\n");

    busy = 1; int previous_writes = writes; cal_new(); type("Busy draft");
    cal_agenda_tick(); assert(writes == previous_writes && strstr(cal_agenda_status(), "queued"));
    assert(cal_agenda_retry_save() == FS_ERR_BUSY); assert(cal_key(KEY_ESC, 0, 0) == CAL_CLOSE);
    cal_reset(); assert(!strcmp(cal_agenda_draft()->title, "Busy draft"));
    busy = 0; cal_agenda_tick(); failed = 1;
    assert(cal_agenda_retry_save() == -1 && cal_agenda_draft()->active);
    assert(strstr(cal_agenda_status(), "failed")); failed = 0;
    assert(cal_agenda_retry_save() == 0); reboot();
    assert(!strcmp(cal_agenda_draft()->title, "Busy draft"));
    click(548, 306); key(KEY_ENTER);
    printf("Calendar storage: leased writes and failed sync retain draft, retry and recovery passed.\n");

    for (int i = 0; i < 20; ++i) {
        char title[40]; snprintf(title, sizeof title, "Appointment %02d", i);
        assert(cal_agenda_add(view_year, view_month, view_day, i * 30, title));
    }
    changed_day(); click(590, 230); assert(first_row == 6);
    click(590, 230); assert(first_row == 12);
    click(335, 230); assert(first_row == 6);
    key(KEY_DOWN); assert(selected_row() == 7);
    for (int i = cal_agenda_count(); i < CAL_AGENDA_MAX; ++i)
        assert(cal_agenda_add(view_year, view_month, view_day, -1, "Capacity"));
    cal_new(); type("Full list entry"); key(KEY_ENTER);
    assert(cal_agenda_count() == CAL_AGENDA_MAX && cal_agenda_draft()->active);
    assert(!strcmp(cal_agenda_draft()->title, "Full list entry"));
    click(548, 306); key(KEY_ENTER);
    printf("Calendar navigation: date agenda paging, stable selection and full-list entry preservation passed.\n");

    cal_new(); type("A long title can be edited without losing its beginning or end");
    key(KEY_HOME); type("[start] "); key(KEY_END); type(" [end]");
    assert(strstr(cal_agenda_draft()->title, "[start]") == cal_agenda_draft()->title);
    assert(strstr(cal_agenda_draft()->title, "[end]"));
    key(KEY_LEFT); key(KEY_BACKSPACE); key(KEY_DELETE);
    render(argc > 1 ? argv[1] : 0);
    cal_draw(90, 76, CAL_W, CAL_H - 1); /* actual default-window client height */
    assert(get_pixel(90 + CAL_W - 88, 76 + 455) != COLOR_WHITE); /* Retry outline drawn, not fallback */
    cal_draw(0, 0, 320, 200); cal_draw(-20, -10, CAL_W, CAL_H);
    printf("Calendar rendering: real font/raster drawing, caret, long fields and small-window fallback passed.\n");
    puts("All Calendar UI functional checks passed."); return 0;
}
