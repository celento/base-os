#include "kernel/thread.h"
#include "kernel/uproc.h"
#include "kernel/kheap.h"
#include "kernel/vm.h"
#include "kernel/platform.h"
#include "kernel/program.h"
#include "kernel/fs.h"

/* 8 pages including the heap's 32-byte header: syscall-side device polling
 * can need about 17 KiB of stack. */
#define KERNEL_STACK_BYTES (8u * 4096u - 32u)

extern unsigned char gdt_user_tls[];
extern void interrupt_return(void);
void thread_switch(uint32_t *save, uint32_t next);

/* Thread 0 needs no setup: the ring holds only it until the first spawn. */
static Thread desktop = {.state = THREAD_RUNNABLE, .next = &desktop};
static Thread *current = &desktop;
static uint32_t next_tid = 1;
static int have_fpu;

static void tls_descriptor(uint32_t base) {
    unsigned char *p = gdt_user_tls;
    p[0] = 0xFF; p[1] = 0xFF;
    p[2] = base; p[3] = base >> 8; p[4] = base >> 16;
    p[5] = 0xF2;                /* present, ring 3, writable data */
    p[6] = 0xCF;                /* 4 KiB granularity, 32-bit, limit 0xFFFFF */
    p[7] = base >> 24;
}

void thread_init(void) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    have_fpu = (d & 1) != 0;
    tls_descriptor(0);
}

Thread *thread_current(void) { return current; }

Thread *thread_create_user(Uproc *process, uint32_t eip, uint32_t esp, uint32_t tls_base,
                           uint32_t eax) {
    Thread *thread = kmalloc(sizeof *thread);
    if (!thread) return 0;
    thread->kernel_stack = kmalloc(KERNEL_STACK_BYTES);
    if (!thread->kernel_stack) {
        kfree(thread);
        return 0;
    }
    thread->process = process;
    thread->tid = next_tid++;
    thread->state = THREAD_RUNNABLE;
    thread->tls_base = tls_base;
    if (have_fpu) {
        unsigned flags;
        __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
        /* Save the live state, capture a clean one, put the live one back. */
        uint8_t live[108] __attribute__((aligned(16)));
        __asm__ volatile("fnsave %0" : "=m"(live) :: "memory");
        /* FNINIT keeps the register contents, so overwrite all eight before
         * capturing; no other program's values reach the new thread. */
        __asm__ volatile("fninit\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\t"
                         "fninit\n\tfnsave %0" : "=m"(thread->fpu) :: "memory");
        __asm__ volatile("frstor %0" :: "m"(live) : "memory");
        __asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
    }
    /* A ring-3 frame for interrupt_return, below it the registers that
     * thread_switch pops, returning into interrupt_return. */
    uint32_t top = ((uint32_t)(uintptr_t)thread->kernel_stack + KERNEL_STACK_BYTES) & ~15u;
    UserFrame *frame = (UserFrame *)(uintptr_t)(top - sizeof(UserFrame));
    kmemset(frame, 0, sizeof *frame);
    frame->ds = frame->es = frame->fs = THREAD_USER_DATA;
    frame->gs = tls_base ? THREAD_USER_TLS : THREAD_USER_DATA;
    frame->eax = eax;
    frame->eip = eip;
    frame->cs = THREAD_USER_CODE;
    frame->eflags = 0x202;
    frame->user_esp = esp;
    frame->user_ss = THREAD_USER_DATA;
    uint32_t *stack = (uint32_t *)frame;
    *--stack = (uint32_t)(uintptr_t)interrupt_return;
    *--stack = 0x2;             /* eflags: interrupts stay off until iret */
    *--stack = 0;               /* ebp */
    *--stack = 0;               /* ebx */
    *--stack = 0;               /* esi */
    *--stack = 0;               /* edi */
    thread->saved_esp = (uint32_t)(uintptr_t)stack;
    /* Insert just behind thread 0 so a new thread runs after every older one. */
    Thread *last = &desktop;
    while (last->next != &desktop) last = last->next;
    thread->next = &desktop;
    last->next = thread;
    return thread;
}

