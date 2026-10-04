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

static int transfer(unsigned lba, unsigned char *buffer, int sectors, int writing) {
    if (!ready || sectors < 0 || lba > capacity || (unsigned)sectors > capacity - lba)
        return -1;
    if (!sectors) return 0;
    if (!buffer) return -1;
    while (sectors) {
        int count = sectors > 128 ? 128 : sectors;
        if (wait_status(0) < 0) goto failed;
        outb(ATA_DEVICE, 0xE0 | ((lba >> 24) & 15)); settle();
        outb(ATA_COUNT, count);
        outb(ATA_LBA0, lba); outb(ATA_LBA1, lba >> 8); outb(ATA_LBA2, lba >> 16);
        outb(ATA_STATUS, writing ? ATA_WRITE : ATA_READ); settle();
        for (int sector = 0; sector < count; ++sector) {
            if (wait_status(1) < 0) goto failed;
            for (int word = 0; word < 256; ++word) {
                if (writing) outw(ATA_DATA, buffer[0] | (unsigned)buffer[1] << 8);
                else {
                    unsigned short value = inw(ATA_DATA);
                    buffer[0] = value; buffer[1] = value >> 8;
                }
                buffer += 2;
            }
            settle();
            platform_poll();
        }
        if (wait_status(0) < 0) goto failed;
        lba += count; sectors -= count;
    }
    return 0;
failed:
    ready = 0; /* Uncertain controller state requires an explicit remount. */
    return -1;
}
int ata_read(unsigned lba, void *buffer, int sectors) {
    return transfer(lba, buffer, sectors, 0);
}
int ata_write(unsigned lba, const void *buffer, int sectors) {
    return transfer(lba, (unsigned char *)buffer, sectors, 1);
}
int ata_flush(void) {
    if (!ready || wait_status(0) < 0) return -1;
    outb(ATA_STATUS, ATA_FLUSH); settle();
    if (wait_status(0) < 0) { ready = 0; return -1; }
    return 0;
}
