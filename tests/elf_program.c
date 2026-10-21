/* Freestanding ELF test program for the platform ABI v2 kernel paths.
 * argv[1] picks the check; the exit status reports the result. Built with
 * the host compiler (-m32 -static -nostdlib) by tools/elf_runtime_test.py. */
#include "sdk/baseos_sys.h"

typedef unsigned int u32;

/* sys(n, a, b, c, d, e, f): int $0x81 with EAX=n and EBX..EBP = a..f. */
long sys(long n, long a, long b, long c, long d, long e, long f);
__asm__(".globl sys\n"
        "sys:\n\t"
        "push %ebp\n\tpush %edi\n\tpush %esi\n\tpush %ebx\n\t"
        "mov 20(%esp), %eax\n\tmov 24(%esp), %ebx\n\tmov 28(%esp), %ecx\n\t"
        "mov 32(%esp), %edx\n\tmov 36(%esp), %esi\n\tmov 40(%esp), %edi\n\t"
        "mov 44(%esp), %ebp\n\t"
        "int $0x81\n\t"
        "pop %ebx\n\tpop %esi\n\tpop %edi\n\tpop %ebp\n\tret\n");

static u32 length(const char *s) { u32 n = 0; while (s[n]) ++n; return n; }
static void say(const char *s) { sys(BOS_SYS_WRITE, 1, (long)s, length(s), 0, 0, 0); }
static int same(const char *a, const char *b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}
static void number(const char *label, u32 value) {
    char text[16];
    int i = 15;
    text[i] = 0;
    do { text[--i] = (char)('0' + value % 10); value /= 10; } while (value);
    say(label);
    say(text + i);
    say("\n");
}
static void __attribute__((noreturn)) finish(int status) {
    sys(BOS_SYS_EXIT, status, 0, 0, 0, 0, 0);
    for (;;) {}
}

static int initialized = 41;     /* .data */
static u32 zeroed[1024];          /* .bss */

static int hello(int argc, char **argv) {
    if (initialized != 41 || zeroed[1023]) return 2;
    say("ELF hello from ring 3\n");
    number("argc ", (u32)argc);
    say(argv[argc - 1]);
    say("\n");
    BosTimespec now;
    if (sys(BOS_SYS_CLOCK_GET, BOS_CLOCK_REALTIME, (long)&now, 0, 0, 0, 0) ||
        now.seconds < 1600000000LL)
        return 3;
    BosTimespec nap = {0, 50000000};
    if (sys(BOS_SYS_SLEEP, (long)&nap, 0, 0, 0, 0, 0)) return 4;
    return 7;
}

static int memory(void) {
    const u32 bytes = 128u << 20;
    long address = sys(BOS_SYS_VM_MAP, 0, bytes, BOS_PROT_READ | BOS_PROT_WRITE,
                       BOS_MAP_PRIVATE | BOS_MAP_ANONYMOUS, -1, 0);
    if (address < 0 && address > -4096) return 2;
    u32 *words = (u32 *)address;
    for (u32 page = 0; page < bytes / 4096; ++page) words[page * 1024 + (page & 1023)] = page ^ 0x5A5A5A5Au;
    for (u32 page = 0; page < bytes / 4096; ++page)
        if (words[page * 1024 + (page & 1023)] != (page ^ 0x5A5A5A5Au) || words[page * 1024 + ((page + 1) & 1023)])
            return 3;
    BosMemInfo info;
    if (sys(BOS_SYS_MEMINFO, (long)&info, 0, 0, 0, 0, 0)) return 4;
    number("ELF memory pages ", info.process_pages);
    if (info.process_pages < bytes / 4096) return 5;
    if (sys(BOS_SYS_VM_PROTECT, address, 4096, BOS_PROT_READ, 0, 0, 0)) return 6;
    if (sys(BOS_SYS_VM_UNMAP, address, bytes, 0, 0, 0, 0)) return 7;
    if (sys(BOS_SYS_MEMINFO, (long)&info, 0, 0, 0, 0, 0) || info.process_pages >= bytes / 4096) return 8;
    say("ELF memory 128 MiB ok\n");
    return 0;
}

/* THREAD_CREATE starts a thread with EAX = argument and ESP = stack. */
typedef struct { void (*function)(long); long argument; } ThreadStart;
void thread_main(ThreadStart *start) { start->function(start->argument); }
void thread_entry(void);
__asm__(".globl thread_entry\n"
        "thread_entry:\n\t"
        "and $-16, %esp\n\tsub $12, %esp\n\tpush %eax\n\tcall thread_main\n\tud2\n");