static void switch_to(Thread *next) {
    Thread *previous = current;
    if (next == previous) return;
    if (previous->process && have_fpu) __asm__ volatile("fnsave %0" : "=m"(previous->fpu) :: "memory");
    current = next;
    if (next->process) {
        tls_descriptor(next->tls_base);
        process_set_kernel_stack(((uint32_t)(uintptr_t)next->kernel_stack + KERNEL_STACK_BYTES) & ~15u);
        if (!previous->process || previous->process != next->process)
            vm_activate(&next->process->space);
        if (have_fpu) __asm__ volatile("frstor %0" :: "m"(next->fpu) : "memory");
    } else {
        process_set_kernel_stack(process_legacy_kernel_stack());
        vm_activate(0);
    }
    thread_switch(&previous->saved_esp, next->saved_esp);
}

/* Next runnable thread after the current one in ring order; wakes waiters
 * whose timeout passed. Thread 0 is always runnable. */
static Thread *pick(void) {
    uint32_t now = timer_milliseconds();
    for (Thread *thread = current->next;; thread = thread->next) {
        if (thread->state == THREAD_WAITING && thread->wake_at &&
            (int32_t)(now - thread->wake_at) >= 0) {
            thread->state = THREAD_RUNNABLE;
            thread->timed_out = 1;
            thread->channel = 0;
        }
        if (thread->state == THREAD_RUNNABLE) return thread;
        if (thread == current) return &desktop;
    }
}

static void reschedule(void) {
    unsigned flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    switch_to(pick());
    __asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
}

static void reap(void) {
    Thread *previous = &desktop;
    for (Thread *thread = desktop.next; thread != &desktop;) {
        Thread *next = thread->next;
        if (thread->state == THREAD_DEAD) {
            previous->next = next;
            uproc_thread_gone(thread->process, thread);
            kfree(thread->kernel_stack);
            kfree(thread);
        } else {
            previous = thread;
        }
        thread = next;
    }
}

int thread_run_users(void) {
    if (current != &desktop) panic("thread: run_users off the desktop thread");
    reap();
    if (desktop.next == &desktop) return 0;
    reschedule();
    reap();
    uint32_t now = timer_milliseconds();
    for (Thread *thread = desktop.next; thread != &desktop; thread = thread->next)
        if (thread->state == THREAD_RUNNABLE ||
            (thread->state == THREAD_WAITING && thread->wake_at &&
             (int32_t)(now - thread->wake_at) >= 0))
            return 1;
    return 0;
}

int thread_wake_due(void) {
    uint32_t now = timer_milliseconds();
    for (Thread *thread = desktop.next; thread != &desktop; thread = thread->next)
        if (thread->state == THREAD_WAITING && thread->wake_at && (int32_t)(now - thread->wake_at) >= 0)
            return 1;
    return 0;
}

void thread_yield(void) { reschedule(); }

int thread_wait(uintptr_t channel, uint32_t wake_at) {
    current->state = THREAD_WAITING;
    current->channel = channel;
    current->wake_at = wake_at;
    current->timed_out = 0;
    reschedule();
    current->wake_at = 0;
    return current->timed_out;
}

unsigned thread_wake_channel(uintptr_t channel, unsigned count) {
    unsigned woken = 0;
    for (Thread *thread = desktop.next; thread != &desktop && woken < count; thread = thread->next)
        if (thread->state == THREAD_WAITING && thread->channel == channel) {
            thread->state = THREAD_RUNNABLE;
            thread->channel = 0;
            ++woken;
        }
    return woken;
}

void thread_kill(Thread *thread) {
    if (!thread || thread == &desktop) panic("thread: killing the desktop thread");
    thread->state = THREAD_DEAD;
    if (thread == current) {
        reschedule();
        panic("thread: dead thread resumed");
    }
}

void thread_kill_process(Uproc *process) {
    for (Thread *thread = desktop.next; thread != &desktop; thread = thread->next)
        if (thread->process == process && thread != current) thread->state = THREAD_DEAD;
}

unsigned thread_live_count(Uproc *process) {
    unsigned count = 0;
    for (Thread *thread = desktop.next; thread != &desktop; thread = thread->next)
        count += thread->process == process && thread->state != THREAD_DEAD;
    return count;
}

void thread_set_tls(uint32_t base) {
    current->tls_base = base;
    tls_descriptor(base);
}

unsigned thread_user_count(void) {
    unsigned count = 0;
    for (Thread *thread = desktop.next; thread != &desktop; thread = thread->next)
        count += thread->state != THREAD_DEAD;
    return count;
}
