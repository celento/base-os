#include "kernel/uproc.h"
#include "kernel/elf.h"
#include "kernel/kheap.h"
#include "kernel/physmem.h"
#include "kernel/platform.h"
#include "kernel/program.h"
#include "kernel/rtc.h"
#include "kernel/fs.h"

#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_ENTRY 9
#define AT_RANDOM 25
#define LOG_MAX 256u
/* Sleeps and timeouts are capped at about 3 days so millisecond deadlines
 * stay well inside the wrap-safe 32-bit range. */
#define WAIT_SECONDS_MAX 262144u

static Uproc *processes;
static uint32_t next_pid = 1;
static int threads_ready;

static Uproc *find(uint32_t pid) {
    for (Uproc *process = processes; process; process = process->next)
        if (process->pid == pid) return process;
    return 0;
}

static void unlink_process(Uproc *process) {
    for (Uproc **link = &processes; *link; link = &(*link)->next)
        if (*link == process) {
            *link = process->next;
            return;
        }
}

/* Every thread is gone: free the address space, and the record too once the
 * host no longer needs the exit status. */
static void process_finish(Uproc *process) {
    vm_destroy(&process->space);
    if (process->state == UPROC_UNKNOWN) {
        unlink_process(process);
        kfree(process);
    }
}

void uproc_thread_gone(Uproc *process, Thread *thread) {
    (void)thread;
    if (process && !--process->threads) process_finish(process);
}

/* The process ends with status; every thread dies. Returns only when the
 * caller is not one of its threads. */
static void log_decimal(int value) {
    char text[12];
    int i = 11;
    unsigned magnitude = value < 0 ? 0u - (unsigned)value : (unsigned)value;
    text[i] = 0;
    do { text[--i] = (char)('0' + magnitude % 10); magnitude /= 10; } while (magnitude);
    if (value < 0) text[--i] = '-';
    platform_log(text + i);
}

static void process_exit(Uproc *process, int status) {
    if (process->state == UPROC_RUNNING) {
        process->state = UPROC_EXITED;
        process->exit_code = status;
        platform_log("uproc: pid ");
        log_decimal((int)process->pid);
        platform_log(" exited status ");
        log_decimal(status);
        platform_log("\n");
    }
    thread_kill_process(process);
    thread_wake_channel((uintptr_t)process->output, ~0u);
    Thread *self = thread_current();
    if (self->process == process) thread_kill(self);
}

static void log_text(const char *text, unsigned length) {
    char line[LOG_MAX + 1];
    if (length > LOG_MAX) length = LOG_MAX;
    kmemcpy(line, text, (int)length);
    line[length] = 0;
    platform_log(line);
}

/* ---------- output ---------- */

static int output_write(Uproc *process, uint32_t buffer, uint32_t length) {
    if (!length) return 0;
    /* Wait for room first: another thread may unmap the buffer meanwhile,
     * so it is checked only after the last point where this thread blocks. */
    while (process->host && process->output_count == UPROC_OUTPUT_BYTES) {
        thread_wait((uintptr_t)process->output, 0);
        if (process->state != UPROC_RUNNING) return -BOS_EIO;
    }
    int result = vm_prepare(&process->space, buffer, length, 0);
    if (result) return result;
    const char *source = (const char *)(uintptr_t)buffer;
    if (!process->host) {
        /* Nobody drains this process: its output goes to the debug log. */
        for (uint32_t done = 0; done < length; done += LOG_MAX)
            log_text(source + done, length - done < LOG_MAX ? length - done : LOG_MAX);
        return (int)length;
    }
    uint32_t written = 0;
    while (written < length && process->output_count < UPROC_OUTPUT_BYTES) {
        unsigned tail = (process->output_head + process->output_count) % UPROC_OUTPUT_BYTES;
        process->output[tail] = source[written++];
        ++process->output_count;
    }
    return (int)written;
}

