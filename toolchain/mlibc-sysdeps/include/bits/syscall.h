#ifndef _MLIBC_SYSCALL_H
#define _MLIBC_SYSCALL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Raw BaseOS system call (int $0x81). Returns a value or a negated errno. */
long __baseos_syscall(long number, long a, long b, long c, long d, long e, long f);

#ifdef __cplusplus
}
#endif

#endif
