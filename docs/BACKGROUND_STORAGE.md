# Background IDE snapshots

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
use the owned [document adapter](RESPONSIVE_DOCUMENT_SAVES.md) on IDE. Format
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

QEMU checks cover REP sector transfers, default/large payload persistence,
reboots, native input and every reference audio sample. In a 128 MiB QEMU
benchmark, guarded rendering roughly halved the duration of heavy 7 MiB and
28 MiB snapshot workloads. Full-volume latency remains substantial. Snapshot
serial records contain timing/generation/result only, never file names or
payload bytes.
