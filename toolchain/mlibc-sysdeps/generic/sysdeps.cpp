// BaseOS system dependencies for mlibc. Calls trap with int $0x81 and return
// a value or a negated errno; the numbers live in sdk/baseos_sys.h.
#include <abi-bits/errno.h>
#include <abi-bits/fcntl.h>
#include <dirent.h>
#include <stddef.h>
#include <sys/stat.h>
#include <abi-bits/vm-flags.h>
#include <bits/ensure.h>
#include <bits/syscall.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/tcb.hpp>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>

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

// A path relative to a directory descriptor becomes an absolute path built
// from the directory's own path; plain and absolute paths pass through.
namespace {
struct AtPath {
	char buffer[2048];
	const char *path = nullptr;
};
} // namespace

static int at_path(int dirfd, const char *path, AtPath &out) {
	if (!path) return EFAULT;
	if (dirfd == AT_FDCWD || path[0] == '/') {
		out.path = path;
		return 0;
	}
	long length = call(BOS_SYS_FD_PATH, dirfd, long(out.buffer), long(sizeof out.buffer));
	if (int e = error_of(length)) return e;
	size_t used = size_t(length) - 1, extra = strlen(path);
	if (used + 1 + extra + 1 > sizeof out.buffer) return ENAMETOOLONG;
	if (extra) {
		if (used && out.buffer[used - 1] != '/') out.buffer[used++] = '/';
		memcpy(out.buffer + used, path, extra + 1);
	}
	out.path = out.buffer;
	return 0;
}

int Sysdeps<Openat>::operator()(int dirfd, const char *path, int flags, mode_t mode, int *fd) {
	AtPath at;
	if (int e = at_path(dirfd, path, at)) return e;
	return sysdep<Open>(at.path, flags, mode, fd);
}

int Sysdeps<Stat>::operator()(fsfd_target target, int fd, const char *path, int flags,
		struct stat *out) {
	(void)flags;
	BosStat info;
	long result;
	if (target == fsfd_target::fd || (target == fsfd_target::fd_path && path && !path[0])) {
		result = call(BOS_SYS_FSTAT, fd, long(&info));
	} else {
		AtPath at;
		if (target == fsfd_target::fd_path) {
			if (int e = at_path(fd, path, at)) return e;
			path = at.path;
		}
		result = call(BOS_SYS_STAT, long(path), long(&info));
	}
	if (int e = error_of(result)) return e;
	memset(out, 0, sizeof *out);
	out->st_ino = info.node;
	out->st_mode = info.mode;
	out->st_nlink = info.links;
	out->st_size = info.size;
	out->st_blksize = 4096;
	out->st_blocks = (info.size + 511) / 512;
	out->st_mtim.tv_sec = time_t(info.modified);
	out->st_atim = out->st_ctim = out->st_mtim;
	return 0;
}

int Sysdeps<OpenDir>::operator()(const char *path, int *handle) {
	return sysdep<Open>(path, O_RDONLY | O_DIRECTORY, 0, handle);
}

// BosDirent records become struct dirent records. Names are short, so a
// quarter of the space holds as many kernel records as will fit converted.
int Sysdeps<ReadEntries>::operator()(int handle, void *buffer, size_t max_size, size_t *bytes_read) {
	char records[512];
	size_t ask = max_size / 4 < sizeof records ? max_size / 4 : sizeof records;
	long result = call(BOS_SYS_READDIR, handle, long(records), long(ask));
	if (int e = error_of(result)) return e;
	size_t done = 0;
	for (long at = 0; at < result;) {
		auto *entry = reinterpret_cast<BosDirent *>(records + at);
		size_t name = strlen(entry->name);
		size_t size = (offsetof(struct dirent, d_name) + name + 1 + 7) & ~size_t(7);
		if (done + size > max_size) return EINVAL;
		auto *out = reinterpret_cast<struct dirent *>(static_cast<char *>(buffer) + done);
		out->d_ino = entry->node;
		out->d_off = 0;
		out->d_reclen = size;
		out->d_type = entry->type;
		memcpy(out->d_name, entry->name, name + 1);
		done += size;
		at += entry->length;
	}
	*bytes_read = done;
	return 0;
}

int Sysdeps<Mkdir>::operator()(const char *path, mode_t mode) {
	return error_of(call(BOS_SYS_MKDIR, long(path), long(mode)));
}

int Sysdeps<Rmdir>::operator()(const char *path) {
	return error_of(call(BOS_SYS_UNLINK, long(path), BOS_AT_REMOVEDIR));
}

int Sysdeps<Unlinkat>::operator()(int dirfd, const char *path, int flags) {
	AtPath at;
	if (int e = at_path(dirfd, path, at)) return e;
	return error_of(call(BOS_SYS_UNLINK, long(at.path), flags & AT_REMOVEDIR ? BOS_AT_REMOVEDIR : 0));
}

