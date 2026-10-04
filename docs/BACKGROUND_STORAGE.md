# Background IDE snapshot candidate

This separate integration branch is reconstructed after the 11:08:52 UTC
workspace replacement. It is not the hourly03 shipped runtime. See
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
GUI saves, native bos_sync(), large RAM copies/compaction and legacy execution
remain synchronous; this is not a fully preemptive kernel.

Complete downloads retain owned bodies during leases; their durability messages
track node identity, original path and non-wrapping runtime content revision.
Editor/Paint/Files report retry without losing private edits or names. Settings
coalesces choices and Todo retains accepted changes while closed; both participate
in shutdown preparation before session save and final disk synchronization.

The REP INSW/OUTSW sector transfer candidate still needs new real-QEMU validation.
Pre-reset 7 MiB concurrent save duration was 69.14 seconds despite responsive
input and correct PCM. Throughput remains open. Large-profile responsiveness was
not established. Serial records contain timing/generation/result only, not file
names or payload bytes.
