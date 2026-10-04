# Resumable ATA transport

The primary-master ATA PIO driver has one controller-owned request. Its
nonblocking interface lets the desktop advance a snapshot without spinning on
BSY, DRQ or FLUSH CACHE. This transport change does not itself make filesystem
serialization, explicit saves or floppy operations asynchronous.

The existing hardware and disk contract remains unchanged:

- Primary channel, master device; 28-bit LBA and 512-byte logical sectors.
- At most 128 sectors in each READ/WRITE SECTORS command.
- Alternate-status polling and four fixed settling reads after selection,
  commands and sector transfers; nIEN remains set.
- No automatic cache flushes, command reordering or disk-format changes.
- An error or phase timeout protects the controller until explicit probe/remount.
- No memory arena, RAM profile, interrupt-mode or native-task ABI changes.

## Interface and ownership

`ata_request_read(lba, buffer, sectors)`,
`ata_request_write(lba, buffer, sectors)` and `ata_request_flush()` return zero
only when the request was accepted. Submission does no port I/O. `ata_poll(n)`
returns the current progress and moves at most `n` complete sectors:

| Result | Meaning | Scheduling |
| --- | --- | --- |
| `ATA_PROGRESS_IDLE` | No accepted request since initialization/probe | No storage work |
| `ATA_PROGRESS_WAIT` | Device must change status or deadline must expire | May sleep until the next ordinary interrupt; keep polling regularly |
| `ATA_PROGRESS_MORE` | Ready work remains, but the sector budget is exhausted | Continue ordinary desktop work and schedule another step without waiting for a timer tick |
| `ATA_PROGRESS_DONE` | All requested commands completed | Release the borrowed buffer and advance the caller's protocol |
| `ATA_PROGRESS_ERROR` | Transport failed; controller is protected | Release the borrowed buffer and report failure |

There is one cooperative owner. A pending request rejects every other request,
including zero-sector requests, synchronous wrappers and `ata_probe()`. These
rejections leave the original buffer, command state, deadline and result intact;
an active-request probe performs no hardware I/O. There is no cancellation or
force-replacement operation. Callers must retain the transfer buffer until DONE
or ERROR, and must not modify its bytes while the transport owns it. A read may
modify only a prefix before an error; an error does not roll back disk writes.

Terminal results persist until another valid request is accepted or an idle
explicit probe begins. Invalid arguments do not erase a terminal result.
`ata_request_active()` reports transport ownership. `ata_ready()` means a
successfully probed, usable controller, including a controller temporarily
waiting for hardware; it does not promise that a sector can transfer now.
`ata_sector_count()` remains the last discovered capacity after a transport
failure, while `ata_ready()` is false.

Zero-sector reads/writes are accepted as immediately DONE if the controller is
usable and idle and the LBA is in `[0, capacity]`; their buffer may be NULL. A
negative count, out-of-range interval or nonzero transfer with NULL buffer is
rejected. `ata_poll(0)` may check status, issue commands and finish a flush; it
never transfers a data word. It returns MORE for a ready data phase, so a caller
must eventually provide a positive sector budget.

## Work and deadline bounds

The request stores an explicit idle/data/command-completion phase, current LBA,
remaining sector counts, a borrowed buffer cursor, direction and phase start
tick. Each polling-loop iteration does one of three things:

1. Advances a protocol phase or issues one command.
2. Transfers exactly one sector (256 fixed PIO word operations).
3. Returns immediately after its status observation.

An unchanged busy/missing-DRQ/unexpected-DRQ state is never polled repeatedly in
one call. The fixed 400 ns settling reads are not readiness loops. The caller's
sector quota bounds data movement; command-transition overhead is bounded by
those sectors and the 128-sector command boundaries. For example, a new
`ata_poll(8)` can transfer up to eight sectors while an already stalled request
uses only one alternate-status read. Zero-budget calls still have bounded
control overhead.

