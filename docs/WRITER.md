# Writer: bounded formatted documents

Writer is a separate, single-instance native desktop app. The plain-text Editor
remains available. Writer stores up to **32,768 ASCII text bytes**, with bold,
italic, underline, heading/body paragraph styles and left/center/right alignment.
Tabs and line breaks are supported. There is no Unicode, DOCX, general RTF import,
image embedding, pagination or printing. RTF is an export format.

## Editing

The toolbar offers B / I / U, Body / Heading, paragraph alignment, Undo / Redo,
Save and Export RTF. Character buttons affect selected text or subsequent typing.
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
Escape closes the bar without changing document text.

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

Native Save As requires `.bwr`; export requires `.rtf`. Existing unrelated names
are rejected. Ordinary Save checks the original filesystem identity, including
after a `.bwr` rename: a deleted file's reused node ID is never overwritten.
Renaming away from `.bwr` requires Save As. A clean
bound document whose file disappears is treated as unsaved and needs Save As.
Every successful save requires `fs_sync()` to finish successfully. Failed writes
or synchronization leave the document open and unsaved. A newly created empty
target is removed after write failure. A target with a pending in-memory write
is retained with its known identity after a sync failure so retry is safe.
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

## Desktop integration contract

`src/writer.h` exposes one client-area module, default 720x520 and minimum
420x260. The caller supplies client origin and dimensions to drawing, clicking
and dragging. Every draw operation is clipped to the appropriate client/page
rectangle as well as the screen. The desktop owns New/Open/Close confirmation,
window title decoration, name dialogs, file association and recovery scheduling.

Input returns a bitmask: changed, request Save, request Save As, or request
Export. Save APIs return `WRITER_SAVE_OK` (1), `WRITER_SAVE_NEEDS_NAME` (0), or
`WRITER_SAVE_ERROR` (-1). Export returns the new nonnegative filesystem ID on
success and -1 on failure. `writer_close()` is called **only after** a completed
Save/Discard/Cancel guard; it resets document content while retaining clipboard.
`writer_tick()` and release are safe before first initialization.

`writer_snapshot()` returns borrowed native bytes in Writer's output buffer.
Those bytes are valid until the next Writer operation. Recovery autosaving must
not mark the document saved. `writer_restore()` validates before publication,
restores bounded caret/selection and only rebinds an identity-matching native
file. A missing/reused binding becomes an unsaved recovered document.

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
