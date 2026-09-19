/*
 * Floppy disk I/O.
 *
 * The BIOS is gone once we are in protected mode, so the 82077 controller is
 * driven directly: commands go out through the FIFO at 0x3F5 and the payload
 * moves over ISA DMA channel 2. Nothing here uses interrupts; the main status
 * register is polled, which is what the rest of the kernel does for PS/2 too.
 *
 * DMA can only reach the low 16MB and a transfer must not cross a 64KB
 * boundary, so every transfer is staged through BOUNCE, which is 64KB aligned
 * and one track long.
 */

#include "persist.h"
#include "fs.h"
#include "platform.h"

#define FDC_DOR    0x3F2
#define FDC_MSR    0x3F4
#define FDC_FIFO   0x3F5
#define FDC_CCR    0x3F7

#define MSR_RQM    0x80
#define MSR_DIO    0x40
#define MSR_BUSY   0x10

#define CMD_SPECIFY     0x03
#define CMD_WRITE       0x45   /* WRITE DATA | MFM */
#define CMD_READ        0x46   /* READ DATA | MFM */
#define CMD_RECALIBRATE 0x07
#define CMD_SENSE_INT   0x08
#define CMD_SEEK        0x0F

#define SECTORS_PER_TRACK sectors_per_track
#define HEADS             2


#define BOUNCE DMA_BASE
#define BOUNCE_MAX 36

static unsigned sectors_per_track;
void disk_configure(unsigned spt) {
    sectors_per_track = (spt == 18 || spt == 36) ? spt : 0;
}
unsigned disk_sector_count(void) { return sectors_per_track * HEADS * 80; }

static int fdc_ready = 0;
static int motor_running = 0;

#ifdef PERSIST_HOST_TEST
extern void fdc_test_out(unsigned short port, unsigned char val);
extern unsigned char fdc_test_in(unsigned short port);
#define outb fdc_test_out
#define inb fdc_test_in
#else
static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

#endif

static int fdc_send(unsigned char b) {
    uint32_t start = timer_ticks();
    while ((uint32_t)(timer_ticks() - start) < 2 * TIMER_HZ) {
        platform_poll();
        unsigned char m = inb(FDC_MSR);
        if ((m & (MSR_RQM | MSR_DIO)) == MSR_RQM) {
            outb(FDC_FIFO, b);
            return 0;
        }
    }
    return -1;
}

static int fdc_recv(void) {
    uint32_t start = timer_ticks();
    while ((uint32_t)(timer_ticks() - start) < 2 * TIMER_HZ) {
        platform_poll();
        unsigned char m = inb(FDC_MSR);
        if ((m & (MSR_RQM | MSR_DIO)) == (MSR_RQM | MSR_DIO))
            return inb(FDC_FIFO);
    }
    return -1;
}

/* Seek and recalibrate have no result phase; they finish when the command
 * and drive-busy bits drop, and the outcome comes back from SENSE INTERRUPT. */
static int fdc_wait_idle(void) {
    uint32_t start = timer_ticks();
    while ((uint32_t)(timer_ticks() - start) < 2 * TIMER_HZ) {
        platform_poll();
        unsigned char m = inb(FDC_MSR);
        if (!(m & (MSR_BUSY | 0x0F)))
            return 0;
    }
    return -1;
}

static int fdc_sense(int *st0, int *cyl) {
    if (fdc_send(CMD_SENSE_INT) < 0)
        return -1;
    int a = fdc_recv();
    int b = fdc_recv();
    if (a < 0 || b < 0)
        return -1;
    if (st0)
        *st0 = a;
    if (cyl)
        *cyl = b;
    return 0;
}

static void motor_on(void) {
    if (motor_running)
        return;
    outb(FDC_DOR, 0x1C);        /* drive 0 selected, reset off, DMA on, motor A */
    timer_delay(TIMER_HZ / 2);  /* At least 500ms, independent of CPU speed. */
    motor_running = 1;
}

static void motor_off(void) {
    outb(FDC_DOR, 0x0C);
    motor_running = 0;
}

static int fdc_recalibrate(void) {
    for (int try = 0; try < 3; try++) {
        if (fdc_send(CMD_RECALIBRATE) < 0 || fdc_send(0) < 0)
            return -1;
        if (fdc_wait_idle() < 0)
            return -1;
        int st0 = 0, cyl = 0;
        if (fdc_sense(&st0, &cyl) < 0)
            return -1;
        if ((st0 & 0xC0) == 0 && cyl == 0)
            return 0;
    }
    return -1;
}

static int fdc_init(void) {
    if (fdc_ready)
        return 0;

    outb(0x0A, 0x06);
    motor_running = 0;
    outb(FDC_DOR, 0x00);
    timer_delay(1);
    outb(FDC_DOR, 0x0C);        /* leave reset with DMA/IRQ enabled */
    timer_delay(1);

    /* A reset posts four pending interrupts, one per drive. */
    for (int i = 0; i < 4; i++)
        if (fdc_sense(0, 0) < 0) return -1;

    outb(FDC_CCR, sectors_per_track == 36 ? 3 : 0); /* 1M or 500k bit/s */

    if (fdc_send(CMD_SPECIFY) < 0 || fdc_send(0xDF) < 0 ||
        fdc_send(0x02) < 0) return -1;

    motor_on();
    if (fdc_recalibrate() < 0)
        return -1;

    fdc_ready = 1;
    return 0;
}