static volatile int flag, counter;
static u32 stacks[2][1024] __attribute__((aligned(16)));

static void spinner(long argument) {
    (void)argument;
    while (!flag) ++counter;      /* only preemption lets the others run */
    sys(BOS_SYS_THREAD_EXIT, 0, 0, 0, 0, 0, 0);
}

static void waker(long argument) {
    BosTimespec nap = {0, 100000000};
    sys(BOS_SYS_SLEEP, (long)&nap, 0, 0, 0, 0, 0);
    flag = (int)argument;
    sys(BOS_SYS_FUTEX_WAKE, (long)&flag, 0x7FFFFFFF, 0, 0, 0, 0);
    sys(BOS_SYS_THREAD_EXIT, 0, 0, 0, 0, 0, 0);
}

static int threads(void) {
    static ThreadStart starts[2] = {{spinner, 0}, {waker, 5}};
    long a = sys(BOS_SYS_THREAD_CREATE, (long)thread_entry, (long)(stacks[0] + 1024), (long)&starts[0], 0, 0, 0);
    long b = sys(BOS_SYS_THREAD_CREATE, (long)thread_entry, (long)(stacks[1] + 1024), (long)&starts[1], 0, 0, 0);
    if (a <= 0 || b <= 0 || a == b) return 2;
    while (!flag) {
        long result = sys(BOS_SYS_FUTEX_WAIT, (long)&flag, 0, 0, 0, 0, 0);
        if (result && result != -BOS_EAGAIN) return 3;
    }
    if (flag != 5 || !counter) return 4;
    BosTimespec span = {0, 20000000};
    int zero = 0;
    if (sys(BOS_SYS_FUTEX_WAIT, (long)&zero, 0, (long)&span, 0, 0, 0) != -BOS_ETIMEDOUT) return 5;
    say("ELF threads ok\n");
    return 0;
}

static int fault(void) {
    say("ELF fault next\n");
    return *(volatile int *)0x40000000 + 1;   /* below the image: unmapped */
}

static long long nanoseconds(void) {
    BosTimespec now;
    sys(BOS_SYS_CLOCK_GET, BOS_CLOCK_MONOTONIC, (long)&now, 0, 0, 0, 0);
    return now.seconds * 1000000000LL + now.nanoseconds;
}

/* The monotonic clock resolves below a millisecond and never steps back,
 * and short sleeps last about as long as asked (not a 70 Hz tick). */
static int clock(void) {
    long long previous = nanoseconds(), step = 1000000000LL;
    for (int i = 0; i < 2000; ++i) {
        long long now = nanoseconds();
        if (now < previous) return 2;
        if (now > previous && now - previous < step) step = now - previous;
        previous = now;
    }
    number("ELF clock step ns ", (u32)step);
    if (step >= 1000000) return 3;
    long long begin = nanoseconds();
    BosTimespec nap = {0, 2000000};
    for (int i = 0; i < 50; ++i)
        if (sys(BOS_SYS_SLEEP, (long)&nap, 0, 0, 0, 0, 0)) return 4;
    long long spent = nanoseconds() - begin;
    if (spent > 4000000000LL) return 5;
    u32 elapsed = (u32)spent / 1000000u;
    number("ELF 50 sleeps of 2 ms took ms ", elapsed);
    if (elapsed < 100 || elapsed > 400) return 5;
    say("ELF clock ok\n");
    return 0;
}

/* A futex mutex (0 free, 1 locked, 2 contended) and a sequence condition. */
static int swap(volatile int *word, int value) {
    __asm__ volatile("xchg %0, %1" : "+r"(value), "+m"(*word) :: "memory");
    return value;
}
static int compare_swap(volatile int *word, int expected, int value) {
    __asm__ volatile("lock cmpxchg %2, %1" : "+a"(expected), "+m"(*word) : "r"(value) : "memory");
    return expected;
}
static void lock(volatile int *m) {
    int c = compare_swap(m, 0, 1);
    if (!c) return;
    if (c != 2) c = swap(m, 2);
    while (c) {
        sys(BOS_SYS_FUTEX_WAIT, (long)m, 2, 0, 0, 0, 0);
        c = swap(m, 2);
    }
}
static void unlock(volatile int *m) {
    if (swap(m, 0) == 2) sys(BOS_SYS_FUTEX_WAKE, (long)m, 1, 0, 0, 0, 0);
}
static void wait_change(volatile int *sequence, volatile int *m) {
    int seen = *sequence;
    unlock(m);
    sys(BOS_SYS_FUTEX_WAIT, (long)sequence, seen, 0, 0, 0, 0);
    lock(m);
}
static void signal_all(volatile int *sequence) {
    __asm__ volatile("lock incl %0" : "+m"(*sequence) :: "memory");
    sys(BOS_SYS_FUTEX_WAKE, (long)sequence, 0x7FFFFFFF, 0, 0, 0, 0);
}