unsigned uproc_take_output(uint32_t pid, char *out, unsigned capacity) {
    Uproc *process = find(pid);
    if (!process || !out) return 0;
    unsigned taken = 0;
    while (taken < capacity && process->output_count) {
        out[taken++] = process->output[process->output_head];
        process->output_head = (process->output_head + 1) % UPROC_OUTPUT_BYTES;
        --process->output_count;
    }
    if (taken) thread_wake_channel((uintptr_t)process->output, ~0u);
    return taken;
}

/* ---------- time ---------- */

static uint32_t days_from_civil(int year, int month, int day) {
    year -= month <= 2;
    int era = year / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + (int)doe - 719468);
}

static uint32_t boot_epoch, boot_epoch_ready;

static void monotonic(BosTimespec *out) {
    uint32_t seconds, nanoseconds;
    timer_clock(&seconds, &nanoseconds);
    out->seconds = seconds;
    out->nanoseconds = (long)nanoseconds;
}

static void realtime(BosTimespec *out) {
    monotonic(out);
    if (!boot_epoch_ready) {
        RtcTime now;
        rtc_read(&now);
        uint32_t seconds = days_from_civil(now.year, now.month, now.day) * 86400u +
                           (uint32_t)(now.hour * 3600 + now.min * 60 + now.sec);
        boot_epoch = seconds - (uint32_t)out->seconds;
        boot_epoch_ready = 1;
    }
    out->seconds += boot_epoch;
}

/* Milliseconds to wait for a relative timespec, rounded up. A wait ends
 * after at least that long: the next millisecond boundary counts as one. */
static int milliseconds_for(const BosTimespec *span, uint32_t *milliseconds) {
    if (span->seconds < 0 || span->nanoseconds < 0 || span->nanoseconds >= 1000000000L) return -BOS_EINVAL;
    uint32_t seconds = span->seconds > WAIT_SECONDS_MAX ? WAIT_SECONDS_MAX : (uint32_t)span->seconds;
    uint32_t part = (uint32_t)span->nanoseconds / 1000000u + ((uint32_t)span->nanoseconds % 1000000u != 0);
    *milliseconds = seconds * 1000u + part;
    return 0;
}

static uint32_t deadline(uint32_t milliseconds) {
    /* +1: the current millisecond is already partly over. */
    uint32_t at = timer_milliseconds() + milliseconds + 1;
    return at ? at : 1;
}

/* ---------- system calls ---------- */

static int copy_in(Uproc *process, void *out, uint32_t address, uint32_t length) {
    int result = vm_prepare(&process->space, address, length, 0);
    if (!result) kmemcpy(out, (const void *)(uintptr_t)address, (int)length);
    return result;
}

static int copy_out(Uproc *process, uint32_t address, const void *in, uint32_t length) {
    int result = vm_prepare(&process->space, address, length, 1);
    if (!result) kmemcpy((void *)(uintptr_t)address, in, (int)length);
    return result;
}

static int sys_futex_wait(Uproc *process, uint32_t address, int expected, uint32_t timeout) {
    if (address & 3) return -BOS_EINVAL;
    uint32_t wake = 0;
    if (timeout) {
        BosTimespec span;
        int result = copy_in(process, &span, timeout, sizeof span);
        if (result) return result;
        uint32_t wait;
        if ((result = milliseconds_for(&span, &wait))) return result;
        wake = deadline(wait);
    }
    int value;
    int result = copy_in(process, &value, address, 4);
    if (result) return result;
    if (value != expected) return -BOS_EAGAIN;
    /* The physical address names the word across threads of this process. */
    if (thread_wait(vm_physical(&process->space, address), wake)) return -BOS_ETIMEDOUT;
    return 0;
}

static int sys_thread_create(Uproc *process, uint32_t entry, uint32_t stack, uint32_t argument,
                             uint32_t tcb) {
    /* The thread starts at entry with ESP = stack and EAX = argument; the
     * caller lays out its stack. */
    if (entry < BOS_USER_BASE || entry >= BOS_USER_END || stack <= BOS_USER_BASE || stack > BOS_USER_END)
        return -BOS_EINVAL;
    Thread *thread = thread_create_user(process, entry, stack, tcb, argument);
    if (!thread) return -BOS_EAGAIN;
    ++process->threads;
    return (int)thread->tid;
}

