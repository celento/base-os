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
static int have_fpu, have_fxsr;

/* With FXSR the x87, MMX and SSE registers travel together. FXSAVE leaves
 * the x87 state loaded, unlike FNSAVE, so reset it for kernel code. */
static void fpu_save(uint8_t *area) {
    if (have_fxsr) __asm__ volatile("fxsave %0\n\tfninit" : "=m"(*(uint8_t (*)[512])area) :: "memory");
    else __asm__ volatile("fnsave %0" : "=m"(*(uint8_t (*)[108])area) :: "memory");
}

static void fpu_restore(const uint8_t *area) {
    if (have_fxsr) __asm__ volatile("fxrstor %0" :: "m"(*(const uint8_t (*)[512])area) : "memory");
    else __asm__ volatile("frstor %0" :: "m"(*(const uint8_t (*)[108])area) : "memory");
}
/* When thread 0 handed over the CPU, and when the current thread started. */
static uint32_t round_start, slice_start;
/* When the current thread got the CPU, for per-process CPU time. */
static uint32_t switched_at;
/* Starving threads get one extra turn per THREAD_STARVE_MS between them, so
 * many low-priority threads take at most one quantum in ten. */
static uint32_t last_boost;

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
    /* FXSR (bit 24) with SSE (bit 25): let programs use SSE registers. */
    have_fxsr = have_fpu && (d & (3u << 24)) == (3u << 24);
    if (have_fxsr) {
        uint32_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= 0x600;           /* OSFXSR, OSXMMEXCPT */
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    }
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
    thread->priority = THREAD_PRIORITY_NORMAL;
    thread->last_run = timer_milliseconds();
    thread->channel = 0;
    thread->wake_at = 0;
    thread->timed_out = 0;
    if (have_fpu) {
        unsigned flags;
        __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
        /* Save the live state, capture a clean one, put the live one back. */
        static uint8_t live[512] __attribute__((aligned(16)));
        fpu_save(live);
        /* FNINIT keeps the register contents, so overwrite all eight (and the
         * SSE registers) before capturing; no other program's values reach
         * the new thread. */
        __asm__ volatile("fninit\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\tfldz\n\t"
                         "fninit" ::: "memory");
        if (have_fxsr) {
            static const uint32_t mxcsr = 0x1f80;   /* all exceptions masked */
            __asm__ volatile("xorps %%xmm0, %%xmm0\n\txorps %%xmm1, %%xmm1\n\txorps %%xmm2, %%xmm2\n\t"
                             "xorps %%xmm3, %%xmm3\n\txorps %%xmm4, %%xmm4\n\txorps %%xmm5, %%xmm5\n\t"
                             "xorps %%xmm6, %%xmm6\n\txorps %%xmm7, %%xmm7\n\tldmxcsr %0" :: "m"(mxcsr) : "memory");
        }
        fpu_save(thread->fpu);
        fpu_restore(live);
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
    uint32_t now = timer_milliseconds();
    if (previous->process) previous->process->cpu_ms += now - switched_at;
    switched_at = now;
    if (previous->process && have_fpu) fpu_save(previous->fpu);
    current = next;
    if (next->process) {
        tls_descriptor(next->tls_base);
        process_set_kernel_stack(((uint32_t)(uintptr_t)next->kernel_stack + KERNEL_STACK_BYTES) & ~15u);
        if (!previous->process || previous->process != next->process)
            vm_activate(&next->process->space);
        if (have_fpu) fpu_restore(next->fpu);
    } else {
        process_set_kernel_stack(process_legacy_kernel_stack());
        vm_activate(0);
    }
    thread_switch(&previous->saved_esp, next->saved_esp);
}

static void wake_due(Thread *thread, uint32_t now) {
    if (thread->state == THREAD_WAITING && thread->wake_at &&
        (int32_t)(now - thread->wake_at) >= 0) {
        thread->state = THREAD_RUNNABLE;
        thread->timed_out = 1;
        thread->channel = 0;
        thread->last_run = now;
    }
}

/* The runnable user thread to run next, or 0; wakes waiters whose timeout
 * passed. The highest priority wins and, within it, the thread that has
 * waited longest since it last ran or became ready. When a boost is due, a
 * lower-priority thread that has waited THREAD_STARVE_MS goes first. */
static Thread *pick_user(uint32_t now, int *boosted) {
    Thread *best = 0, *starved = 0;
    for (Thread *thread = desktop.next; thread != &desktop; thread = thread->next) {
        wake_due(thread, now);
        if (thread->state != THREAD_RUNNABLE) continue;
        if (now - thread->last_run >= THREAD_STARVE_MS &&
            (!starved || (int32_t)(thread->last_run - starved->last_run) < 0))
            starved = thread;
        if (!best || thread->priority > best->priority ||
            (thread->priority == best->priority && (int32_t)(thread->last_run - best->last_run) < 0))
            best = thread;
    }
    *boosted = 0;
    if (starved && starved->priority < best->priority && now - last_boost >= THREAD_STARVE_MS) {
        *boosted = 1;
        return starved;
    }
    return best;
}

/* Thread 0 gets the CPU back once the round has used a quantum, so many busy
 * threads delay the desktop no more than one. */
static Thread *pick(void) {
    uint32_t now = timer_milliseconds();
    if (current != &desktop && now - round_start >= THREAD_QUANTUM_MS) return &desktop;
    int boosted;
    Thread *next = pick_user(now, &boosted);
    if (!next) return &desktop;
    if (boosted) last_boost = now;
    if (current == &desktop) round_start = now;
    if (next != current) slice_start = now;
    next->last_run = now;
    return next;
}

static void reschedule(void) {
    unsigned flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
    switch_to(pick());
    __asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
}

void thread_tick(void) {
    if (current == &desktop) return;
    uint32_t now = timer_milliseconds();
    current->last_run = now;
    /* The round's deadline also bounds a thread that took over late in it
     * after another blocked or yielded. */
    if (now - slice_start >= THREAD_QUANTUM_MS || now - round_start >= THREAD_QUANTUM_MS) {
        unsigned flags;
        __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
        switch_to(&desktop);
        __asm__ volatile("pushl %0; popfl" :: "r"(flags) : "memory", "cc");
        return;
    }
    /* A higher-priority thread whose sleep ended does not wait for the
     * rest of this slice. */
    for (Thread *thread = desktop.next; thread != &desktop; thread = thread->next) {
        if (thread->priority <= current->priority) continue;
        wake_due(thread, now);
        if (thread->state == THREAD_RUNNABLE) {
            reschedule();
            return;
        }
    }
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
            /* Starvation counts from when it could run again. */
            thread->last_run = timer_milliseconds();
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

Thread *thread_find(Uproc *process, uint32_t tid) {
    for (Thread *thread = desktop.next; thread != &desktop; thread = thread->next)
        if (thread->process == process && thread->tid == tid && thread->state != THREAD_DEAD)
            return thread;
    return 0;
}

unsigned thread_user_count(void) {
    unsigned count = 0;
    for (Thread *thread = desktop.next; thread != &desktop; thread = thread->next)
        count += thread->state != THREAD_DEAD;
    return count;
}
