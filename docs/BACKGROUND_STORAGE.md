# Background IDE snapshots

This implementation was reconstructed after the 11:08:52 UTC workspace
replacement and verified for the hourly04 integration. It was not shipped in
hourly03. See
`docs/session-2026-10-04/RECOVERY.md` for old commit provenance and verification
boundaries; old test results do not automatically validate reconstructed source.

IDE snapshots retain the original dual-slot v4/v5 formats and full durability
ordering: serialize/validate, write inactive payload, flush, read/check payload,
write header, flush, and read/check the committed header and payload. Only final
success advances generation and clears dirty RAM. Uncertain commit outcomes
protect the disk until remount. Floppy-only synchronization remains synchronous.

Each step performs at most 4 KiB of CPU work, a bounded validation unit, or eight
ATA sectors. A top-level desktop turn has a 256-step hard cap and stops when a
PIT tick changes, including after device polling. Audio/network/input-device
polling never dispatches application input or filesystem mutations. Active ATA
waits remain polled because IRQ14 is disabled; an idle desktop otherwise uses
HLT. Snapshot leases prevent FS mutation while reads, private document edits and
native computation continue. Busy errors preserve app state for a retry.

Completion tickets retain terminal results until released, with a separate
reserved blocking-sync subscriber. Incarnation/serial wrap is rejected. Explicit
Editor/Paint/Files GUI saves, native bos_sync(), large RAM copies/compaction and
legacy execution remain synchronous; Writer/Spreadsheet native saves and exports
now use the owned [document adapter](RESPONSIVE_DOCUMENT_SAVES.md) on IDE. Format
encoding and atomic RAM replacement still happen before background durability; this is not a fully preemptive kernel. The additive
[native async service](NATIVE_ASYNC_SYNC.md) provides per-process completion
handles on IDE through an independent internal subscriber, reaped separately
from application result retention.

Complete downloads retain owned bodies during leases; their durability messages
track node identity, original path and non-wrapping runtime content revision.
Editor/Paint/Files report retry without losing private edits or names. Settings
coalesces choices and Todo retains accepted changes while closed; both participate
in shutdown preparation before session save and final disk synchronization. Calendar
also stages accepted appointments and its exact unfinished entry while closed;
its status distinguishes queued, filesystem-RAM and durable disk state. See
`docs/CALENDAR.md`.

Fresh QEMU checks verify REP sector transfers, default/large payload persistence,
reboots, native input and every reference audio sample. Guarded rendering reduced
one matched 7 MiB run from 61.24 s to 27.97 s and 28 MiB from 147.49 s to 75.53 s. These heavy
benchmarks used 128 MiB RAM for both disk profiles and are single-run observations.
The final stable-frame publication change repeated the default-disk workload at
29.286 s with complete frames and exact audio/data; no new large timing result is
claimed for that final change. Default 64 MiB and large 128 MiB staged frame tests
and packaged boots are distinct. Full-volume latency remains substantial.

See `docs/SNAPSHOT_RESPONSIVENESS.md` and the checkpoint evidence for exact source
hashes and test scope. Serial records contain timing/generation/result only,
never file names or payload bytes.
