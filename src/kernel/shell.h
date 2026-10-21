#ifndef BASEOS_SHELL_H
#define BASEOS_SHELL_H
/* The kernel/shell contract.
 *
 * The kernel owns the machine: memory, storage, time, devices, the ordered
 * input stream, the framebuffer, processes and background services. A shell
 * owns everything a person sees and touches: windows or screens, launching,
 * input routing and app hosting. Exactly one shell is linked into a kernel
 * image (make BASEOS_SHELL=<name>), and it exports `baseos_shell`.
 *
 * A shell talks to the kernel only through the kernel's public headers
 * (kernel/kernel.h and the service headers it names). The kernel calls the
 * shell only through this table, so replacing the shell never changes a
 * kernel source file. See docs/ARCHITECTURE.md. */
typedef struct {
    const char *name;
    /* Storage is mounted and devices are initialized, but input is not live
     * yet. Load preferences, choose a display mode and seed files here. */
    void (*prepare)(void);
    /* The input stream is live. Draw the first frame and restore state. */
    void (*start)(void);
    /* One cooperative turn, called after the kernel has pumped its services.
     * Return nonzero while work is pending; zero lets the kernel halt until
     * the next timer tick when it has no pending storage or input either. */
    int (*turn)(void);
} Shell;

extern const Shell baseos_shell;
#endif
