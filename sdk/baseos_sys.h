#ifndef BASEOS_SYS_H
#define BASEOS_SYS_H
/*
 * BaseOS system calls for ELF programs (platform ABI v2).
 *
 * Shared by the kernel and the C library port (toolchain/mlibc-sysdeps).
 * Programs trap with `int $0x81`: EAX holds the call number and EBX, ECX, EDX,
 * ESI, EDI, EBP hold up to six arguments. EAX returns a nonnegative result or
 * a negated error number. Error, open-flag, mmap and clock numbers use the
 * Linux i386 values so the C library can share their published definitions.
 *
 * The BEX1/BEX2 interface in baseos_abi.h is separate and unchanged.
 */

#define BOS_SYS_VECTOR 0x81

/* User window: every ELF process owns [BOS_USER_BASE, BOS_USER_END). */
#define BOS_USER_BASE 0x40000000u
#define BOS_USER_END 0xC0000000u
#define BOS_USER_TEXT 0x40100000u
#define BOS_USER_STACK_TOP 0xBFFF0000u
#define BOS_USER_STACK_DEFAULT (8u << 20)

enum {
    BOS_SYS_EXIT = 1,         /* (status): ends every thread of the process */
    BOS_SYS_LOG = 2,          /* (text, length): kernel debug log */
    BOS_SYS_WRITE = 3,        /* (fd, buffer, length) */
    BOS_SYS_READ = 4,         /* (fd, buffer, length) */
    BOS_SYS_OPEN = 5,         /* (path, flags, mode) -> fd */
    BOS_SYS_CLOSE = 6,        /* (fd) */
    BOS_SYS_SEEK = 7,         /* (fd, offset_low, offset_high, whence, result*) */
    BOS_SYS_VM_MAP = 8,       /* (hint, length, prot, flags, fd, page_offset) -> address */
    BOS_SYS_VM_UNMAP = 9,     /* (address, length) */
    BOS_SYS_VM_PROTECT = 10,  /* (address, length, prot) */
    BOS_SYS_TCB_SET = 11,     /* (pointer): %gs base for this thread */
    BOS_SYS_CLOCK_GET = 12,   /* (clock, BosTimespec*) */
    BOS_SYS_SLEEP = 13,       /* (BosTimespec* request, BosTimespec* remaining) */
    BOS_SYS_YIELD = 14,       /* () */
    BOS_SYS_FUTEX_WAIT = 15,  /* (address, expected, BosTimespec* timeout or 0) */
    BOS_SYS_FUTEX_WAKE = 16,  /* (address, count) -> woken */
    BOS_SYS_THREAD_CREATE = 17, /* (entry, stack, argument, tcb) -> thread id; the
                                 * thread starts at entry with ESP = stack,
                                 * EAX = argument and %gs on tcb (if nonzero) */
    BOS_SYS_THREAD_EXIT = 18, /* () */
    BOS_SYS_GETPID = 19,      /* () */
    BOS_SYS_GETTID = 20,      /* () */
    BOS_SYS_STAT = 21,        /* (path, BosStat*) */
    BOS_SYS_FSTAT = 22,       /* (fd, BosStat*) */
    BOS_SYS_ISATTY = 23,      /* (fd) -> 0 or -ENOTTY */
    BOS_SYS_MEMINFO = 24,     /* (BosMemInfo*) */
    BOS_SYS_THREAD_PRIORITY = 25, /* (thread id or 0 for this thread, priority) */
    BOS_SYS_READDIR = 26,     /* (fd, buffer, length) -> bytes of BosDirent records, 0 at the end */
    BOS_SYS_MKDIR = 27,       /* (path, mode) */
    BOS_SYS_UNLINK = 28,      /* (path, flags): BOS_AT_REMOVEDIR removes an empty directory */
    BOS_SYS_RENAME = 29,      /* (from, to): replaces a file at to */
    BOS_SYS_DUP = 30,         /* (fd) -> lowest free fd */
    BOS_SYS_DUP2 = 31,        /* (fd, target) -> target */
    BOS_SYS_PIPE = 32,        /* (int fds[2]): read end, write end */
    BOS_SYS_CHDIR = 33,       /* (path) */
    BOS_SYS_GETCWD = 34,      /* (buffer, length) -> length including the NUL */
    BOS_SYS_FSYNC = 35,       /* (fd): write a file's contents back to the volume */
    BOS_SYS_FTRUNCATE = 36,   /* (fd, length) */
    BOS_SYS_FD_PATH = 37,     /* (fd, buffer, length) -> length including the NUL: a directory's path */
    BOS_SYS_LIMIT
};

