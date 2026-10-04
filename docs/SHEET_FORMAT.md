# Spreadsheet display metadata and native wire format

`sheet_model.[ch]` stores display formats separately from the exact cell sources
and cached three-decimal fixed-point values. `sheet_codec.[ch]` reads native
versions 1 and 2. This document specifies the bytes accepted and emitted by that
codec; it is not an in-memory `SheetDoc` dump.

## Display metadata

`sheet_init` installs General format on every cell and a width of **104 pixels**
on every column. `sheet_set_format` / `sheet_get_format` use zero-based row and
column coordinates; `sheet_set_column_width` / `sheet_get_column_width` use a
zero-based column. Widths are integer pixels in **48..320**, inclusive. Getters
require an output pointer and reject invalid stored metadata; setters reject an
out-of-grid coordinate or unsupported value without changing the document.
Neither getter output may overlap the document.

| Format byte / enum | Numeric or successful formula display |
|---|---|
| 0 / `SHEET_FORMAT_GENERAL` | Plain decimal, up to three fractional digits, no unnecessary zeroes |
| 1 / `SHEET_FORMAT_FIXED2` | Exactly two fractional digits, such as `1.24` |
| 2 / `SHEET_FORMAT_CURRENCY` | Dollar prefix and two fractional digits, such as `$1.24` or `-$1.24` |
| 3 / `SHEET_FORMAT_PERCENT` | Value multiplied by 100, exactly two fractional digits and `%`, such as `12.50%` for `0.125` |

Fixed2 and Currency round to the nearest hundredth, with ties away from zero:
`1.235` becomes `1.24`, `-1.235` becomes `-1.24`. Rounded negative zero is
suppressed: `-0.004` displays `0.00` or `$0.00`. Percent uses the exact cached
value; three-decimal calculation precision makes its second fractional digit
zero. The full supported numeric range is safe to display, including
`-214748364.80%` for `-2147483.648`. Currency is a literal dollar display, with
no locale selection, thousands grouping or currency conversion.

`sheet_format_display` renders this metadata-aware cached display. Text, empty
cells and calculation-error labels remain unchanged under every format. Its
output is NUL-terminated; `written` excludes NUL. `out=NULL, capacity=0` measures
the required characters. Insufficient capacity leaves output and `written`
unchanged. Neither output may overlap any part of the document or each other.

Formatting never changes the source, kind, numeric cache, calculation precision
or formula result. `sheet_set` preserves an existing format when entering,
replacing or clearing a value. A formatted EMPTY cell is valid and retains its
format for a later value. A complete document snapshot includes both formats and
widths; `sheet_init`, native v1 decoding and CSV import reset them to defaults.
Recalculate after source edits before reading any cached display.

## Common native header

All multi-byte integers are unsigned **little-endian**. There is no alignment
padding, string terminator, trailer, embedded checksum or authentication field.
The common header is exactly **16 bytes**:

| Byte offset | Byte count | Value |
|---|---:|---|
| 0 | 4 | ASCII `BSH1` (`42 53 48 31` hexadecimal), for both versions |
| 4 | 2 | Version: 1 or 2 |
| 6 | 2 | Flags: 0 |
| 8 | 2 | Rows: 128 |
| 10 | 2 | Columns: 26 |
| 12 | 4 | Sparse record count: 0..3328 |

Row-major cell indices are `row * 26 + column`, from 0 (A1) through 3327
(Z128). Every record must have an index strictly greater than the preceding
record; duplicates and descending indices are rejected. All records must fit
within the supplied file length and the last source byte must end exactly at
EOF. Cached calculation results and dependency traversal state are never stored.

## Version 1: unchanged original format

Records begin at byte **16**. Each has the following four-byte prefix, followed
immediately by its source bytes:

| Record-relative offset | Byte count | Value |
|---|---:|---|
| 0 | 2 | Cell index, 0..3327 |
| 2 | 1 | Kind: 1 TEXT, 2 NUMBER, or 3 FORMULA |
| 3 | 1 | Source byte length, 0..95 |
| 4 | length | Exact source bytes, without NUL |

EMPTY cells are omitted. Explicit empty TEXT is retained with kind 1 and length
0. On read, every format becomes General and every column becomes 104 pixels.
The empty v1 sheet is the 16-byte header. Maximum size is
`16 + 3328 * (4 + 95)` = **329,488 bytes** (`SHEET_NATIVE_V1_MAX_SIZE`).

## Version 2: widths and per-cell display format

