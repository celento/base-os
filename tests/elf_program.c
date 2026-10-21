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
