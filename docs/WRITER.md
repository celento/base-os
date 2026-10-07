# Writer: bounded formatted documents

Writer is a separate, single-instance native desktop app. The plain-text Editor
remains available. Writer stores up to **32,768 ASCII text bytes**, with bold,
italic, underline, heading/body paragraph styles and left/center/right alignment.
Tabs and line breaks are supported. There is no Unicode, DOCX, general RTF import,
image embedding, paginated editing or guest printing. RTF and PDF are export formats. PDF files can be viewed or printed on another
computer; there is no guest PDF viewer or printer driver.

## Editing

The toolbar offers B / I / U, Body / Heading, paragraph alignment, Undo / Redo,
Save, RTF and PDF export. Character buttons affect selected text or subsequent typing.
Paragraph buttons affect every touched paragraph; a selection ending exactly at
the next paragraph's start does not include that next paragraph. Mixed selected
character styles are turned on consistently by the corresponding button.

- Ctrl+B / Ctrl+I / Ctrl+U: bold / italic / underline
- Ctrl+1 / Ctrl+0: heading / body
- Ctrl+L / Ctrl+E / Ctrl+R: left / center / right
- Ctrl+F / Ctrl+H: find / replace; F3 / Shift+F3: next / previous
- Ctrl+A / Ctrl+C / Ctrl+X / Ctrl+V: select all / copy / cut / paste
- Ctrl+Z / Ctrl+Y or Ctrl+Shift+Z: undo / redo
- Ctrl+S / Ctrl+Shift+S: Save / Save As
- Ctrl+Shift+E: Export RTF
- Ctrl+Shift+P: Export PDF; choose Letter or A4 in the name dialog
- Arrows, Home/End, Page Up/Page Down: visual navigation
- Ctrl+Left/Right: word navigation; Ctrl+Home/End: document boundaries
- Shift with navigation, Shift-click, or mouse drag: extend the selection
- Mouse wheel: scroll; click the scrollbar to reposition the view

Wrapping uses the proportional UI font advances, not a character grid. Body text
uses the bundled 95-glyph ASCII UI font. Headings use a 1.5x rendering of those
same glyphs with fixed-point area-coverage resampling, so arbitrary ASCII headings work without relying on the limited
Logo font. Bold and italic are bounded synthetic treatments. Word wrapping,
alignment, scaled advances, selection and caret hit testing share the same line
geometry. A caret at a soft line ending can stay visually on that line.

History retains eight complete prior operations plus the current state. Adjacent
typing, Backspace and Delete coalesce into separate runs of up to 512 keystrokes,
ending on a one-second pause, navigation/selection, formatting, paste, save or
paragraph break. Undo
restores text, formatting, caret and selection. A new edit after undo discards
redo. History is in RAM; it is not serialized into a document or recovery draft.
Undo back to a successfully saved revision removes the unsaved marker.

## Find and replace

The Find toolbar button or Ctrl+F opens a one-line query bar. Ctrl+H adds a
replacement field. Enter/F3 finds the next match; Shift+Enter/F3 searches backward,
and both wrap through the document. Aa toggles ASCII case sensitivity. Tab moves
between fields; Ctrl+A/C/X/V and Shift+arrows work within the focused field.
Escape closes the bar without changing document text. Save, Save As, RTF/PDF export
and document undo/redo remain available while a search field is focused.

Replace or Ctrl+Enter replaces the selected match and selects the following
match when one exists. All or Ctrl+Shift+Enter replaces every non-overlapping
match as **one undo operation**. Replacement text inherits each match's first
character style; all untouched text and paragraph properties are preserved.
Fields accept up to 63 printable ASCII characters; multiline queries and
replacements are not supported. Overlong or unsupported clipboard input is
rejected without changing a field. Replace All preflights the full resulting
length and changes nothing when the document limit would be exceeded.

The status bar reports whitespace-delimited word count and text byte count.
These counts refer to the entire document, not only the current selection.

