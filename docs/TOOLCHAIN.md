# Cross toolchain for ELF programs

`toolchain/build.py` builds a GNU cross toolchain whose target is BaseOS:

```sh
python3 toolchain/build.py --jobs 4
build/toolchain/bin/i686-baseos-gcc hello.c -o hello
build/toolchain/bin/i686-baseos-g++ hello.cpp -o hello
```

The result produces static ELF32 i686 programs for the platform ABI v2
described in [PLATFORM_V2.md](PLATFORM_V2.md). With QEMU stopped, copy one to
the data disk and start it from Terminal:

```sh
python3 tools/volume.py build/baseos-data.img import hello /Programs/hello
# in BaseOS Terminal:  start /Programs/hello
```

## What it builds

| Part | Version | Notes |
| --- | --- | --- |
| binutils | 2.43.1 | Adds the `i686-baseos` target (ELF32 i386 emulation) |
| GCC | 14.2.0 | C and C++, static only, POSIX threads model, `__cxa_atexit` |
| mlibc | commit `a1858073` | Static C library with the ANSI, POSIX, glibc and BSD option sets |
| libstdc++ | from GCC 14.2.0 | Built against mlibc |

Sources are pinned by version and SHA-256 (mlibc by git commit) and downloaded
once into `build/toolchain-src`. Every step leaves a stamp in
`build/toolchain/.stamps`, so an interrupted build resumes where it stopped.
`--rebuild-mlibc` rebuilds the C library and libstdc++ after a change to the
BaseOS port. Host requirements: a C/C++ compiler, make, bison, flex, texinfo,
GMP, MPFR, MPC, meson 1.3 or newer, ninja, git and curl. Most of the build
time is GCC.

## Target conventions

The GCC target header (`gcc/config/baseos.h` in the patch) defines
`__baseos__` and `__unix__`, links statically with
`-Ttext-segment=0x40100000 -z max-page-size=4096`, and uses
`crt1.o crti.o crtbegin.o ... crtend.o crtn.o` with `-lc`. C++ code gets
`_GNU_SOURCE` like other glibc-compatible targets, since libstdc++ needs it.

## The C library port

`toolchain/mlibc-sysdeps` is the BaseOS system-dependency layer for mlibc,
copied into the mlibc tree as `sysdeps/baseos`:

- `crt-x86/crt1.S` passes the initial stack and `main` to mlibc's entry.
- `generic/syscall.S` traps with `int $0x81`.
- `generic/sysdeps.cpp` maps mlibc's system dependencies onto the calls in
  `sdk/baseos_sys.h` (console I/O, anonymous memory, TLS, futexes, clocks,
  sleeping, process and thread ids). Calls that the kernel does not offer yet
  return `ENOSYS`, so the library builds now and gains them as the kernel
  grows.
- `include/mlibc/sysdeps.hpp` lists the implemented system dependencies.

mlibc's Linux ABI headers supply the numbering for errno, open flags and
`mmap` flags, which `sdk/baseos_sys.h` matches.

## Licenses

The binutils and GCC patches modify GPL-3.0 projects and carry their license.
They change only the build tools: nothing from them is linked into the kernel
or the shells. Programs built with the toolchain link mlibc (MIT) and, for
C++, libstdc++ and libgcc, which are covered by the GCC Runtime Library
Exception. The mlibc port in `toolchain/mlibc-sysdeps` is MIT, like mlibc.

## Known rough edges

- Static programs are large (about 1.1 MB stripped for a C program that uses
  stdio and threads) because mlibc is not yet built with per-function
  sections and the link does not garbage-collect them.
- The linker warns that `crtend.o` lacks a `.note.GNU-stack` section. The
  warning is harmless: BaseOS ignores the executable-stack flag.
- Thread stacks (2 MiB by default, with at least one guard page) are not
  freed after the thread is joined; mlibc does not release them yet.
