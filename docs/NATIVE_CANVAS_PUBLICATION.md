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
used 91,512 bytes in the original hourly04 publication implementation; the
complete-input overflow checkpoint measured 91,516 bytes (732,128 for eight).
These are historical footprints, before later binding/input fields. The current
structure is compile-time checked against the same fixed subarena before script
scratch. The bounded publication copy neither allocates nor polls/dispatches
applications. The original publication change kept the kernel renderer unchanged.

## Host verification

ASan/UBSan checks on 2026-10-04 passed:

- `test_native*.py`: 12 checks, including exact process syscall/state transitions,
  Terminal working/published bytes and geometry, all eight slots, reset/reuse,
  text-triggered rendering, Stop/close, synchronous compatibility, existing SDK,
  launch/arguments, and exact partial/full renderer equivalence.
- `test_kernel_layout.py`: 5 checks, including new arena and unchanged bounds.
- `test_foundation.py`: 14 checks, including reserved E820 exclusion and graphics.
- `test_storage_busy.py`: 2 checks; `test_audio.py`: 2 checks.
- Production `make -j4` succeeded: 474,816 initialized bytes, 320,422 packed bytes.

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

Both default (64 MiB RAM/16 MiB disk) and large (128 MiB/64 MiB) passed all seven
boundaries: present, yield, nonzero sleep, zero sleep, resize up, resize down and
explicit return 42. Each boundary retained the old complete frame in 15 sampled
screenshots before switching. Ctrl+C during a 20-second unfinished frame retained
the old view; close/reused-slot launch reset it correctly. No panic occurred.

The first default attempt failed because its first screenshot raced the launcher's
dismissal. Its follow-up capture showed the complete old canvas; the test now
waits for the unrelated UI to settle and saves the actual failing screenshot.
That unsuccessful attempt is retained at
`/workspace/shared/baseos-test-tmp/baseos-native-publication-default-n1490hbf`.
It is not counted as a passed run.

Results and exact source/build hashes are in `session-2026-10-04/`:
`NATIVE_PUBLICATION_DEFAULT_RESULTS.json`, `NATIVE_PUBLICATION_LARGE_RESULTS.json`,
and `NATIVE_PUBLICATION_PROVENANCE.sha256`. The build was from `f3be79c` plus this
publication diff. Screenshot directories are recorded in each result. These are
functional publication checks; they make no new performance or audio claim.

## Unchanged snapshot/audio workload

After the functional runs, the unchanged `snapshot_responsiveness_test.py` and
`snapshot_responsive_app.c` passed the default-profile workload with the same
45-second audio source and verified original seed. This runner uses 128 MiB RAM
for both volume profiles; the independent default functional run above used the
standard 64 MiB. It verified all file bytes after durable save and again after
reboot. The snapshot took 2,050 PIT ticks (29.286 s); eight input-to-visible sample
upper bounds were 21.94–124.91 ms, and the mouse sample was 35.11 ms. These are
single-run observations, not guarantees or a claimed timing improvement.

Every one of the 3,972,096 source samples in 1,986,048 stereo frames matched the
existing PCM comparison tolerance (peak error 1; 45.035 s; zero leading frames).
`tools/check_snapshot_canvas.py` then checked the complete 3,840-pixel footer
against the decoded phase in all 15 saved live screenshots and both reboot
screenshots. No cleared or partial footer was present. This check reads only saved
screenshots and does not observe guest memory.

The first snapshot invocation stopped before launching QEMU because its supplied
seed path did not exist; the corrected invocation reused
`/workspace/shared/snapshot-rep-default-attempt1/seed.img`. Full evidence is
`/workspace/shared/snapshot-publication-f3be79c-attempt2`; repository summaries
are `NATIVE_PUBLICATION_SNAPSHOT_RESULTS.json` and
`NATIVE_PUBLICATION_FOOTER_RESULTS.json` beside the other session results.
The snapshot result records the source commit `3bca6a7`; the exact tested kernel
still carries its earlier `f3be79c +changes` build label and matches the frozen
source/build hashes. No production code changed after that build.

![Complete frame during snapshot keyboard input](../screenshots/native-publication-snapshot-echo.png)
![Last complete frame retained after Ctrl+C](../screenshots/native-publication-stopped.png)
![Published 320×200 canvas on the large profile](../screenshots/native-publication-large-resized.png)

The later [conservative canvas occlusion](NATIVE_CANVAS_OCCLUSION.md) optimization
uses this same published-frame contract to suppress fully hidden canvas-only
repaints. Its host evidence and remaining production-verification limits are
documented separately.