static int sys_meminfo(Uproc *process, uint32_t address) {
    PhysmemStats stats;
    BosMemInfo info;
    kmemset(&info, 0, sizeof info);
    if (physmem_stats_all(&stats) == PHYS_OK) {
        info.total_pages = stats.total;
        info.free_pages = stats.free;
    }
    info.struct_size = sizeof info;
    info.page_bytes = VM_PAGE;
    info.process_pages = process->space.resident_pages + process->space.table_pages;
    info.mapped_bytes = process->space.reserved_bytes;
    return copy_out(process, address, &info, sizeof info);
}

static int syscall(Uproc *process, UserFrame *f) {
    uint32_t a = f->ebx, b = f->ecx, c = f->edx, d = f->esi, e = f->edi, g = f->ebp;
    switch (f->eax) {
    case BOS_SYS_EXIT:
        process_exit(process, (int)a);
        return 0;
    case BOS_SYS_LOG: {
        char text[LOG_MAX];
        if (b > LOG_MAX) b = LOG_MAX;
        int result = copy_in(process, text, a, b);
        if (result) return result;
        log_text(text, b);
        platform_log("\n");
        return 0;
    }
    case BOS_SYS_WRITE:
        if (a != 1 && a != 2) return -BOS_EBADF;
        return output_write(process, b, c);
    case BOS_SYS_READ:
        if (a != 0) return -BOS_EBADF;
        return 0;
    case BOS_SYS_OPEN:
        return -BOS_ENOENT;
    case BOS_SYS_CLOSE:
        return a <= 2 ? 0 : -BOS_EBADF;
    case BOS_SYS_SEEK:
        return a <= 2 ? -BOS_ESPIPE : -BOS_EBADF;
    case BOS_SYS_ISATTY:
        return a <= 2 ? 0 : -BOS_EBADF;
    case BOS_SYS_VM_MAP: {
        (void)e;
        (void)g;
        if (!(d & BOS_MAP_ANONYMOUS)) return -BOS_ENOSYS;
        uint32_t address;
        int result = vm_map(&process->space, a, b, c, (d & BOS_MAP_FIXED) != 0, &address);
        return result ? result : (int)address;
    }
    case BOS_SYS_VM_UNMAP:
        return vm_unmap(&process->space, a, b);
    case BOS_SYS_VM_PROTECT:
        return vm_protect(&process->space, a, b, c);
    case BOS_SYS_TCB_SET:
        thread_set_tls(a);
        f->gs = THREAD_USER_TLS;
        return 0;
    case BOS_SYS_CLOCK_GET: {
        BosTimespec now;
        if (a == BOS_CLOCK_REALTIME) realtime(&now);
        else if (a == BOS_CLOCK_MONOTONIC) monotonic(&now);
        else return -BOS_EINVAL;
        return copy_out(process, b, &now, sizeof now);
    }
    case BOS_SYS_SLEEP: {
        BosTimespec span, none = {0, 0};
        uint32_t wait;
        int result = copy_in(process, &span, a, sizeof span);
        if (result || (result = milliseconds_for(&span, &wait))) return result;
        thread_wait(0, deadline(wait));
        return b ? copy_out(process, b, &none, sizeof none) : 0;
    }
    case BOS_SYS_YIELD:
        thread_yield();
        return 0;
    case BOS_SYS_FUTEX_WAIT:
        return sys_futex_wait(process, a, (int)b, c);
    case BOS_SYS_FUTEX_WAKE: {
        if (a & 3) return -BOS_EINVAL;
        uint32_t physical = vm_physical(&process->space, a);
        return physical ? (int)thread_wake_channel(physical, b) : 0;
    }
    case BOS_SYS_THREAD_CREATE:
        return sys_thread_create(process, a, b, c, d);
    case BOS_SYS_THREAD_EXIT:
        /* The last live thread ends the process with status 0. */
        if (thread_live_count(process) == 1) process_exit(process, 0);
        thread_kill(thread_current());
        return 0;
    case BOS_SYS_GETPID:
        return (int)process->pid;
    case BOS_SYS_GETTID:
        return (int)thread_current()->tid;
    case BOS_SYS_MEMINFO:
        return sys_meminfo(process, a);
    case BOS_SYS_THREAD_PRIORITY: {
        Thread *thread = a ? thread_find(process, a) : thread_current();
        if (!thread) return -BOS_ESRCH;
        if ((int)b < THREAD_PRIORITY_LOW || (int)b > THREAD_PRIORITY_HIGH) return -BOS_EINVAL;
        thread->priority = (int)b;
        return 0;
    }
    default:
        return -BOS_ENOSYS;
    }
}

