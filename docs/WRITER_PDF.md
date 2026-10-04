# Writer paginated PDF export

`src/writer_pdf.c` is an original, small freestanding-C PDF 1.4 exporter for
`WriterDoc`. It produces ordinary PDF bytes for viewing or printing with a host
PDF application. It does not implement a guest PDF viewer, guest printer driver,
PDF import, or paginated editing. The standalone module does not create files or
change Writer's native document, history, selection, binding, or dirty flag.

## Page and text layout

- `WRITER_PDF_LETTER`: 612 x 792 points (8.5 x 11 inches), portrait.
- `WRITER_PDF_A4`: 595.276 x 841.890 points (210 x 297 mm, rounded to 0.001 pt),
  portrait. A point is 1/72 inch.
- All page margins are 72 points. Text uses 12-point body or 18-point heading
  size, independently of bold/italic/underline, with 16- or 24-point line height.
  The baseline is one font size below each line box's top. There is no extra
  paragraph spacing; empty paragraphs occupy their own line boxes.
- The four standard Type 1 fonts are Times-Roman, Times-Bold, Times-Italic and
  Times-BoldItalic with explicit WinAnsiEncoding. Printable ASCII is exact;
  literal parentheses and backslash are escaped. In particular, ASCII apostrophe
  uses **quotesingle**, not the StandardEncoding quoteright glyph.
- Advances use the documented Adobe AFM glyph widths (1/1000 em). All coordinates
  use integer thousandths of a point; decimal serialization does not accumulate
  per-character rounding. No kerning or ligature substitution is requested.
- Underlines are explicit strokes 0.1 em below the baseline and 0.05 em thick,
  matching the AFM underline metrics. Underlined tabs include their advance.
- Left, center and right alignment use the complete line advance width, including
  leading/trailing spaces. Italic glyph ink may naturally overhang its advance;
  it is not clipped at the one-inch text boundary.
- Greedy word wrapping uses spaces/tabs after visible text as break opportunities.
  Input spaces are retained, including whitespace at the end of a wrapped line.
  Indentation is not discarded or stranded on an artificial empty line. A token
  wider than the full line hard-wraps without adding hyphens.
- Tabs move to the next multiple of four normal-space advances from the unaligned
  line's origin: 12-point body or 18-point heading intervals. They remain layout
  movement, not a literal tab glyph. Alignment is applied after measurement.
- Every LF starts a paragraph. Consecutive LF and the empty paragraph after a
  final LF consume line boxes. A new page starts before a line that cannot fit;
  nothing is truncated at the bottom margin. A completely empty document has
  one blank page. Exactly 40 body or 27 heading lines fit on a letter page;
  A4 holds 43 body or 29 heading lines. Mixed sizes use their actual line boxes.

PDF layout is independent of the Writer screen's bundled UI font and window
width. Standard-font readers provide Times or a metrically compatible substitute;
font programs are not embedded, and identical glyph outlines across viewers are
not promised. The exported pages are not a screenshot of the editing window.
There are no widow/orphan rules, keep-with-next headings, manual page breaks,
headers/footers, page numbers, images, tables, links, outlines, encryption, scripts,
forms, tagging or PDF/A claims. Native BWR is the exact editable format. PDF text
extractors may normalize whitespace, line wraps, blank paragraphs and tab stops.

## API and atomic error handling

```c
int writer_pdf_export(const WriterDoc *doc, unsigned paper,
                      unsigned char *out, unsigned capacity,
                      unsigned *written, unsigned *pages);
```

`written` and `pages` are required, separate output pointers. Pass `out = NULL,
capacity = 0` to query the **exact** byte count and page count. A real output buffer
may have exactly that byte count; no NUL terminator is added. Its capacity does
not need to be a multiple of a page or filesystem sector.

- `WRITER_PDF_OK` (0): complete output or successful size query.
- `WRITER_PDF_INVALID` (-1): invalid document, paper kind, arguments or overlapping
  document/output/count ranges.
- `WRITER_PDF_CAPACITY` (-2): the valid export does not fit the supplied buffer.

The full model and exact size are validated before any output byte is written.
On either failure, output bytes, both counts, and the **entire native document**
remain unchanged. All supplied ranges must be valid for their stated lengths and
remain stable during the call. `platform_poll()` services devices only; it must
not dispatch an edit, change files, or reuse the borrowed serialization buffer.

