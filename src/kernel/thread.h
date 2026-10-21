#ifndef THREAD_H
#define THREAD_H
#include <stdint.h>

/* Kernel threads for ELF processes. Thread 0 is the desktop: kmain's loop,
 * the shell and the legacy BEX runtime all run on it with the legacy stacks.
 * Every other thread belongs to an ELF process (kernel/uproc.c), has its own
 * kernel stack and runs its program in ring 3.
 *
 * The kernel is not preemptive. A user thread loses the CPU when its quantum
 * ends while it runs in ring 3, or when a system call blocks or yields.
 * Thread 0 hands the CPU to user threads by calling thread_run_users and gets
 * it back after at most one quantum of user time, however many threads are
 * runnable. User threads are picked by priority, round-robin within a level;
 * a thread passed over for THREAD_STARVE_MS runs next whatever its level.
 * Interrupt handlers never switch threads except through the ring-3 timer
 * and system call paths. */

#define THREAD_QUANTUM_MS 10u
#define THREAD_STARVE_MS 100u
enum { THREAD_PRIORITY_LOW = -1, THREAD_PRIORITY_NORMAL = 0, THREAD_PRIORITY_HIGH = 1 };

/* Ring-3 entry frame: interrupts.asm's pushad block, data segments, vector
 * and error code, then the processor's privilege-change frame. */
typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t gs, fs, es, ds;
    uint32_t vector, error, eip, cs, eflags, user_esp, user_ss;
} UserFrame;

/* Flat ring-3 selectors for ELF programs, and the per-thread TLS segment. */
#define THREAD_USER_CODE 0x33u
#define THREAD_USER_DATA 0x3Bu
#define THREAD_USER_TLS 0x43u

enum {
    THREAD_RUNNABLE = 1,
    THREAD_WAITING,   /* on a channel, with an optional wake tick */
    THREAD_DEAD
};

struct Uproc;
typedef struct Thread {
    uint32_t saved_esp;
    uint8_t *kernel_stack;      /* 0 for thread 0 */
    struct Uproc *process;      /* 0 for thread 0 */
    uint32_t tid;
    int state;
    uintptr_t channel;          /* what a waiting thread waits for */
    uint32_t wake_at;           /* nonzero: give up waiting at this millisecond */
    int timed_out;
    int priority;               /* THREAD_PRIORITY_* */
    uint32_t last_run;          /* millisecond it was last picked or created */
    uint32_t tls_base;
    struct Thread *next;        /* ring of every thread */
    struct Thread *sibling;     /* threads of the same process */
    uint8_t fpu[108] __attribute__((aligned(16)));
} Thread;

void thread_init(void);
Thread *thread_current(void);

/* A new runnable thread that enters ring 3 at eip with esp and EAX = eax.
 * Returns 0 when memory is short. */
Thread *thread_create_user(struct Uproc *process, uint32_t eip, uint32_t esp, uint32_t tls_base,
                           uint32_t eax);

/* Thread 0 only: run user threads until the round comes back. Returns
 * nonzero while some user thread is runnable. Also frees dead threads. */
int thread_run_users(void);

/* Current user thread only. */
void thread_yield(void);
/* Timer path, current user thread in ring 3: ends its quantum when due and
 * returns the CPU to thread 0. */
void thread_tick(void);
/* The live thread with this id in process, or 0. */
Thread *thread_find(struct Uproc *process, uint32_t tid);
/* Wait on channel until thread_wake_channel or the timeout millisecond
 * (timer_milliseconds; 0: none).
 * Returns 0 when woken, nonzero on timeout. */
int thread_wait(uintptr_t channel, uint32_t wake_at);
/* Wakes up to count threads waiting on channel; returns how many. */
unsigned thread_wake_channel(uintptr_t channel, unsigned count);
/* Mark a thread dead. The current thread never returns from this. */
void thread_kill(Thread *thread);

/* Mark every thread of process dead except the current one. */
void thread_kill_process(struct Uproc *process);
/* Threads of process that are not dead. */
unsigned thread_live_count(struct Uproc *process);
/* Point the TLS segment at base (current thread). */
void thread_set_tls(uint32_t base);
/* Nonzero when a waiting user thread's timeout has passed. */
int thread_wake_due(void);
/* Number of threads that are not dead, thread 0 excluded. */
unsigned thread_user_count(void);
#endif
