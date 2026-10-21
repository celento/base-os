/*
 * BaseOS kernel core: boot sequence, serial diagnostics, PS/2 acquisition,
 * framebuffer setup and the cooperative service loop. Everything a person
 * sees belongs to the shell linked into this image (kernel/shell.h).
 */

#include "kernel/kernel.h"
#include "kernel/shell.h"
#include "kernel/platform.h"
#include "kernel/physmem.h"
#include "kernel/thread.h"
#include "kernel/persist.h"
#include "kernel/native_sync.h"
#include "kernel/gfx.h"
#include "kernel/audio.h"
#include "kernel/net.h"
#include "kernel/download.h"
#include "build_info.h"

#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_DATA_PORT   0x60
#define QEMU_SHUTDOWN_PORT   0x604
#define VBOX_SHUTDOWN_PORT   0xB004

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ---------- Serial ---------- */

void serial_write(char c) {
    char text[2] = {c, 0};
    platform_log(text);
}

void kprint_debug(const char *str) { platform_log(str); }

void kprint_uint(unsigned v) {
    char tmp[10];
    int n = 0;
    if (v == 0) {
        serial_write('0');
        return;
    }
    while (v && n < 10) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n)
        serial_write(tmp[--n]);
}

void kprint_hex32(uint32_t v) {
    static const char *hex = "0123456789ABCDEF";
    kprint_debug("0x");
    for (int i = 7; i >= 0; i--)
        serial_write(hex[(v >> (i * 4)) & 0xF]);
}

/* ---------- PS/2 mouse (polled) ---------- */

static int mouse_ok = 0;
static int mouse_type;
InputIngress device_input;

static int mouse_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 2) == 0)
            return 1;
    }
    return 0;
}

static int mouse_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 1)
            return 1;
    }
    return 0;
}

static void mouse_flush(void) {
    for (int i = 0; i < 32; i++) {
        if (!(inb(0x64) & 1))
            break;
        (void)inb(0x60);
    }
}

static int mouse_cmd(uint8_t cmd) {
    if (!mouse_wait_write())
        return 0;
    outb(0x64, 0xD4);
    if (!mouse_wait_write())
        return 0;
    outb(0x60, cmd);
    if (!mouse_wait_read())
        return 0;
    uint8_t ack = inb(0x60);
    return ack == 0xFA;
}

static void mouse_init(void) {
    mouse_flush();

    if (!mouse_wait_write())
        return;
    outb(0x64, 0xA8);

    if (!mouse_wait_write())
        return;
    outb(0x64, 0x20);
    if (!mouse_wait_read())
        return;
    uint8_t status = inb(0x60);
    status &= (uint8_t)~0x03; /* Both PS/2 IRQs stay masked; input is polled. */
    status &= (uint8_t)~0x20;
    if (!mouse_wait_write())
        return;
    outb(0x64, 0x60);
    if (!mouse_wait_write())
        return;
    outb(0x60, status);

    if (!mouse_cmd(0xF6))
        return;
    /* QEMU's IntelliMouse negotiation: 200, 100, 80 samples/second. */
    mouse_type=0;
    if(mouse_cmd(0xF3)&&mouse_cmd(200)&&mouse_cmd(0xF3)&&mouse_cmd(100)&&
       mouse_cmd(0xF3)&&mouse_cmd(80)&&mouse_cmd(0xF2)&&mouse_wait_read()){
        mouse_type=inb(0x60);
    }
    if (!mouse_cmd(0xF4))
        return;

    mouse_flush();
    mouse_ok = 1;
    input_mouse_type(&device_input, (unsigned)mouse_type);
}

int kernel_pointer_ready(void) { return mouse_ok; }

/* ---------- Keyboard and device polling ---------- */

static int input_ready;
void platform_poll(void){if(input_ready)drain_8042();audio_poll();net_poll();}
void fs_background_poll(void){platform_poll();}

unsigned input_acquire(unsigned budget) {
    unsigned acquired=0;
    /* Acquisition only, including when rendering/storage/syscalls poll us. */
    for (unsigned i = 0; i < budget; i++) {
        uint8_t status = inb(KEYBOARD_STATUS_PORT);
        if (!(status & 1)) break;
        uint8_t data = inb(KEYBOARD_DATA_PORT);
        ++acquired;
        uint32_t ticks = timer_ticks();
        if (status & 0xc0) input_device_loss(&device_input, ticks);
        else if (status & 0x20) input_mouse_byte(&device_input, data, ticks);
        else input_keyboard_byte(&device_input, data, ticks);
    }
    return acquired;
}
void drain_8042(void) { (void)input_acquire(32); }

/* ---------- Video init ---------- */

