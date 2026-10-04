# Owned native durability completions

The additive native platform ABI gives desktop tasks owned, opaque durability
completion handles. BEX1 headers and calls 0–17 are unchanged. In particular,
legacy `bos_sync()` remains blocking and keeps its existing result contract.
`BOS_FEATURE_OWNED_SYNC` and `BOS_FEATURE_OPERATION_WAIT` describe the new service;
applications must query capabilities before using it. The asynchronous service
is available only on mounted default/large IDE backends in desktop-task context.
Floppy and protected floppy recovery views report unsupported. No new request
can fall through to the old synchronous floppy request path.

## Boundary and outcomes

A successful `bos_sync_begin(&operation)` accepts one exact durability boundary:
all filesystem mutations that finished before this call. The snapshot leases
live filesystem state from acceptance through final verification or failure.
Any later mutation attempt during the lease returns busy without side effects.
If a snapshot was already running, the request joins that same leased state;
there cannot have been an intervening successful filesystem mutation. There is
no deferred queue whose later acceptance silently expands the promised boundary.
Multiple applications can join, even when autosave or a built-in owns the commit.

Acceptance returns `BOS_OK`; it does not mean saved. `bos_sync_poll(operation)`
returns `BOS_PENDING` until final result, then `BOS_OK` only after the filesystem
has completed its full durable protocol:

1. Serialize and validate the immutable snapshot, including its checksum.
2. Write inactive payload, flush, and read back/check the payload.
3. Write commit header, flush, and read back/check header and complete payload.
4. Select the new generation and clear dirty RAM only after final validation.

A clean IDE volume yields an immediately completed handle with no I/O. This is
also valid for an already-clean, protected IDE snapshot. Dirty protected storage
rejects a new request with `BOS_E_PROTECTED`. Ordinary storage failure completes
accepted handles with `BOS_E_IO`; dirty RAM remains available. An uncertain header
commit still protects subsequent disk writes until remount. Flush/readback
success depends on the existing emulator/host storage contract; external backups
remain necessary.

Later writes after completion are outside an earlier handle's boundary, even
if that handle remains open and keeps returning success. Request a new boundary
after those writes. If completion was not collected before a successful remount
invalidated the internal ticket, that handle reports `BOS_E_STALE` rather than
claiming an unobserved result. An already-collected terminal result is a historical
outcome and stays retained across remount; it does not describe the new mount.

## Ownership, wait and cleanup

Each process can retain at most four completion handles, within a system capacity
of 32. Every accepted handle is unique for this boot; serial exhaustion fails
closed with `BOS_E_CAPACITY` rather than recycling an old handle. The process key
is generation-qualified independently of the display slot. Foreign, released or
old-process handles return `BOS_E_STALE` and cannot release another owner's work.

`bos_sync_wait(operation, milliseconds)` can suspend a task until completion or
its bounded timeout. Waiting yields to the desktop; it never spins through a
complete save on a retained kernel stack. A timeout leaves the operation pending
and owned; poll or wait again, then release it. Check the ABI query for the
supported timeout limit and execution context. The process dispatcher owns the
wait deadline and cleanup; the durability service itself neither schedules nor
dispatches applications.

`bos_sync_release(operation)` discards one completion interest immediately,
including pending work. Normal exit, external stop, failure, task clear and slot
reuse discard all records owned by that process generation. Neither release nor
exit cancels an already-started commit. Other subscribers continue to receive
its exact result. Even if every owner closes, storage continues and its internal
ticket is reaped at completion.

## Kernel implementation and execution contexts

`native_sync.c` owns a single private `FsSyncTicket` and a fixed 32-record table.
Applications never receive or release an FS ticket. `native_sync_tick()` collects
one terminal FS outcome, copies it to all current pending records and releases
the underlying ticket promptly. Completed app records are then detached, so an
app retaining its result cannot starve autosave or another process's next save.
Begin, poll, release and owner cleanup also collect a terminal result if present.
Each call scans at most the fixed table size and performs metadata work only;
none calls `fs_sync_step`, polls ATA or copies snapshot payloads.

The filesystem has three independent fixed subscriber records:

- Slot 0 retains its legacy explicit/autosave ownership and release rules.
- Slot 1 retains the legacy blocking `fs_sync()` join/result behavior.
- Slot 2 is reserved exclusively for the native completion coordinator.

`fs_sync_request_owned()` reserves slot 2 and starts or joins the current commit.
A retained slot 0 result is neither taken nor replaced. A retained completed native
result does not keep slot 2 occupied. Adding this subscriber does not change the
snapshot wire format, generation selection, serialization, readback ordering,
backoff or protection rules. `fs_incarnation()` exposes the existing no-wrap
mount identity for other kernel services; it is not a durable app handle.

All coordinator and FS calls run in the existing serialized desktop/syscall
context. IRQ and device-only polling never call app code. The desktop's actual
`storage_poll()` services the coordinator before and after its existing bounded
FS pump, including idle and terminal turns. Its 256-step/PIT-tick bounds are
unchanged. Native waits do not add reentrant dispatch to `platform_poll()`.

Production i386 `-Os` compilation of the coordinator measures 906 text bytes,
416 BSS bytes including alignment, and a largest own stack frame of 48 bytes
for begin. Its named control state is 400 bytes with a compile-time 512-byte cap.
These are function/object measurements, not full caller/interrupt stack bounds.
The FS's third result adds 16 named bytes and stays within its existing 1536-byte
control-state cap. No high-memory or layout reservations change.

## Verification and limits

Run the ordinary host checks with:

```sh
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  PYTHONPATH=tests python3 -m unittest test_native_sync test_storage_pump test_fs_sync
```

`test_native_sync` executes the real coordinator and filesystem with supported
host disk fixtures under ASan/UBSan. It covers two owners, cross-owner rejection,
coalescing, busy mutation exclusion, all-owner close, new owner generations,
joining autosave/explicit/blocking saves, retained old outcomes during a new
commit, prompt reaping, clean and protected volumes, ordinary transport failure,
uncertain-commit protection, remount staleness, table limits and direct counter
exhaustion boundaries. It checks that request/poll/release perform no storage
work, observes the unchanged seven transport submissions and bounds every poll
to eight sectors. Its full large-profile case commits 256 nodes and 33,543,168
bytes through two owners in 58,917 bounded steps, then reloads exact data while
preserving lower-profile arenas.

`test_storage_pump` compiles the actual extracted desktop pump and checks both
coordinator service hooks without weakening the pre-existing step/time/device
order assertions. Existing `test_fs_sync` covers default and large snapshot
compatibility, retained old explicit results and the independent blocking slot.
Leak detection is disabled because the sandbox runs under ptrace; address and
undefined-behavior checks remain enabled.

These host tests and object builds do not establish actual guest scheduling,
process-fault cleanup, UI latency or disk persistence across a QEMU reboot.
Those require separately integrated process/ABI tests and ordinary guest
workloads. Writer/Spreadsheet native Save/Save As and RTF/PDF/CSV exports now use
the private [document adapter](RESPONSIVE_DOCUMENT_SAVES.md); their own guest
acceptance is independently gated. Other built-in GUI save flows and legacy
native saves still block. This is not general kernel preemption or a claim that
every save caller has been converted.
