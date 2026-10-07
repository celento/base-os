# Writer document formats

Writer's editable document model contains at most **32,768 text bytes**. The
native format preserves the complete model. RTF is an export format for other
applications; this codec does not import RTF, DOC, DOCX, HTML, or PDF.

The codec is independent of the window, font, filesystem, and application APIs.
Its only platform operation is periodic `platform_poll()`, which collects input
without dispatching application actions. There is no heap allocation,
document-sized stack allocation, implicit global document, or raw structure
dump. Caller-owned buffers provide all document and output storage.

## Text and formatting model

`src/writer_codec.h` defines `WriterDoc`:

```c
typedef struct {
    unsigned length;
    unsigned char text[32768];
    unsigned char style[32769];
    unsigned char paragraph[32769];
} WriterDoc;
```

- `length` is a byte count from 0 through 32,768, inclusive. Text is not
  NUL-terminated. A full-capacity document uses every text-array slot.
- Valid text bytes are printable ASCII (`0x20` through `0x7e`), TAB (`0x09`), and
  LF (`0x0a`). Embedded NUL, CR, DEL, other control characters, UTF-8, and every
  other byte above `0x7e` are rejected. There is no replacement, dropping of
  unsupported bytes, or prefix-only import.
- `style[i]` uses bit 0 for bold, bit 1 for italic, and bit 2 for underline.
  All eight combinations are valid. Other bits are reserved and rejected.
  `style[length]` stores the insertion style, including for an empty document.
- Paragraph starts are index 0 and indices immediately following LF, including
  `length` when the last text byte is LF. The low two bits of a paragraph byte
  select left (0), center (1), or right (2) alignment. Value 3 is invalid.
  Bit 2 (`0x04`) selects the heading appearance. All other bits are invalid.
- Every paragraph byte at a **non-start** index from 0 through `length` must be
  zero. Thus `paragraph[length]` is zero unless the document is empty or ends in
  LF. Consecutive LF bytes describe consecutive empty paragraphs with independent
  attributes.
- Array bytes after the logical document are ignored by validation and export.
  Initialization and successful imports clear the unused storage. These bytes
  are never serialized.

### Plain text import

`writer_plain_import` accepts the text repertoire above and sets all formatting
to normal, left-aligned body text. It accepts LF and Windows CRLF line endings,
including a mixture: **each CRLF pair is explicitly converted to one LF**. A lone
CR is rejected, including one at the end. No other byte conversion is performed.
The source bytes remain untouched. The entire source is validated and its
normalized length checked before the destination changes.

An empty input is valid; `NULL` is allowed only when its length is zero. The
normalized result must fit 32,768 bytes. Thus up to 65,536 raw input bytes are
accepted when all are CRLF pairs; a 32,769-byte LF/printable-only source is rejected
rather than truncated. Native BWR1 input remains strict LF-only and never applies
this plain-text conversion.

## Native BWR1 format, version 1

The native format is a compact, deterministic binary representation, commonly
saved with a `.bwr` extension. Its meaning never depends on C padding, pointer
size, machine endianness, or unused array storage. All multi-byte integers are
unsigned **little-endian** values.

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 4 | ASCII magic `BWR1` (`42 57 52 31` in hexadecimal) |
| 4 | 2 | Version, exactly 1 |
| 6 | 2 | Flags, exactly 0 |
| 8 | 4 | Text length `N`, at most 32,768 |
| 12 | 4 | Reserved, exactly 0 |
| 16 | `N` | Text bytes |
| `16 + N` | `N + 1` | Character style bytes, including insertion style |
| `17 + 2N` | `N + 1` | Canonical paragraph bytes, including the final slot |

The **exact** file length is `18 + 3N` bytes. An empty document is 18 bytes;
a full-capacity document is 98,322 bytes. No terminator, trailer, optional
padding, checksum, or extension section is present. Unknown magic, versions,
flags, reserved values, invalid text/formatting, a nonzero non-start paragraph
byte, truncation, or even one trailing byte rejects the whole file. Length is
bounded before any multiplication or payload access.

For example, the default empty document is:

```text
42 57 52 31 01 00 00 00 00 00 00 00 00 00 00 00 00 00
```

For a document containing `A` followed by LF, the payload is two text bytes,
three style bytes, and three paragraph bytes. The last paragraph byte describes
the empty paragraph after LF; the final style byte is the insertion style.

