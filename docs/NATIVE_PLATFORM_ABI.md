# Additive native platform ABI 1.0

This is compatibility-preserving groundwork for independent applications, not
Win95/98 platform parity. It implements discoverable owner-bound file versions,
conditional replacement and asynchronous IDE durability completion. It does not
add larger process memory, directory enumeration, a general window/event API,
IPC, user networking, threads or kernel preemption.

## Compatibility boundary

The BEX1 header is still exactly four little-endian words: magic `BEX1`, entry
offset, complete file byte length and required-zero reserved word. The entire
process remains 65,536 bytes, with offset pointers. Calls **0–17 retain their
numbers, arguments and results**. In particular:

- Call 12 remains a one-based Terminal slot, or zero in synchronous `exec`.
- Calls 6/8/13 resolve paths afresh and are not version-consistent readers.
- Calls 7/15 remain unconditional RAM writes; their temporary busy result is -2.
- Call 16 and `bos_sync()` remain blocking and return 0/-1 as before.
- Unknown syscall numbers still return -1. Old kernels return -1 for query18.
- Disk snapshots and application document formats are unchanged.

The committed fixtures in `tests/fixtures/bex1-hour05` are the **original built
bytes**, not rebuilt examples. Their manifest identifies clean source revision
`a7ea36be6db5ae2cd477dfe36e8e58db48145048`, the frozen hourly05 build and SHA-256s.
Hash/header validation alone is not an execution test; guest validation records
must identify their exact tested kernel separately.

## Discover features and limits

`sdk/baseos_abi.h` is the freestanding shared kernel/SDK wire definition. All
integers are explicitly 32-bit and all structures have checked wire sizes.
`bos_abi_query(&info,sizeof(info))` requests ABI major1 and returns `BOS_OK` or
an extended error. **Always check the result before reading output.**

A query accepts a capacity of at least16. It copies the smaller of capacity and
the kernel's full96-byte structure; `struct_size` reports that full size. Bytes
past the copied prefix are untouched. Invalid capacity/range/reserved arguments
or unsupported major leave output unchanged. The first16 bytes contain full
structure size, major, minor and feature bits. Applications should allocate a
zeroed current structure if intentionally requesting only a prefix, read only
fields covered by both sizes, and ignore unknown feature bits/reserved outputs.
Future fields may be appended; incompatible meanings require a new major.

Feature bits are:

- `BOS_FEATURE_VERSIONED_FILES`: open/info/read/conditional replace/close, with
  exclusive empty-file creation. Present in both task and `exec` contexts.
- `BOS_FEATURE_PROCESS_ID`: a new nonzero process identity in the query.
- `BOS_FEATURE_OWNED_SYNC`: prompt request/poll/release in a desktop task on a
  currently supported IDE backend. Not advertised for floppy/protected fallback
  or synchronous `exec`.
- `BOS_FEATURE_OPERATION_WAIT`: bounded task suspension for owned sync handles,
  advertised only where the owned asynchronous service is available.

The query reports context, process identity, runtime backend file limit,
per-process/global service capacities, copy/replacement/path limits, wait limit
and timer frequency. Async limits are zero when that context does not offer the
service. Resource capacity is a maximum, not a promise of currently free slots.
A later backend change or resource exhaustion is still reported by each call.

`stack_reserved_bytes=16384` describes the SDK linker's image/BSS reservation at
the top of the existing64KiB region. **It is not a separately protected stack or
a guarantee of16384 usable stack bytes.** Initial ESP is65520, leaving16368 bytes
above the image/BSS bound before startup/call frames. Handwritten BEX1 code must
follow that layout itself. No physical arena, window index or device address is
part of this new contract. `processes_total=9` is a capacity for **retained
execution contexts**: eight desktop task contexts plus one synchronous `exec`
context. The synchronous execution temporarily pauses desktop-task scheduling;
it does not add a ninth native window or ninth independently scheduled task.
The desktop still has an **eight-window limit**, and applications have no
process-creation/spawn syscall. This field is not a count of free desktop slots
or a promise that the user can launch nine apps from the desktop.

## Owned resources and errors

Handles are opaque32-bit numbers. Process, file and operation allocation domains
are distinct; each uses nonzero serials that never wrap or reset during a boot.
Exhaustion fails closed. Applications must not derive meaning from their bits,
write them to persistent storage for later reuse, or pass them to another app.
A reused Terminal gets a fresh identity, independently of the old slot API.

Every new service checks the owning process and resource lifetime. Closing a
handle invalidates it. Normal exit, nonzero exit, generic failure completion,
external Stop, Terminal close/clear and slot reuse release all owned resources.
No user pointer or stack reference survives a syscall. Services are serialized
in normal kernel/syscall context; ring-0 work is not preempted by another app.

New calls use `BOS_OK=0` and `BOS_PENDING=1` where applicable, and the following
common errors (defined in the header). Legacy errors are deliberately unchanged.

