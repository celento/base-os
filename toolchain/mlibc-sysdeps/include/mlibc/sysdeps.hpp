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
	// Referenced unconditionally by the BSD pty helpers; descriptors other
	// than the console arrive with the file-descriptor work.
	Dup2
{};

template<typename Tag>
using Sysdeps = SysdepOf<BaseosSysdepTags, Tag>;

struct SysdepTraits {
	static constexpr bool usesRtNetlink = false;
};

} // namespace mlibc
