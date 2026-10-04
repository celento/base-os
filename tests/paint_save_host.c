/* Ordinary save flows and explicit error returns; no fuzzing or memory faults. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "history.h"
static unsigned char node_arena[FS_CAPACITY], image_arena[FS_IMG_CAPACITY], pool_arena[FS_POOL_CAPACITY];
static unsigned char paint_arena[PAINT_CAPACITY], history_arena[PAINT_HISTORY_CAPACITY];
#undef FS_BASE
#undef FS_IMG_BASE
#undef FS_POOL_BASE
#undef PAINT_MEM
#undef PAINT_HISTORY_BASE
#define FS_BASE ((uintptr_t)node_arena)
#define FS_IMG_BASE ((uintptr_t)image_arena)
#define FS_POOL_BASE ((uintptr_t)pool_arena)
#define PAINT_MEM ((uintptr_t)paint_arena)
#define PAINT_HISTORY_BASE ((uintptr_t)history_arena)
#include "../src/fs.c"
#include "ata_async_stub.h"
static unsigned char floppy[DISK_SECTORS * SECTOR_SIZE], ide[DATA_DISK_SECTORS * SECTOR_SIZE];
static int ide_present;
int platform_memory_range_available(uint32_t base, uint32_t end) { (void)base; (void)end; return 0; }
void platform_log(const char *text) { (void)text; }
uint32_t timer_ticks(void) { return 0; }
unsigned disk_sector_count(void) { return DISK_SECTORS; }
int disk_read(unsigned lba, void *buf, int count) {
    assert(count >= 0 && lba + count <= DISK_SECTORS);
    memcpy(buf, floppy + lba * SECTOR_SIZE, count * SECTOR_SIZE); return 0;
}
int disk_write(unsigned lba, const void *buf, int count) {
    assert(count >= 0 && lba + count <= DISK_SECTORS);
    memcpy(floppy + lba * SECTOR_SIZE, buf, count * SECTOR_SIZE); return 0;
}
int ata_probe(void) { return ide_present; }
unsigned ata_sector_count(void) { return DATA_DISK_SECTORS; }
int ata_read(unsigned lba, void *buf, int count) {
    assert(count >= 0 && lba + count <= DATA_DISK_SECTORS);
    memcpy(buf, ide + lba * SECTOR_SIZE, count * SECTOR_SIZE); return 0;
}
int ata_write(unsigned lba, const void *buf, int count) {
    assert(count >= 0 && lba + count <= DATA_DISK_SECTORS);
    memcpy(ide + lba * SECTOR_SIZE, buf, count * SECTOR_SIZE); return 0;
}
int ata_flush(void) { return 0; }
#define PAINT_W 160
#define PAINT_H 100
#define PAINT_NWELL2 16
#define PT_PENCIL 0
#define COLOR_WHITE 15
#define COLOR_BLACK 0
#define WK_FILES 4
#define PICTURE_BYTES (8 + PAINT_W * PAINT_H)
static History paint_history;
static unsigned char *paint_pix;
static int paint_ready, paint_tool, paint_color;
static uint8_t paint_well2[PAINT_NWELL2];
static const uint32_t paint_well2_rgb[PAINT_NWELL2] = {0};
static uint8_t idx24(uint32_t color) { return (uint8_t)color; }
static int dirty, name_failed, name_dlg, name_target, files_open, refreshes;
static const char *name_failure_message;
static char dialog_name[FS_NAME_LEN];
static int find_open_kind(int kind) { assert(kind == WK_FILES); return files_open ? 0 : -1; }
static void fm_refresh(void) { ++refreshes; }
static void namedlg_open(int target, const char *initial) {
    name_target = target; name_dlg = dirty = 1;
    name_failed = 0; name_failure_message = 0;
    kstrcpy(dialog_name, initial);
}
static unsigned mutations, write_calls;
static int return_write_error;
static int paint_test_write(int id, const char *data, int size) {
    ++mutations; ++write_calls;
    if (return_write_error) return -1;
    return fs_write(id, data, size);
}
static int paint_test_create(int dir, const char *name) { ++mutations; return fs_create(dir, name); }
static int paint_test_mkdir(int dir, const char *name) { ++mutations; return fs_mkdir(dir, name); }
static int paint_test_delete(int id) { ++mutations; return fs_delete(id); }
#define fs_write paint_test_write
#define fs_create paint_test_create
#define fs_mkdir paint_test_mkdir
#define fs_delete paint_test_delete
#include "paint_kernel.inc"
#undef fs_write
#undef fs_create
#undef fs_mkdir
#undef fs_delete

static FsNode saved_nodes[FS_MAX_NODES];
static unsigned saved_identities[FS_MAX_NODES], saved_revisions[FS_MAX_NODES];
static unsigned char saved_pool[FS_POOL_CAPACITY], saved_pixels[PAINT_W * PAINT_H];
static unsigned char saved_history[PAINT_HISTORY_CAPACITY];
static unsigned saved_pool_used;
static History saved_history_state;
static void capture(void) {
    memcpy(saved_nodes, nodes, sizeof saved_nodes);
    memcpy(saved_identities, identities, sizeof saved_identities);
    memcpy(saved_revisions, content_revisions, sizeof saved_revisions);
    saved_pool_used = pool_used;
    memcpy(saved_pool, (void *)pool_base, pool_used);
    memcpy(saved_pixels, paint_pix, sizeof saved_pixels);
    memcpy(saved_history, history_arena, sizeof saved_history);
    saved_history_state = paint_history;
    mutations = write_calls = 0; dirty = refreshes = 0;
}
static void expect_canvas(void) {
    assert(!memcmp(saved_pixels, paint_pix, sizeof saved_pixels));
    assert(!memcmp(saved_history, history_arena, sizeof saved_history));
    assert(!memcmp(&saved_history_state, &paint_history, sizeof paint_history));
}
static void expect_unchanged(void) {
    for (int id = 0; id < FS_MAX_NODES; ++id) {
        assert(nodes[id].used == saved_nodes[id].used);
        if (!nodes[id].used) continue;
        assert(!memcmp(&nodes[id], &saved_nodes[id], sizeof(FsNode)));
        assert(fs_identity(id) == saved_identities[id]);
        assert(fs_content_revision(id) == saved_revisions[id]);
    }
    assert(pool_used == saved_pool_used);
    assert(!memcmp(saved_pool, (void *)pool_base, pool_used));
    expect_canvas(); assert(!refreshes);
}
static void reset(int ide_backend) {
    assert(!fs_sync_busy());
    memset(floppy, 0, sizeof floppy); memset(ide, 0, sizeof ide); ide_present = ide_backend;
    if (ide_backend) {
        unsigned marker[] = {DATA_MARKER_MAGIC, DATA_MARKER_VERSION, DATA_DISK_SECTORS,
            DATA_SLOT_SECTORS, DATA_FIRST_LBA, DATA_SECOND_LBA, 0};
        marker[6] = crc32(marker, 24); memcpy(ide, marker, sizeof marker);
    }
    assert(!fs_init() && fs_load_disk() == FS_LOAD_BLANK);
    assert(!fs_empty_dir(fs_root()) && !fs_sync());
    paint_ready = 0; paint_pix = 0; memset(&paint_history, 0, sizeof paint_history); paint_init();
    for (unsigned i = 0; i < PAINT_W * PAINT_H; ++i) paint_pix[i] = (unsigned char)(i * 37 + i / PAINT_W);
    memset(history_arena, 0x5c, sizeof history_arena); paint_history.count = 3; paint_history.cursor = 2;
    dirty = name_failed = name_dlg = return_write_error = refreshes = files_open = 0;
    mutations = write_calls = 0; name_failure_message = 0; dialog_name[0] = 0;
}
static int make_file(int parent, const char *name, const char *data) {
    int id = fs_create(parent, name); assert(id >= 0);
    assert(fs_write(id, data, (int)strlen(data)) == (int)strlen(data)); return id;
}
static void expect_failure(const char *name, const char *message) {
    assert(!paint_write_named(name));
    assert(name_failure_message && !strcmp(name_failure_message, message));
    assert(!dirty); expect_unchanged();
}
static void check_bos(const char *name, const unsigned char *pixels) {
    static const unsigned char header[] = {'B', 'O', 'S', '1', 160, 0, 100, 0};
    int pics = fs_find_child(0, "Pictures"), id = fs_find_child(pics, name);
    assert(fs_is_dir(pics) && id >= 0 && !fs_is_dir(id) && !fs_is_app(id));
    assert(fs_size(id) == PICTURE_BYTES && !memcmp(fs_data(id), header, sizeof header));
    assert(!memcmp(fs_data(id) + sizeof header, pixels, PAINT_W * PAINT_H));
}
static void dialog_and_destinations(int backend) {
    reset(backend); make_file(0, "guard", "Keep existing root data."); capture();
    paint_save(); assert(name_dlg && name_target == 1 && !name_failed);
    assert(!strcmp(dialog_name, "untitled.pbm") && !name_failure_message);
    expect_unchanged(); assert(!mutations && fs_find_child(0, "Pictures") < 0);
    /* Canceling the normal dialog has no path to clean up. */
    name_dlg = 0; paint_save(); expect_unchanged(); assert(!mutations);
    for (int kind = 0; kind < 2; ++kind) {
        int blocked = kind ? fs_create_app(0, "Pictures") : make_file(0, "Pictures", "Preserve this file.");
        assert(blocked >= 0); capture(); paint_save();
        assert(name_dlg && name_failed && !strcmp(name_failure_message, "Pictures is not a folder."));
        expect_unchanged(); assert(!mutations); dirty = 0;
        expect_failure("art.pbm", "Pictures is not a folder."); assert(!mutations);
        assert(!fs_delete(blocked));
    }
    int pics = fs_mkdir(0, "Pictures"); assert(pics >= 0);
    int file = make_file(pics, "occupied.pbm", "An unrelated ordinary document.");
    int app = fs_create_app(pics, "application"), dir = fs_mkdir(pics, "folder");
    assert(file >= 0 && app >= 0 && dir >= 0); make_file(dir, "child", "Keep nested data.");
    const char *names[] = {"occupied.pbm", "application", "folder"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
        capture(); expect_failure(names[i], "Name exists. Choose another name."); assert(!mutations);
    }
    make_file(pics, "untitled.pbm", "First existing picture name."); capture(); paint_save();
    assert(!name_failed && !strcmp(dialog_name, "untitled 2.pbm")); expect_unchanged(); assert(!mutations);
    printf("Paint dialog, occupied Pictures path and file/app/directory collisions passed (%s)\n", fs_storage_name());
}
static void success_and_write_retry(int backend, int existing_pictures) {
    reset(backend); make_file(0, "guard", "Existing root bytes and identity.");
    if (existing_pictures) {
        int pics = fs_mkdir(0, "Pictures"); assert(pics >= 0); make_file(pics, "guard", "Existing picture sibling.");
    }
    capture(); return_write_error = 1;
    expect_failure("retry.pbm", "Picture not saved. Try again."); assert(write_calls == 1);
    int pics = fs_find_child(0, "Pictures"); assert((pics >= 0) == existing_pictures);
    assert(pics < 0 || fs_find_child(pics, "retry.pbm") < 0);
    /* The failed compound save also leaves no path in the next snapshot. */
    assert(!fs_sync() && !fs_init() && !fs_load_disk());
    pics = fs_find_child(0, "Pictures"); assert((pics >= 0) == existing_pictures);
    assert(pics < 0 || fs_find_child(pics, "retry.pbm") < 0);
    assert(!strcmp(fs_data(fs_find_child(0, "guard")), "Existing root bytes and identity."));
    return_write_error = 0; files_open = 1; capture();
    assert(paint_write_named("retry.pbm") && !name_failure_message && dirty && refreshes == 1);
    check_bos("retry.pbm", saved_pixels); expect_canvas();
    capture(); expect_failure("retry.pbm", "Name exists. Choose another name."); assert(!mutations);
    assert(!fs_sync() && !fs_init() && !fs_load_disk()); check_bos("retry.pbm", saved_pixels);
    capture(); paint_save(); assert(!name_failed); expect_unchanged(); dirty = 0;
    assert(paint_write_named("second.pbm")); check_bos("second.pbm", saved_pixels);
    printf("Paint returned-write rollback, explicit retry and exact BOS1 remount passed (%s, Pictures %s)\n",
           fs_storage_name(), existing_pictures ? "existing" : "new");
}
static void invalid_names(void) {
    const char *names[] = {"bad/name", ".", "..", "", "a-name-that-is-far-too-long-for-this-filesystem"};
    for (int existing = 0; existing <= 1; ++existing) {
        reset(1); make_file(0, "guard", "Unchanged.");
        if (existing) assert(fs_mkdir(0, "Pictures") >= 0);
        for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
            capture(); expect_failure(names[i], "Save failed. Check the file name."); assert(!write_calls);
            assert((fs_find_child(0, "Pictures") >= 0) == existing);
        }
    }
    puts("Paint invalid-name create rejection rolls back new Pictures and preserves existing folders");
}
static void fill_nodes(int target) {
    while (fs_node_count() < target) {
        char name[FS_NAME_LEN]; snprintf(name, sizeof name, "node-%d", fs_node_count());
        assert(fs_create(0, name) >= 0);
    }
}
static void node_capacity(int backend) {
    for (int existing = 0; existing <= 1; ++existing) {
        reset(backend); make_file(0, "guard", "Keep these bytes.");
        if (existing) assert(fs_mkdir(0, "Pictures") >= 0);
        fill_nodes(fs_node_limit() - (existing ? 0 : 1)); capture();
        expect_failure("full.pbm", "Not enough free file slots."); assert(!mutations);
        assert((fs_find_child(0, "Pictures") >= 0) == existing);
        int last = fs_node_limit() - (existing ? 1 : 2); assert(!fs_delete(last));
        capture(); assert(paint_write_named("full.pbm")); check_bos("full.pbm", saved_pixels);
    }
    printf("Paint full node table and two-node admission/retry passed (%s)\n", fs_storage_name());
}
static void fill_bytes(unsigned amount) {
    static unsigned char data[FS_FILE_MAX];
    memset(data, 0x63, sizeof data);
    while (amount) {
        unsigned n = amount > fs_file_limit() ? fs_file_limit() : amount;
        char name[FS_NAME_LEN]; snprintf(name, sizeof name, "fill-%d", fs_node_count());
        int id = fs_create(0, name); assert(id >= 0);
        assert(fs_write(id, (const char *)data, (int)n) == (int)n); amount -= n;
    }
}
static void byte_capacity(void) {
    for (int existing = 0; existing <= 1; ++existing) {
        reset(1); if (existing) assert(fs_mkdir(0, "Pictures") >= 0);
        fill_bytes(fs_capacity()); capture();
        expect_failure("full.pbm", "Not enough space for this picture."); assert(!mutations);
        assert((fs_find_child(0, "Pictures") >= 0) == existing);
        int full = fs_find_child(0, existing ? "fill-2" : "fill-1"); assert(full >= 0);
        int length = fs_size(full); assert(length > PICTURE_BYTES);
        assert(fs_write(full, fs_data(full), length - PICTURE_BYTES) == length - PICTURE_BYTES);
        capture(); assert(paint_write_named("full.pbm")); check_bos("full.pbm", saved_pixels);
        assert(fs_used_bytes() == fs_capacity()); assert(!fs_sync() && !fs_init() && !fs_load_disk());
        check_bos("full.pbm", saved_pixels);
    }
    /* The projected folder plus file records matter after the legacy 64-node reservation. */
    for (int existing = 0; existing <= 1; ++existing) {
        reset(1); if (existing) assert(fs_mkdir(0, "Pictures") >= 0);
        fill_bytes(fs_capacity() - PICTURE_BYTES); fill_nodes(FS_LEGACY_NODES); capture();
        expect_failure("metadata.pbm", "Not enough space for this picture."); assert(!mutations);
        int full = fs_find_child(0, existing ? "fill-2" : "fill-1"), length = fs_size(full);
        int overhead = existing ? 40 : 80;
        assert(fs_write(full, fs_data(full), length - overhead) == length - overhead);
        capture(); assert(paint_write_named("metadata.pbm")); check_bos("metadata.pbm", saved_pixels);
        assert(fs_used_bytes() == fs_capacity()); assert(!fs_sync() && !fs_init() && !fs_load_disk());
        check_bos("metadata.pbm", saved_pixels);
    }
    puts("Paint full byte capacity, projected record overhead and exact-boundary retry/remount passed");
}
static void busy_retry(int existing_pictures) {
    reset(1); make_file(0, "guard", "Snapshot data remains untouched.");
    if (existing_pictures) assert(fs_mkdir(0, "Pictures") >= 0);
    FsSyncTicket ticket; assert(!fs_sync_request(&ticket) && fs_sync_busy());
    capture(); paint_save(); assert(name_dlg && name_failed && !strcmp(dialog_name, "untitled.pbm"));
    assert(!strcmp(name_failure_message, "Disk saving. Retry Save shortly."));
    expect_unchanged(); assert(!mutations); dirty = 0;
    kstrcpy(dialog_name, "retained-name.pbm");
    for (int i = 0; i < 8; ++i) {
        expect_failure(dialog_name, "Disk saving. Retry Save shortly."); assert(!mutations);
        assert(!strcmp(dialog_name, "retained-name.pbm"));
        assert(fs_sync_step() != FS_SYNC_IDLE && fs_sync_busy());
    }
    unsigned steps = 0;
    while (fs_sync_busy()) { assert(fs_sync_step() != FS_SYNC_IDLE); assert(++steps < 100000); }
    assert(!fs_sync_result(ticket) && !fs_sync_release(ticket));
    assert(paint_write_named(dialog_name)); check_bos(dialog_name, saved_pixels); expect_canvas();
    assert(!fs_sync() && !fs_init() && !fs_load_disk()); check_bos(dialog_name, saved_pixels);
    printf("Paint real async lease rejects without mutation and retries retained name (%s Pictures)\n",
           existing_pictures ? "existing" : "new");
}
int main(void) {
    for (int backend = 0; backend <= 1; ++backend) {
        dialog_and_destinations(backend); node_capacity(backend);
        for (int existing = 0; existing <= 1; ++existing) success_and_write_retry(backend, existing);
    }
    invalid_names(); byte_capacity(); busy_retry(0); busy_retry(1);
    puts("Paint save-safety host tests passed"); return 0;
}