/* Error numbers (Linux i386 values). */
#define BOS_EPERM 1
#define BOS_ENOENT 2
#define BOS_ESRCH 3
#define BOS_EINTR 4
#define BOS_EIO 5
#define BOS_EBADF 9
#define BOS_EAGAIN 11
#define BOS_ENOMEM 12
#define BOS_EACCES 13
#define BOS_EFAULT 14
#define BOS_EBUSY 16
#define BOS_EEXIST 17
#define BOS_ENOTDIR 20
#define BOS_EISDIR 21
#define BOS_EINVAL 22
#define BOS_EMFILE 24
#define BOS_ENOTTY 25
#define BOS_EFBIG 27
#define BOS_ENOSPC 28
#define BOS_ESPIPE 29
#define BOS_EROFS 30
#define BOS_ERANGE 34
#define BOS_ENAMETOOLONG 36
#define BOS_ENOSYS 38
#define BOS_ENOTEMPTY 39
#define BOS_EXDEV 18
#define BOS_EPIPE 32
#define BOS_ETIMEDOUT 110

/* open flags (Linux i386 values). */
#define BOS_O_ACCMODE 3
#define BOS_O_RDONLY 0
#define BOS_O_WRONLY 1
#define BOS_O_RDWR 2
#define BOS_O_CREAT 0100
#define BOS_O_EXCL 0200
#define BOS_O_TRUNC 01000
#define BOS_O_APPEND 02000
#define BOS_O_DIRECTORY 0200000
#define BOS_AT_REMOVEDIR 0x200

/* SEEK whence. */
#define BOS_SEEK_SET 0
#define BOS_SEEK_CUR 1
#define BOS_SEEK_END 2

/* BosStat.mode type bits (POSIX values). */
#define BOS_S_IFMT 0170000
#define BOS_S_IFIFO 0010000
#define BOS_S_IFCHR 0020000
#define BOS_S_IFDIR 0040000
#define BOS_S_IFREG 0100000

/* BosDirent.type. */
#define BOS_DT_FIFO 1
#define BOS_DT_CHR 2
#define BOS_DT_DIR 4
#define BOS_DT_REG 8

/* Memory protection and mapping flags (Linux values). */
#define BOS_PROT_READ 1
#define BOS_PROT_WRITE 2
#define BOS_PROT_EXEC 4
#define BOS_MAP_SHARED 0x01
#define BOS_MAP_PRIVATE 0x02
#define BOS_MAP_FIXED 0x10
#define BOS_MAP_ANONYMOUS 0x20

/* Clocks (Linux values). */
#define BOS_CLOCK_REALTIME 0
#define BOS_CLOCK_MONOTONIC 1

typedef struct {
    long long seconds;
    long nanoseconds;
} BosTimespec;

typedef struct {
    unsigned int struct_size;
    unsigned int mode;            /* BOS_S_IF* and permission bits */
    unsigned int node;            /* stable while the file exists */
    unsigned int links;
    long long size;
    long long modified;           /* seconds since 1970 (UTC), 0 when unknown */
} BosStat;

/* READDIR records, each length bytes (a multiple of 4), name NUL-terminated. */
typedef struct {
    unsigned int node;
    unsigned short length;
    unsigned char type;           /* BOS_DT_* */
    char name[1];
} BosDirent;

typedef struct {
    unsigned int struct_size;
    unsigned int page_bytes;
    unsigned int total_pages, free_pages;
    unsigned int process_pages;   /* frames owned by the calling process */
    unsigned int mapped_bytes;    /* reserved user mappings, touched or not */
} BosMemInfo;

#endif
