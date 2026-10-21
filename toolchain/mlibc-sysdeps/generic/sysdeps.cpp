// BaseOS system dependencies for mlibc. Calls trap with int $0x81 and return
// a value or a negated errno; the numbers live in sdk/baseos_sys.h.
#include <abi-bits/errno.h>
#include <abi-bits/vm-flags.h>
#include <bits/ensure.h>
#include <bits/syscall.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/tcb.hpp>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "baseos_sys.h"

namespace {

long call(long n, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0, long f = 0) {
	return __baseos_syscall(n, a, b, c, d, e, f);
}

int error_of(long result) { return result < 0 && result > -4096 ? int(-result) : 0; }

} // namespace

namespace mlibc {

void Sysdeps<LibcLog>::operator()(const char *message) {
	call(BOS_SYS_LOG, long(message), long(strlen(message)));
}

void Sysdeps<LibcPanic>::operator()() {
	sysdep<LibcLog>("mlibc: panic");
	call(BOS_SYS_EXIT, 127);
	__builtin_trap();
}

void Sysdeps<Exit>::operator()(int status) {
	call(BOS_SYS_EXIT, status);
	__builtin_trap();
}

int Sysdeps<Isatty>::operator()(int fd) {
	return error_of(call(BOS_SYS_ISATTY, fd));
}

int Sysdeps<Write>::operator()(int fd, const void *buffer, size_t count, ssize_t *written) {
	long result = call(BOS_SYS_WRITE, fd, long(buffer), long(count));
	if (int e = error_of(result)) return e;
	*written = result;
	return 0;
}

int Sysdeps<Read>::operator()(int fd, void *buffer, size_t count, ssize_t *read) {
	long result = call(BOS_SYS_READ, fd, long(buffer), long(count));
	if (int e = error_of(result)) return e;
	*read = result;
	return 0;
}

int Sysdeps<Open>::operator()(const char *path, int flags, mode_t mode, int *fd) {
	long result = call(BOS_SYS_OPEN, long(path), flags, long(mode));
	if (int e = error_of(result)) return e;
	*fd = int(result);
	return 0;
}

int Sysdeps<Close>::operator()(int fd) {
	return error_of(call(BOS_SYS_CLOSE, fd));
}

int Sysdeps<Seek>::operator()(int fd, off_t offset, int whence, off_t *new_offset) {
	long long position = offset;
	long long result_position = 0;
	long result = call(BOS_SYS_SEEK, fd, long(uint32_t(position)), long(uint32_t(position >> 32)),
	                   whence, long(&result_position));
	if (int e = error_of(result)) return e;
	*new_offset = off_t(result_position);
	return 0;
}

int Sysdeps<TcbSet>::operator()(void *pointer) {
	// The kernel points this thread's TLS descriptor at the TCB and loads %gs.
	return error_of(call(BOS_SYS_TCB_SET, long(pointer)));
}

int Sysdeps<VmMap>::operator()(void *hint, size_t size, int prot, int flags, int fd,
		off_t offset, void **window) {
	if (offset % 4096) return EINVAL;
	long result = call(BOS_SYS_VM_MAP, long(hint), long(size), prot, flags, fd,
	                   long(offset / 4096));
	if (int e = error_of(result)) return e;
	*window = reinterpret_cast<void *>(result);
	return 0;
}

int Sysdeps<VmUnmap>::operator()(void *pointer, size_t size) {
	return error_of(call(BOS_SYS_VM_UNMAP, long(pointer), long(size)));
}

int Sysdeps<VmProtect>::operator()(void *pointer, size_t size, int prot) {
	return error_of(call(BOS_SYS_VM_PROTECT, long(pointer), long(size), prot));
}

int Sysdeps<AnonAllocate>::operator()(size_t size, void **pointer) {
	return sysdep<VmMap>(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
	                     -1, 0, pointer);
}

int Sysdeps<AnonFree>::operator()(void *pointer, size_t size) {
	return sysdep<VmUnmap>(pointer, size);
}

int Sysdeps<FutexWait>::operator()(int *pointer, int expected, const struct timespec *time) {
	BosTimespec timeout;
	if (time) {
		timeout.seconds = time->tv_sec;
		timeout.nanoseconds = time->tv_nsec;
	}
	return error_of(call(BOS_SYS_FUTEX_WAIT, long(pointer), expected, time ? long(&timeout) : 0));
}

int Sysdeps<FutexWake>::operator()(int *pointer, bool all) {
	long result = call(BOS_SYS_FUTEX_WAKE, long(pointer), all ? 0x7FFFFFFF : 1);
	return error_of(result);
}

int Sysdeps<ClockGet>::operator()(int clock, time_t *seconds, long *nanoseconds) {
	BosTimespec now;
	if (int e = error_of(call(BOS_SYS_CLOCK_GET, clock, long(&now)))) return e;
	*seconds = time_t(now.seconds);
	*nanoseconds = now.nanoseconds;
	return 0;
}

int Sysdeps<Sleep>::operator()(time_t *seconds, long *nanoseconds) {
	BosTimespec request{*seconds, *nanoseconds}, remaining{0, 0};
	int e = error_of(call(BOS_SYS_SLEEP, long(&request), long(&remaining)));
	*seconds = time_t(remaining.seconds);
	*nanoseconds = remaining.nanoseconds;
	return e;
}

void Sysdeps<Yield>::operator()() {
	call(BOS_SYS_YIELD);
}

int Sysdeps<Dup2>::operator()(int, int, int) {
	return ENOSYS;
}

pid_t Sysdeps<GetPid>::operator()() {
	return pid_t(call(BOS_SYS_GETPID));
}

pid_t Sysdeps<GetTid>::operator()() {
	return pid_t(call(BOS_SYS_GETTID));
}

} // namespace mlibc
