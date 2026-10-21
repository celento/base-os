# Platform ABI v2: ELF processes

BaseOS runs ordinary static ELF32 programs next to the existing BEX1/BEX2
runtime. Each program gets a private address space of up to 2 GiB, any number
of threads, and system calls through `int $0x81`. The BEX runtime, its ABI and
its tests are unchanged.

This page covers the kernel side. The cross compiler and C library are in
[TOOLCHAIN.md](TOOLCHAIN.md); the call numbers and structures are in
`sdk/baseos_sys.h`.

## What a program sees

| Item | Value |
| --- | --- |
| Format | ELF32, i386, `ET_EXEC`, static, `PT_LOAD` segments page-aligned in file and memory |
| User window | `0x40000000` to `0xC0000000` (2 GiB) |
| Usual text address | `0x40100000` |
| Stack | 8 MiB reserved below `0xBFFF0000`, demand-zero; the page below is unmapped |
| Anonymous mappings | Grow down from `0xBF000000` unless a hint or `MAP_FIXED` says otherwise |
| Initial stack | System V: `argc`, `argv`, empty `envp`, auxv with `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_ENTRY`, `AT_RANDOM` |
| Segments | Code `0x33`, data `0x3B`, TLS `0x43` (`%gs`, base set by `TCB_SET`) |
| System calls | `int $0x81`, number in EAX, arguments in EBX, ECX, EDX, ESI, EDI, EBP, result or negated errno in EAX |

Error, `mmap` flag and clock numbers use the Linux i386 values so the C library
can reuse its definitions.

## System calls in this release

| Call | Behavior now |
| --- | --- |
| `EXIT`, `THREAD_EXIT` | End the process, or one thread (the last thread ends the process with status 0) |
| `READ`, `WRITE` | On any descriptor. The console (0, 1 and 2 at start) writes to the host's output buffer (a Terminal), or the serial log when no host drains it; a full buffer blocks the writer. Console reads return end of file |
| `OPEN`, `CLOSE`, `SEEK`, `FTRUNCATE`, `FSYNC` | Files and directories on the native volume. `O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`, `O_DIRECTORY` (Linux values) |
| `STAT`, `FSTAT`, `ISATTY` | `BosStat`: type and permission bits, a stable node number, size and modification time |
| `READDIR` | `BosDirent` records: `.`, `..`, then the directory's entries |
| `MKDIR`, `UNLINK`, `RENAME` | `UNLINK` with `BOS_AT_REMOVEDIR` removes an empty directory; `RENAME` replaces an existing file |
| `DUP`, `DUP2`, `PIPE` | Shared open files; 4 KiB pipes that block, report end of file once writers close and `EPIPE` once readers do |
| `CHDIR`, `GETCWD` | Per-process working directory for relative paths |
| `VM_MAP`, `VM_UNMAP`, `VM_PROTECT` | Anonymous private mappings; file mappings return `ENOSYS` |
| `TCB_SET` | Points this thread's TLS segment at the TCB and loads `%gs` |
| `CLOCK_GET` | `REALTIME` (CMOS clock at first use plus uptime) and `MONOTONIC`, with sub-microsecond resolution (PIT counter) |
| `SLEEP`, `YIELD` | Sleeps end at the first millisecond boundary after the requested time |
| `FUTEX_WAIT`, `FUTEX_WAKE` | Keyed by physical address, with optional relative timeout (millisecond resolution) |
| `THREAD_CREATE` | `(entry, stack, argument, tcb)`; the thread starts as `entry(argument)` with a zero return address |
| `THREAD_PRIORITY` | `(thread id or 0, priority)` with -1 low, 0 normal, 1 high; `ESRCH` for a thread outside the process |
| `GETPID`, `GETTID`, `MEMINFO`, `LOG` | As named |

## Files

`kernel/ufile.c` gives each process a descriptor table that grows as needed.
Descriptors name shared, reference-counted open files, so `DUP`, `DUP2` and
threads see one offset. Native volume files are replaced atomically as a whole,
so a file opened for writing keeps its contents in a kernel buffer: writes,
seeks and `FTRUNCATE` change the buffer, and `FSYNC`, the last close or the
process's exit writes it back with one `fs_write`. While a volume snapshot is
in progress the writer waits and retries; a process that is killed instead
leaves its unsaved files to the kernel loop, which writes them once the volume
accepts writes again. Two descriptors that open the same file for writing each
keep their own copy, and the last one written back wins. Volume limits apply
(names up to 23 bytes, the per-file size limit, `ENOSPC` when the node table is
full). The data disk now uses BaseFS 2 (docs/BASEFS2.md), and these limits go
when files load on demand. The C library maps
`stdio`, `opendir`/`readdir`, `stat`, `mkdir`, `rename`, `remove`, `pipe`, `dup`,
`chdir` and `getcwd` onto these calls; `*at` calls support `AT_FDCWD` and
absolute paths only.

## Memory

`kernel/physmem.c` now manages two zones. The compatibility pool below 256 MiB
keeps its exact behavior for BEX programs. A second zone covers every usable
E820 frame from 256 MiB to the top of RAM below 4 GiB. Its metadata (five
bytes per frame) takes the first usable run above 256 MiB. ELF programs and the
kernel heap allocate from the high zone first, so BEX capacity does not change.
There is no per-process page cap: a program can use whatever RAM is free.

`kernel/kheap.c` is a small kernel heap for objects whose count is not fixed
at build time (processes, threads, address-space areas, kernel stacks). It
takes frames below 1 GiB so they stay identity-mapped under every root.