## Memory and work bounds

There is no heap, output-sized stack array, page array, per-character scratch,
mutable global data, floating point or external PDF library. Caller-owned output
is the only variable-size storage. Four immutable 95-glyph tables occupy 760
bytes. The i386 `-Os` object measured 4,874 bytes of code/read-only data and **zero
BSS/data**, with no unresolved runtime dependencies other than `platform_poll`
and `writer_doc_validate`. GCC's largest individual stack frame was 176 bytes;
the exporter's bounded call chain is below 1 KiB, excluding platform polling.

The implementation makes a constant number of linear passes:

1. Validate the document and count layout pages.
2. Measure the full PDF body plus fixed-width xref/trailer sizes.
3. Only when the output fits, write the body.
4. Replay the body once into a count-only sink, directly appending each object's
   offset to the real xref table. This is **one pass**, not one rescan per object.

Each content-stream length is an indirect integer object, so no backpatch or
saved offsets are needed. Work is O(text bytes + page count); wrapping looks ahead
at most one bounded line. Device polling occurs at every line, every 128 scanned
text positions and every 4096 serialized bytes, including count-only passes.

The model contains at most 32,768 text bytes and 32,769 lines. The smallest page
capacity is 27 heading lines, so at most **1,214 pages** and 3,648 live PDF objects
are produced. A conservative bound of 256 bytes per input byte plus 1024 per page
and 2048 fixed bytes is under 10 MiB, safely inside all 32-bit counters. This is a
counter-safety bound, not a promised application buffer size.

Writer's existing serialization buffer is **512 KiB**. It fits common documents,
including the full newline-heavy fixture, but cannot fit every rich model. Host
measurements (letter unless noted):

| Deterministic fixture | PDF bytes | Pages |
| --- | ---: | ---: |
| Styled short page | 2,839 | 1 |
| 100 mixed-style paragraphs | 29,405 | 6 |
| 32,768-byte unbroken token, A4 | 74,962 | 20 |
| 32,768 LF bytes, heading paragraphs | 276,662 | 1,214 |
| 32,768 bytes, style changes every character, heading | 2,274,017 | 23 |

The last case rejects the 512 KiB output buffer without writing anything. It also
exceeds the default data volume's 2 MiB per-file limit. An explicit large volume
has a 16 MiB per-file limit, but that does **not** enlarge Writer's output arena.
No large-volume arenas are borrowed. Floppy per-file storage remains 16 KiB.

## Writer file and UI integration

`writer_export_pdf(parent, name, paper)` in `writer.c` integrates the model using
Writer's existing 512 KiB output buffer. The PDF toolbar button and Ctrl+Shift+P
open a named export dialog with explicit Letter/A4 controls. RTF keeps its
separate toolbar action and Ctrl+Shift+E shortcut. Both buttons fit the same
88px toolbar allocation, with a 4px gap, at the 420px minimum client width.

The wrapper validates the filename, destination folder and paper; checks for a
new target; measures exact PDF size/pages; and checks the 512 KiB arena, mounted
per-file limit, free node slots and byte capacity **for the projected node count**.
All this happens before file creation. It serializes the complete PDF and checks
both resulting counts, then creates the new ordinary file and writes atomically.
A write failure removes only that newly created, identity-matching empty file.
A successful result requires `fs_sync()`; native save state is never changed.

If sync fails, the error says that the PDF exists in RAM and the dialog remains
open. Repeating the same name and paper can retry **sync only**, provided the
most recent pending export's node identity, document revision, paper, size and
every byte of regenerated output still match. No retry overwrites a file. An
external replacement, reused node, document edit or paper change requires a new
name. New/Open/Restore/Close clear pending ownership; cancelling the dialog does
not delete the RAM export. A later filesystem sync can persist it, but Writer
never reports successful export until a sync call succeeds. Export does not
clear the native dirty marker, rebind a `.bwr`, or alter text/styles, history or
caret/selection. The ordinary unsaved-document guards still apply.