int Sysdeps<Mkdirat>::operator()(int dirfd, const char *path, mode_t mode) {
	AtPath at;
	if (int e = at_path(dirfd, path, at)) return e;
	return sysdep<Mkdir>(at.path, mode);
}

int Sysdeps<Renameat>::operator()(int olddirfd, const char *old_path, int newdirfd, const char *new_path) {
	AtPath from, to;
	if (int e = at_path(olddirfd, old_path, from)) return e;
	if (int e = at_path(newdirfd, new_path, to)) return e;
	return sysdep<Rename>(from.path, to.path);
}

int Sysdeps<Rename>::operator()(const char *path, const char *new_path) {
	return error_of(call(BOS_SYS_RENAME, long(path), long(new_path)));
}

int Sysdeps<Access>::operator()(const char *path, int mode) {
	(void)mode;
	BosStat info;
	return error_of(call(BOS_SYS_STAT, long(path), long(&info)));
}

int Sysdeps<Faccessat>::operator()(int dirfd, const char *path, int mode, int flags) {
	(void)flags;
	AtPath at;
	if (int e = at_path(dirfd, path, at)) return e;
	return sysdep<Access>(at.path, mode);
}

int Sysdeps<Dup>::operator()(int fd, int flags, int *newfd) {
	(void)flags;
	long result = call(BOS_SYS_DUP, fd);
	if (int e = error_of(result)) return e;
	*newfd = int(result);
	return 0;
}

int Sysdeps<Dup2>::operator()(int fd, int flags, int newfd) {
	(void)flags;
	return error_of(call(BOS_SYS_DUP2, fd, newfd));
}

int Sysdeps<Pipe>::operator()(int *fds, int flags) {
	(void)flags;
	return error_of(call(BOS_SYS_PIPE, long(fds)));
}

int Sysdeps<Chdir>::operator()(const char *path) {
	return error_of(call(BOS_SYS_CHDIR, long(path)));
}

int Sysdeps<GetCwd>::operator()(char *buffer, size_t size) {
	return error_of(call(BOS_SYS_GETCWD, long(buffer), long(size)));
}

int Sysdeps<Fsync>::operator()(int fd) {
	return error_of(call(BOS_SYS_FSYNC, fd));
}

int Sysdeps<Ftruncate>::operator()(int fd, size_t size) {
	return error_of(call(BOS_SYS_FTRUNCATE, fd, long(size)));
}

// Descriptor flags: there is no exec, so close-on-exec is accepted and kept
// nowhere; status flags read back as set at open.
int Sysdeps<Fcntl>::operator()(int fd, int request, va_list args, int *result) {
	switch (request) {
	case F_DUPFD:
	case F_DUPFD_CLOEXEC: {
		int lowest = va_arg(args, int);
		(void)lowest;
		return sysdep<Dup>(fd, 0, result);
	}
	case F_GETFD:
	case F_SETFD:
	case F_SETFL:
		*result = 0;
		return error_of(call(BOS_SYS_ISATTY, fd)) == EBADF ? EBADF : 0;
	case F_GETFL:
		*result = O_RDWR;
		return 0;
	default:
		return EINVAL;
	}
}

// One user, no signals delivered from outside: handlers and masks are
// accepted, and a signal sent to this process ends it the way the default
// action would.
int Sysdeps<Sigaction>::operator()(int, const struct sigaction *__restrict, struct sigaction *__restrict old) {
	if (old) memset(old, 0, sizeof *old);
	return 0;
}

int Sysdeps<Sigprocmask>::operator()(int, const sigset_t *__restrict, sigset_t *__restrict old) {
	if (old) memset(old, 0, sizeof *old);
	return 0;
}

int Sysdeps<ThreadSigmask>::operator()(int, const sigset_t *__restrict, sigset_t *__restrict old) {
	if (old) memset(old, 0, sizeof *old);
	return 0;
}

int Sysdeps<Kill>::operator()(pid_t pid, int signal) {
	if (pid != pid_t(call(BOS_SYS_GETPID)) && pid != 0) return ESRCH;
	if (signal) call(BOS_SYS_EXIT, 128 + signal);
	return 0;
}

int Sysdeps<Tgkill>::operator()(int, int, int signal) {
	if (signal) call(BOS_SYS_EXIT, 128 + signal);
	return 0;
}

int Sysdeps<Uname>::operator()(struct utsname *out) {
	memset(out, 0, sizeof *out);
	strcpy(out->sysname, "BaseOS");
	strcpy(out->nodename, "baseos");
	strcpy(out->release, "2");
	strcpy(out->version, "2");
	strcpy(out->machine, "i686");
	return 0;
}

