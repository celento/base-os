/* Ordinary Todo workflows and returned storage errors, with the real FS model.
 * No memory faults, fuzzing, debugger or QEMU work. */
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
static unsigned todo_writes, todo_mutations;
static int reject_write, reject_create, reject_mkdir;
static int todo_test_write(int id, const char *data, int n) {
    ++todo_writes; ++todo_mutations;
    return reject_write ? -1 : fs_write(id, data, n);
}
static int todo_test_mkdir(int parent, const char *name) {
    ++todo_mutations;
    return reject_mkdir ? -1 : fs_mkdir(parent, name);
}
static int todo_test_create(int parent, const char *name) {
    ++todo_mutations;
    return reject_create ? -1 : fs_create(parent, name);
}
static int todo_test_delete(int id) { ++todo_mutations; return fs_delete(id); }
#define fs_write todo_test_write
#define fs_mkdir todo_test_mkdir
#define fs_create todo_test_create
#define fs_delete todo_test_delete
#include "../src/todo.c"
#undef fs_write
#undef fs_mkdir
#undef fs_create
#undef fs_delete
static void reset_todo(void) {
    td_pending = td_state = td_field_len = 0;
    todo_writes = todo_mutations = 0;
    reject_write = reject_create = reject_mkdir = 0;
    todo_load();
}
static void reset_volume(int marked) {
    assert(!fs_sync_busy());
    memset(floppy, 0, sizeof floppy); memset(ide, 0, sizeof ide);
    ide_present = marked; fail_write = transport_writes = 0; now = 0;
    if (marked) {
        unsigned marker[] = {DATA_MARKER_MAGIC, DATA_MARKER_VERSION, DATA_DISK_SECTORS,
            DATA_SLOT_SECTORS, DATA_FIRST_LBA, DATA_SECOND_LBA, 0};
        marker[6] = crc32(marker, 24); memcpy(ide, marker, sizeof marker);
    }
    assert(!fs_init() && fs_load_disk() == FS_LOAD_BLANK);
    assert(!fs_empty_dir(fs_root()) && !fs_sync());
    reset_todo();
}
static void type(const char *text) {
    todo_focus_field();
    while (*text) assert(todo_field_char(*text++));
}
static void add(const char *text) { type(text); assert(todo_field_enter()); }
static void status(int state, const char *fragment) {
    assert(td_state == state && strstr(todo_status(), fragment));
}
static void bytes(const char *expected) {
    int f = todo_file(); assert(f >= 0);
    assert(fs_size(f) == (int)strlen(expected) && !memcmp(fs_data(f), expected, strlen(expected)));
}
static void drain(void) {
    unsigned count = 0;
    while (fs_sync_busy()) { assert(fs_sync_step() != FS_SYNC_IDLE); assert(++count < 100000); }
}
static void reboot(void) { assert(!fs_init() && !fs_load_disk()); reset_todo(); }
static FsSyncTicket begin_snapshot(void) {
    FsSyncTicket ticket; assert(fs_needs_sync());
    assert(!fs_sync_request(&ticket) && fs_sync_busy()); return ticket;
}
static void finish_snapshot(FsSyncTicket ticket) {
    drain(); assert(!fs_sync_result(ticket)); assert(!fs_sync_release(ticket));
}
static void busy_coalescing_and_reopen(void) {
    reset_volume(1); add("milk"); status(TODO_RAM, "RAM"); bytes(" milk\n");
    FsSyncTicket ticket = begin_snapshot();
    unsigned mutations = todo_mutations;
    assert(todo_toggle(0)); add("bread"); assert(todo_toggle(1)); assert(todo_toggle(1));
    type("draft");
    status(TODO_QUEUED, "queued"); bytes(" milk\n");
    assert(todo_count() == 2 && todo_done(0) && !todo_done(1));
    for (int i = 0; i < 1000; ++i) assert(!todo_tick());
    assert(todo_mutations == mutations && todo_writes == 1);
    todo_load(); assert(todo_count() == 2 && todo_done(0) && !strcmp(todo_field(), "draft"));
    assert(todo_retry_save() == FS_ERR_BUSY && todo_mutations == mutations);
    finish_snapshot(ticket);
    assert(todo_tick()); bytes("xmilk\n bread\n");
    assert(todo_writes == 2 && td_pending == 0); status(TODO_RAM, "RAM");
    for (int i = 0; i < 1000; ++i) assert(!todo_tick());
    assert(todo_writes == 2);
    todo_load(); assert(todo_count() == 2 && !strcmp(todo_field(), "draft"));
    fs_autosync(); assert(fs_sync_busy()); drain(); assert(todo_tick()); status(TODO_SAVED, "saved");
    reboot(); assert(todo_count() == 2 && todo_done(0) && !todo_done(1));
    assert(!strcmp(todo_text(0), "milk") && !strcmp(todo_text(1), "bread") && !*todo_field());
    puts("Todo: repeated busy edits coalesce once, reopen/draft retention and durable reboot passed");
}
static void missing_paths_and_stale_ids(void) {
    reset_volume(1);
    int trigger = fs_create(0, "trigger"); assert(trigger > 0);
    FsSyncTicket ticket = begin_snapshot();
    int count = fs_node_count(); add("new task");
    assert(!todo_mutations && fs_node_count() == count && fs_find_child(0, "prefs") < 0);
    finish_snapshot(ticket); assert(todo_tick()); bytes(" new task\n");
    assert(fs_node_count() == count + 2 && todo_writes == 1);
    assert(!todo_retry_save()); status(TODO_SAVED, "saved");
    int p = fs_find_child(0, "prefs"), f = todo_file(); unsigned identity = fs_identity(f);
    assert(!fs_delete(f));
    int unrelated = fs_create(p, "personal");
    assert(unrelated == f && fs_identity(unrelated) != identity);
    assert(fs_write(unrelated, "keep", 4) == 4);
    assert(todo_toggle(0)); bytes("xnew task\n");
    assert(!strcmp(fs_data(unrelated), "keep"));
    p = fs_find_child(0, "prefs"); f = todo_file();
    assert(!fs_rename(p, "old prefs"));
    assert(todo_toggle(0)); bytes(" new task\n");
    assert(fs_parent(todo_file()) != p && !strcmp(fs_data(f), "xnew task\n"));
    puts("Todo: missing paths defer without creation and fresh paths reject stale node reuse passed");
}
static void occupied_paths_and_changed_sources(void) {
    for (int kind = 0; kind < 4; ++kind) {
        reset_volume(1);
        int p = kind < 2 ? 0 : fs_mkdir(0, "prefs");
        const char *name = kind < 2 ? "prefs" : "todo";
        int f = kind == 0 ? fs_create(p, name) : kind == 1 || kind == 3 ? fs_create_app(p, name) : fs_mkdir(p, name);
        assert(f > 0);
        if (kind == 0) assert(fs_write(f, "personal", 8) == 8);
        int count = fs_node_count(); unsigned used = fs_used_bytes();
        add("retained"); type("draft"); status(TODO_PATH_FAILED, "occupied");
        for (int i = 0; i < 1000; ++i) assert(!todo_tick());
        todo_load(); assert(todo_count() == 1 && !strcmp(todo_field(), "draft"));
        assert(!todo_mutations && fs_node_count() == count && fs_used_bytes() == used);
        assert(todo_prepare_shutdown() < 0 && !todo_mutations);
        assert(!fs_rename(f, "unrelated")); assert(!todo_retry_save()); bytes(" retained\n");
    }
    for (int replacement = 0; replacement < 2; ++replacement) {
        reset_volume(1); add("original"); assert(!todo_retry_save());
        int f = todo_file(); unsigned identity = fs_identity(f);
        if (replacement) {
            assert(!fs_delete(f)); assert(fs_create(fs_find_child(0, "prefs"), "todo") == f);
            assert(fs_identity(f) != identity);
        }
        assert(fs_write(f, " external\n", 10) == 10);
        assert(todo_tick()); status(TODO_RELOAD, "reopen");
        unsigned writes = todo_writes;
        assert(!td_pending && !todo_prepare_shutdown() && todo_writes == writes);
        assert(todo_toggle(0)); status(TODO_CONFLICT, "changed");
        assert(todo_count() == 1 && todo_done(0));
        assert(todo_retry_save() < 0 && todo_writes == writes);
        assert(!strcmp(fs_data(f), " external\n"));
        assert(!fs_rename(f, "personal")); assert(!todo_retry_save()); bytes("xoriginal\n");
    }
    reset_volume(1); add("original"); assert(!todo_retry_save());
    int f = todo_file(); assert(fs_write(f, " external\n", 10) == 10);
    assert(todo_tick()); status(TODO_RELOAD, "reopen");
    todo_load(); assert(!td_pending && todo_count() == 1 && !strcmp(todo_text(0), "external"));
    assert(!todo_retry_save());

    reset_volume(1); add("RAM task"); f = todo_file();
    assert(fs_write(f, " external\n", 10) == 10);
    assert(todo_tick()); status(TODO_CONFLICT, "changed");
    assert(td_pending && todo_prepare_shutdown() < 0);
    assert(!strcmp(todo_text(0), "RAM task") && !strcmp(fs_data(f), " external\n"));

    reset_volume(1); add("old"); FsSyncTicket ticket = begin_snapshot(); add("latest");
    finish_snapshot(ticket); f = todo_file(); int p = fs_parent(f);
    assert(!fs_delete(f) && fs_create(p, "todo") == f);
    assert(fs_write(f, " external\n", 10) == 10);
    assert(todo_tick()); status(TODO_CONFLICT, "changed");
    assert(todo_count() == 2 && !strcmp(todo_text(1), "latest"));
    assert(todo_prepare_shutdown() < 0 && !strcmp(fs_data(f), " external\n"));
    puts("Todo: occupied app/directory, changed content and same-ID replacement protection passed");
}
static void full_nodes_and_bytes(void) {
    reset_volume(1); char name[24];
    while (fs_node_count() < fs_node_limit() - 1) {
        snprintf(name, sizeof name, "entry-%d", fs_node_count()); assert(fs_create(0, name) > 0);
    }
    int count = fs_node_count(); add("retained"); status(TODO_FULL_FAILED, "No room");
    assert(!todo_mutations && fs_node_count() == count && fs_find_child(0, "prefs") < 0);
    for (int i = 0; i < 1000; ++i) assert(!todo_tick());
    assert(todo_prepare_shutdown() < 0 && !todo_mutations);
    assert(!fs_delete(fs_find_child(0, "entry-1"))); assert(!todo_retry_save()); bytes(" retained\n");
    reset_volume(1);
    /* Cross the 64-node threshold: /prefs and todo consume 80 metadata bytes. */
    while (fs_node_count() < 64) {
        snprintf(name, sizeof name, "entry-%d", fs_node_count()); assert(fs_create(0, name) > 0);
    }
    static char payload[FS_FILE_MAX]; memset(payload, 'd', sizeof payload);
    unsigned remaining = fs_capacity() - 64; int index = 1;
    while (remaining) {
        snprintf(name, sizeof name, "entry-%d", index++); int f = fs_find_child(0, name); assert(f > 0);
        unsigned n = remaining > sizeof payload ? sizeof payload : remaining;
        assert(fs_write(f, payload, (int)n) == (int)n); remaining -= n;
    }
    count = fs_node_count(); unsigned used = fs_used_bytes(); add("task");
    status(TODO_FULL_FAILED, "No room"); assert(!todo_mutations && fs_node_count() == count && fs_used_bytes() == used);
    assert(!fs_delete(fs_find_child(0, "entry-1"))); assert(!todo_retry_save()); bytes(" task\n");
    /* A full volume can still toggle a same-sized Todo file. */
    unsigned free_bytes = fs_capacity() - fs_used_bytes();
    index = 5;
    while (free_bytes) {
        snprintf(name, sizeof name, "entry-%d", index++); int f = fs_find_child(0, name); assert(f > 0);
        unsigned n = free_bytes > sizeof payload ? sizeof payload : free_bytes;
        assert(fs_write(f, payload, (int)n) == (int)n); free_bytes -= n;
    }
    assert(todo_toggle(0)); bytes("xtask\n");
    add("larger"); status(TODO_FULL_FAILED, "No room"); bytes("xtask\n");
    assert(todo_count() == 2 && !strcmp(todo_text(1), "larger"));
    puts("Todo: node/full-byte preflight, metadata allowance, retained list and replacement allowance passed");
}
static void ordinary_rejections_rollback(void) {
    for (int stage = 0; stage < 3; ++stage) {
        reset_volume(1);
        reject_mkdir = stage == 0; reject_create = stage == 1; reject_write = stage == 2;
        add("retained"); status(TODO_WRITE_FAILED, "not saved");
        assert(fs_node_count() == 1 && fs_find_child(0, "prefs") < 0);
        unsigned mutations = todo_mutations;
        for (int i = 0; i < 1000; ++i) assert(!todo_tick());
        assert(todo_mutations == mutations && todo_count() == 1);
        reject_mkdir = reject_create = reject_write = 0;
        assert(!todo_retry_save()); bytes(" retained\n");
    }
    reset_volume(1); add("original"); reject_write = 1;
    assert(todo_toggle(0)); status(TODO_WRITE_FAILED, "not saved"); bytes(" original\n");
    reject_write = 0;
    int trigger = fs_create(0, "trigger"); assert(trigger > 0);
    FsSyncTicket ticket = begin_snapshot();
    assert(todo_retry_save() == FS_ERR_BUSY); status(TODO_QUEUED, "queued");
    finish_snapshot(ticket); assert(todo_tick()); bytes("xoriginal\n");
    puts("Todo: returned create/write errors roll back only new nodes and do not retry-spin passed");
}
static void disk_failure_and_shutdown(void) {
    reset_volume(1); add("old"); FsSyncTicket ticket = begin_snapshot();
    add("latest"); assert(todo_prepare_shutdown() == 0 && !fs_sync_busy());
    assert(!fs_sync_result(ticket)); assert(!fs_sync_release(ticket));
    bytes(" old\n latest\n"); status(TODO_RAM, "RAM"); assert(fs_needs_sync());
    /* The caller's final snapshot, not the older joined one, persists latest. */
    assert(!fs_sync()); assert(todo_tick()); status(TODO_SAVED, "saved");
    reboot(); assert(todo_count() == 2 && !strcmp(todo_text(1), "latest"));
    reset_volume(1); add("old"); ticket = begin_snapshot(); add("latest"); fail_write = 1;
    assert(todo_prepare_shutdown() == 0); assert(fs_sync_result(ticket) < 0); assert(!fs_sync_release(ticket));
    bytes(" old\n latest\n"); status(TODO_SYNC_FAILED, "RAM only");
    assert(fs_sync() < 0); assert(!todo_tick());
    unsigned writes = todo_writes; int disk_writes = transport_writes;
    for (int i = 0; i < 1000; ++i) assert(!todo_tick());
    assert(todo_writes == writes && transport_writes == disk_writes);
    fail_write = 0; assert(!todo_retry_save()); status(TODO_SAVED, "saved");
    assert(todo_writes == writes); reboot(); assert(todo_count() == 2);
    reset_volume(1); add("backoff"); fail_write = 1;
    for (int i = 0; i < 3; ++i) {
        fs_autosync(); assert(fs_sync_busy()); drain(); todo_tick(); now += 5 * TIMER_HZ;
    }
    status(TODO_SYNC_FAILED, "RAM only"); disk_writes = transport_writes;
    for (int i = 0; i < 1000; ++i) { fs_autosync(); todo_tick(); }
    assert(transport_writes == disk_writes); fail_write = 0; assert(!todo_retry_save());
    reset_volume(0); ide_present = 1; memcpy(ide, "unknown", 7);
    assert(!fs_init() && fs_load_disk() < 0); reset_todo(); add("recover");
    status(TODO_SYNC_FAILED, "RAM only"); disk_writes = transport_writes;
    assert(todo_prepare_shutdown() == 0 && fs_sync() < 0 && todo_retry_save() < 0);
    assert(transport_writes == disk_writes && !memcmp(ide, "unknown", 7));
    assert(todo_count() == 1 && !strcmp(todo_text(0), "recover"));
    puts("Todo: shutdown stages newest state; disk failure/backoff/read-only remain honest passed");
}
static void legacy_format_and_limits(void) {
    reset_volume(0); int p = fs_mkdir(0, "prefs"), f = fs_create(p, "todo");
    const char *source = "Xupper\n lower\n\n";
    assert(fs_write(f, source, (int)strlen(source)) == (int)strlen(source));
    assert(!fs_sync()); reset_todo();
    assert(todo_count() == 2 && todo_done(0) && !todo_done(1));
    for (int i = 2; i < TODO_MAX; ++i) add("another");
    type("kept field"); assert(!todo_field_enter() && todo_count() == TODO_MAX);
    assert(!todo_toggle(-1) && !todo_toggle(TODO_MAX));
    todo_load(); assert(todo_count() == TODO_MAX && !strcmp(todo_field(), "kept field"));
    assert(!todo_retry_save()); status(TODO_SAVED, "saved"); reboot(); assert(todo_count() == TODO_MAX);
    reset_volume(1); type("12345678901234567890123"); assert(!todo_field_char('x'));
    assert(todo_field_enter()); assert(!strcmp(todo_text(0), "12345678901234567890123"));
    assert(!todo_retry_save()); reboot(); assert(strlen(todo_text(0)) == FS_NAME_LEN - 1);
    puts("Todo: unchanged line format, legacy floppy, capacity/field/text boundaries passed");
}
static void unrelated_source_files(void) {
    const char *sources[]={"Ordinary notes in the reserved path\n",
        " 123456789012345678901234\n",
        " a\n b\n c\n d\n e\n f\n g\n h\n i\n j\n k\n l\n m\n"};
    for(unsigned i=0;i<sizeof sources/sizeof sources[0];i++){
        reset_volume(1);int p=fs_mkdir(0,"prefs"),f=fs_create(p,"todo");
        int length=(int)strlen(sources[i]);assert(fs_write(f,sources[i],length)==length);
        assert(!fs_sync());reset_todo();
        assert(todo_count()==0&&!td_pending);status(TODO_UNSUPPORTED,"existing bytes kept");
        assert(!todo_prepare_shutdown()&&fs_size(f)==length&&!memcmp(fs_data(f),sources[i],length));
        add("my task");assert(td_pending);status(TODO_CONFLICT,"changed");
        assert(todo_retry_save()<0&&fs_size(f)==length&&!memcmp(fs_data(f),sources[i],length));
        assert(!fs_rename(f,"previous-todo.txt"));assert(!todo_retry_save());
        assert(fs_size(f)==length&&!memcmp(fs_data(f),sources[i],length));
        int replacement=fs_find_child(p,"todo");assert(replacement>=0&&replacement!=f);
        assert(fs_size(replacement)==9&&!memcmp(fs_data(replacement)," my task\n",9));
    }
    puts("Todo: unrelated, oversized-line and excess-row files remain unchanged");
}
int main(void) {
    busy_coalescing_and_reopen(); missing_paths_and_stale_ids();
    occupied_paths_and_changed_sources(); full_nodes_and_bytes();
    ordinary_rejections_rollback(); disk_failure_and_shutdown(); legacy_format_and_limits();
    unrelated_source_files();
    return 0;
}