The UI does not import, display or print PDF in the guest. Writer explicitly
rejects `.pdf` names and `%PDF-` signatures as export-only input. Use the original
`.bwr` for editing and a host PDF reader for viewing or printing exported pages.

## Sources and attribution

The format follows Adobe's *PDF Reference, Third Edition, version 1.4*: section
3.2 (objects and literal strings), 3.4 (file structure/xref), 3.6 (document/page
tree), chapter 4 (graphics), and chapter 5 (text and standard Type 1 fonts).
The [PDF Association specification archive](https://pdfa.org/resource/pdf-specification-archive/)
indexes the original Adobe specifications and their errata. Adobe's original
legacy download endpoints may be unavailable; the archive also identifies
historical copies. Standard-font behavior is also described in Adobe's
[Base14 font documentation](https://experienceleague.adobe.com/en/tools/aem-api-documentation/cloud-service/javadoc/com/adobe/fontengine/font/Base14.html).

Only the numeric width data is reused. It is derived from Adobe's AFM 4.1
Times files, version 002.000, distributed in Matplotlib's `pdfcorefonts` collection.
[The upstream metrics](https://github.com/matplotlib/matplotlib/tree/main/lib/matplotlib/mpl-data/fonts/pdfcorefonts)
and their retained permission/copyright notices are documented in
`third_party/adobe-core14/NOTICE.txt` and the unmodified `readme.txt` beside it.
The reduced WinAnsi tables are prominently identified as modified data in the C
source. The PDF writer/layout code is original, with no copied PDF runtime.

## Verification

```sh
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p 'test_writer*.py' -v
```

`tests/writer_pdf_test.c` tests named ordinary input/capacity cases under ASan and
UBSan and emits reproducible PDFs plus original BWR fixtures. Tests cover empty
and final-empty pages, all 95 printable characters, all eight inline states,
all 64 state transitions, all alignments, both font sizes/paper types, tabs,
indentation, hard wrapping, complete native preservation, exact/short output
buffers, invalid model/arguments, maximum text, maximum page count and polling.
It is not a fuzz, memory-fault or guest disk test.

`tests/test_writer_pdf.py` additionally checks every xref offset and indirect
stream length. Independent pypdf strict parsing validates all 1,214 pages and
compares exact printable text/style/size order. Independent ReportLab standard
font metrics check glyph/run positions, alignment and underline endpoints. Poppler
extracts every word of representative styled/multipage documents in order without
duplicates. MuPDF renders representative letter/A4 pages without warnings. Missing
optional readers are explicitly skipped, never counted as independent success.

Implementation validation used GCC 14.2, pypdf 6.10.0, ReportLab 4.4.9,
Poppler 25.03.0 and MuPDF 1.26.11. System Poppler and MuPDF PNGs of styled, ASCII
and multipage examples were visually inspected for font selection, readable
spacing, complete text, margins and pagination. A bundled Poppler wrapper in the
validation environment selected unsuitable substitute fonts for regular/bold;
using system Poppler with installed Nimbus Roman fonts and MuPDF's built-in
standard fonts gave correct matching layout. This is a renderer configuration
caveat, not a promise of identical outlines in every PDF program. Guest export UI
and file persistence require their own integration/QEMU checks.

## Integrated verification

`tests/writer_host.c` exercises the actual file wrapper, minimum-width toolbar
hits, both export shortcuts while searching, exact per-file/storage boundaries,
projected-node accounting, full node slots, failed create/write/sync, repeated
sync-only retry, changed bytes, changed size, reused IDs, changed document/paper,
native binding/selection/history preservation, read-only PDF rejection and
512 KiB rejection. The ordinary host suite uses ASan/UBSan.

`python3 tools/writer_pdf_input_test.py build` uses a copy of the unmodified packed
production floppy and a fresh disposable data volume. It drives only normal
QMP PS/2 keys/mouse and screendumps: no debugger, paused boot, guest-memory
observations or injected calls. The stopped volume must contain exact native and
RTF bytes, complete Letter/A4 PDFs and no cancelled/invalid/over-capacity targets.
pypdf strictly parses every saved page and checks text/dimensions; MuPDF renders
first/last pages. It preserves screenshots of paper selection, 420px controls,
errors and the native close guard. Readers are required for this end-to-end
check; their absence is an error rather than an independent-validation claim.
