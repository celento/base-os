#include "platform.h"
#include "program.h"

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

/* pushad, normalized vector/error, then the processor's ring-0 frame. */
typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t gs, fs, es, ds;
    uint32_t vector, error, eip, cs, eflags;
} InterruptFrame;
void interrupt_dispatch(InterruptFrame *f) {
    if (f->vector == 32) {
        ticks++;
        outb(0x20, 0x20);
        process_interrupt((uint32_t *)f);
        return;
    }
    if (process_interrupt((uint32_t *)f)) return;
    /* PIC IRQ7/15 can be spurious even when masked. */
    if (f->vector == 39 || f->vector == 47) {
        uint16_t pic = f->vector == 39 ? 0x20 : 0xa0;
        outb(pic, 0x0b);
        if (!(inb(pic) & 0x80)) {
            if (pic == 0xa0) outb(0x20, 0x20);
            return;
        }
    }
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
    process_init();
    struct __attribute__((packed)) { uint16_t limit; uint32_t base; } descriptor = {
        sizeof(idt) - 1, (uintptr_t)idt
    };
    __asm__ volatile("lidt %0" :: "m"(descriptor) : "memory");
    /* Move legacy IRQs away from CPU exception vectors; only PIT IRQ0 is
     * unmasked. PS/2 and floppy remain polled and do not mutate app state. */
    outb(0x21, 0xff); outb(0xa1, 0xff);
    outb(0x20, 0x11); io_wait(); outb(0xa0, 0x11); io_wait();
    outb(0x21, 32); io_wait(); outb(0xa1, 40); io_wait();
    outb(0x21, 4); io_wait(); outb(0xa1, 2); io_wait();
    outb(0x21, 1); io_wait(); outb(0xa1, 1); io_wait();
    outb(0x21, 0xfe); outb(0xa1, 0xff);
    unsigned divisor = (1193182u + TIMER_HZ / 2) / TIMER_HZ;
    outb(0x43, 0x34);  /* PIT channel 0, low/high bytes, rate generator. */
    outb(0x40, divisor & 255); outb(0x40, divisor >> 8);
    __asm__ volatile("sti" ::: "memory");
}
uint32_t timer_ticks(void) { return ticks; }
__attribute__((weak)) void platform_poll(void) {}
void timer_delay(unsigned delay) {
    uint32_t start = ticks;
    while ((uint32_t)(ticks - start) < delay) { platform_poll(); __asm__ volatile("hlt"); }
}