#define SPINNERS 24
#define WORKERS 4
#define ITEMS 4000
static u32 spin_stacks[SPINNERS][512] __attribute__((aligned(16)));
static u32 worker_stacks[2 * WORKERS][1024] __attribute__((aligned(16)));
static volatile u32 spun[SPINNERS];
static volatile int stop_spinning, queue_lock, queue_changed, finished;
static u32 queue[16], head, tail;
static volatile u32 consumed_sum, consumed_count;

static void low_spinner(long index) {
    while (!stop_spinning) ++spun[index];
    sys(BOS_SYS_THREAD_EXIT, 0, 0, 0, 0, 0, 0);
}
static void producer(long index) {
    for (u32 i = 1; i <= ITEMS; ++i) {
        lock(&queue_lock);
        while (tail - head == 16) wait_change(&queue_changed, &queue_lock);
        queue[tail++ & 15] = (u32)index * ITEMS + i;
        signal_all(&queue_changed);
        unlock(&queue_lock);
    }
    sys(BOS_SYS_THREAD_EXIT, 0, 0, 0, 0, 0, 0);
}
static void consumer(long unused) {
    (void)unused;
    lock(&queue_lock);
    for (;;) {
        while (head == tail && consumed_count < WORKERS * ITEMS) wait_change(&queue_changed, &queue_lock);
        if (head == tail) break;
        consumed_sum += queue[head++ & 15];
        ++consumed_count;
        signal_all(&queue_changed);
    }
    unlock(&queue_lock);
    __asm__ volatile("lock incl %0" : "+m"(finished) :: "memory");
    sys(BOS_SYS_FUTEX_WAKE, (long)&finished, 1, 0, 0, 0, 0);
    sys(BOS_SYS_THREAD_EXIT, 0, 0, 0, 0, 0, 0);
}

/* Many low-priority spinners, a high-priority sleeper that must still wake
 * on time, and producers and consumers on a futex mutex and condition. The
 * desktop's turn rate during this check is verified by the fixture. */
static int scheduler(void) {
    static ThreadStart spin_starts[SPINNERS], worker_starts[2 * WORKERS];
    for (int i = 0; i < SPINNERS; ++i) {
        spin_starts[i] = (ThreadStart){low_spinner, i};
        long tid = sys(BOS_SYS_THREAD_CREATE, (long)thread_entry, (long)(spin_stacks[i] + 512),
                       (long)&spin_starts[i], 0, 0, 0);
        if (tid <= 0 || sys(BOS_SYS_THREAD_PRIORITY, tid, -1, 0, 0, 0, 0)) return 2;
    }
    if (sys(BOS_SYS_THREAD_PRIORITY, 0, 1, 0, 0, 0, 0) ||
        sys(BOS_SYS_THREAD_PRIORITY, 0, 2, 0, 0, 0, 0) != -BOS_EINVAL ||
        sys(BOS_SYS_THREAD_PRIORITY, 0x7FFFFFFF, 0, 0, 0, 0, 0) != -BOS_ESRCH)
        return 3;
    long long begin = nanoseconds();
    BosTimespec nap = {0, 2000000};
    for (int i = 0; i < 50; ++i)
        if (sys(BOS_SYS_SLEEP, (long)&nap, 0, 0, 0, 0, 0)) return 4;
    u32 elapsed = (u32)(nanoseconds() - begin) / 1000000u;
    number("ELF high-priority 50 sleeps of 2 ms took ms ", elapsed);
    if (elapsed > 400) return 5;
    for (int i = 0; i < 2 * WORKERS; ++i) {
        worker_starts[i] = (ThreadStart){i < WORKERS ? producer : consumer, i};
        if (sys(BOS_SYS_THREAD_CREATE, (long)thread_entry, (long)(worker_stacks[i] + 1024),
                (long)&worker_starts[i], 0, 0, 0) <= 0)
            return 6;
    }
    for (;;) {
        int seen = finished;
        if (seen == WORKERS) break;
        BosTimespec dbg = {1, 0};
        sys(BOS_SYS_FUTEX_WAIT, (long)&finished, seen, (long)&dbg, 0, 0, 0);
        number("DBG consumed ", consumed_count); number("DBG head ", head); number("DBG tail ", tail);
    }
    u32 expected = 0;
    for (u32 p = 0; p < WORKERS; ++p) expected += p * ITEMS * ITEMS + ITEMS * (ITEMS + 1) / 2;
    if (consumed_count != WORKERS * ITEMS || consumed_sum != expected) return 7;
    /* Wait long enough for every starving spinner to get its turn. */
    BosTimespec settle = {0, 0};
    for (int i = 0; i < SPINNERS; ++i) {
        settle.nanoseconds = 100000000;
        while (!spun[i]) sys(BOS_SYS_SLEEP, (long)&settle, 0, 0, 0, 0, 0);
    }
    stop_spinning = 1;
    number("ELF items through the queue ", consumed_count);
    say("ELF scheduler ok\n");
    return 0;
}

