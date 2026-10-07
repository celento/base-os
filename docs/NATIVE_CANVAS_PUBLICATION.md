# Stable native canvas publication

Native tasks draw into per-Terminal working pixels. The renderer reads a separate,
complete published frame and its own on/width/height metadata. Publication occurs
before explicit `present`, `yield`, `sleep` (including zero) and application
exit/return, including a nonzero exit status. A positive `bos_sync_wait`
(1–60,000 ms) also publishes pending drawing only when its operation is still
pending and the call actually suspends. Zero-time polling and immediate
completion/error do not publish; `bos_ui_wait` never publishes. A timer slice,
external Stop, or generic failure completion cannot publish. Text and lifecycle
updates remain independent. BASIC and legacy synchronous `exec` keep direct
drawing behavior.

Eight maximum frames use 512,000 bytes in the explicitly asserted 512 KiB arena
at `0x600000`–`0x680000`. This is in the existing E820-validated Paint-to-disk-DMA
gap, with unchanged 64 MiB/default and 128 MiB/large requirements. Each Terminal
used 91,512 bytes in the original publication implementation; with complete-input
overflow handling it measured 91,516 bytes (732,128 for eight).
These are historical footprints, before later binding/input fields. The current
structure is compile-time checked against the same fixed subarena before script
scratch. The bounded publication copy neither allocates nor polls/dispatches
applications. The original publication change kept the kernel renderer unchanged.

## Host verification

The following host suites run with ASan/UBSan:

- `test_native*.py`: 12 checks, including exact process syscall/state transitions,
  Terminal working/published bytes and geometry, all eight slots, reset/reuse,
  text-triggered rendering, Stop/close, synchronous compatibility, existing SDK,
  launch/arguments, and exact partial/full renderer equivalence.
- `test_kernel_layout.py`: 5 checks, including new arena and unchanged bounds.
- `test_foundation.py`: 14 checks, including reserved E820 exclusion and graphics.
- `test_storage_busy.py`: 2 checks; `test_audio.py`: 2 checks.

The process host test extracts production functions and replaces only privileged
interrupt-flag assembly around the unrelated sync call, which it never executes.
Generic failure completion is tested directly; no guest faults are induced.
The Terminal test uses the exact canvas draw helper; existing renderer equivalence
and the normal-input guest check cover full composition.

## Production desktop verification

`python3 tools/native_publication_input_test.py build [--profile large]` uses a
normal SDK app, disposable volumes, ordinary PS/2 input, and screenshots only.
The app clears its working frame, does ordinary CPU work across roughly two
seconds of PIT slices, then finishes the frame and explicitly publishes. During
that interval a text update plus opening/dismissing the launcher forces full
composition. Every sampled canvas must equal either the prior complete frame or
the next complete frame; incomplete pixels fail the check.

The check covers both default (64 MiB RAM/16 MiB disk) and large (128 MiB/64 MiB)
profiles and seven boundaries: present, yield, nonzero sleep, zero sleep, resize
up, resize down and explicit return 42. Each boundary must retain the old complete
frame in 15 sampled screenshots before switching. Ctrl+C during a 20-second
unfinished frame retains the old view; close/reused-slot launch resets it. The
test waits for unrelated UI such as the launcher to settle before sampling and
saves any failing screenshot. These are functional publication checks; they make
no performance or audio claim.

## Snapshot/audio workload

`tools/snapshot_responsiveness_test.py` with `tests/snapshot_responsive_app.c`
also exercises publication during a durable snapshot with a 45-second audio
source. This runner uses 128 MiB RAM for both volume profiles. It verifies all
file bytes after durable save and again after reboot, and compares decoded audio
against the existing PCM tolerance. `tools/check_snapshot_canvas.py` then checks
the complete 3,840-pixel footer against the decoded phase in the saved live and
reboot screenshots; no cleared or partial footer is accepted. This check reads
only saved screenshots and does not observe guest memory.

The later [conservative canvas occlusion](NATIVE_CANVAS_OCCLUSION.md) optimization
uses this same published-frame contract to suppress fully hidden canvas-only
repaints. Its host tests and remaining limits are documented separately.
