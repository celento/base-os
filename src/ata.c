/* BaseOS's optional data disk. The boot floppy remains independently usable.
 * Legacy ATA primary-master, 512-byte sectors, LBA28 READ/WRITE SECTORS.
 * Polling uses the alternate status register and leaves hardware IRQs disabled.
 * Explicit cache flushes form the durability barriers in fs.c's commit protocol.
 * QEMU implementation reference: https://github.com/qemu/qemu/blob/master/hw/ide/core.c
 */
#include "ata.h"
#include "platform.h"

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
static void outw(unsigned short port, unsigned short value) {
    __asm__ volatile("outw %0,%1" :: "a"(value), "Nd"(port) : "memory");
}
#endif

static unsigned capacity;
static int ready;
enum RequestPhase { REQUEST_IDLE, REQUEST_WAIT_IDLE, REQUEST_DATA,
                    REQUEST_FINISH, REQUEST_DONE, REQUEST_ERROR };
static struct {
    enum RequestPhase phase;
    unsigned char *buffer;
    unsigned lba, left, chunk_left, started;
    int writing, flushing;
} request;
_Static_assert(sizeof(request) <= 40, "ATA request must remain small and fixed-size");

int ata_request_active(void) {
    return request.phase == REQUEST_WAIT_IDLE || request.phase == REQUEST_DATA ||
           request.phase == REQUEST_FINISH;
}
int ata_ready(void) { return ready; }

static void settle(void) {
    /* Four alternate-status reads supply the ATA 400 ns selection delay. */
    for (int i = 0; i < 4; ++i) (void)inb(ATA_CONTROL);
}
static int wait_status(int data_phase) {
    unsigned start = timer_ticks();
    do {
        unsigned char status = inb(ATA_CONTROL);
        if (!status || status == 0xFF) return -1;
        if (!(status & ATA_BUSY)) {
            if (status & (ATA_ERROR | ATA_FAULT)) return -1;
            if (data_phase ? (status & ATA_DRQ) : !(status & ATA_DRQ)) return 0;
        }
        platform_poll();
    } while ((unsigned)(timer_ticks() - start) < 2 * TIMER_HZ);
    return -1;
}
int ata_probe(void) {
    unsigned short identify[256];
    if (ata_request_active()) return -1;
    request.phase = REQUEST_IDLE;
    request.buffer = 0;
    ready = 0; capacity = 0;
    outb(ATA_CONTROL, 2); /* nIEN: the controller is always polled. */
    outb(ATA_DEVICE, 0xA0); settle();
    unsigned char status = inb(ATA_STATUS);
    if (!status || status == 0xFF) return 0;
    outb(ATA_COUNT, 0); outb(ATA_LBA0, 0); outb(ATA_LBA1, 0); outb(ATA_LBA2, 0);
    outb(ATA_STATUS, ATA_IDENTIFY); settle();
    status = inb(ATA_STATUS);
    if (!status || status == 0xFF) return 0;
    if (wait_status(1) < 0) return -1;
    if (inb(ATA_LBA1) || inb(ATA_LBA2)) return -1; /* Not an ATA disk. */
    for (int i = 0; i < 256; ++i) identify[i] = inw(ATA_DATA);
    settle();
    if (wait_status(0) < 0 || (identify[0] & 0x8000) ||
        !(identify[49] & 0x200) || !(identify[83] & 0x1000)) return -1;
    /* Reject disks that report logical sectors other than 512 bytes. */
    if ((identify[106] & 0xC000) == 0x4000 && (identify[106] & 0x1000) &&
        ((unsigned)identify[117] | (unsigned)identify[118] << 16) != 256) return -1;
    capacity = identify[60] | (unsigned)identify[61] << 16;
    if (!capacity || capacity > 0x10000000u) { capacity = 0; return -1; }
    ready = 1;
    return 1;
}
unsigned ata_sector_count(void) { return capacity; }

