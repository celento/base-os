# Incremental IDE snapshots

The filesystem offers a cooperative request/step/result API for default and
explicit large-profile IDE volumes. It preserves the existing two-snapshot
wire format, capacities, generation selection, CRC32 and durability barriers.
Explicit saves retain their blocking compatibility API. The floppy path and
mount/probe remain synchronous.

## Ownership and safe application work

A pending job exclusively leases the live node table, live file-data pool and
existing image staging arena from acceptance through verified completion or
failure. There is no third arena, copy-on-write layer, suspended kernel stack,
or application callback from filesystem/ATA polling. The image arena previously
used by aliased `fs_write` is the same arena used by the snapshot.

Every mutator checks `fs_sync_busy()` before its first allocation, metadata
change, alias staging copy, compaction or recursive child operation. Create,
mkdir, app-create, write, rename, move, copy, delete and empty-directory return
`FS_ERR_BUSY` (-2) with no filesystem or image side effects while leased.
`fs_init` and `fs_empty_dir` now return `int`; callers that previously discarded
their `void` result remain source-compatible. Initialization and remount reject
busy jobs before resetting state, probing media or changing arena ownership.

Reads, rendering, input handling, private Editor/Writer/Sheet buffers and native
compute may continue. Consumers must handle busy at the *start of their own
compound application operation* if it has effects outside the filesystem.
Borrowed `fs_data` pointers remain stable during the lease; ordinary filesystem
mutations after completion may move them as before. Code outside the filesystem
must not write either leased arena directly.

## Application feedback and retries

Writer and Spreadsheet check the lease before Save, Save As or Export changes
private save bookkeeping. Busy reports "Disk is saving; retry shortly." Existing
bindings, clean/dirty state and pending Spreadsheet cell edits stay intact, with
private editing and reading still available. Retrying a native owned save after
release keeps its identity; a retained PDF export still uses its identity/content
checked sync-only retry. This does not make GUI saves asynchronous: successful
Save continues to require the blocking API's verified durable completion.

Files Paste checks before consuming Cut or changing its completed-copy retry
guard. Busy returns its existing error result for an unstarted paste, or RAM-only
for a previously completed paste awaiting sync. It never allocates another copy.
The RAM-only duplicate helper preserves `FS_ERR_BUSY` for desktop callers.

Terminal only rejects filesystem-mutating `touch`, `mkdir` and `rm` commands;
the busy result also survives command-script dispatch. Reading, navigation,
manuals and native launch continue normally. The native SDK exposes the same -2
as `BOS_ERR_BUSY`, and the Counter, Notebook and DocStats sources explain that a
write should be retried shortly. These source updates do not overwrite installed
example binaries. Complete network downloads waiting for storage use the
Download manager's wait message instead of claiming body reception is ongoing.

## API and completion ownership

`FsSyncTicket` is an incarnation/serial pair. A request reserves the single
async completion record and returns zero only for acceptance. For IDE this
performs only fixed-size control initialization, with no serialization or I/O.
A clean request is immediately durable but still requires release. The floppy
request performs its legacy blocking save before returning; its ticket reports
the final result. Unavailable requests return -1, busy requests -2, and rejected
requests do not modify the supplied output ticket.

`fs_sync_result(ticket)` returns:

- `FS_SYNC_PENDING` (1) while that job is running
- 0 only after fully verified durability (or an already-clean request)
- -1 for a failed save, retaining dirty RAM
- `FS_SYNC_STALE` (-3) for an unowned, released or invalidated ticket

Explicit results remain until `fs_sync_release(ticket)`. Release rejects a
pending job with `FS_ERR_BUSY`, and rejects an old/foreign ticket as stale.
There is no cancellation or force replacement. Successful initialization or
remount changes the incarnation and invalidates previous tickets. Serials and
incarnations refuse allocation before wrapping; they never reuse an identity.

The three fixed completion records have separate roles:

1. The one async client, including internally owned autosaves.
2. The synchronous `fs_sync` wrapper or joiner.
3. The kernel-owned native completion coordinator. Its bounded per-process
   handles never expose the raw FS ticket; see [owned native durability](NATIVE_ASYNC_SYNC.md).

An active job records its subscribers. `fs_sync()` joining an async job gets
its own result before any autosave result is reaped. It does not consume an
explicit async client's result. Once the job finishes, that explicit result
may remain retained while later blocking saves use the synchronous record.
`fs_sync()` returns zero only after the requested/joined job's final validation;
it never returns queued success. A pending job is joined even if another caller
observes clean state. Reentrant use of the synchronous record fails busy.

`fs_autosync()` retains its existing dirty/read-only/failure-backoff admission
rules. On IDE it only requests work and owns/reaps its own record. On floppy it
blocks as before. Its completion updates the existing failure count and storage
status once. The desktop must call `fs_sync_step()` to make IDE progress.