static int fdc_seek(int cyl, int head) {
    for (int try = 0; try < 3; try++) {
        if (fdc_send(CMD_SEEK) < 0 || fdc_send((unsigned char)(head << 2)) < 0 ||
            fdc_send((unsigned char)cyl) < 0)
            return -1;
        if (fdc_wait_idle() < 0)
            return -1;
        int st0 = 0, pcn = 0;
        if (fdc_sense(&st0, &pcn) < 0)
            return -1;
        if ((st0 & 0xC0) == 0 && pcn == cyl)
            return 0;
    }
    return -1;
}

static void dma_setup(unsigned int addr, int len, int to_disk) {
    unsigned int last = (unsigned int)len - 1;
    (void)inb(0x08);            /* Clear stale terminal-count status. */
    outb(0x0A, 0x06);           /* mask channel 2 */
    outb(0x0C, 0xFF);           /* clear the byte-pair flip-flop */
    outb(0x0B, to_disk ? 0x4A : 0x46);
    outb(0x0C, 0xFF);
    outb(0x04, (unsigned char)(addr & 0xFF));
    outb(0x04, (unsigned char)((addr >> 8) & 0xFF));
    outb(0x81, (unsigned char)((addr >> 16) & 0xFF));
    outb(0x0C, 0xFF);
    outb(0x05, (unsigned char)(last & 0xFF));
    outb(0x05, (unsigned char)((last >> 8) & 0xFF));
    outb(0x0A, 0x02);           /* unmask channel 2 */
}

/* One command, one track, no head switching. Returns 0 on success. */
static int fdc_track_io(int to_disk, int cyl, int head, int sec, int count) {
    dma_setup(BOUNCE, count * SECTOR_SIZE, to_disk);

    unsigned char command[] = {
        to_disk ? CMD_WRITE : CMD_READ, (unsigned char)(head << 2),
        (unsigned char)cyl, (unsigned char)head, (unsigned char)sec,
        2, (unsigned char)(sec + count - 1), 0x1B, 0xFF
    };
    int st[7];
    for (unsigned i = 0; i < sizeof(command); ++i)
        if (fdc_send(command[i]) < 0) goto failed;
    for (int i = 0; i < 7; i++) {
        st[i] = fdc_recv();
        if (st[i] < 0) goto failed;
    }
    outb(0x0A, 0x06);
    /* DMA terminal count must confirm all bytes. ST1 EOC is allowed only
     * with a normal ST0; all other status errors reject the transaction. */
    if (!(inb(0x08) & 4) || (st[0] & 0xC0) || (st[1] & ~0x80) || st[2])
        return -1;
    return 0;
failed:
    outb(0x0A, 0x06);
    return -1;
}

static int disk_io(int to_disk, unsigned int lba, unsigned char *buf, int sectors) {
    if (sectors < 0 || !sectors_per_track ||
        lba > disk_sector_count() || (unsigned)sectors > disk_sector_count() - lba)
        return -1;
    if (!sectors) return 0;
    if (!buf) return -1;

    while (sectors > 0) {
        int cyl = (int)(lba / (SECTORS_PER_TRACK * HEADS));
        int rem = (int)(lba % (SECTORS_PER_TRACK * HEADS));
        int head = rem / SECTORS_PER_TRACK;
        int sec = rem % SECTORS_PER_TRACK + 1;

        int count = SECTORS_PER_TRACK - sec + 1;
        if (count > sectors)
            count = sectors;
        if (count > BOUNCE_MAX)
            count = BOUNCE_MAX;
        int bytes = count * SECTOR_SIZE;

        int ok = -1;
        for (int attempt = 0; attempt < 3 && ok < 0; attempt++) {
            if (fdc_init() == 0) {
                motor_on();
                /* Every retry seeks again after reset/recalibration. */
                if (fdc_seek(cyl, head) == 0) {
                    if (to_disk) kmemcpy((void *)BOUNCE, buf, bytes);
                    __asm__ volatile("" ::: "memory");
                    ok = fdc_track_io(to_disk, cyl, head, sec, count);
                    __asm__ volatile("" ::: "memory");
                }
            }
            if (ok < 0) {
                outb(0x0A, 0x06);
                motor_off();
                fdc_ready = 0; /* A fresh reset also discards stale FIFO data. */
            }
        }
        if (ok < 0) {
            motor_off();
            fdc_ready = 0;
            return -1;
        }
        if (!to_disk)
            kmemcpy(buf, (const void *)BOUNCE, bytes);

        buf += bytes;
        lba += (unsigned int)count;
        sectors -= count;
    }

    motor_off();
    return 0;
}

int disk_read(unsigned int lba, void *buf, int sectors) {
    return disk_io(0, lba, (unsigned char *)buf, sectors);
}

int disk_write(unsigned int lba, const void *buf, int sectors) {
    return disk_io(1, lba, (unsigned char *)buf, sectors);
}