| Name | Value | Meaning |
|---|---:|---|
| `BOS_E_UNSUPPORTED` | -1000 | ABI/service not offered in this context/backend |
| `BOS_E_INVALID` | -1001 | Invalid argument, flags, capacity or offset range |
| `BOS_E_NOT_FOUND` | -1002 | Requested path absent |
| `BOS_E_CAPACITY` | -1003 | Bounded resources/storage or nonreusable tokens exhausted |
| `BOS_E_BUSY` | -1004 | Temporary backend lease/resource contention; retry later |
| `BOS_E_STALE` | -1005 | Closed, wrong-owner, wrong-type or invalidated handle |
| `BOS_E_CHANGED` | -1006 | Bound content/object no longer matches; reopen explicitly |
| `BOS_E_CANCELLED` | -1007 | Reserved vocabulary; no cancellation service advertised |
| `BOS_E_PROTECTED` | -1008 | Requested write not permitted |
| `BOS_E_IO` | -1009 | Durability/storage failure |
| `BOS_E_TIMEOUT` | -1010 | Wait deadline reached; operation remains alive |

## Registers and output rules

EAX contains the call and then its result. Arguments below are EBX, ECX, EDX,
ESI, EDI, in that order. Other registers are preserved. Unused arguments are zero.
Output pointers are offsets in the existing user region, not kernel pointers.
The whole declared output range is checked before any service effect; success
copies only the current structure. Errors never copy partial metadata.

| ID | Arguments | Result |
|---:|---|---|
|18 query|output, capacity, requested major,0,0|`BOS_OK`; output query prefix|
|19 sync begin|handle output,0,0,0,0|`BOS_OK`; one owned handle|
|20 sync poll|handle,0,0,0,0|`BOS_PENDING`, durable `BOS_OK`, or error|
|21 sync wait|handle, milliseconds,0,0,0|terminal result, pending for0ms, or timeout|
|22 sync release|handle,0,0,0,0|`BOS_OK` after local interest is discarded|
|23 file open|path, path length, flags, info output, info capacity|`BOS_OK`|
|24 file info|handle, info output, info capacity,0,0|`BOS_OK`|
|25 file read-at|handle, buffer, capacity, file offset,0|byte count or error|
|26 conditional replace|handle, data, byte count, info output, info capacity|`BOS_OK`|
|27 file close|handle,0,0,0,0|`BOS_OK`|

File metadata is32 bytes: structure size, handle, file size, granted flags,
opaque revision and three zero reserved words. Info capacities must be at least
32. `OPEN_READ=1`, `OPEN_WRITE=2`; `OPEN_CREATE=4` requires all three flags and
creates an absent empty `/Documents` file exclusively. A collision is CHANGED.
Replacement is an atomic RAM update, not durable success. Successful replacement
refreshes this handle's bound revision; another previously opened handle gets
CHANGED. See [versioned files](VERSIONED_FILES.md) for rename/move/delete/remount,
capacity and access semantics. Directory APIs are not present or advertised.

## Async progress, waiting and saving

Begin does not serialize a whole volume or wait for I/O. The coordinator keeps
filesystem tickets private, supports32 completion records /4 per process, joins
a compatible leased snapshot, and records each owner's result separately. Its
receipt covers mutations completed before begin; it does not promise that later
mutations are durable. A completed receipt remains readable until release.
Dropping one receipt never cancels the shared commit or consumes another owner's
result. See [owned sync](NATIVE_ASYNC_SYNC.md) for backend and remount details.

Wait0ms is a poll. Wait1–60000ms checks completion and, if pending, saves only the
handle and wrap-safe deadline, publishes the caller's pending canvas, and returns
the desktop its kernel stack. It resumes the user instruction after the syscall
when completion is observed or the rounded-up PIT deadline expires. Completion
wins when observed at the deadline. Timeout does not release the handle. Input
stays queued, and close/Stop remains available while waiting. This is bounded
cooperative service waiting, not a general event API or a real-time guarantee.

Existing apps need no source change. New source can use
`bos_sync_compatible(allow_legacy_blocking)` in a synchronous save routine:

- On a capable task/backend it requests then waits through normal scheduling.
- Passing1 explicitly permits old blocking syscall16 when the query is unsupported
  or the current context/backend does not offer asynchronous sync.
- Passing0 returns UNSUPPORTED in those cases.
- BUSY, CAPACITY, I/O and other failures **never silently fall back** to blocking
  sync. Timeout renews the caller's bounded wait; users can still close the task.

For an app that keeps editing during saving, use begin/poll/release directly and
retain private pending edits. These additions do not convert existing built-in
save callbacks or old binaries to asynchronous code automatically.

## Verification scope

`test_native_platform.py` runs ordinary dispatcher, query-prefix, context,
copy-range, timeout/wrap, file-routing and lifecycle scenarios with production
functions and deterministic service stubs under ASan/UBSan. It separately checks
frozen BEX1 fixture hashes/headers and the SDK fallback helper. It executes no
protected machine instructions, guest faults, fuzzing or malformed executables.
`test_native_publication_process.py` continues to exercise the exact old frame
publication rules. Real service correctness and guest/desktop responsiveness
need their own module and integration tests; these host checks do not substitute
for those gates.
