#ifndef UPROC_H
#define UPROC_H
#include <stdint.h>
#include "kernel/vm.h"
#include "kernel/thread.h"
#include "kernel/ufile.h"

/* ELF processes (platform ABI v2, sdk/baseos_sys.h). Each process owns a
 * private address space, one or more threads and a text output buffer that
 * its host (for example a Terminal) drains. A process stays listed after it
 * exits until its host releases it, so the exit status is never lost.
 * Serialized kernel context only (thread 0). */
#define UPROC_OUTPUT_BYTES 8192u
#define UPROC_ARGS_MAX 16u
#define UPROC_ARG_BYTES 1024u

enum { UPROC_UNKNOWN = 0, UPROC_RUNNING = 1, UPROC_EXITED = 2 };

typedef struct Uproc {
    VmSpace space;
    uint32_t pid;
    int state;
    int exit_code;
    unsigned threads;          /* not yet reaped */
    uint32_t cpu_ms;           /* time its threads held the CPU */
    uint32_t host;             /* opaque tag chosen by the host */
    char name[32];
    char output[UPROC_OUTPUT_BYTES];
    unsigned output_head, output_count;
    UFileTable files;
    struct Uproc *next;
} Uproc;

/* Start image (an ELF file's bytes, copied before return) with argv. Returns
 * 0 and *pid, or a negated BOS_E* error. */
int uproc_spawn(const void *image, uint32_t size, const char *const *argv, unsigned argc,
                uint32_t host, uint32_t *pid);
/* UPROC_RUNNING or UPROC_EXITED (with *exit_code), or UPROC_UNKNOWN. */
int uproc_status(uint32_t pid, int *exit_code);
/* Move up to capacity buffered output bytes into out; returns the count. */
unsigned uproc_take_output(uint32_t pid, char *out, unsigned capacity);
/* Ends a running process with status -1. */
int uproc_kill(uint32_t pid);
/* Forget an exited process (kills a running one first). */
int uproc_release(uint32_t pid);
/* Next pid after the given one (0 starts), or 0 at the end. */
uint32_t uproc_next(uint32_t after, uint32_t *host);

typedef struct {
    uint32_t pid, host;
    int state;                 /* UPROC_RUNNING or UPROC_EXITED */
    unsigned threads;          /* live threads */
    uint32_t pages;            /* resident and page-table frames */
    uint32_t cpu_ms;
    char name[32];
} UprocInfo;
/* Nonzero and *out for a listed process; 0 otherwise. */
int uproc_info(uint32_t pid, UprocInfo *out);

/* For kernel/ufile.c, on the process's own thread: user memory access
 * (0 or a negated BOS_E*), and console output (bytes written or an error). */
int uproc_copy_in(Uproc *process, void *out, uint32_t address, uint32_t length);
int uproc_copy_out(Uproc *process, uint32_t address, const void *in, uint32_t length);
int uproc_console_write(Uproc *process, uint32_t buffer, uint32_t length);

/* Called by platform.c for every interrupt; nonzero when handled. */
int uproc_interrupt(uint32_t *registers);
/* Called by thread.c when it frees a dead thread. */
void uproc_thread_gone(Uproc *process, Thread *thread);
#endif
