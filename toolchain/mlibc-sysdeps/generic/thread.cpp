// Thread creation for BaseOS. BOS_SYS_THREAD_CREATE starts the new thread at
// __mlibc_thread_entry with ESP pointing at the words PrepareStack laid out.
#include <abi-bits/errno.h>
#include <abi-bits/vm-flags.h>
#include <bits/ensure.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/tcb.hpp>
#include <mlibc/thread.hpp>
#include <stdint.h>

#include "baseos_sys.h"

extern "C" long __baseos_syscall(long n, long a, long b, long c, long d, long e, long f);
extern "C" void __mlibc_thread_entry();

extern "C" void __mlibc_thread_trampoline(void *entry, Tcb *tcb, void *argument) {
	// The kernel already loaded %gs from the TCB; wait until the creator has
	// published our thread id.
	while (__atomic_load_n(&tcb->tid, __ATOMIC_RELAXED) == 0)
		mlibc::sysdep<FutexWait>(&tcb->tid, 0, nullptr);
	__atomic_fetch_or(&tcb->cancelBits, tcbCancelEnableBit, __ATOMIC_RELAXED);
	tcb->invokeThreadFunc(entry, argument);
	mlibc::thread_exit(tcb->returnValue);
}

namespace {

constexpr size_t default_stack = 2 * 1024 * 1024;
constexpr size_t page = 4096;

} // namespace

namespace mlibc {

int Sysdeps<PrepareStack>::operator()(void **stack, void *entry, void *argument, void *tcb,
		size_t *stack_size, size_t *guard_size, void **stack_base) {
	if (!*stack_size) *stack_size = default_stack;
	*stack_size = (*stack_size + page - 1) & ~(page - 1);
	if (*stack) {
		*guard_size = 0;
		*stack_base = *stack;
	} else {
		// At least one guard page, so an overflow faults instead of
		// silently running into the next mapping.
		*guard_size = *guard_size ? (*guard_size + page - 1) & ~(page - 1) : page;
		void *base;
		if (int e = sysdep<VmMap>(nullptr, *stack_size + *guard_size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, &base))
			return e;
		// The guard pages at the bottom stay mapped but inaccessible.
		if (*guard_size)
			sysdep<VmProtect>(base, *guard_size, PROT_NONE);
		*stack_base = static_cast<char *>(base) + *guard_size;
	}
	// Three words for the entry stub, keeping the stack 16-byte aligned once
	// they are popped.
	auto top = reinterpret_cast<uintptr_t>(*stack_base) + *stack_size;
	auto words = reinterpret_cast<void **>((top & ~uintptr_t(15)) - 3 * sizeof(void *) - 4);
	words[0] = entry;
	words[1] = tcb;
	words[2] = argument;
	*stack = words;
	return 0;
}

int Sysdeps<Clone>::operator()(void *tcb, pid_t *pid_out, void *stack) {
	long result = __baseos_syscall(BOS_SYS_THREAD_CREATE, long(&__mlibc_thread_entry), long(stack),
	                               0, long(tcb), 0, 0);
	if (result < 0 && result > -4096) return int(-result);
	*pid_out = pid_t(result);
	return 0;
}

[[noreturn]] void Sysdeps<ThreadExit>::operator()() {
	__baseos_syscall(BOS_SYS_THREAD_EXIT, 0, 0, 0, 0, 0, 0);
	__builtin_trap();
}

} // namespace mlibc
