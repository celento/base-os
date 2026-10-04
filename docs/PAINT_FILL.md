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

These are host checks of production logic and input helpers. They do not claim
a new QEMU visual result.

On 2026-10-04 the focused command passed under ASan/UBSan, the complete kernel
built successfully, and the existing `tests/ui_guest.c` fixture compiled against
the changed source. A six-test ordinary-workflow group also passed:

```sh
ASAN_OPTIONS=detect_leaks=0 PYTHONPATH=tests python3 -m unittest \
  test_paint_fill test_paint_save test_storage_busy test_storage_pump \
  test_editor_binding -v
```

The general aggregate suite was stopped rather than used as completion evidence
because it includes unrelated legacy input-truncation probes. No QEMU run was
performed for this host-only checkpoint.