`kernel/vm.c` gives each process its own page directory. Outside the user
window it copies the kernel's supervisor identity mappings; inside it holds
only the process's 4 KiB pages. Areas reserve ranges, frames arrive on first
touch, and `PROT_NONE` keeps contents by clearing only the user bit. Kernel
code reads or writes user memory only after `vm_prepare` has checked the area
and made the pages present, so a bad pointer returns `EFAULT` instead of
faulting in the kernel. Frames the kernel itself touches stay below 1 GiB;
user frames between 1 and 3 GiB are cleared through a brief switch to the
kernel root.

## Timer

The PIT interrupts at about 1 kHz. `timer_milliseconds()` counts milliseconds
and `timer_clock()` adds the PIT counter for sub-microsecond reads that never
step backwards. The legacy `TIMER_HZ` tick (70 Hz) that the desktop, apps and
BEX tasks use is derived from the same interrupt, so their timing is
unchanged. When idle, the desktop loop still turns once per legacy tick, and
sooner when a sleeping user thread is due.

## Device interrupts

`platform_irq_attach(line, handler)` opens a PIC line. With a handler, the
handler runs inside the interrupt; the PS/2 keyboard and mouse use this to
move controller bytes into a ring. Without one, the line is masked when it
fires and reported by `platform_irq_take()`; the kernel loop services the
device and reopens the line with `platform_irq_rearm()`. The SB16 (IRQ5) and
RTL8139 (its PCI line) work this way, so an idle desktop wakes when an audio
block completes or a frame arrives instead of at the next 70 Hz tick. ATA
stays polled, with nIEN set, because its waits are short and bounded. A
line already attached keeps its first owner. Device interrupts
are dispatched before the BEX and ELF runtimes see the vector, so a device
interrupt during a program never ends it.

## Threads and scheduling

`kernel/thread.c` adds kernel threads with their own 32 KiB kernel stacks.
Thread 0 is the desktop: `kmain`'s loop, the shell and the BEX runtime keep
running on the legacy stacks exactly as before.

The kernel stays non-preemptive. A user thread loses the CPU after a 10 ms
quantum of its own while it runs in ring 3, or when a system call blocks or
yields. The desktop loop calls `thread_run_users()` once per turn, which hands
user threads one 10 ms round in total and then takes the CPU back, so the
desktop keeps turning about every 10 ms however many threads are busy. The
desktop halts only when no user thread is runnable, so a busy program gets the
CPU between desktop turns and an idle one costs nothing. x87 state is saved
and restored eagerly for user threads.

Threads have three priorities: low (-1), normal (0, the default) and high (1),
set with `THREAD_PRIORITY`. The highest runnable level runs, round-robin within
it. A high-priority thread whose sleep ends takes the CPU at the next
millisecond instead of waiting for a lower one's quantum. A thread that has
waited 100 ms to run gets a turn whatever its level, but such turns come at
most once per 100 ms, so low-priority work can take one quantum in ten from
busier levels and never starves.

Each thread switch loads the next thread's kernel stack into the TSS, its
address space into CR3 (when the process changes) and its TLS base into the
`0x43` descriptor.

## Faults

A page fault in ring 3 first tries the address space (demand-zero pages). An
invalid access, or any other CPU exception in ring 3, ends that process with a
shell-style status (139 for a segmentation fault, 132 for an invalid opcode,
136 for arithmetic errors) and logs the vector and EIP to serial. The desktop
and other processes keep running. A fault in ring 0 is still a kernel panic.

## Starting programs

From Terminal, `start FILE [DOCUMENT]` checks for the ELF magic. ELF programs
receive the program path as `argv[0]` and the document path as `argv[1]`.
Their output appears in that Terminal, cut into lines, followed by
`Program exited with status N.` `stop` ends the program and closing the
Terminal releases it. BEX files go through the existing launcher unchanged.

System Monitor's Tasks tab lists running ELF programs after the native tasks,
with process id, live threads, memory (resident and page-table frames) and CPU
time, from `uproc_info()`. CPU time is charged per process at every thread
switch, in milliseconds. Its Stop program button ends the process; the
Terminal that started it reports the exit status.

## Tests

- `tools/elf_runtime_test.py` builds `tests/elf_program.c` as a freestanding
  static ELF, links `tests/elf_guest.c` in place of the desktop against the
  production kernel objects, and boots it with 512 MiB. It checks argument
  passing, `.bss`, the real-time clock, a 128 MiB anonymous mapping touched
  page by page, threads with futexes while one thread spins (the desktop loop
  must keep running), fault isolation, and that every process frame and heap
  object comes back. The scheduler check runs 24 low-priority spinners, a
  high-priority sleeper that must wake on time, and four producers and four
  consumers on a futex mutex and condition; the desktop must keep turning at
  least 30 times a second throughout. The files check creates, reads, appends,
  renames, lists and removes files and directories, uses a pipe and `DUP2`, and
  leaves a file open at exit that must reach the volume.
- `tools/elf_libc_test.py` builds `examples/elf/hello.c`, `hello.cpp` and
  `files.c` with the cross toolchain and runs them from an IDE data volume.
- `tools/elf_terminal_input_test.py` drives the real desktop with PS/2 input:
  it starts the program twice from Terminal and checks the scrollback.

## Not yet

`mmap` of files, signals, environment variables, `fork`/`exec` and console
input belong to the later phases of the roadmap (the block-based volume, window
and input APIs).
