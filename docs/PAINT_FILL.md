# Paint Fill coverage and undo

Fill replaces exactly the four-way-connected region containing the clicked
pixel with the selected palette color. A one-pixel boundary blocks the fill;
diagonally touching pixels and neighboring row ends are separate. A blank
160 × 100 canvas fills all 16,000 pixels from any canvas position.

## Bounded implementation

The old depth-first fill queued neighbors before checking their colors and
marked a pixel only after popping it. Duplicate entries could saturate its
16,000-entry stack, causing the capacity checks to silently drop reachable
neighbors. An ordinary white canvas filled black from logical pixel `(60,48)`
left 2,147 white pixels in the original production function; this was a fill
coverage failure independent of mouse positioning or saving.

The replacement marks each matching pixel with the selected color when it is
queued. Since old and new colors differ, every pixel can enter the queue only
once. It processes at most 16,000 entries and checks at most four neighbors per
entry, with no allocation, recursion, or capacity-based dropping of work.

The fixed 16-bit index queue uses 32,000 bytes at Paint offset `0x4000` through
`0xBCFF`, inclusive. The canvas occupies offsets `0` through `0x3E7F`; the
unchanged Save staging starts at `0xC000`; undo history uses its separate arena.
Compile-time checks cover index width, queue/canvas separation, queue/staging
separation and the staging arena bound. Fill therefore does not overwrite
staged image bytes, even though these operations currently run serially.

One real fill records one ordinary history step. Clicking a pixel already in
the selected color does not change pixels, scratch storage or history, and
preserves any pending redo. Undo and redo restore the complete before/after
canvas. Existing history capacity and non-Fill tools are unchanged.

## Verification

```sh
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p test_paint_fill.py -v
```

The sanitizer-enabled host test extracts the production constants and current
Paint initialization, drawing, geometry, input, fill and undo helpers from
`src/kernel.c`, and links the real `src/history.c`. Only screen rendering and
palette RGB conversion use host seams; expected canvas bytes are specified
from ordinary drawings. No alternate flood-fill implementation, fuzz inputs
or intentional memory faults are used.

The 119 exact fill/no-op cases cover:

- Full canvas from the reported logical `(60,48)` position, center, four corners
  and four edge midpoints; black, white, regular palette and extended indices
- Nested outlined shapes, their interior, surrounding ring and exterior
- A one-pixel winding 8,050-pixel corridor, filled from both ends
- Separate large regions, isolated single pixels, diagonal contact and row ends
- Exact preservation of every boundary and unselected region
- One unique queued index per changed pixel, in-range indices, unchanged unused
  scratch bytes, canvas gaps, Save staging and arena guards
- One undo record per fill, exact undo/redo, eight-step history rollover,
  new-edit redo truncation and same-color clicks with pending redo

These are host checks of production logic and input helpers. The separate
ordinary-QEMU confirmation is described below.

On 2026-10-04 the focused command passed under ASan/UBSan, the complete kernel
built successfully, and the existing `tests/ui_guest.c` fixture compiled against
the changed source. A six-test ordinary-workflow group also passed:

```sh
ASAN_OPTIONS=detect_leaks=0 PYTHONPATH=tests python3 -m unittest \
  test_paint_fill test_paint_save test_storage_busy test_storage_pump \
  test_editor_binding -v
```

The general aggregate suite was stopped rather than used as completion evidence
because it includes unrelated legacy input-truncation probes.


## Separate ordinary-QEMU confirmation

On 2026-10-04 the normal production kernel at commit `ee0aaa9` passed a focused
Fill workflow in QEMU with disposable boot/data images. The runner reused the
original observed mouse position: screen `(400,300)`, logical canvas `(60,48)`.
It launched Paint through the normal app launcher, selected Fill with Tab,
clicked the blank canvas using normal mouse input, saved through Ctrl+S, used
Ctrl+Z/Ctrl+Y, and shut down through **System → Shutdown**.

After QEMU exited successfully, the host parsed the saved filesystem snapshot
and compared the entire 16,008-byte BOS1 file for each step:

- `full.pbm`: exact header and all 16,000 pixels black
- `undo.pbm`: exact header and all 16,000 pixels white
- `redo.pbm`: exact header and all 16,000 pixels black
- `noop-undo.pbm`: same-color Fill followed by Undo restored all-white pixels
- `noop-redo.pbm`: Redo after that restored all-black pixels

An independent root document stayed byte-for-byte unchanged. The run used only
keyboard/mouse input and framebuffer screenshots, with no guest-memory reads,
injected guest test code, debugger, fault cases or fuzzing. Screenshots confirmed
the selected Fill tool and complete before/after canvas; the exact-pixel claims
come from the saved files read after normal shutdown. The run exited normally
with no panic, and the selected durable snapshot had generation 9.

Tested kernel SHA-256:
`ebb5864b212ad8d9d06d965b30f13646c98f7106a011d94b0d72455c770e950a`.

Every all-black BOS1 output SHA-256:
`f8451c4288896f308a3abcb7072bca9c4c93a38db85c3a7e576313ff5895e41d`.

Every all-white BOS1 output SHA-256:
`4444a239a8ab798050e8e0e1b3584e01e90e2a7110806574d48b933fcdb1e71a`.