## Import, save and export

Open `.bwr` files as native documents. Plain-text import accepts printable ASCII,
tabs, LF and CRLF. **CRLF pairs are normalized to LF in the editable copy**; the
source file is never changed. Lone CR, NUL, unsupported controls, non-ASCII bytes,
and normalized text over 32,768 bytes reject the complete import. Raw CRLF input
may be up to 65,536 bytes. Imported plain text is unbound and needs Save As a new
`.bwr` file, so formatting can never silently overwrite the source `.txt`.

The native v1 format preserves all supported text and styles exactly. The entire
input is validated before replacing current work. Unsupported version, malformed
length, invalid character/paragraph properties and trailing bytes reject the
whole open. See [WRITER_FORMAT.md](WRITER_FORMAT.md) for the wire format and RTF
references.

Native Save As requires `.bwr`; RTF and PDF export require `.rtf` and `.pdf`. Existing unrelated names
are rejected. Ordinary Save checks the original filesystem identity, including
after a `.bwr` rename: a deleted file's reused node ID is never overwritten.
Renaming away from `.bwr` requires Save As. A clean
bound document whose file disappears is treated as unsaved and needs Save As.
Ordinary Save also compares the source's exact byte count, FNV-1a and CRC32
fingerprints with the baseline captured at Open or the previous RAM write. An
externally changed same-ID file requires a new Save As; no source bytes change.
IDE saves publish the complete RAM replacement, then return while an owned
background durability boundary runs. Only its verified successful completion
marks the submitted revision saved. Newer edits and pending RAM-only saves remain
unsaved, including when undo returns to an older saved revision. Floppy-only
saves still wait through the explicit compatibility path. Failed writes or
synchronization keep the document open. Only a newly created incomplete empty
placeholder is removed; a complete RAM replacement remains bound for safe retry.
Serialized size is checked against the mounted volume's per-file limit before
creating a new target. Native storage is approximately three times text length;
the full 32 KiB text limit therefore needs the optional data volume.

RTF export creates a separate file, never rebinds the native document or clears
its dirty marker. It emits standard ASCII RTF controls for the supported styles,
alignment, 12pt body / 18pt heading sizes, tabs and paragraph breaks. Syntax
characters (`\\`, `{`, `}`) are escaped. The export workspace is 512 KiB; unusually
dense alternating styles/paragraph attributes can exceed it, producing a clear
error before any file is created. The native document is unaffected.

RTF interoperability is verified with the independent Pandoc reader for text,
bold, italic and underline. An independent test parser checks paragraph alignment,
font sizes, empty/final paragraphs and all inline states. This is a deliberately
small standards-compatible RTF export, not a claim of complete RTF support or
pixel-identical layout in every external word processor.

## Paginated PDF export

`writer_pdf_export` in `src/writer_pdf.c/.h` serializes the current rich model into
ordinary letter or A4 portrait PDF pages, using standard Times fonts, 12/18-point
body/headings, bold/italic/underline, left/center/right alignment, word wrapping
and one-inch margins. It is independent of the screen layout and filesystem.
The model exporter takes caller-owned output, measures before writing, and rejects
insufficient capacity without changing either output or native work. It adds no
arena/BSS allocation. See [WRITER_PDF.md](WRITER_PDF.md) for exact layout, failure
semantics, standard-font attribution, independent host verification and the safe
file/UI integration details.

Click **PDF** or press **Ctrl+Shift+P**. The export dialog defaults to Letter and
lets you select Letter or A4 with the mouse. Tab/Shift+Tab moves through filename,
Letter, A4, Cancel and Export. On a paper control, Left/Right changes paper;
Space/Enter selects it. Enter from the filename or Export writes the file;
Escape or Cancel closes the dialog. The selected paper is highlighted, with a
separate keyboard-focus outline. The compact RTF/PDF buttons fit the existing
420px minimum toolbar without reducing its other controls.