Each protocol phase has a two-second deadline based on `timer_ticks()`.
Submission, command issuance, a completed sector transfer and transition to the
next command begin new phases. Merely polling, reaching a quota, rejecting an
overlap or observing hardware waiting never restarts the deadline. Unsigned
elapsed-tick subtraction handles normal timer wraparound. Timeout is checked
only when the observed status requires a hardware wait: BSY, missing data DRQ,
or unexpected continuing DRQ in an idle/completion phase. Ready data or command
completion remains valid after a longer caller scheduling gap, including legacy
application execution. A zero-budget poll of a ready data phase still returns
MORE without resetting the deadline; if a later observation requires a hardware
wait after that deadline, it fails. A genuine timeout remains terminal even if
the hardware later becomes ready. Poll regularly to detect stalls promptly;
interrupt-driven timer service must remain available to the caller.

`ata_poll` never calls `platform_poll`, dispatches application work, allocates,
sleeps or changes interrupt enablement. The synchronous compatibility wrappers
use the same request machine with a one-sector quota, calling `platform_poll`
while waiting and after each transferred sector, including the final one.
`ata_probe()` remains a blocking boot/remount operation with the original bounded
IDENTIFY waits. No application can probe through an active request.

## Completion and durability

Read/write DONE includes command completion after the last data sector. Merely
finishing data transfer is insufficient: BSY or lingering DRQ keeps the request
pending. WRITE SECTORS completion retains its previous meaning; it is not a
FLUSH CACHE barrier.

A flush waits for idle, issues exactly one FLUSH CACHE command and waits for
its completion before DONE. The caller must retain the existing snapshot
protocol: payload write, completed payload flush, payload readback/verification,
header write, completed header flush, and complete committed-snapshot readback.
It must advance its durable generation only after the entire protocol succeeds.
No following request can bypass an outstanding write or flush.

ERR, DF, absent-bus status and timeout terminate the request and clear readiness.
ERR/DF are ignored while BSY is asserted, as before. A failed pre-command flush
wait now also protects the controller, consistently with all other uncertain
transport failures. Validation and overlap rejections do not protect a healthy
controller. Only an explicit successful probe restores its usability; the driver
does not silently retry or remount during a filesystem commit.

## Verification

The deterministic `tests/ata_host.c` fixture runs the actual driver against an
ordinary ATA status/port model. Its sanitizer-enabled host test covers:

- Existing synchronous read/write/flush semantics and per-sector device service.
- Exact 128/128/1 chunking and byte contents for 257-sector reads and writes.
- Budgets 0, 1, 2, 7, 8 and 128, command-boundary continuation and fixed settling
  overhead; no platform callbacks inside asynchronous polls.
- Borrowed-buffer ownership, rejected overlaps/probes/synchronous calls,
  invalid arguments, zero-length requests and sticky terminal results.
- Delayed command readiness, between-sector BSY, delayed final completion and
  delayed flush completion, with write/flush/read command ordering.
- Eight stalled protocol states, repeated polls at the same tick, exact timeout
  boundaries, rejected requests not refreshing deadlines, progress refreshing
  deadlines and unsigned tick wraparound.
- Late polls of ready idle/data/command/flush phases, zero-budget ready polls
  beyond the deadline, genuine unready timeouts and sticky failure after hardware
  subsequently becomes ready.
- Zero/FF bus status, ERR and DF before commands and during data/completion/flush
  phases; BSY with ERR/DF; protection until remount.
- Absent disks, unsupported IDENTIFY flags/signatures, non-512 logical sectors,
  zero/too-large capacities, and LBA28's final address and master-selection nibble.

Run it with:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p test_foundation.py -k test_ata -v
make -j3 build/kernel.bin
```

On 2026-10-04 the focused host test passed with ASan/UBSan; the 14-test foundation
suite also passed, and the production kernel linked successfully. With GCC
14.2.0 and the production i386 flags, `ata.o` grows from 1,163 to 2,080 bytes of
text, and from 8 to 40 bytes of BSS. The request itself is 32 bytes on i386, has
no allocated buffer, and is guarded by a fixed-size compile-time assertion.

No QEMU result or desktop-latency improvement is claimed by this isolated driver
check. Integrated filesystem/main-loop validation must establish responsiveness,
exact flush ordering across snapshot phases, rebooted contents and both supported
RAM profiles. No saved disk images are needed by the host tests or kernel build.