A width table immediately follows the header at byte **16**. It contains 26
little-endian unsigned 16-bit widths in A..Z order. Width A occupies offsets
16..17; width Z occupies offsets 66..67. Every value must be **48..320**. Zero is
invalid, not a default-width sentinel. Table size is exactly **52 bytes**.

Records begin at byte **68**. Each has the following five-byte prefix:

| Record-relative offset | Byte count | Value |
|---|---:|---|
| 0 | 2 | Cell index, 0..3327 |
| 2 | 1 | Kind: 0 EMPTY, 1 TEXT, 2 NUMBER, or 3 FORMULA |
| 3 | 1 | Source byte length, 0..95 |
| 4 | 1 | Format byte: 0 General, 1 Fixed2, 2 Currency, 3 Percent |
| 5 | length | Exact source bytes, without NUL |

An EMPTY record is accepted **only** with length 0 and a nondefault format
(1..3). It preserves a formatted empty cell. General EMPTY cells are omitted;
all absent cells decode as General EMPTY. Explicit empty TEXT may use any of
the four formats. AUTO (4) is an API input convenience, never a stored kind.

The version-2 encoder emits a record for every non-EMPTY cell and every
non-General EMPTY cell. It emits the complete width table even when only one
cell format has changed. A widths-only sheet has zero records and occupies
68 bytes. A widths-only sheet with one formatted EMPTY cell occupies 73 bytes.
Maximum size is `16 + 52 + 3328 * (5 + 95)` = **332,868 bytes**
(`SHEET_NATIVE_MAX_SIZE`). The width table and records contain no reserved bytes.

## Source validation and atomicity

Both versions use identical source rules: printable ASCII (32..126), TAB (9),
LF (10), and CR (13) are permitted; NUL, DEL, non-ASCII and UTF-8 BOM are not.
NUMBER must have complete plain signed decimal syntax. FORMULA must start with
`=`. TEXT may contain any permitted source bytes, including no bytes. Invalid
formula syntax, numerical overflow and excess precision remain legitimate
source content and reopen with an explicit calculation error.

Decode validates the entire header, widths, record order, formats, kinds,
lengths and source bytes before changing the destination. Invalid versions,
flags, dimensions, widths, format bytes, record sequences and trailing data
return -1 without changing existing source, caches or metadata. Accepted content
is installed and recalculated. Input cannot overlap the destination document.

Encode validates and measures first. It needs no fresh cached calculations and
does not modify the model. It writes neither partial bytes nor a new `written`
value on failure. `out=NULL, capacity=0` reports exact byte length. Codec output
is not NUL-terminated. Storage-layer identity checks, failure-atomic writes,
durability and accidental-change fingerprints remain separate responsibilities.

## Compatibility and CSV

The encoder chooses version 1 whenever **all** cell formats and widths have
their defaults. Existing documents therefore retain their exact previous
native bytes. Nondefault metadata selects version 2. Restoring all metadata to
defaults selects version 1 again. The reader also accepts a version-2 file
containing only default metadata and normalizes its next save to version 1.
Older version-1-only BaseOS readers reject version 2; they cannot preserve the
new formatting. New readers accept both versions.

CSV remains numerical value interchange using `sheet_format`, never
`sheet_format_display`. A cell displayed as `$1.24` can export `1.235`, and a
cell displayed as `12.50%` exports `0.125`. Formula sources export their computed
values. Formatted EMPTY cells do not enlarge the CSV rectangle. Widths and
formats are not exported; CSV import always installs General and 104-pixel
widths. CSV retains its existing literal-formula import and quoting behavior.

## Verification

`tests/test_sheet_format.py` compiles the ordinary functional host harness with
ASan/UBSan. It verifies all four numeric displays, both signs, rounding carries,
negative zero, numeric extrema, text/empty/error displays, metadata bounds,
retention through edit/clear, v1 byte compatibility, v2 exact reopening,
formatted-empty and widths-only documents, maximum v2 size, output-capacity
atomicity, invalid metadata file rejection, and unchanged formula/CSV values.
Python independently reconstructs the complete v2 fixture byte-for-byte with
`struct` and parses its CSV with the standard `csv` reader. These are named
functional cases, with no fuzzing or deliberate memory-fault probes.

Freestanding production-flag i386 compilation adds no heap, mutable global
model, BSS, floating point, or 64-bit arithmetic runtime dependency. Measured
on 2026-10-04, `sheet_model.o` text is 6,853 bytes and `sheet_codec.o` text is
3,326 bytes; each has zero data/BSS. The display formatter's own stack frame is
112 bytes; the largest own model/codec frame remains 192 bytes. These are host
and object checks, not a claim of completed guest UI/recovery verification.