PDF export preflights the exact serialized size against the 512 KiB output buffer,
the mounted per-file limit, projected node capacity and available byte storage
before creating any file. The full output is then serialized before creation.
A failed write removes only its newly created, identity-matching empty target.
Export never changes native binding, dirty state, caret/selection or undo/redo.
Existing files cannot be overwritten.

Accepted IDE exports dismiss the filename dialog immediately and show a compact
exporting footer; editing and other windows remain available. An admission error
keeps the dialog open. A later disk error retains the complete RAM export and
reports in Writer without stealing focus. Reopen export to retry the **same
filename and paper** while that revision is current; a valid owned retry is
prefilled. Writer verifies mount, parent/name, node identity, content version,
source revision, options and every serialized byte, then retries synchronization
without rewriting. Changed document, paper or file requires a new name. RTF and
PDF share one bounded failed-export descriptor, separate from native save state.
New/Open/Restore/Close clear retry authority. A cancelled dialog does not delete the RAM file; ordinary later disk sync may
persist it. A successful export does not save unsaved native edits, so the usual
Save/Discard/Cancel guard still applies.

## Desktop integration contract

`src/writer.h` exposes one client-area module, default 720x520 and minimum
420x260. The caller supplies client origin and dimensions to drawing, clicking
and dragging. Every draw operation is clipped to the appropriate client/page
rectangle as well as the screen. The desktop owns New/Open/Close confirmation,
window title decoration, name dialogs, file association and recovery scheduling.

Input returns a bitmask: changed, request Save, request Save As, or request
RTF export or PDF export. Save APIs return `WRITER_SAVE_OK` (1),
`WRITER_SAVE_PENDING` (2), `WRITER_SAVE_NEEDS_NAME` (0), or `WRITER_SAVE_ERROR` (-1).
Export returns a durable nonnegative filesystem ID, `WRITER_EXPORT_PENDING` (-2),
or error (-1). These are explicitly distinct from the coordinator's BOS codes.
One top-level hook after storage service collects results independently of blink
and focus. Save-before-Close/New/Open retains exact document owner/request and
window/target identities; it finishes only after matching success with no newer
unsaved edits. Cancel cancels navigation, never the already-started disk commit. `writer_close()` is called **only after** a completed
Save/Discard/Cancel guard; it resets document content while retaining clipboard.
`writer_tick()` and release are safe before first initialization.

`writer_snapshot()` returns borrowed native bytes in Writer's output buffer.
Those bytes are valid until the next Writer operation. Recovery autosaving must
not mark the document saved. `writer_restore()` validates before publication,
restores bounded caret/selection and only rebinds an identity-matching native
file. Because runtime identities do not survive reboot, the desktop persists a
versioned binding sidecar: `writer_binding()` returns the cached baseline source
fingerprint and `writer_binding_matches()` compares a candidate file read-only.
The desktop must match that record before passing a recovery file to restore;
missing/mismatched metadata requires an unbound recovery. Restore captures the
verified current file baseline, not the possibly different unsaved draft. A missing/reused binding becomes an unsaved recovered document. Fingerprints are
size plus two independent 32-bit noncryptographic checks, for accidental external
replacement detection rather than authentication. Hashing occurs only on
open/save/recovery matching, never during drawing or routine dirty queries. A
successful RAM write updates the baseline even if disk sync then fails, so retry
can safely recognize its own pending write.

The desktop overrides two weak plain-text clipboard hooks. `writer_clipboard_set`
returns a generation; `writer_clipboard_get` returns full byte length or -1 for
unavailable/over-capacity data. The desktop generation must advance on **every**
clipboard write, even identical bytes. Writer retains formatting internally only
when generation, byte length and all bytes still match its own copied selection.
External clipboard ownership always pastes as plain text. No shared rich format
or silent truncation is assumed. The default weak implementation provides local
Writer copy/paste when there is no desktop integration.

## Bounds and validation

