# BEX1 owned-page backing (C2b)

Desktop process records now store exactly sixteen private physical frame
addresses instead of an inline 65,536-byte saved image. This is the first real
client of the bounded allocator. BEX1, public syscalls 0–27, its 49,152-byte file
cap, 64 KiB aperture, 16 KiB linker stack reservation, initial ESP 65,520, eight
process records and Terminal display IDs 1–8 remain unchanged. Legacy synchronous
exec uses its existing aperture, reports task ID 0 and allocates no saved pages.
There is no heap, BEX2, page-table switch, larger image or new window interface.

## Creation, copying and release

Creation validates the complete image and copied argument first, reserves a free
record and consumes the existing nonwrapping process serial. It asks the real
allocator for sixteen `PHYS_BEX1_BACKING` frames. All sixteen are zeroed before
any frame list is published; creation then copies the executable page by page.
The image, BSS and stack therefore occupy one logical 64 KiB image even when
physical pages are scattered. The final partial executable page stays zero
beyond the file. The public output process handle is written only on success.

Allocation failure clears the uncommitted record and leaves the caller's output,
existing processes and allocator accounting unchanged. The consumed owner serial
is not reused. The private `PROCESS_CREATE_MEMORY` result produces an explicit
Terminal backing-memory error without resetting the previous canvas. Binding or
start failure uses the existing stop/reap rollback path. No runtime or hidden
compile-time inline fallback exists; the parent commit is the development
rollback. The entire TASK reservation is retained, including the newly unused
control-table slack and the unchanged syscall stack at `0x030E0000`.

Before each desktop slice, sixteen page copies gather 65,536 bytes into the
unchanged aperture. After `process_resume` returns, x87/kernel state is restored,
then active/current state is cleared. Only a continuing process scatters its
65,536 bytes back. Exit or deferred Stop releases the sixteen frames directly
through their owner/kind-checked immutable list. It does no final outgoing copy.
A successful release clears the private list before the DONE record can be
reaped. Repeated Stop/reap, normal/nonzero exit, failed attachment and reset all
share that single resource-release path. A broken internal ownership invariant
panics rather than silently discarding the only reference to live frames.

Retained desktop backing is never involved in synchronous exec. Its existing
file/sync cleanup still uses its own serial, and retained desktop process pages
remain allocated and byte-for-byte unchanged. A waiting process owns no active
kernel stack or borrowed user pointer; stopping it releases its pages only after
return, independently of the shared filesystem snapshot's lifetime.

## Layout and verification scope

The real i386 compiler measures `NativeTask` at 508 bytes, eight records at
4,064 bytes, ending at `0x03000FE0`. The production assertion keeps that actual
end below page metadata at `0x03090000`; metadata still ends exactly at the
unchanged syscall-stack boundary. The full static TASK MiB remains excluded from
allocation. Physical page lists and accounting stay kernel-private.

The deterministic host lifecycle fixture extracts unchanged production process
functions and links production `physmem.c`/`bootinfo.c`. Only hardware entry,
x87 operations, wrapper selection and address-to-host-buffer translation are
adapters. It performs no guest memory access. It covers 64/128 MiB allocator maps,
not-ready creation, one/two/eight live processes, fragmented backing, exact-cap
and partial-page image copies, zeroed reuse, 64 KiB in/out per continuing slice,
no outgoing copy on exit, pending Stop, finite capacity, failed attachment,
reset, stale handles and synchronous exec with retained desktop images.

Existing ordinary task, argument, expanded SDK and native media guest fixtures
inherit the real `kmain`, including validated RAM/video and late allocator
initialization. Their added assertions require ready accounting, sixteen frames
per direct launch and final restoration of the previous free/allocated/kind
counts before their success markers. They never reinitialize a ready allocator.
The original frozen BEX1 fixtures remain untouched.

Focused compilation is not guest evidence. Normal production guest gates remain
required on 64 MiB/default and 128 MiB/large, including unchanged frozen BEX1
behavior, input/sleep/x87, published output, close/reset/reuse, shared sync-owner
cleanup, persistence, counts and latency. The separately assigned verification
worker owns those runs. No performance improvement or released integration is
claimed by this source change.

### Implementation checks (2026-10-04)

- The explicitly selected ordinary aggregate passed all 32 tests:
  `PYTHONPATH=tests python3 -m unittest -v test_process_lifetime test_physmem
  test_process_bindings test_native_platform test_native_publication
  test_native_publication_process test_native_arguments test_native_launch
  test_native_titles test_native_render test_native_rectangle test_native_files
  test_native_sync test_sysmon test_foundation.NativeTests.test_bootinfo
  test_kernel_layout`. The lifecycle and existing native C fixtures use
  ASan/UBSan; the bounded allocator fixture links the same production core.
- The standalone `optional_memory_host.c` fixture passed with ASan/UBSan.
  Frozen BEX1 fixture hashes matched their manifest; `sdk/`, those fixture bytes
  and `src/layout.h` have no change from the parent commit.
- `make -j2 build/baseos.img` passed without compiler warnings using the existing
  verified NASM toolchain. This first build was development-dirty at `0c5ccd0`;
  its kernel measured 505,813 text, 316 data and 107,856 BSS bytes, with
  `__kernel_end=0x195E90` below `STACK_BOTTOM=0x1F0000`. Its initialized payload
  was 506,156 bytes and packed payload 342,413 bytes. A final clean build and
  its exact provenance are recorded separately with the handoff.
- Five ordinary guest source files (`task_guest`, `native_arguments_guest`,
  `sdk_expanded_guest`, `task_media_guest`, and `mpeg_av_guest`) compiled with
  actual previously generated task/x87 and MP3/MPEG fixture headers. MPEG also
  compiled with native tasks disabled. These were compile-only checks; no QEMU
  or helper guest execution was performed by this implementation task.

During fixture migration, the publication-only fixture's fabricated live record
was replaced with real allocated backing before teardown, and the Terminal
error assertion was corrected to account for its existing trailing generic
command-error line. The final aggregate above passed after those fixture fixes.
