# Responsive built-in document persistence

Writer and Spreadsheet use the existing native durability coordinator without
changing its protocol, 32-result/4-per-owner limits, FS subscribers or public ABI.
The central private kernel owner allocator issues `0xF0000000 | serial`, with a
nonreused 28-bit serial and explicit exhaustion. Window taskbar sequence is only
a UI association, never a document durability owner.

## Publication, completion and revisions

Each singleton holds one 52-byte value-only operation record and one bounded
failed-export descriptor (Writer 72 bytes, Sheet 64). No output, model, history,
dialog or filesystem pointer survives a call. Existing fixed output arenas are
reused. Encoding and atomic RAM replacement remain synchronous; only IDE
snapshot durability is removed from the interaction path. No specific latency
is guaranteed.

Native RAM publication updates the binding fingerprint immediately. Its dirty
state stays set until owned success matches the exact mount incarnation,
nonzero node identity and content revision, file size, native type and binding.
Completion advances the durable baseline to the **submitted** model revision,
never current edits. Both revision components are compared in history, typing
groups, saved/submitted/export metadata and Writer layout/statistics caches.
Epoch/counter identities never repeat; terminal exhaustion refuses allocation
before model/history mutation. Unknown FS version tokens never authorize a clean
binding or successful completion.

A shared top-level hook immediately after storage service collects both models,
then checks guarded desktop continuations. No IRQ/device/syscall callback applies
model changes. A duplicate native Save returns explicit pending and neither
rewrites nor silently queues newer edits. Other active work/leases reject before
committing the Sheet editor or mutating FS. Capacity/protected/I/O errors retain
complete RAM output and private edits; no IDE error invokes blocking fallback.
The explicit unsupported-backend branch preserves blocking floppy compatibility.

## Desktop and export behavior

Native results are error -1, needs-name 0, durable 1 and pending 2. Export results
are error -1, pending -2 or a nonnegative durable file ID. No BOS result is passed
through the old boolean save dispatcher.

Accepted Save As/export hides its name dialog without calling cancellation.
Normal pending work allows editing and other windows. Save-before-Close/New/Open
holds exact owner/handle, window slot/sequence/kind and requested Open target
mount/identity/version. It executes only after matching success and a fresh dirty
check. Newer work is re-prompted. Cancel discards navigation only; the immutable
snapshot remains in progress. Late failures leave the document open.

RTF, PDF and CSV never rebind or clean the native model. A failed export retains
its complete RAM file, even zero-byte CSV. An owned retry verifies requested
parent/name, mount, node, content version, full source revision, PDF paper and
all deterministically regenerated output bytes. It is sync-only. One newer
complete export can replace retry authority without deleting the older output.
Native save does not consume a failed export descriptor. New/Open/Restore/Close
release interests and retry authority without cancelling storage or deleting
complete outputs. Reopened export dialogs prefill only a currently usable owned
failed output.

## Recovery and shutdown

Recovery snapshots remain borrowed buffers, never adapter storage. Session
saving still skips an active snapshot lease. New edits during pending save are
private RAM until a later recovery snapshot is installed and synchronized.
Shutdown still joins an active boundary, collects model results without executing
navigation, stages the latest session draft, then performs final blocking sync.
Failure keeps the desktop and drafts available. Existing recovery wire formats
are unchanged. Writer's source-only sidecar pairing limitation is not addressed here.

## Open/reopen boundary

Successful native Open retains the existing RAM-filesystem semantics: it starts
clean relative to the current RAM file bytes. If a user explicitly discards or
closes a model with a RAM-only replacement and reopens that file, the new lifetime
does not inherit the old model's completion receipt. Its initial clean state is
not a new per-file durability proof. There is no filesystem-wide
durable-version tracking across reopen. The strict owned-completion rule applies
to Save/Save As within their logical document lifetime. Complete RAM files remain
available and shutdown still refuses a failed final synchronization.

## Verification scope

Ordinary sanitized host suites cover the real adapter/coordinator/FS, both real
models on supported default and large disks, maximum native model sizes, exact
extracted desktop continuation/name-dialog functions, failed-output retries,
finite identity/revision boundaries, stale targets and recovery/shutdown ordering.
The existing compatibility suites continue to exercise explicit floppy-style
synchronous behavior. Native file formats and model limits remain unchanged.

Writer's fixed arena is 1,999,164 bytes and Spreadsheet's is 2,844,936, both
within their existing reservations. No output-sized allocation is added.
Per-function stack reports must be distinguished from cumulative call-chain
stack usage.

Guest testing uses ordinary QEMU serial/QMP/PS2 input: private edits and another
window stay responsive while the matching durable boundary is pending, stopped
disk output is checked exactly, and documents are checked after reboot on
default/large IDE plus explicit floppy compatibility. On both IDE profiles,
submitted native bytes are distinguished from recovered newer drafts. Host
success alone does not establish guest responsiveness or reboot durability.