static int fault_status(uint32_t vector) {
    if (vector == 13 || vector == 14) return 128 + 11;       /* SIGSEGV */
    if (vector == 6) return 128 + 4;                         /* SIGILL */
    if (vector == 0 || vector == 16 || vector == 19) return 128 + 8; /* SIGFPE */
    if (vector >= 32) return 128 + 31;                       /* SIGSYS */
    return 128 + 6;                                          /* SIGABRT */
}

static void log_hex(uint32_t value) {
    char text[11] = "0x";
    for (int i = 0; i < 8; ++i) text[2 + i] = "0123456789ABCDEF"[(value >> (28 - 4 * i)) & 15];
    text[10] = 0;
    platform_log(text);
}

int uproc_interrupt(uint32_t *registers) {
    Thread *thread = thread_current();
    Uproc *process = thread->process;
    UserFrame *f = (UserFrame *)registers;
    if (!process || (f->cs & 3) != 3) return 0;
    if (f->vector == 32) {
        thread_tick();
        return 1;
    }
    if (f->vector == BOS_SYS_VECTOR) {
        f->eax = (uint32_t)syscall(process, f);
        return 1;
    }
    if (f->vector == 14) {
        uint32_t address;
        __asm__ volatile("mov %%cr2,%0" : "=r"(address));
        if (vm_fault(&process->space, address, (f->error & 2) != 0)) return 1;
        platform_log("uproc: page fault at ");
        log_hex(address);
        platform_log(" ");
    }
    /* Device interrupts (including spurious ones) stay with platform.c. Any
     * other vector a program reaches, such as the BEX call gate, ends it. */
    if (f->vector > 32 && f->vector < 48) return 0;
    platform_log("uproc: pid ");
    log_hex(process->pid);
    platform_log(" vector ");
    log_hex(f->vector);
    platform_log(" eip ");
    log_hex(f->eip);
    platform_log("\n");
    process_exit(process, fault_status(f->vector));
    return 1;
}

/* ---------- lifecycle ---------- */

static int build_stack(Uproc *process, const ElfImage *image, const char *const *argv,
                       unsigned argc, uint32_t *out_sp) {
    uint32_t top = BOS_USER_STACK_TOP, base, sp = top;
    int result = vm_map(&process->space, top - BOS_USER_STACK_DEFAULT, BOS_USER_STACK_DEFAULT,
                        BOS_PROT_READ | BOS_PROT_WRITE, 1, &base);
    if (result) return result;
    uint32_t strings[UPROC_ARGS_MAX];
    /* Argument strings, 16 random bytes, then the aligned vector block. */
    unsigned string_bytes = 0;
    for (unsigned i = 0; i < argc; ++i) string_bytes += (unsigned)kstrlen(argv[i]) + 1;
    unsigned words = 1 + argc + 1 + 1 + 2 * 7;
    uint32_t lowest = ((top - string_bytes - 16) & ~15u) - words * 4;
    lowest &= ~15u;
    result = vm_prepare(&process->space, lowest, top - lowest, 1);
    if (result) return result;
    for (unsigned i = argc; i-- > 0;) {
        unsigned length = (unsigned)kstrlen(argv[i]) + 1;
        sp -= length;
        kmemcpy((void *)(uintptr_t)sp, argv[i], (int)length);
        strings[i] = sp;
    }
    sp -= 16;
    uint32_t random = sp;
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t low, high;
        __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
        ((uint32_t *)(uintptr_t)random)[i] = low * 2654435761u ^ high ^ timer_ticks() ^ (i << 24);
    }
    uint32_t *vector = (uint32_t *)(uintptr_t)lowest;
    *out_sp = lowest;
    *vector++ = argc;
    for (unsigned i = 0; i < argc; ++i) *vector++ = strings[i];
    *vector++ = 0;
    *vector++ = 0;   /* empty environment */
    uint32_t aux[] = {AT_PHDR, image->phdr, AT_PHENT, image->phent, AT_PHNUM, image->phnum,
                      AT_PAGESZ, VM_PAGE, AT_ENTRY, image->entry, AT_RANDOM, random,
                      AT_NULL, 0};
    for (unsigned i = 0; i < sizeof aux / sizeof aux[0]; ++i) *vector++ = aux[i];
    return 0;
}

