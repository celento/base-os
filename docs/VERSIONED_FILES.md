# Version-bound native documents

The additive ABI file service binds an open document to one runtime content
version. Existing BEX1 headers, syscalls 0–17 and pathname APIs are unchanged.
Query `BOS_FEATURE_VERSIONED_FILES` before using the extended service; an older
kernel returns its legacy `-1` for the unknown query. The shared wire definitions
are in `sdk/baseos_abi.h`; the SDK supplies wrappers in `sdk/baseos.h`.

These calls retain their 32 KiB whole-buffer replacement limit. The separately
negotiated [ABI 1.3 transaction candidate](NATIVE_FILE_TRANSACTIONS.md) adds
private chunked upload and conditional RAM acceptance up to 256 KiB for desktop
IDE tasks, with separate content-durability confirmation. Standalone release
qualification is pending; no old file call or UI 1.1 behavior changes.

## Contract

- Open an ordinary file using an absolute printable ASCII path of 1–128 bytes.
  `READ` and `WRITE` rights are independent. Opening a directory or application
  shortcut is invalid. A read-only open may target any readable ordinary file.
- `READ|WRITE|CREATE` exclusively creates an empty file below the current
  `/Documents`. If anything already occupies the path, return `CHANGED` without
  touching it. Missing parent directories are not created. Creation requires a
  writable backend and available node, byte-budget, handle and version resources.
- A successful open copies 32-byte `BosFileInfo`: structure size, opaque handle,
  byte size, granted rights, opaque nonzero revision, and three zero reserved
  words. `CREATE` is an operation flag and is not retained among the rights.
  No node index, data pointer, timestamp or persistence-generation encoding is
  exposed. The revision is only an equality token.
- Read-at copies at most 4096 bytes. Offsets at/beyond EOF, including `UINT_MAX`,
  return zero; capacity zero is allowed. A short final read leaves the remainder
  untouched. Source files retain the backend limits: 16,383 bytes on floppy,
  2 MiB on default IDE, and 16 MiB on the explicit large IDE profile.
- Info, read and replace compare the mounted incarnation, node identity and
  content revision with the opened version. A changed, deleted, recreated or
  remounted target returns `CHANGED` with no copied bytes or metadata. Earlier
  successful read chunks are still old-version bytes; discard the assembled
  document and close/reopen before restarting after `CHANGED`. The service does
  not retain an old snapshot or silently advance an existing reader.
- Conditional replace takes at most 32,768 bytes, further bounded by the backend's
  per-file limit and free byte capacity. It either replaces every byte in RAM or
  leaves the document untouched. Success returns `OK` and copied current metadata,
  advances only the writer's handle to the newly committed revision, and leaves
  other handles pinned to their former version. Even writing identical bytes
  creates a fresh revision. Empty replacement is valid. A competing newer writer
  therefore causes `CHANGED` rather than a lost update.
- Close releases a handle even if its target has changed or been deleted. A
  closed handle, another owner's handle or an unknown handle returns `STALE`.
  The service never retains application buffers after a call.

The dispatcher validates the whole caller-supplied memory range before invoking
these functions. Structure output capacity, pointer validation and execution
context are specified by the shared ABI/SDK. Native file operations execute
serially in both supported native contexts; each synchronous `exec` invocation
has its own owner and the dispatcher releases it at completion. Desktop tasks
release handles on normal exit, stop, fault and owner-slot reuse.

## Rename, move and protection

A handle follows the opened node through rename/move if its content has not
changed. It does not follow the original pathname to a replacement node. Reads
continue after an allowed rename or move. Each write rechecks that the node is
currently below the directory currently named `/Documents` at the root. Moving
it elsewhere, or renaming that ancestor, cannot preserve write authority. Moving
it back into the current Documents tree restores the handle's write eligibility
if its version is unchanged.

A protected backend returns `PROTECTED` for write-open, create and replace.
Read-only opens and reads remain available. A snapshot lease returns `BUSY` for
create/replace with no side effects; existing handles can still read or close.
This stricter write service does not change legacy pathname writes or built-ins,
which can retain their existing RAM-only behavior on protected storage.

## RAM completion and durability

Successful replace means the complete new content is in RAM. It does not mean a
verified disk snapshot is durable. Request and observe the separate owned sync
operation when durability is needed. A failed/cancelled sync does not roll back
an already successful replacement. No snapshot format, disk layout or filesystem
capacity is changed by this service.

## Bounded resources and finite generations

There are 64 file records globally, at most 8 per owner. An open handle consumes
one record until close or owner cleanup, including after `CHANGED`. All limits
are queryable; readers can use more than one handle for the same document.

File handles draw from a monotonically increasing, nonzero 28-bit serial domain
with a service tag. Handles are opaque and never recycled, including after
service reinitialization. Exhaustion returns `CAPACITY` before creating a file
or publishing output. Closing handles frees records but does not restore the
serial budget. Owner tokens are independently generation-qualified.

Filesystem node identities and content revisions are nonwrapping 32-bit
counters across reinitialization/remount within this boot. The final nonzero
value is usable. Legacy allocation/write may continue after exhaustion, but its
new identity/revision is zero (unknown); the version-bound service refuses to
open an unknown version. Conditional writes preflight revision availability;
exclusive creation also preflights identity availability. Existing known-version
reads remain usable after allocation exhaustion, and existing file replacement
does not require a new node identity. Runtime mount incarnation uses the existing
nonwrapping counter and invalidates earlier handles on reconfiguration. Reboot
invalidates all application resources; no runtime token belongs on disk.

## Execution and error rules

The current kernel is single-CPU and app syscalls are serialized. File checks
and a bounded read copy cannot be interleaved with application mutation. Longer
FS copies may service devices only; that polling must not dispatch apps, remount,
or mutate the FS. This is not a multithreaded/SMP locking protocol. Introducing
kernel concurrency requires replacing this serialization assumption first.

All rejected operations leave output bytes/metadata untouched. Errors include:
`INVALID` for invalid parameters/rights or non-ordinary targets; `NOT_FOUND` for
unresolved paths/parents; `CAPACITY` for exhausted file slots, handles, identities,
revisions, nodes, bytes or the backend file limit; `BUSY` for a snapshot lease;
`PROTECTED` for denied write/read rights, confinement or a protected backend;
`STALE` for invalid owner/handle pairs; and `CHANGED` for version conflicts or an
exclusive-create collision. The new service advertises no directory enumeration,
directory creation, append, seek cursor, deletion, rename, retained snapshots,
large atomic replacement or general permission model.

## Ordinary functional evidence

Run:

```
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p test_native_files.py -v
```

The host tests execute the real filesystem and service with ASan/UBSan. They
cover two concurrent readers/writers, changes between chunks, competing and
identical replacements, untouched failed outputs, rename/move/current ancestry,
delete/recreate, remount, owner cleanup/reuse, snapshot busy/protected retry,
exact transfer/path/floppy limits, full normal node/byte/handle capacity, exact
complete 2 MiB and 16 MiB streams by two readers, and direct unit-level finite
counter boundaries. The host fixtures include existing filesystem scaffolding;
only the normal entry points described above are run. LeakSanitizer is disabled
because this environment runs under tracing; ASan/UBSan stay enabled.

These host checks do not establish guest dispatcher, scheduling, durable reboot
or old-binary compatibility results. Those require the separate integrated SDK,
owned-sync and guest validation. No fault-injection, memory-bug, fuzz or debugger
socket/pipe probe is included in this suite.
