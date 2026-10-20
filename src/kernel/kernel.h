#ifndef BASEOS_KERNEL_H
#define BASEOS_KERNEL_H
/* Core kernel services for shells, defined in kernel/kernel.c.
 *
 * The other public kernel services keep their own headers: fs.h (files),
 * gfx.h and display.h (framebuffer and modes), audio.h, net.h, download.h,
 * program.h (processes), native_ui.h (native windows), platform.h (time,
 * logging, panic) and rtc.h (wall clock). */
#include <stdint.h>
#include "kernel/fs.h"
#include "kernel/input_ingress.h"

/* Serial diagnostics. Test harnesses match these lines exactly. */
void kprint_debug(const char *text);
void kprint_uint(unsigned value);
void kprint_hex32(uint32_t value);
void serial_write(char c);

/* The one ordered device stream. The kernel acquires PS/2 bytes into it; the
 * active shell is its only consumer and pops samples with input_pop(). */
extern InputIngress device_input;
/* Move up to `budget` pending controller bytes into device_input. Returns the
 * number acquired. Never dispatches shell or application work. */
unsigned input_acquire(unsigned budget);
void drain_8042(void);
/* Nonzero once the PS/2 mouse answered initialization. */
int kernel_pointer_ready(void);

/* Background service events, accumulated until the shell takes them. */
enum { KERNEL_EVENT_STORAGE = 1u, KERNEL_EVENT_DOWNLOADS = 2u };
/* Return and clear the pending events selected by `mask`. */
unsigned kernel_events_take(unsigned mask);
/* One bounded storage turn: native sync completions and snapshot progress.
 * Raises KERNEL_EVENT_STORAGE when the visible storage status changes. */
enum FsSyncProgress kernel_storage_poll(void);
/* Audio, network and background download service. Raises
 * KERNEL_EVENT_DOWNLOADS when a download finished writing its file. */
void kernel_services_poll(void);

/* Stop the machine. Callers flush storage first; this does not return on
 * QEMU or VirtualBox and halts elsewhere. */
void kernel_power_off(void);
#endif
