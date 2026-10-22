#include <stdint.h>
#include <stdlib.h>
#include <bits/ensure.h>
#include <mlibc/elf/startup.h>

extern "C" void __dlapi_enter(uintptr_t *);

extern char **environ;

// Hardware capabilities as Linux reports them in AT_HWCAP. The x86 fenv code
// touches the SSE state only when bit 25 is set; the kernel saves just the
// x87 state per thread and leaves SSE off, so none are reported.
extern "C" [[gnu::visibility("hidden")]] unsigned long __hwcap = 0;

extern "C" void __mlibc_entry(int (*main_fn)(int argc, char *argv[], char *env[]),
		uintptr_t *entry_stack) {
	__dlapi_enter(entry_stack);
	auto result = main_fn(mlibc::entry_stack.argc, mlibc::entry_stack.argv, environ);
	exit(result);
}