/* File descriptors on the native volume, pipes and descriptor copies. */
static int files(void) {
    static char block[3000], back[3100];
    char cwd[64];
    if (sys(BOS_SYS_MKDIR, (long)"/elf-files", 0755, 0, 0, 0, 0) ||
        sys(BOS_SYS_MKDIR, (long)"/elf-files", 0755, 0, 0, 0, 0) != -BOS_EEXIST ||
        sys(BOS_SYS_CHDIR, (long)"/elf-files", 0, 0, 0, 0, 0) ||
        sys(BOS_SYS_GETCWD, (long)cwd, sizeof cwd, 0, 0, 0, 0) != 11 || !same(cwd, "/elf-files"))
        return 2;
    for (int i = 0; i < 3000; ++i) block[i] = (char)('a' + i % 26);
    long fd = sys(BOS_SYS_OPEN, (long)"data.txt", BOS_O_CREAT | BOS_O_RDWR, 0644, 0, 0, 0);
    if (fd < 3 || sys(BOS_SYS_WRITE, fd, (long)block, 3000, 0, 0, 0) != 3000) return 3;
    long long position = -1;
    if (sys(BOS_SYS_SEEK, fd, 10, 0, BOS_SEEK_SET, (long)&position, 0) || position != 10 ||
        sys(BOS_SYS_READ, fd, (long)back, sizeof back, 0, 0, 0) != 2990 || back[0] != block[10] ||
        back[2989] != block[2999])
        return 4;
    BosStat st;
    if (sys(BOS_SYS_FSTAT, fd, (long)&st, 0, 0, 0, 0) || st.size != 3000 ||
        (st.mode & BOS_S_IFMT) != BOS_S_IFREG || sys(BOS_SYS_CLOSE, fd, 0, 0, 0, 0, 0))
        return 5;
    fd = sys(BOS_SYS_OPEN, (long)"/elf-files/data.txt", BOS_O_WRONLY | BOS_O_APPEND, 0, 0, 0, 0);
    if (fd < 0 || sys(BOS_SYS_WRITE, fd, (long)"xyz", 3, 0, 0, 0) != 3 || sys(BOS_SYS_CLOSE, fd, 0, 0, 0, 0, 0) ||
        sys(BOS_SYS_STAT, (long)"data.txt", (long)&st, 0, 0, 0, 0) || st.size != 3003)
        return 6;
    if (sys(BOS_SYS_OPEN, (long)"data.txt", BOS_O_CREAT | BOS_O_EXCL | BOS_O_WRONLY, 0, 0, 0, 0) != -BOS_EEXIST ||
        sys(BOS_SYS_RENAME, (long)"data.txt", (long)"moved.txt", 0, 0, 0, 0) ||
        sys(BOS_SYS_OPEN, (long)"data.txt", BOS_O_RDONLY, 0, 0, 0, 0) != -BOS_ENOENT)
        return 7;
    fd = sys(BOS_SYS_OPEN, (long)"moved.txt", BOS_O_RDONLY, 0, 0, 0, 0);
    if (fd < 0 || sys(BOS_SYS_READ, fd, (long)back, sizeof back, 0, 0, 0) != 3003 ||
        back[3000] != 'x' || sys(BOS_SYS_WRITE, fd, (long)"no", 2, 0, 0, 0) != -BOS_EBADF)
        return 8;
    sys(BOS_SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    /* The directory lists ".", ".." and moved.txt. */
    long dir = sys(BOS_SYS_OPEN, (long)".", BOS_O_RDONLY | BOS_O_DIRECTORY, 0, 0, 0, 0);
    int seen = 0;
    long bytes;
    while ((bytes = sys(BOS_SYS_READDIR, dir, (long)back, 64, 0, 0, 0)) > 0)
        for (long at = 0; at < bytes; at += ((BosDirent *)(back + at))->length) {
            BosDirent *entry = (BosDirent *)(back + at);
            if (same(entry->name, ".") || same(entry->name, "..")) seen += entry->type == BOS_DT_DIR;
            else if (same(entry->name, "moved.txt") && entry->type == BOS_DT_REG) seen += 4;
            else return 9;
        }
    if (dir < 0 || bytes || seen != 6 || sys(BOS_SYS_CLOSE, dir, 0, 0, 0, 0, 0)) return 9;
    /* A pipe carries bytes and reports end of file once the writer closes. */
    int ends[2];
    if (sys(BOS_SYS_PIPE, (long)ends, 0, 0, 0, 0, 0) ||
        sys(BOS_SYS_WRITE, ends[1], (long)"ping", 4, 0, 0, 0) != 4 ||
        sys(BOS_SYS_CLOSE, ends[1], 0, 0, 0, 0, 0) ||
        sys(BOS_SYS_READ, ends[0], (long)back, 16, 0, 0, 0) != 4 || back[0] != 'p' ||
        sys(BOS_SYS_READ, ends[0], (long)back, 16, 0, 0, 0) != 0 ||
        sys(BOS_SYS_ISATTY, ends[0], 0, 0, 0, 0, 0) != -BOS_ENOTTY)
        return 10;
    sys(BOS_SYS_CLOSE, ends[0], 0, 0, 0, 0, 0);
    if (sys(BOS_SYS_DUP2, 1, 9, 0, 0, 0, 0) != 9 || sys(BOS_SYS_ISATTY, 9, 0, 0, 0, 0, 0)) return 11;
    sys(BOS_SYS_WRITE, 9, (long)"ELF dup2 output\n", 16, 0, 0, 0);
    if (sys(BOS_SYS_CHDIR, (long)"..", 0, 0, 0, 0, 0) ||
        sys(BOS_SYS_UNLINK, (long)"elf-files", BOS_AT_REMOVEDIR, 0, 0, 0, 0) != -BOS_ENOTEMPTY ||
        sys(BOS_SYS_UNLINK, (long)"elf-files/moved.txt", 0, 0, 0, 0, 0) ||
        sys(BOS_SYS_UNLINK, (long)"elf-files", BOS_AT_REMOVEDIR, 0, 0, 0, 0) ||
        sys(BOS_SYS_STAT, (long)"/elf-files", (long)&st, 0, 0, 0, 0) != -BOS_ENOENT)
        return 12;
    /* A file written and left open is saved when the process exits. */
    fd = sys(BOS_SYS_OPEN, (long)"/elf-exit.txt", BOS_O_CREAT | BOS_O_WRONLY | BOS_O_TRUNC, 0644, 0, 0, 0);
    if (fd < 0 || sys(BOS_SYS_WRITE, fd, (long)"kept", 4, 0, 0, 0) != 4) return 13;
    say("ELF files ok\n");
    return 0;
}

static int gate(void) {
    __asm__ volatile("int $0x80");   /* the BEX call gate is not for ELF programs */
    return 0;
}

int start(int argc, char **argv) {
    if (argc < 2) return 1;
    /* A Terminal passes a document path; its last component names the check. */
    for (char *p = argv[1]; *p; ++p)
        if (*p == '/') argv[1] = p + 1;
    if (same(argv[1], "hello")) return hello(argc, argv);
    if (same(argv[1], "memory")) return memory();
    if (same(argv[1], "threads")) return threads();
    if (same(argv[1], "fault")) return fault();
    if (same(argv[1], "gate")) return gate();
    if (same(argv[1], "clock")) return clock();
    if (same(argv[1], "scheduler")) return scheduler();
    if (same(argv[1], "files")) return files();
    return 1;
}

__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile("mov %esp, %eax\n\t"
                     "lea 4(%eax), %edx\n\t"     /* argv */
                     "and $-16, %esp\n\t"
                     "sub $8, %esp\n\t"
                     "push %edx\n\t"
                     "push (%eax)\n\t"           /* argc */
                     "call start\n\t"
                     "push %eax\n\t"
                     "push %eax\n\t"
                     "call finish_status\n");
}

__attribute__((noreturn, used)) void finish_status(int unused, int status) {
    (void)unused;
    finish(status);
}
