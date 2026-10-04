# Stable native canvas publication

Native tasks draw into per-Terminal working pixels. The renderer reads a separate,
complete published frame and its own on/width/height metadata. Publication occurs
before explicit `present`, `yield`, `sleep` (including zero) and application
exit/return, including a nonzero exit status. A timer slice, external Stop, or
generic failure completion cannot publish. Text and lifecycle updates remain
independent. BASIC and legacy synchronous `exec` keep direct drawing behavior.

Eight maximum frames use 512,000 bytes in the explicitly asserted 512 KiB arena
at `0x600000`–`0x680000`. This is in the existing E820-validated Paint-to-disk-DMA
gap, with unchanged 64 MiB/default and 128 MiB/large requirements. Each Terminal
now uses 91,512 bytes; all eight still fit before the existing script scratch.
The bounded publication copy neither allocates nor polls/dispatches applications.
The existing kernel renderer is unchanged.

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