static void video_init(void) {
    const BootInfo *bi = (const BootInfo *)BOOTINFO_ADDR;
    if (!video_info_valid(bi))
        panic("no supported, verified linear framebuffer");
    fb_w = bi->width;
    fb_h = bi->height;
    fb_bpp = bi->bpp;
    fb_pitch = bi->pitch;
    lfb = (uint8_t *)bi->lfb;
    gfx_init((uint8_t *)FB_BASE, lfb, fb_w, fb_h, fb_bpp, fb_pitch);
    gfx_set_flip_hook(platform_poll);

    kprint_debug("Video ");
    kprint_uint((unsigned)fb_w);
    serial_write('x');
    kprint_uint((unsigned)fb_h);
    serial_write('x');
    kprint_uint((unsigned)fb_bpp);
    kprint_debug(" pitch=");
    kprint_uint((unsigned)fb_pitch);
    kprint_debug(" LFB=");
    kprint_hex32((uint32_t)(uintptr_t)lfb);
    kprint_debug(" flags=");
    kprint_uint(bi->magic == BOOTINFO_MAGIC ? bi->flags : 0);
    serial_write('\n');
}

/* ---------- Background services ---------- */

static unsigned pending_events;

unsigned kernel_events_take(unsigned mask) {
    unsigned taken = pending_events & mask;
    pending_events &= ~mask;
    return taken;
}

/* Device polling never dispatches app work or filesystem mutations. */
enum FsSyncProgress kernel_storage_poll(void) {
    native_sync_tick();
    const char *before = fs_storage_status();
    unsigned started = timer_ticks();
    enum FsSyncProgress progress = FS_SYNC_IDLE;
    for (unsigned unit = 0; unit < 256; unit++) {
        progress = fs_sync_step();
        if (progress == FS_SYNC_IDLE || progress == FS_SYNC_FINISHED || timer_ticks() != started) break;
        platform_poll();
        if (timer_ticks() != started) break;
    }
    native_sync_tick();
    if (before != fs_storage_status()) pending_events |= KERNEL_EVENT_STORAGE;
    return progress;
}

void kernel_services_poll(void) {
    audio_poll();
    net_poll();
    if (download_tick()) pending_events |= KERNEL_EVENT_DOWNLOADS;
}

void kernel_power_off(void) {
    outw(QEMU_SHUTDOWN_PORT, 0x2000);
    outw(VBOX_SHUTDOWN_PORT, 0x2000);
    asm volatile("hlt");
}

/* ---------- kmain ---------- */

void kmain(void) {
    kprint_debug("Kernel started\nBuild " BASEOS_BUILD_LABEL "\n");
    platform_validate_memory();
    disk_configure(((const BootInfo *)BOOTINFO_ADDR)->sectors_per_track);

    video_init();
    if (physmem_init() != PHYS_OK)
        panic("owned-page metadata/map initialization failed");
    PhysmemStats pages;
    if (physmem_stats(&pages) != PHYS_OK)
        panic("owned-page accounting unavailable");
    kprint_debug("Owned pages total="); kprint_uint(pages.total);
    kprint_debug(" allocated="); kprint_uint(pages.allocated);
    kprint_debug(" high_water="); kprint_uint(pages.high_water); serial_write('\n');
    audio_init();
    net_init();

    fs_init();
    int mounted = fs_load_disk();
    audio_configure_source_workspace(fs_large_profile(), fs_large_arenas_available());
    if (mounted == 0)
        kprint_debug("FS loaded from disk\n");
    else if (mounted == FS_LOAD_BLANK)
        kprint_debug("FS seeded fresh\n");
    else
        kprint_debug("FS load failed; disk protected, session is RAM-only\n");

    kprint_debug("Shell ");
    kprint_debug(baseos_shell.name);
    serial_write('\n');
    baseos_shell.prepare();
    kprint_debug("FS ready\n");

    /* The shell may have changed the display mode while preparing. */
    input_init(&device_input, fb_w, fb_h, fb_w / 2, fb_h / 2);
    mouse_init();
    if (mouse_ok)
        kprint_debug("Mouse ready\n");
    else
        kprint_debug("Mouse init failed\n");

    drain_8042();

    input_ready=1;
    baseos_shell.start();

    while (1) {
        kernel_storage_poll();
        kernel_services_poll();
        int busy = baseos_shell.turn();
        /* ELF program threads run until the next timer tick, then the
         * desktop gets its turn again. */
        busy |= thread_run_users();
        /* ATA uses polling, not IRQ14: do not sleep once per sector wait.
         * The timer interrupts every millisecond; the desktop still turns
         * once per TIMER_HZ tick when idle, or sooner for a sleeping thread. */
        if(!busy&&!fs_sync_busy()&&!input_pending(&device_input)){
            uint32_t tick=timer_ticks();
            do __asm__ volatile("hlt");
            while(timer_ticks()==tick&&!thread_wake_due());
        }
    }
}
