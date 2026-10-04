# Spreadsheet

The bounded Spreadsheet client is implemented in `src/sheet.[ch]`. It uses the
real formula engine and file codecs described in [SHEET_MODEL.md](SHEET_MODEL.md).
The desktop owns its window, launcher, filename dialogs, Save/Discard/Cancel
prompts and recovery sidecars. This guide describes the client module; its host
checks do not claim QEMU or desktop integration has been completed.

## Grid and editing

- One sheet, **26 columns × 128 rows**, **A1 through Z128**.
- Default client size **720 × 520**, minimum **420 × 260**. All drawing is clipped
  to the client and each cell; ordinary text never spills into adjacent cells.
- Click a cell to select it. Shift-click, Shift+arrows or a mouse drag selects a
  rectangular range. Click a column/row header to select that whole column/row;
  the top-left corner or Ctrl+A selects the entire grid. Escape collapses a range.
- The address field shows the active cell. Click it, choose **Go to**, or press
  **Ctrl+G**; enter a complete A1 address and press Enter. Invalid addresses keep
  the field open and leave the selection unchanged.
- Typing begins a replacement for the active cell. **F2**, **Edit**, a double
  click, or clicking the formula bar edits its exact source instead. **Enter**
  commits and moves down; **Tab** commits and moves right. Shift reverses either
  movement. **Escape** discards the pending edit. Clicking another cell commits
  first. The formula bar supports arrows, Home/End, Shift selection, Ctrl+A,
  Backspace/Delete and text copy/cut/paste.
- Arrow keys move one cell. Page Up/Down moves a viewport. Home/End moves to
  column A/Z; Ctrl+Home/End moves to A1/Z128. Ctrl+arrows moves to that grid edge.
  Keyboard navigation and address jumps reveal the active cell. Scrollbars and
  the mouse wheel can explore without moving the selection; resizing reveals
  the current active cell.
- Delete or Backspace in the grid clears the selected range in one operation.
  Ctrl+Z undoes; Ctrl+Y or Ctrl+Shift+Z redoes. There are **four undo operations**,
  grouped by committed cell/range edits. A new edit discards the redo branch.
  Ctrl+Z while a cell/address edit is open cancels that pending edit first.

Each cell holds at most **95 ASCII bytes**. Over-limit typing/paste is rejected
visibly, never silently shortened. Imported embedded TAB/CR/LF remain inside
that cell and are shown as `?` in the grid/formula bar; exact bytes are retained
by F2 editing, native files and quoted clipboard interchange.

Numbers have **three fixed decimal places** and a checked range of
**−2,147,483.648 through +2,147,483.647**. Trailing source zeroes are retained in
native files even when the displayed value omits them. Numeric/formula values
are right aligned; text is left aligned. Formula errors are visible in red:
`#SYNTAX!`, `#REF!`, `#DIV/0!`, `#OVERFLOW!`, `#PRECISION!`, `#VALUE!`, `#CYCLE!`
and `#DEPTH!`. Saving a formula with an error is allowed; it remains editable.

