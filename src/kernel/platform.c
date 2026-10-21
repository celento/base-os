#include "kernel/platform.h"
#include "kernel/program.h"
#include "kernel/uproc.h"
#include "../sdk/baseos_sys.h"

static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p) : "memory");
}
static inline uint8_t inb(uint16_t p) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p) : "memory");
    return v;
}
static void io_wait(void) { outb(0x80, 0); }

void platform_log(const char *s) {
    while (*s) {
        unsigned n = 100000;
        while (!(inb(0x3fd) & 0x20) && --n) { }
        if (!n) return;
        outb(0x3f8, (uint8_t)*s++);
    }
}
static void hex(uint32_t v) {
    char s[9];
    for (int i = 0; i < 8; ++i) s[i] = "0123456789ABCDEF"[(v >> (28 - 4*i)) & 15];
    s[8] = 0;
    platform_log(s);
}
void panic(const char *message) {
    __asm__ volatile("cli");
    platform_log("PANIC: ");
    platform_log(message);
    platform_log("\n");
    for (;;) __asm__ volatile("hlt");
}

typedef struct __attribute__((packed)) {
    uint16_t low, selector;
    uint8_t zero, attributes;
    uint16_t high;
} Gate;
static Gate idt[256];
extern void *isr_table[256];
static volatile uint32_t ticks;
/* The PIT interrupts at about 1 kHz. Legacy ticks (TIMER_HZ) are counted
 * from the same interrupt, so code written for 70 Hz keeps its timing. */
#define PIT_HZ 1193182u
#define PIT_DIVISOR ((PIT_HZ + TIMER_IRQ_HZ / 2) / TIMER_IRQ_HZ)
#define PIT_PERIOD_NS 999847u           /* PIT_DIVISOR / PIT_HZ seconds */
#define PIT_TICK_NS_X1024 858209u       /* 1e9 / PIT_HZ, times 1024 */
static volatile uint32_t milliseconds, clock_seconds, clock_nanoseconds;
static uint32_t legacy_phase, millisecond_phase;
/* Device IRQs 1-15. A line with a handler runs it in the interrupt; any
 * other attached line is masked until the kernel loop has serviced it. */
static PlatformIrqHandler irq_handlers[16];
static volatile uint16_t irq_attached, irq_deferred_pending;
static volatile uint32_t irq_counts[16];
static uint16_t irq_mask = 0xfffe;   /* only IRQ0 (PIT) at start */

static void irq_mask_write(void) {
    uint16_t mask = irq_mask;
    /* Any open slave line needs the IRQ2 cascade. */
    if ((mask & 0xff00) != 0xff00) mask = (uint16_t)(mask & ~4u);
    outb(0x21, mask & 255);
    outb(0xa1, mask >> 8);
}
static void irq_eoi(unsigned irq) {
    if (irq >= 8) outb(0xa0, 0x20);
    outb(0x20, 0x20);
}
/* IRQ7 and IRQ15 can be spurious; the in-service bit tells. */
static int irq_spurious(unsigned irq) {
    if (irq != 7 && irq != 15) return 0;
    uint16_t pic = irq == 7 ? 0x20 : 0xa0;
    outb(pic, 0x0b);
    if (inb(pic) & 0x80) return 0;
    if (pic == 0xa0) outb(0x20, 0x20);
    return 1;
}
static void irq_dispatch(unsigned irq) {
    if (irq_spurious(irq)) return;
    if (!(irq_attached & (1u << irq))) {
        irq_eoi(irq);
        return;
    }
    ++irq_counts[irq];
    if (irq_handlers[irq]) {
        irq_handlers[irq]();
    } else {
        irq_mask |= (uint16_t)(1u << irq);
        irq_mask_write();
        irq_deferred_pending |= (uint16_t)(1u << irq);
    }
    irq_eoi(irq);
}
void platform_irq_attach(unsigned irq, PlatformIrqHandler handler) {
    /* IRQ2 is the cascade; a line already in use keeps its first owner. */
    if (irq == 0 || irq == 2 || irq > 15 || (irq_attached & (1u << irq))) return;
    unsigned flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    irq_handlers[irq] = handler;
    irq_attached |= (uint16_t)(1u << irq);
    irq_mask &= (uint16_t)~(1u << irq);
    irq_mask_write();
    __asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
}
unsigned platform_irq_take(void) {
    unsigned flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    unsigned pending = irq_deferred_pending;
    irq_deferred_pending = 0;
    __asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
    return pending;
}
int platform_irq_pending(void) { return irq_deferred_pending != 0; }
uint32_t platform_irq_count(unsigned irq) { return irq < 16 ? irq_counts[irq] : 0; }
void platform_irq_rearm(unsigned lines) {
    lines &= irq_attached;
    if (!lines) return;
    unsigned flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    irq_mask &= (uint16_t)~lines;
    irq_mask_write();
    __asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
}