int Sysdeps<GetHostname>::operator()(char *buffer, size_t size) {
	if (size < 7) return ENAMETOOLONG;
	strcpy(buffer, "baseos");
	return 0;
}

uid_t Sysdeps<GetUid>::operator()() { return 0; }
uid_t Sysdeps<GetEuid>::operator()() { return 0; }
gid_t Sysdeps<GetGid>::operator()() { return 0; }
gid_t Sysdeps<GetEgid>::operator()() { return 0; }
pid_t Sysdeps<GetPpid>::operator()() { return 0; }

int Sysdeps<Umask>::operator()(mode_t, mode_t *old) {
	*old = 022;
	return 0;
}

// Files have no permission bits or access times to change.
int Sysdeps<Chmod>::operator()(const char *path, mode_t) {
	return sysdep<Access>(path, 0);
}

int Sysdeps<Fchmod>::operator()(int fd, mode_t) {
	return error_of(call(BOS_SYS_ISATTY, fd)) == EBADF ? EBADF : 0;
}

int Sysdeps<Fchmodat>::operator()(int dirfd, const char *path, mode_t mode, int) {
	AtPath at;
	if (int e = at_path(dirfd, path, at)) return e;
	return sysdep<Chmod>(at.path, mode);
}

int Sysdeps<Utimensat>::operator()(int dirfd, const char *path, const struct timespec *, int) {
	if (!path) return 0;
	AtPath at;
	if (int e = at_path(dirfd, path, at)) return e;
	return sysdep<Access>(at.path, 0);
}

int Sysdeps<Readlink>::operator()(const char *path, void *, size_t, ssize_t *) {
	int e = sysdep<Access>(path, 0);
	return e ? e : EINVAL;
}

int Sysdeps<Ioctl>::operator()(int, unsigned long, void *, int *) {
	return ENOTTY;
}

int Sysdeps<Fdatasync>::operator()(int fd) {
	return sysdep<Fsync>(fd);
}

int Sysdeps<ClockGetres>::operator()(int, time_t *seconds, long *nanoseconds) {
	*seconds = 0;
	*nanoseconds = 1000000;
	return 0;
}

// Positioned I/O moves the shared offset and puts it back.
int Sysdeps<Pread>::operator()(int fd, void *buffer, size_t count, off_t offset, ssize_t *read) {
	off_t old;
	if (int e = sysdep<Seek>(fd, 0, SEEK_CUR, &old)) return e;
	if (int e = sysdep<Seek>(fd, offset, SEEK_SET, &offset)) return e;
	int e = sysdep<Read>(fd, buffer, count, read);
	sysdep<Seek>(fd, old, SEEK_SET, &old);
	return e;
}

int Sysdeps<Pwrite>::operator()(int fd, const void *buffer, size_t count, off_t offset, ssize_t *written) {
	off_t old;
	if (int e = sysdep<Seek>(fd, 0, SEEK_CUR, &old)) return e;
	if (int e = sysdep<Seek>(fd, offset, SEEK_SET, &offset)) return e;
	int e = sysdep<Write>(fd, buffer, count, written);
	sysdep<Seek>(fd, old, SEEK_SET, &old);
	return e;
}

static void fill_statvfs(struct statvfs *out) {
	memset(out, 0, sizeof *out);
	BosMemInfo info;
	memset(&info, 0, sizeof info);
	call(BOS_SYS_MEMINFO, long(&info));
	out->f_bsize = out->f_frsize = 4096;
	out->f_blocks = info.total_pages ? info.total_pages : 65536;
	out->f_bfree = out->f_bavail = info.free_pages;
	out->f_namemax = 255;
}

int Sysdeps<Statvfs>::operator()(const char *path, struct statvfs *out) {
	if (int e = sysdep<Access>(path, 0)) return e;
	fill_statvfs(out);
	return 0;
}

int Sysdeps<Fstatvfs>::operator()(int fd, struct statvfs *out) {
	if (error_of(call(BOS_SYS_ISATTY, fd)) == EBADF) return EBADF;
	fill_statvfs(out);
	return 0;
}

int Sysdeps<Sysconf>::operator()(int number, long *value) {
	switch (number) {
	case _SC_NPROCESSORS_CONF:
	case _SC_NPROCESSORS_ONLN:
		*value = 1;
		return 0;
	case _SC_PAGESIZE:
		*value = 4096;
		return 0;
	default:
		return EINVAL;
	}
}

pid_t Sysdeps<GetPid>::operator()() {
	return pid_t(call(BOS_SYS_GETPID));
}

pid_t Sysdeps<GetTid>::operator()() {
	return pid_t(call(BOS_SYS_GETTID));
}

} // namespace mlibc
