/* BaseOS's optional data disk (primary master) and exchange disks on the
 * secondary channel. The boot floppy remains independently usable.
 * Legacy ATA, 512-byte sectors, LBA28 READ/WRITE SECTORS.
 * Polling uses the alternate status register and leaves hardware IRQs disabled.
 * Explicit cache flushes form the durability barriers in fs.c's commit protocol.
 * QEMU implementation reference: https://github.com/qemu/qemu/blob/master/hw/ide/core.c
 */
#include "kernel/ata.h"
#include "kernel/platform.h"

#define ATA_DATA 0x1F0
#define ATA_COUNT 0x1F2
#define ATA_LBA0 0x1F3
#define ATA_LBA1 0x1F4
#define ATA_LBA2 0x1F5
#define ATA_DEVICE 0x1F6
#define ATA_STATUS 0x1F7
#define ATA_CONTROL 0x3F6
#define ATA_BUSY 0x80
#define ATA_READY 0x40
#define ATA_FAULT 0x20
#define ATA_DRQ 0x08
#define ATA_ERROR 0x01
#define ATA_READ 0x20
#define ATA_WRITE 0x30
#define ATA_IDENTIFY 0xEC
#define ATA_FLUSH 0xE7

#ifdef ATA_HOST_TEST
extern unsigned char ata_test_in(unsigned short port);
extern void ata_test_out(unsigned short port, unsigned char value);
extern unsigned short ata_test_inw(unsigned short port);
extern void ata_test_outw(unsigned short port, unsigned short value);
#define inb ata_test_in
#define outb ata_test_out
#define inw ata_test_inw
#define outw ata_test_outw
#else
static unsigned char inb(unsigned short port) {
    unsigned char value;
    __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}
