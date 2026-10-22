#pragma once

#include <mlibc/sysdep-signatures.hpp>

namespace mlibc {

// System calls BaseOS provides to ELF programs; see sdk/baseos_sys.h.
struct BaseosSysdepTags :
	LibcPanic,
	LibcLog,
	Exit,
	Isatty,
	Write,
	Read,
	Open,
	Close,
	Seek,
	TcbSet,
	AnonAllocate,
	AnonFree,
	VmMap,
	VmUnmap,
	VmProtect,
	FutexWait,
	FutexWake,
	ClockGet,
	Sleep,
	Yield,
	GetPid,
	GetTid,
	PrepareStack,
	Clone,
	ThreadExit,
	Stat,
	OpenDir,
	ReadEntries,
	Openat,
	Mkdir,
	Rmdir,
	Unlinkat,
	Rename,
	Access,
	Faccessat,
	Dup,
	Dup2,
	Pipe,
	Chdir,
	GetCwd,
	Fsync,
	Ftruncate,
	Mkdirat,
	Renameat,
	Fcntl,
	Sigaction,
	Sigprocmask,
	ThreadSigmask,
	Kill,
	Tgkill,
	Uname,
	GetHostname,
	GetUid,
	GetEuid,
	GetGid,
	GetEgid,
	GetPpid,
	Umask,
	Chmod,
	Fchmod,
	Fchmodat,
	Utimensat,
	Readlink,
	Ioctl,
	Fdatasync,
	ClockGetres,
	Pread,
	Pwrite,
	Statvfs,
	Fstatvfs,
	Sysconf
{};

template<typename Tag>
using Sysdeps = SysdepOf<BaseosSysdepTags, Tag>;

struct SysdepTraits {
	static constexpr bool usesRtNetlink = false;
};

} // namespace mlibc