/* pushad, normalized vector/error, then the processor's ring-0 frame. */
typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t gs, fs, es, ds;
    uint32_t vector, error, eip, cs, eflags;
} InterruptFrame;
void interrupt_dispatch(InterruptFrame *f) {
    if (f->vector == 32) {
        int legacy = 0;
        clock_nanoseconds += PIT_PERIOD_NS;
        if (clock_nanoseconds >= 1000000000u) { clock_nanoseconds -= 1000000000u; ++clock_seconds; }
        millisecond_phase += PIT_PERIOD_NS;
        while (millisecond_phase >= 1000000u) { millisecond_phase -= 1000000u; ++milliseconds; }
        legacy_phase += TIMER_HZ;
        if (legacy_phase >= TIMER_IRQ_HZ) { legacy_phase -= TIMER_IRQ_HZ; ++ticks; legacy = 1; }
        outb(0x20, 0x20);
        /* BEX tasks keep their per-tick scheduling; an ELF thread in ring 3
         * is checked against its quantum every millisecond. */
        if (legacy) process_interrupt((uint32_t *)f);
        uproc_interrupt((uint32_t *)f);
        return;
    }
    /* Device IRQs come before the BEX and ELF runtimes, which end a
     * program for any vector they do not own. */
    if (f->vector > 32 && f->vector < 48) {
        irq_dispatch(f->vector - 32);
        return;
    }
    if (process_interrupt((uint32_t *)f)) return;
    if (uproc_interrupt((uint32_t *)f)) return;
    platform_log("EXCEPTION vector="); hex(f->vector);
    platform_log(" error="); hex(f->error);
    platform_log(" eip="); hex(f->eip);
    platform_log("\n");
    panic("CPU exception");
}

void platform_init(void) {
    __asm__ volatile("cli");
    outb(0x3f9, 0); outb(0x3fb, 0x80);
    outb(0x3f8, 3); outb(0x3f9, 0);
    outb(0x3fb, 3); outb(0x3fa, 0xc7); outb(0x3fc, 0x0b);
    for (unsigned i = 0; i < 256; ++i) {
        uintptr_t address = (uintptr_t)isr_table[i];
        idt[i] = (Gate){address & 0xffff, 8, 0, 0x8e, address >> 16};
    }
    idt[128].attributes=0xee;
    idt[BOS_SYS_VECTOR].attributes = 0xee;
    process_init();
    struct __attribute__((packed)) { uint16_t limit; uint32_t base; } descriptor = {
        sizeof(idt) - 1, (uintptr_t)idt
    };
    __asm__ volatile("lidt %0" :: "m"(descriptor) : "memory");
    /* Move legacy IRQs away from CPU exception vectors; only PIT IRQ0 is
     * unmasked until a driver attaches its line. Floppy remains polled. */
    outb(0x21, 0xff); outb(0xa1, 0xff);
    outb(0x20, 0x11); io_wait(); outb(0xa0, 0x11); io_wait();
    outb(0x21, 32); io_wait(); outb(0xa1, 40); io_wait();
    outb(0x21, 4); io_wait(); outb(0xa1, 2); io_wait();
    outb(0x21, 1); io_wait(); outb(0xa1, 1); io_wait();
    irq_mask_write();
    outb(0x43, 0x34);  /* PIT channel 0, low/high bytes, rate generator. */
    outb(0x40, PIT_DIVISOR & 255); outb(0x40, PIT_DIVISOR >> 8);
    __asm__ volatile("sti" ::: "memory");
}
uint32_t timer_ticks(void) { return ticks; }
uint32_t timer_milliseconds(void) { return milliseconds; }
void timer_clock(uint32_t *seconds, uint32_t *nanoseconds) {
    static uint32_t last_seconds, last_nanoseconds;
    unsigned flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    uint32_t s = clock_seconds, ns = clock_nanoseconds;
    outb(0x43, 0x00);  /* latch channel 0 */
    uint32_t count = inb(0x40);
    count |= (uint32_t)inb(0x40) << 8;
    outb(0x20, 0x0a);  /* read IRR: an undelivered IRQ0 means the period ended */
    int pending = inb(0x20) & 1;
    uint32_t elapsed = count && count <= PIT_DIVISOR ? PIT_DIVISOR - count : 0;
    if (pending && elapsed < PIT_DIVISOR / 2) elapsed += PIT_DIVISOR;
    ns += (elapsed * PIT_TICK_NS_X1024) >> 10;
    while (ns >= 1000000000u) { ns -= 1000000000u; ++s; }
    /* Never step backwards across the latch and interrupt races. */
    if (s < last_seconds || (s == last_seconds && ns < last_nanoseconds)) {
        s = last_seconds;
        ns = last_nanoseconds;
    }
    last_seconds = s;
    last_nanoseconds = ns;
    __asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
    *seconds = s;
    *nanoseconds = ns;
}
__attribute__((weak)) void platform_poll(void) {}
void timer_delay(unsigned delay) {
    uint32_t start = ticks;
    while ((uint32_t)(ticks - start) < delay) { platform_poll(); __asm__ volatile("hlt"); }
}