int uproc_spawn(const void *image, uint32_t size, const char *const *argv, unsigned argc,
                uint32_t host, uint32_t *pid) {
    if (!image || !pid || argc > UPROC_ARGS_MAX || (argc && !argv)) return -BOS_EINVAL;
    unsigned argument_bytes = 0;
    for (unsigned i = 0; i < argc; ++i) argument_bytes += (unsigned)kstrlen(argv[i]) + 1;
    if (argument_bytes > UPROC_ARG_BYTES) return -BOS_EINVAL;
    if ((uintptr_t)image + size > PHYS_KERNEL_LIMIT) return -BOS_EFAULT;
    if (!threads_ready) {
        thread_init();
        threads_ready = 1;
    }
    BosHandle owner = process_owner_allocate();
    if (owner == BOS_HANDLE_INVALID) return -BOS_EAGAIN;
    Uproc *process = kmalloc(sizeof *process);
    if (!process) return -BOS_ENOMEM;
    int result = vm_create(&process->space, owner);
    if (result) {
        kfree(process);
        return result;
    }
    ElfImage loaded;
    uint32_t sp = 0;
    result = elf_load(&process->space, image, size, &loaded);
    if (!result) {
        vm_activate(&process->space);
        result = build_stack(process, &loaded, argv, argc, &sp);
        vm_activate(0);
    }
    Thread *thread = result ? 0 : thread_create_user(process, loaded.entry, sp, 0, 0);
    if (!thread) {
        vm_destroy(&process->space);
        kfree(process);
        return result ? result : -BOS_ENOMEM;
    }
    process->pid = next_pid++;
    process->state = UPROC_RUNNING;
    process->threads = 1;
    process->host = host;
    if (argc) {
        const char *name = argv[0], *slash = name;
        for (const char *p = name; *p; ++p)
            if (*p == '/') slash = p + 1;
        unsigned length = 0;
        while (slash[length] && length < sizeof process->name - 1) {
            process->name[length] = slash[length];
            ++length;
        }
    }
    process->next = processes;
    processes = process;
    *pid = process->pid;
    return 0;
}

int uproc_status(uint32_t pid, int *exit_code) {
    Uproc *process = find(pid);
    if (!process) return UPROC_UNKNOWN;
    if (process->state == UPROC_EXITED && exit_code) *exit_code = process->exit_code;
    return process->state;
}

int uproc_kill(uint32_t pid) {
    Uproc *process = find(pid);
    if (!process || process->state != UPROC_RUNNING) return -BOS_ESRCH;
    process_exit(process, -1);
    return 0;
}

int uproc_release(uint32_t pid) {
    Uproc *process = find(pid);
    if (!process) return -BOS_ESRCH;
    if (process->state == UPROC_RUNNING) process_exit(process, -1);
    process->state = UPROC_UNKNOWN;
    if (!process->threads) {
        unlink_process(process);
        kfree(process);
    }
    return 0;
}

uint32_t uproc_next(uint32_t after, uint32_t *host) {
    uint32_t best = 0;
    Uproc *chosen = 0;
    for (Uproc *process = processes; process; process = process->next)
        if (process->state != UPROC_UNKNOWN && process->pid > after && (!best || process->pid < best)) {
            best = process->pid;
            chosen = process;
        }
    if (chosen && host) *host = chosen->host;
    return best;
}