static void outb(unsigned short port, unsigned char value) {
    __asm__ volatile("outb %0,%1" :: "a"(value), "Nd"(port) : "memory");
}
static unsigned short inw(unsigned short port) {
    unsigned short value;
    __asm__ volatile("inw %1,%0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}
#endif

/* Each DRQ sector remains the transfer/error-accounting boundary. */
static void transfer_sector(unsigned short data, unsigned char *buffer, int writing) {
#ifdef ATA_HOST_TEST
    for (unsigned word = 0; word < 256; ++word) {
        if (writing) outw(data, buffer[0] | (unsigned)buffer[1] << 8);
        else {
            unsigned short value = inw(data);
            buffer[0] = value; buffer[1] = value >> 8;
        }
        buffer += 2;
    }
#else
    unsigned words = 256;
    if (writing)
        __asm__ volatile("cld; rep outsw" : "+S"(buffer), "+c"(words)
                         : "d"(data) : "memory", "cc");
    else
        __asm__ volatile("cld; rep insw" : "+D"(buffer), "+c"(words)
                         : "d"(data) : "memory", "cc");
#endif
}

enum RequestPhase { REQUEST_IDLE, REQUEST_WAIT_IDLE, REQUEST_DATA,
                    REQUEST_FINISH, REQUEST_DONE, REQUEST_ERROR };
typedef struct {
    enum RequestPhase phase;
    unsigned char *buffer;
    unsigned lba, left, chunk_left, started;
    unsigned char writing, flushing;
    unsigned char selected;     /* this unit is selected on its channel */
} Request;
_Static_assert(sizeof(Request) <= 40, "ATA request must remain small and fixed-size");

/* Port offsets from a channel's command block. */
enum { REG_DATA = 0, REG_COUNT = 2, REG_LBA0, REG_LBA1, REG_LBA2, REG_DEVICE, REG_STATUS };

typedef struct {
    unsigned short base, control;
    unsigned char select;       /* 0xA0 master, 0xB0 slave */
    unsigned capacity;
    int ready;
    Request request;
} Unit;

static Unit units[ATA_UNITS] = {
    {ATA_DATA, ATA_CONTROL, 0xA0, 0, 0, {0}},
    {0x170, 0x376, 0xA0, 0, 0, {0}},
    {0x170, 0x376, 0xB0, 0, 0, {0}},
};
#define PRIMARY (&units[0])

static int active(const Unit *u) {
    return u->request.phase == REQUEST_WAIT_IDLE || u->request.phase == REQUEST_DATA ||
           u->request.phase == REQUEST_FINISH;
}
/* Units on one channel share its registers: one request at a time. */
static int channel_busy(const Unit *u) {
    for (int i = 0; i < ATA_UNITS; ++i)
        if (units[i].base == u->base && active(&units[i])) return 1;
    return 0;
}
int ata_request_active(void) { return active(PRIMARY); }
int ata_ready(void) { return PRIMARY->ready; }

static void settle(const Unit *u) {
    /* Four alternate-status reads supply the ATA 400 ns selection delay. */
    for (int i = 0; i < 4; ++i) (void)inb(u->control);
}
static int wait_status(const Unit *u, int data_phase) {
    unsigned start = timer_ticks();
    do {
        unsigned char status = inb(u->control);
        if (!status || status == 0xFF) return -1;
        if (!(status & ATA_BUSY)) {
            if (status & (ATA_ERROR | ATA_FAULT)) return -1;
            if (data_phase ? (status & ATA_DRQ) : !(status & ATA_DRQ)) return 0;
        }
        platform_poll();
    } while ((unsigned)(timer_ticks() - start) < 2 * TIMER_HZ);
    return -1;
}
static int probe(Unit *u) {
    unsigned short identify[256];
    if (channel_busy(u)) return -1;
    u->request.phase = REQUEST_IDLE;
    u->request.buffer = 0;
    u->ready = 0; u->capacity = 0;
    outb(u->control, 2); /* nIEN: the controller is always polled. */
    outb(u->base + REG_DEVICE, u->select); settle(u);
    unsigned char status = inb(u->base + REG_STATUS);
    if (!status || status == 0xFF) return 0;
    outb(u->base + REG_COUNT, 0); outb(u->base + REG_LBA0, 0);
    outb(u->base + REG_LBA1, 0); outb(u->base + REG_LBA2, 0);
    outb(u->base + REG_STATUS, ATA_IDENTIFY); settle(u);
    status = inb(u->base + REG_STATUS);
    if (!status || status == 0xFF) return 0;
    if (wait_status(u, 1) < 0) return -1;
    if (inb(u->base + REG_LBA1) || inb(u->base + REG_LBA2)) return -1; /* Not an ATA disk. */
    for (int i = 0; i < 256; ++i) identify[i] = inw(u->base + REG_DATA);
    settle(u);
    if (wait_status(u, 0) < 0 || (identify[0] & 0x8000) ||
        !(identify[49] & 0x200) || !(identify[83] & 0x1000)) return -1;
    /* Reject disks that report logical sectors other than 512 bytes. */
    if ((identify[106] & 0xC000) == 0x4000 && (identify[106] & 0x1000) &&
        ((unsigned)identify[117] | (unsigned)identify[118] << 16) != 256) return -1;
    u->capacity = identify[60] | (unsigned)identify[61] << 16;
    if (!u->capacity || u->capacity > 0x10000000u) { u->capacity = 0; return -1; }
    u->ready = 1;
    return 1;
}
int ata_probe(void) { return probe(PRIMARY); }
unsigned ata_sector_count(void) { return PRIMARY->capacity; }

static int request_transfer(Unit *u, unsigned lba, unsigned char *buffer, int sectors, int writing) {
    if (channel_busy(u) || !u->ready || sectors < 0 || lba > u->capacity ||
        (unsigned)sectors > u->capacity - lba || (sectors && !buffer))
        return -1;
    Request *r = &u->request;
    r->buffer = sectors ? buffer : 0;
    r->lba = lba;
    r->left = (unsigned)sectors;
    r->chunk_left = 0;
    r->writing = writing;
    r->flushing = 0;
    r->selected = 0;
    r->started = timer_ticks();
    r->phase = sectors ? REQUEST_WAIT_IDLE : REQUEST_DONE;
    return 0;
}
int ata_request_read(unsigned lba, void *buffer, int sectors) {
    return request_transfer(PRIMARY, lba, buffer, sectors, 0);
}
int ata_request_write(unsigned lba, const void *buffer, int sectors) {
    return request_transfer(PRIMARY, lba, (unsigned char *)buffer, sectors, 1);
}
static int request_flush(Unit *u) {
    if (channel_busy(u) || !u->ready) return -1;
    u->request.buffer = 0;
    u->request.flushing = 1;
    u->request.selected = 0;
    u->request.started = timer_ticks();
    u->request.phase = REQUEST_WAIT_IDLE;
    return 0;
}
int ata_request_flush(void) { return request_flush(PRIMARY); }
static enum AtaProgress request_failed(Unit *u) {
    u->ready = 0; /* Uncertain controller state requires an explicit remount. */
    u->request.buffer = 0;
    u->request.phase = REQUEST_ERROR;
    return ATA_PROGRESS_ERROR;
}
static enum AtaProgress request_waiting(Unit *u) {
    /* Count time across polls only when the observed hardware still requires
     * a wait. Ready hardware is not a timeout after a caller scheduling gap. */
    return (unsigned)(timer_ticks() - u->request.started) >= 2 * TIMER_HZ ?
           request_failed(u) : ATA_PROGRESS_WAIT;
}
static enum AtaProgress poll_unit(Unit *u, unsigned sector_budget) {
    Request *r = &u->request;
    if (!active(u)) {
        return r->phase == REQUEST_DONE ? ATA_PROGRESS_DONE :
               r->phase == REQUEST_ERROR ? ATA_PROGRESS_ERROR : ATA_PROGRESS_IDLE;
    }
    unsigned transferred = 0;
    /* Status belongs to the selected unit: on a shared channel another unit
     * (or a probe of it) may have been selected last. The primary channel
     * has one unit, selected with each command. */
    if (u != PRIMARY && !r->selected) {
        outb(u->base + REG_DEVICE, u->select); settle(u);
        r->selected = 1;
    }
    for (;;) {
        /* A loop iteration must advance a protocol phase, transfer one sector,
         * or return. Never repeat an observation to wait for hardware. */
        unsigned char status = inb(u->control);
        if (!status || status == 0xFF) return request_failed(u);
        if (status & ATA_BUSY) return request_waiting(u);
        if (status & (ATA_ERROR | ATA_FAULT)) return request_failed(u);

        if (r->phase == REQUEST_WAIT_IDLE) {
            if (status & ATA_DRQ) return request_waiting(u);
            if (r->flushing) {
                outb(u->base + REG_STATUS, ATA_FLUSH); settle(u);
                r->phase = REQUEST_FINISH;
            } else {
                r->chunk_left = r->left > 128 ? 128 : r->left;
                outb(u->base + REG_DEVICE, (u->select | 0x40) | ((r->lba >> 24) & 15)); settle(u);
                outb(u->base + REG_COUNT, r->chunk_left);
                outb(u->base + REG_LBA0, r->lba);
                outb(u->base + REG_LBA1, r->lba >> 8);
                outb(u->base + REG_LBA2, r->lba >> 16);
                outb(u->base + REG_STATUS, r->writing ? ATA_WRITE : ATA_READ); settle(u);
                r->phase = REQUEST_DATA;
            }
            r->started = timer_ticks();
        } else if (r->phase == REQUEST_DATA) {
            if (!(status & ATA_DRQ)) return request_waiting(u);
            if (transferred == sector_budget) return ATA_PROGRESS_MORE;
            transfer_sector(u->base + REG_DATA, r->buffer, r->writing);
            r->buffer += 512;
            ++transferred; ++r->lba; --r->left;
            if (!--r->chunk_left) r->phase = REQUEST_FINISH;
            settle(u);
            r->started = timer_ticks();
        } else { /* REQUEST_FINISH: command/flush completion, not just data. */
            if (status & ATA_DRQ) return request_waiting(u);
            if (r->flushing || !r->left) {
                r->buffer = 0;
                r->phase = REQUEST_DONE;
                return ATA_PROGRESS_DONE;
            }
            r->phase = REQUEST_WAIT_IDLE;
            r->started = timer_ticks();
        }
    }
}
enum AtaProgress ata_poll(unsigned sector_budget) { return poll_unit(PRIMARY, sector_budget); }
static int complete_request(Unit *u) {
    for (;;) {
        /* A one-sector budget keeps the legacy device-service opportunity
         * after every sector, including the final one. */
        unsigned left = u->request.left;
        enum AtaProgress progress = poll_unit(u, 1);
        if (progress == ATA_PROGRESS_MORE || progress == ATA_PROGRESS_WAIT ||
            (!u->request.flushing && u->request.left != left)) platform_poll();
        if (progress == ATA_PROGRESS_DONE) return 0;
        if (progress == ATA_PROGRESS_ERROR || progress == ATA_PROGRESS_IDLE) return -1;
    }
}
int ata_read(unsigned lba, void *buffer, int sectors) {
    return ata_request_read(lba, buffer, sectors) < 0 ? -1 : complete_request(PRIMARY);
}
int ata_write(unsigned lba, const void *buffer, int sectors) {
    return ata_request_write(lba, buffer, sectors) < 0 ? -1 : complete_request(PRIMARY);
}
int ata_flush(void) {
    return ata_request_flush() < 0 ? -1 : complete_request(PRIMARY);
}

/* ---------- exchange disks ---------- */

static Unit *unit_of(int unit) { return unit > 0 && unit < ATA_UNITS ? &units[unit] : 0; }
int ata_unit_probe(int unit) { Unit *u = unit_of(unit); return u ? probe(u) : -1; }
unsigned ata_unit_sectors(int unit) { Unit *u = unit_of(unit); return u && u->ready ? u->capacity : 0; }
int ata_unit_read(int unit, unsigned lba, void *buffer, int sectors) {
    Unit *u = unit_of(unit);
    return !u || request_transfer(u, lba, buffer, sectors, 0) < 0 ? -1 : complete_request(u);
}
int ata_unit_write(int unit, unsigned lba, const void *buffer, int sectors) {
    Unit *u = unit_of(unit);
    return !u || request_transfer(u, lba, (unsigned char *)buffer, sectors, 1) < 0 ? -1 : complete_request(u);
}
int ata_unit_flush(int unit) {
    Unit *u = unit_of(unit);
    return !u || request_flush(u) < 0 ? -1 : complete_request(u);
}