## Step bounds and scheduling

Each IDE `fs_sync_step()` does one of these bounded units:

- Stage one fixed-size record, or at most 4,096 file-data bytes.
- Initialize the fixed-size validator, or perform at most 64 graph operations.
- CRC at most 4,096 bytes, with at most one sector of padding/header work.
- Submit one ATA request, or call `ata_poll(8)` once.

The validator covers record sizes/IDs/names/types, total capacity, unique IDs,
root shape, parent presence/directory type, maximum depth/path length, cycles
and duplicate sibling names. It shares its implementation with mounted-image
validation, which drains it synchronously. A graph operation visits at most two
40-byte records and fixed-length names; no unbounded byte scan hides inside it.

Progress is `FS_SYNC_IDLE`, `FS_SYNC_WAIT`, `FS_SYNC_MORE` or
`FS_SYNC_FINISHED`. MORE requests another scheduled quantum without an otherwise
unnecessary timer sleep. WAIT means the controller must change state; ordinary
device service and rendering/input continue. A desktop may pump several bounded
units per main-loop turn with a wall/PIT-time budget. Active ATA phases retain
the transport's two-second deadline, so the controller must continue to receive
polls. The blocking wrapper calls device-only `fs_background_poll()` between
quanta, preserving audio/network/input service without dispatching applications.

The production i386 `-Os` build measured 1,220 bytes for the job and 32 bytes for
the original two completion records, plus 12 bytes of counters/ownership flags: 1,264 bytes
of named control state. A compile-time assertion caps this at 1,536 bytes on
both 32-bit and host builds (the 64-bit host uses 1,276 bytes). Object BSS grows
1,280 bytes including compiler alignment. No profile arena constants change.
The `fs_sync_step` own stack frame is 144 bytes; its bounded validator helper is
160 bytes, and `fs_sync` is 48 bytes. These are compiler frame measurements,
not a claim that the whole caller/callee chain or interrupt stack is that size.
The native coordinator adds a third 16-byte result record, making the current
named i386 FS control state 1,280 bytes (1,292 on the 64-bit host), still below
the same 1,536-byte assertion. The synchronous mounted-image validator's own frame changes from 1,248 to 1,136
bytes; its new helper adds a separate bounded frame. Production `fs.o` text grows
3,604 bytes relative to 064d79b with the measured GCC flags.

## Commit ordering and errors

The order is unchanged:

1. Serialize and validate the entire immutable live snapshot; calculate CRC.
2. Write the inactive payload and complete its cache flush.
3. Read back that payload and verify its CRC.
4. Write the new header and complete its cache flush.
5. Read back and check the complete committed header and payload, CRC and graph.
6. Only then select the new active slot/generation and clear dirty state.

No submission, transfer completion, payload verification or header flush alone
constitutes success. Reads use the same leased staging image. A failed read may
have overwritten part of that image, but the live pool and nodes remain intact.
Before-header failures preserve the active old snapshot. Once header submission
is accepted, any failure or mismatching committed readback protects further disk
writes until a remount resolves which complete snapshot is newest. RAM may be
edited after a failed job, and the dirty bytes remain visible for recovery.

Read-only recovery, blank marked-volume migration, default/large file and node
limits, profile-specific arena selection and audio workspace ownership remain
unchanged. There is no automatic retry past the existing three-failure/backoff
policy and no disk format conversion.

## Verification

`PYTHONPATH=tests python3 -m unittest test_fs_sync` runs deterministic ordinary
host tests with ASan/UBSan. It checks the lease at every phase (including aliases,
recursive operations and attempted remount), stable reads, WAIT/MORE scheduling,
exact transport order, durable-only results, explicit retained results, joined
autosave reaping, immediate subsequent blocking saves, failed joins/retries,
serial/incarnation exhaustion, every request/poll error boundary, pre/post-header
readback mismatches, full 256-node default and large capacities, and untouched
lower arenas on large-profile commits. The transport adapter is bounded to the
caller's sector quota; real ATA register/deadline behavior is tested separately
in `ata_host.c`.

Existing CRC/zlib, mounted-image, default/large volume, node-capacity, clipboard,
Files view, Editor/session and related application host suites exercise blocking
compatibility against the same implementation. Actual responsive desktop/QEMU
integration is a separate verification layer; these core tests alone do not
claim responsiveness under a concurrent guest workload.

Application coverage: the `test_writer`, `test_writer_dialog`, `test_sheet_ui`,
`test_file_clipboard`, `test_storage_busy`, `test_native_launch` and
`test_native_arguments` unittest modules check ordinary busy/release/retry
flows, clean and dirty document bindings, uncommitted cell edits, pending owned
copies/PDFs, source identity replacement, honest native-example messages and
Terminal reads/scripts/native launch. These host tests do not claim a guest
responsiveness result.