static int request_transfer(unsigned lba, unsigned char *buffer, int sectors, int writing) {
    if (ata_request_active() || !ready || sectors < 0 || lba > capacity ||
        (unsigned)sectors > capacity - lba || (sectors && !buffer))
        return -1;
    request.buffer = sectors ? buffer : 0;
    request.lba = lba;
    request.left = (unsigned)sectors;
    request.chunk_left = 0;
    request.writing = writing;
    request.flushing = 0;
    request.started = timer_ticks();
    request.phase = sectors ? REQUEST_WAIT_IDLE : REQUEST_DONE;
    return 0;
}
int ata_request_read(unsigned lba, void *buffer, int sectors) {
    return request_transfer(lba, buffer, sectors, 0);
}
int ata_request_write(unsigned lba, const void *buffer, int sectors) {
    return request_transfer(lba, (unsigned char *)buffer, sectors, 1);
}
int ata_request_flush(void) {
    if (ata_request_active() || !ready) return -1;
    request.buffer = 0;
    request.flushing = 1;
    request.started = timer_ticks();
    request.phase = REQUEST_WAIT_IDLE;
    return 0;
}
static enum AtaProgress request_failed(void) {
    ready = 0; /* Uncertain controller state requires an explicit remount. */
    request.buffer = 0;
    request.phase = REQUEST_ERROR;
    return ATA_PROGRESS_ERROR;
}
static enum AtaProgress request_waiting(void) {
    /* Count time across polls only when the observed hardware still requires
     * a wait. Ready hardware is not a timeout after a caller scheduling gap. */
    return (unsigned)(timer_ticks() - request.started) >= 2 * TIMER_HZ ?
           request_failed() : ATA_PROGRESS_WAIT;
}
enum AtaProgress ata_poll(unsigned sector_budget) {
    if (!ata_request_active()) {
        return request.phase == REQUEST_DONE ? ATA_PROGRESS_DONE :
               request.phase == REQUEST_ERROR ? ATA_PROGRESS_ERROR : ATA_PROGRESS_IDLE;
    }
    unsigned transferred = 0;
    for (;;) {
        /* A loop iteration must advance a protocol phase, transfer one sector,
         * or return. Never repeat an observation to wait for hardware. */
        unsigned char status = inb(ATA_CONTROL);
        if (!status || status == 0xFF) return request_failed();
        if (status & ATA_BUSY) return request_waiting();
        if (status & (ATA_ERROR | ATA_FAULT)) return request_failed();

        if (request.phase == REQUEST_WAIT_IDLE) {
            if (status & ATA_DRQ) return request_waiting();
            if (request.flushing) {
                outb(ATA_STATUS, ATA_FLUSH); settle();
                request.phase = REQUEST_FINISH;
            } else {
                request.chunk_left = request.left > 128 ? 128 : request.left;
                outb(ATA_DEVICE, 0xE0 | ((request.lba >> 24) & 15)); settle();
                outb(ATA_COUNT, request.chunk_left);
                outb(ATA_LBA0, request.lba);
                outb(ATA_LBA1, request.lba >> 8);
                outb(ATA_LBA2, request.lba >> 16);
                outb(ATA_STATUS, request.writing ? ATA_WRITE : ATA_READ); settle();
                request.phase = REQUEST_DATA;
            }
            request.started = timer_ticks();
        } else if (request.phase == REQUEST_DATA) {
            if (!(status & ATA_DRQ)) return request_waiting();
            if (transferred == sector_budget) return ATA_PROGRESS_MORE;
            for (int word = 0; word < 256; ++word) {
                if (request.writing)
                    outw(ATA_DATA, request.buffer[0] | (unsigned)request.buffer[1] << 8);
                else {
                    unsigned short value = inw(ATA_DATA);
                    request.buffer[0] = value; request.buffer[1] = value >> 8;
                }
                request.buffer += 2;
            }
            ++transferred; ++request.lba; --request.left;
            if (!--request.chunk_left) request.phase = REQUEST_FINISH;
            settle();
            request.started = timer_ticks();
        } else { /* REQUEST_FINISH: command/flush completion, not just data. */
            if (status & ATA_DRQ) return request_waiting();
            if (request.flushing || !request.left) {
                request.buffer = 0;
                request.phase = REQUEST_DONE;
                return ATA_PROGRESS_DONE;
            }
            request.phase = REQUEST_WAIT_IDLE;
            request.started = timer_ticks();
        }
    }
}
static int complete_request(void) {
    for (;;) {
        /* A one-sector budget keeps the legacy device-service opportunity
         * after every sector, including the final one. */
        unsigned left = request.left;
        enum AtaProgress progress = ata_poll(1);
        if (progress == ATA_PROGRESS_MORE || progress == ATA_PROGRESS_WAIT ||
            (!request.flushing && request.left != left)) platform_poll();
        if (progress == ATA_PROGRESS_DONE) return 0;
        if (progress == ATA_PROGRESS_ERROR || progress == ATA_PROGRESS_IDLE) return -1;
    }
}
int ata_read(unsigned lba, void *buffer, int sectors) {
    return ata_request_read(lba, buffer, sectors) < 0 ? -1 : complete_request();
}
int ata_write(unsigned lba, const void *buffer, int sectors) {
    return ata_request_write(lba, buffer, sectors) < 0 ? -1 : complete_request();
}
int ata_flush(void) {
    return ata_request_flush() < 0 ? -1 : complete_request();
}