The fixed `WRITER_BASE` arena holds nine document snapshots, a validated staging
document, a rich clipboard copy, all 32,769 possible line records and a 512 KiB
serialization buffer. A compile-time assertion ensures the complete structure
fits `WRITER_CAPACITY` (0x1F0000). No general heap is used. Large data do not live
in kernel BSS. Long copies, parsing and layout service devices through
`platform_poll()` only; callbacks must not dispatch applications or mutate files.

Run the deterministic host suites (ASan/UBSan):

```sh
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p 'test_writer*.py' -v
```

The tests cover formatting, paragraph edits/joins, native round trips and complete
input validation, clipboard generation ownership, capacity rejection, all possible
line slots, proportional wrapping and alignment, headings, navigation, selection,
undo/redo branching, style-preserving find/replace and atomic capacity failures,
file identity/reuse, failed save/sync, recovery, and drawing
outside-client preservation at normal/minimum/offscreen sizes. Codec tests verify
all 64 inline-style transition pairs in the independent Pandoc reader when it is
available. Guest integration tests are separate from these host-module checks.


## Verified desktop integration

`python3 tools/writer_input_test.py build` uses actual PS/2 input against the normal
production kernel at 800x600, never a modified test kernel or a saved user disk.
It verifies centered headings, bold/italic and rich clipboard attributes, native
Save As, independent RTF export, undo/redo, New/Close cancellation, Save before
close, discarded drafts staying discarded, and CRLF imports leaving their source
untouched. A complete 32,768-byte document rejects an extra character, saves exact
styles, and recovers its changed styled draft and caret after an actual reboot.
The exported production RTF was also read successfully by Pandoc.

The desktop saves native recovery bytes in `prefs/writer-draft.bwr`, with the
existing version-1 session window/path/caret record. Restored drafts are marked
unsaved conservatively. Storage preflight includes the complete rich snapshot
before changing any draft; shrinking rich/plain drafts are written first. A
failed save or failed Save As leaves the owning document and pending action open.


`python3 tools/writer_lifecycle_test.py build` additionally verifies real Save As
extension/collision errors, Open cancellation, clean orphan recovery after a node
ID is reused, identical plain bytes replacing rich clipboard ownership, and
Save-before-Open/New. Its supported read-only recovery-mode test keeps Writer's
failed-save draft open and confirms every byte of the unrecognized disposable
disk remains unchanged. This is an ordinary UI/storage test, not fault injection.


### Recovery source conflicts

The desktop stores a versioned 24-byte `prefs/writer-binding` record alongside the
native draft. It contains the original serialized size, FNV-1a and CRC32. On reboot,
the resolved source must match that baseline before the recovered draft can save
back to it. Missing metadata or changed bytes restore the complete draft as an
unbound, unsaved document requiring Save As. Checksums detect ordinary content
changes; they are not cryptographic authentication. The source file is untouched.

The same baseline check runs before an ordinary Save, so another native app cannot
silently replace a file's bytes and then have them overwritten by a stale Writer
copy. A successful RAM write updates the baseline even if the subsequent disk sync
fails, allowing safe retry. No fingerprint scan runs in drawing or dirty queries.
`tools/writer_binding_test.py` verifies a real C app's same-ID/same-size replacement,
a matching reboot, a stopped-volume replacement and missing legacy metadata.

`tools/writer_search_test.py` verifies the actual 420-pixel client, mouse case toggle,
style-preserving Replace All and one-step undo/redo, wrapped F3 navigation, Edit-menu
routing, live counts, and complete capacity rejection with unchanged history.

## Responsive persistence verification

See [RESPONSIVE_DOCUMENT_SAVES.md](RESPONSIVE_DOCUMENT_SAVES.md) for the ownership,
revision, recovery and bounded-memory contract and exact verification scope.
Encoding, fingerprinting and atomic RAM replacement remain synchronous.
Writer does not add Unicode, larger models, new file formats or a preemptive kernel.
