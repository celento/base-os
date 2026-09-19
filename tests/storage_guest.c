/* Linked only into a disposable QEMU test image. */
#include "platform.h"
#include "persist.h"
#include "fs.h"
static char data[FS_MAX_SIZE - 1];
static void require(int ok) { if (!ok) panic("storage self-test failed"); }
void storage_guest(void) {
    platform_validate_memory();
    disk_configure(((BootInfo *)BOOTINFO_ADDR)->sectors_per_track);
    fs_init();
    require(fs_load_disk() == FS_LOAD_BLANK);
    fs_empty_dir(0);
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = (char)(i * 7);
    for (int i = 0; i < 4; ++i) {
        char name[] = "file0"; name[4] += i;
        int id = fs_create(0, name);
        require(id > 0);
        require(fs_write(id, data, sizeof(data)) == sizeof(data));
    }
    require(fs_sync() == 0);
    data[sizeof(data) - 1] = 99;
    require(fs_write(fs_find_child(0, "file3"), data, sizeof(data)) == sizeof(data));
    require(fs_sync() == 0);
    fs_init(); require(fs_load_disk() == 0);
    int file = fs_find_child(0, "file3");
    require(fs_size(file) == sizeof(data));
    for (unsigned i = 0; i < sizeof(data); ++i) require(fs_data(file)[i] == data[i]);
    platform_log("HARDWARE-STORAGE-PASS\n");
    for (;;) __asm__ volatile("hlt");
}