Typing `=A1*2` creates a formula; `'=A1*2` creates literal text without the first
apostrophe. Typing complete plain decimal syntax creates a number; other input
is text. **F2 on an existing TEXT cell preserves its literal kind**, including
numeric-looking/formula-looking text. To replace its kind, leave the editor and
start typing a new value. SUM, AVG, MIN, MAX and COUNT support rectangular ranges;
see [the formula language](SHEET_MODEL.md#formula-language) for exact coercion,
precision, nesting and reference semantics. Values always come from that engine.

## Clipboard

Ctrl+C copies a selection and Ctrl+X copies then clears it. A failed publication
to the shared clipboard prevents Cut and preserves the previous private copy.
Ctrl+V pastes starting at the **active cell shown in the address field**. It does
not repeat a value to fill a selected rectangle. A successful range paste selects
its resulting rectangle and is one undo step.

Self-owned copies preserve exact source text, number spelling, formulas, EMPTY
versus explicit empty TEXT, and cell kinds. **Relative formula references are not
rewritten**: copying `=A1*2` anywhere leaves the exact formula `=A1*2`. There are
no absolute `$` references or fill-handle semantics.

The shared text representation is **TSV of calculated display values**. Fields
containing TAB, CR, LF or quotes are quoted, and embedded quotes are doubled.
When another app owns the clipboard, complete decimals import as NUMBER; all
other nonempty fields, including `=...` and apostrophes, import as literal TEXT.
Quoted TSV supports embedded line breaks inside one cell. Empty fields clear a
cell; a final row terminator adds no extra row. Ragged rows affect only the fields
actually present. Clipboard text is at most **65,535 bytes**, and the separate
private kind/source record is bounded to **65,536 bytes**; oversized selections,
cells and pastes extending beyond Z128 are rejected atomically. Select a smaller
range when either copy representation is too large. An unavailable shared
clipboard never falls back to stale private data.

## Native files and CSV

- **Save / Ctrl+S** saves lossless native **`.bsh`**. **Save As / Ctrl+Shift+S**
  uses a new native filename. Existing unrelated names are rejected. Saving back
  to the bound native name requires the original filesystem identity and exact
  cached baseline size/FNV-1a/CRC-32 fingerprints to match its current bytes.
  These are accidental-change detectors, not cryptographic authentication.
- Opening a `.bsh` validates the complete file before replacing the current
  document. Opening a `.csv` imports an editable, dirty **unbound copy**. Save
  then requires a new `.bsh`, so the CSV source is never overwritten.
- **CSV / Ctrl+Shift+E** creates a new **`.csv` value export**. It preserves the
  native binding and dirty status; calculated values replace formula sources.
  CSV cannot retain exact text/number kinds or distinguish explicit empty text.
  **Formula-like literal text is exported verbatim and another spreadsheet
  program may execute it.** BaseOS CSV import keeps it literal. CSV quoting
  escapes delimiters; it is not a formula-neutralization mechanism.
- The filesystem write must accept the complete encoded file, and `fs_sync()`
  must succeed before native work is marked saved. Failed writes/syncs keep
  work open and dirty. After a successful RAM write but failed disk sync, the
  source baseline updates to those pending RAM bytes so Save can retry safely.
  Failed export sync leaves the new export pending in memory and reports this;
  native dirty state/binding are unchanged.
- A malformed import or recovery snapshot leaves the entire existing sheet,
  selection and pending editor intact. The desktop must guard new/open/close
  using its normal Save/Discard/Cancel workflow.

## Integration and memory

All public client functions are prefixed `spreadsheet_` to avoid colliding with
`sheet_init(SheetDoc *)`. Request bits (`CHANGED=1`, `SAVE=2`, `SAVE_AS=4`,
`EXPORT=8`) let the desktop manage dialogs. Header `sheet.h` documents the full
API and borrowed buffer lifetimes.

The fixed arena starts at **0x720000**, after SB16 DMA, with **0x2E0000 bytes**
reserved before `DESK_CACHE`. Its exact **2,824,636-byte** footprint is asserted:

- Five document snapshots, each including selection and revision metadata
- One staging model
- 642,432-byte maximum CSV/native output scratch
- 65,536-byte private clipboard record

Large documents are never in kernel BSS or on the C stack. Transfers service
`platform_poll()` every 4 KiB; the real model and codec also poll bounded work.
Small mutable runtime state remains in BSS. No poll callback may mutate the
sheet, filesystem or clipboard, or reenter a document operation.

Shared `spreadsheet_clipboard_set/get` hooks may delegate to the desktop Writer
hooks. Set must accept the entire string and return a nonzero changed generation,
or return zero with the previous clipboard unchanged. Get returns the complete
length or −1 if unavailable/too large. The weak standalone hooks implement a
local-only private clipboard using an internal −2 get result; real desktop hooks
must not use that result.

Recovery snapshots encode a pending cell edit into a temporary candidate **without
committing it, disturbing the editor, or changing undo history**. Source bindings
still refer to the saved baseline, not the current dirty draft. Read-only getters
and source fingerprint checks do not invalidate snapshot output. Before restore,
the desktop must verify its persisted source baseline fingerprint; otherwise it
must pass file `-1` and identity `0`, leaving the recovered draft unbound/dirty.
The recovery view stores active/anchor row-major indices, not pixel positions.

## Verification

```sh
PATH=/workspace/shared/baseos-tools/bin:$PATH \
TMPDIR=/workspace/shared/baseos-test-tmp ASAN_OPTIONS=detect_leaks=0 \
  python3 -m unittest discover -s tests -p 'test_sheet_*.py' -v
```

The focused UI harness uses real `gfx.c`, font rendering and ordinary host
filesystem/clipboard stubs under AddressSanitizer and UndefinedBehaviorSanitizer.
It covers staged edits/cancel, literal kinds, formula recalculation, four-step
history/branching, navigation and address validation, mouse ranges, exact/private
and quoted/external TSV, failed copy/Cut publication and retained old private
records, maximum-cell/copy limits, native files, safe CSV import/export, source
identity and same-ID/same-size external changes, failed writes/syncs, source
binding fingerprints, non-disruptive recovery and aliased snapshot restore.
Every pixel outside normal/minimum/tiny/offscreen client rectangles is checked
unchanged. Python independently verifies saved BSH1 records and exported CSV.
No intentional memory-fault probes, fuzzing or QEMU instances are used here.

A freestanding i386 build with production flags also checks unresolved symbols,
object BSS and stack usage. Guest window/menu integration and real disk/session
recovery testing remain the desktop integration owner's verification work.