The format detects structural and unsupported-value errors, not arbitrary
changes between otherwise valid values. Storage integrity and verified disk
commit remain responsibilities of the filesystem/save path.

## RTF export

The exporter follows Microsoft's
[Rich Text Format Specification, version 1.9.1](https://officeprotocoldoc.z19.web.core.windows.net/files/Archive_References/%5BMSFT-RTF%5D.pdf),
particularly Basic Entities (pages 7–10), Paragraph Formatting (pages 78–80),
Character Formatting (pages 130–132), and Special Characters (pages 142–143).

It emits an ASCII-only `\rtf1` document with an ANSI declaration and an Arial
font-table entry. Reader font substitution is permitted. Literal backslash and
braces are escaped; TAB becomes `\tab` and LF becomes `\par`. Paragraph controls
select left, center, or right alignment. Body text uses `\fs24` (12 pt), headings
`\fs36` (18 pt). Heading size is independent of bold, italic, and underline.
The exporter sets these character properties explicitly at the start and changes
them when needed, including their off forms. Control words are delimited before
literal text. Properties are retained across paragraphs where unchanged.

Empty paragraphs and trailing LF are emitted. Final insertion formatting is
included, but other applications can normalize empty paragraphs or insertion
state. Native files are the exact editable round-trip format. Export does not
promise identical wrapping, pagination, fonts, or a semantic outline/heading
style in another word processor. There are no images, tables, links, embedded
objects, fields, or external-resource references.

The exact output size is measured before any output write. The Writer application
uses a **512 KiB export buffer**. Dense alternating character/paragraph formatting
can exceed this buffer: the full-capacity alternating-paragraph fixture produces
901,218 bytes. Such an export fails cleanly without changing the document or
writing a partial export. A caller with a sufficiently large buffer can export
the same document. Repeated identical formatting is not redundantly emitted.

## API and failure guarantees

All `int` functions return 0 for success or -1 for invalid input, aliasing, or
insufficient output capacity. `writer_doc_init` initializes an empty document;
`writer_doc_validate` checks every meaningful text and formatting byte.

```c
int writer_plain_import(WriterDoc *, const unsigned char *, unsigned);
int writer_native_decode(WriterDoc *, const unsigned char *, unsigned);
int writer_native_encode(const WriterDoc *, unsigned char *, unsigned, unsigned *);
int writer_rtf_export(const WriterDoc *, unsigned char *, unsigned, unsigned *);
```

The last two arguments of encode/export are output capacity and a required
pointer to the written byte count. Pass `out = NULL, capacity = 0` to query the
exact required count without writing a file. A non-NULL output succeeds when
capacity equals the count; **no NUL terminator** is added. A NULL output with
nonzero capacity is invalid. On failure, output bytes and the caller's count
remain unchanged.

Import/decode validate the whole supplied document before committing anything
to the destination, so the entire existing `WriterDoc` is unchanged on failure.
Sources must not overlap destination documents. Encode/export output buffers
and count pointers must not overlap the source document or each other. The
functions reject those overlaps; in-place conversion is not supported. Buffers
must be valid for their supplied lengths and stable during calls.

## Reproduction and test scope

```sh
ASAN_OPTIONS=detect_leaks=0 python3 -m unittest discover -s tests -p test_writer_codec.py -v
```

The host C tests compile with warnings-as-errors, AddressSanitizer, and
UndefinedBehaviorSanitizer. Named deterministic cases cover zero and maximum
lengths, every supported/unsupported byte class, mixed LF/CRLF normalization,
lone-CR rejection, maximum raw versus normalized sizes, late invalid bytes, every
truncation of the sample native file, oversized lengths, little-endian fields,
reserved bits, trailing junk, insertion formatting, all paragraph slot rules,
alias rejection, exact/undersized buffers, unchanged destinations on failure,
cooperative polling, and the 512 KiB export limitation. These are ordinary host
format tests; they do not boot an emulator or modify a disk image.

The Python tests independently interpret the exported subset to check exact text,
all 64 inline style transitions, alignment, font size, empty paragraphs, escaping,
and insertion state. If installed, **Pandoc** additionally reads the actual RTF
files and checks visible text and bold/italic/underline semantics, including all
64 transitions. Its AST
normalizes tabs to spaces and omits empty paragraphs, so those features and font
size/alignment are checked by the explicit token/state reader instead. A missing
Pandoc reports skipped interoperability tests, not a claimed independent pass.
