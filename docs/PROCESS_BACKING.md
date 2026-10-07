# BEX1 owned-page backing

Desktop process records now store exactly sixteen private physical frame
addresses instead of an inline 65,536-byte saved image. This is the first real
client of the bounded allocator. BEX1, public syscalls 0–27, its 49,152-byte file
cap, 64 KiB aperture, 16 KiB linker stack reservation, initial ESP 65,520, eight
process records and Terminal display IDs 1–8 remain unchanged. Legacy synchronous
exec uses its existing aperture, reports task ID 0 and allocates no saved pages.
This backing adds no heap, page-table switch, larger image or new window interface.

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
start failure uses the existing stop/reap rollback path. No runtime or
compile-time inline fallback exists. The entire TASK reservation is retained, including the newly unused
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
initialization. Their assertions require ready accounting, sixteen frames
per direct launch and final restoration of the previous free/allocated/kind
counts before their success markers. They never reinitialize a ready allocator.
The original BEX1 fixtures in `tests/fixtures/bex1-legacy` are unchanged.

### Host tests

```sh
PYTHONPATH=tests python3 -m unittest -v test_process_lifetime test_physmem \
  test_process_bindings test_native_platform test_native_publication \
  test_native_publication_process test_native_arguments test_native_launch \
  test_native_titles test_native_render test_native_rectangle test_native_files \
  test_native_sync test_sysmon test_foundation.NativeTests.test_bootinfo \
  test_kernel_layout
```

The lifecycle and existing native C fixtures use ASan/UBSan; the bounded
allocator fixture links the same production core. The standalone
`tests/optional_memory_host.c` fixture also runs with ASan/UBSan.
