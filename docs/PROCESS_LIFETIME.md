# Independent process lifetime (C1)

This internal foundation leaves BEX1, syscalls 0–27, the fixed 64 KiB image,
49,152-byte image cap, 16 KiB linker stack reservation, initial ESP 65,520,
eight desktop task contexts and synchronous exec unchanged. It does not add
pages, a heap, larger programs, detached execution, reassignment, more windows,
or a new application identity namespace.

## Ownership and state

The existing nonwrapping, boot-lifetime process owner handle identifies each
record. Allocation takes a FREE record independently of the Terminal slot.
Lookup compares the complete nonzero handle; indices are never accepted as
handles. IDs survive completion and are erased only when its record is reaped.

The phases are EMPTY → CREATING → CREATED → READY/SLEEPING → EXITING → DONE →
EMPTY. Creation validates and copies the file and startup argument before
publishing its output handle. CREATED does not run. Binding copies a bounded
callback table and a (process, Terminal slot, binding generation) value; start
requires that attachment. Unbinding an unscheduled CREATED record preserves
its owned resources. There is no public unbind/detach command.

A Terminal owns scrollback and working/published canvases, while the process
owns image, registers, x87 state, input queue, wake/wait state, copied startup
argument and file/sync resources. Contextual callbacks resolve all three
attachment fields and change an explicit Terminal pointer without touching
selection. A reused slot rejects callbacks from every earlier binding.

The Terminal's binding generation is preserved through reset and fails closed
at exhaustion. Sysmon's existing instance guard is now the process handle.
Call 12 reads the explicitly bound one-based display slot, so record zero bound
to Terminal five still reports 6. Synchronous exec still reports 0.

## Teardown ordering

1. Application completion records its value and APP reason, while generic
   completion records ERROR. Explicit app exit publishes first, including a
   nonzero result. External Stop records STOP and does not publish.
2. The interrupt path marks EXITING and leaves the process. It does not release
   files, sync interests, image storage, owner identity or callback state.
3. After `process_resume`/`process_enter` returns, x87/kernel context is restored
   before active state is cleared. A continuing desktop process saves its fixed
   image exactly once. An exiting process does not copy an outgoing image.
4. In inactive kernel context, owner release runs once and clears wait/input.
   A shared filesystem commit continues under the existing sync coordinator.
5. DONE retains the exact handle/value/reason until Terminal consumes it. Reap
   then clears the record. Stale/repeated stop/reap never affects a replacement.

Idle close/reset stops and reaps before releasing the Terminal attachment.
An unexpected active close records a deferred stop and returns false; the caller
must not erase/reuse the Terminal or window. No process is recursively dispatched.
The next inactive scheduler turn handles a pending stop before its sleep/wait
can delay cleanup. The stop request itself never publishes. An explicit
present/yield/sleep/exit already occurring in that active slice still publishes
under the existing contract; an explicit APP exit recorded before return keeps
its result rather than being replaced by STOP. No further slice follows the
pending stop. The normal desktop cannot issue Stop during a slice, so these are
internal deferred-request semantics. These guards do not add kernel preemption.

## Verification and remaining gates

Focused host checks exercise real extracted production creation, state,
lookup, dispatcher, scheduler, final-copy and owner-release code. Hardware entry,
x87 instructions and page setup are explicit test spies, not emulated hardware.
Terminal tests separately use the real drawing/output helpers with contextual
bindings. File and shared-sync coordinator suites exercise real services.

Host/build results are recorded with the integration handoff. A host result is
not guest proof. Before release, the normal runner still needs frozen BEX1
fixtures and two-task input/sleep/x87/owner cleanup, stop/close/reset/reuse,
publication and persisted-output checks on 64 MiB/default and 128 MiB/large.
Only the separately assigned verification task may run those guest gates.

### C1 implementation checks (2026-10-04)

- Focused ordinary aggregate: 20 tests passed with ASan/UBSan:
  `PYTHONPATH=tests python3 -m unittest -v test_process_lifetime
  test_process_bindings test_native_platform test_native_publication
  test_native_publication_process test_native_arguments test_native_launch
  test_native_titles test_native_render test_native_rectangle test_native_files
  test_native_sync`.
- Additional Terminal metadata, SDK canvas, downloads and Terminal move fixtures
  passed. Four migrated guest C files compiled with actual generated BEX/FPU,
  MP3 and MPEG fixtures; their objects have no old `process_task_*` symbols.
  The broader `features_host` fixture was compile-checked only, not executed.
- `make -j2 build/kernel.elf build/kernel.packed` passed with the existing NASM
  toolchain and no compiler warnings. No QEMU was run for this implementation.
- Actual i386 `NativeTask` is 65,980 bytes: eight records use 527,840 bytes and
  end at `0x03080DE0`, leaving 61,984 bytes before the next-stage metadata start
  `0x03090000`. The new metadata constant activates a production non-overlap
  assertion when C2 is integrated. The unchanged syscall stack starts
  `0x030E0000`. Each Terminal is 91,520 bytes; eight use 732,160 bytes.
- The first build has 502,731 text, 316 data and 107,792 BSS bytes;
  `__kernel_end=0x195230`, below `STACK_BOTTOM=0x1F0000`. Its packed initialized
  payload is 340,107 bytes. These are build measurements, not runtime timings.

The dispatcher/publication tests now share a source selector because the core
uses handles instead of slot-indexed `task_at`. It copies the exact production
functions and retains only the existing narrow legacy-sync interrupt-flag shim.
The new lifecycle test additionally executes production scheduling, save/restore
ordering and release paths, using explicit entry/FPU spies, so changing a
textual extractor is not the only evidence. Terminal-only fixtures retain
slot-labelled views for readable assertions but resolve distinct opaque handles
through independent fixture records. Guest helpers now create/bind/start and
consume/reap explicitly; they do not change executable bytes or syscall data.
