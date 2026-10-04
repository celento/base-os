/* Ordinary full-volume mount/save/remount benchmark on disposable images.
 * tools/fs_crc_benchmark.py links this entry with otherwise normal objects. */
#include "platform.h"
#include "persist.h"
#include "fs.h"

/* Fixed document modification time makes independently saved images comparable. */
unsigned crc_benchmark_clock(void) { return 1234567890u; }

static void require(int condition, const char *message) {
    if (!condition) panic(message);
}
static void number(unsigned value) {
    char buffer[11]; unsigned n = 0;
    do { buffer[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) { char out[2] = {buffer[--n], 0}; platform_log(out); }
}
static unsigned begin(const char *phase) {
    platform_log("CRC-BENCH-"); platform_log(phase); platform_log("-BEGIN\n");
    return timer_ticks();
}
static void end(const char *phase, unsigned started) {
    unsigned elapsed = timer_ticks() - started;
    platform_log("CRC-BENCH-"); platform_log(phase); platform_log("-TICKS=");
    number(elapsed); platform_log("\n");
}
static void verify(void) {
    require(fs_node_count() == 256 && fs_used_bytes() == 8377344u &&
            fs_capacity() == 8377344u, "crc benchmark: full volume mismatch");
    require(!fs_storage_status() && !fs_needs_sync(), "crc benchmark: dirty storage");
    for (int id = 1; id <= 4; ++id) {
        unsigned n = id < 4 ? FS_FILE_MAX : 8377344u - 3 * FS_FILE_MAX;
        require(fs_size(id) == (int)n, "crc benchmark: file size mismatch");
        const unsigned char *bytes = (const unsigned char *)fs_data(id);
        for (unsigned i = 0; i < n; ++i)
            require(bytes[i] == (unsigned char)(i * 37u + (i >> 16) + 19u),
                    "crc benchmark: file byte mismatch");
    }
}
void fs_crc_guest(void) {
    platform_validate_memory();
    disk_configure(((BootInfo *)BOOTINFO_ADDR)->sectors_per_track);
    fs_init();
    unsigned started = begin("MOUNT");
    require(fs_load_disk() == 0, "crc benchmark: mount failed");
    end("MOUNT", started); verify();
    require(fs_write(1, fs_data(1), fs_size(1)) == FS_FILE_MAX,
            "crc benchmark: ordinary overwrite failed");
    require(fs_modified(1) == 1234567890u, "crc benchmark: fixed clock missing");
    started = begin("SAVE");
    require(fs_sync() == 0, "crc benchmark: save failed");
    end("SAVE", started); verify();
    fs_init();
    started = begin("REMOUNT");
    require(fs_load_disk() == 0, "crc benchmark: remount failed");
    end("REMOUNT", started); verify();
    require(fs_modified(1) == 1234567890u, "crc benchmark: modification time mismatch");
    platform_log("CRC-BENCH-PASS\n");
    for (;;) __asm__ volatile("hlt");
}
