# Spreadsheet core and interchange

The isolated spreadsheet engine is in `src/sheet_model.[ch]` and
`src/sheet_codec.[ch]`. It is ready for an eventual `.bsh` desktop application;
this change does **not** add a window, filesystem binding, arena reservation,
menu command, or Makefile kernel integration. The existing `decimal.c` provides
checked arithmetic. All model storage belongs to the caller.

## Bounds and memory

- Grid: **26 columns × 128 rows**, A1 through Z128; zero-based row/column API.
- Cell source: **95 bytes**, plus a NUL terminator. Printable ASCII, TAB, CR and
  LF are accepted; NUL within text, DEL, non-ASCII and UTF-8 BOM are rejected.
- One `SheetCell`: 104 bytes. One `SheetDoc`: **352,768 bytes**, comprising
  346,112 bytes of cells and a 6,656-byte private dependency-index stack.
- No heap, static model, physical address, document-sized C stack, floating point,
  or compiler 64-bit division runtime is required. Align `SheetDoc` normally.
- Expression nesting: **16** parenthesis/function levels. Unary signs are parsed
  iteratively. Dependency depth can span **all 3,328 cells** and is independent
  of the expression nesting limit.
- Numeric representation: signed 32-bit fixed point, **three decimal places**;
  exact supported interval **−2,147,483.648 through +2,147,483.647**. Do not hide
  this range or the text limit in the eventual input UI.
- A native file needs at most **329,488 bytes**. A CSV export needs at most
  **642,432 bytes**. Query the exact size before reserving/writing file data.

`platform_poll()` is called during model, range and codec work. Callbacks must
not mutate the input/document/output or reenter the same document operation.
The implementation is not a concurrent-edit API. The normal platform's
non-reentrant device/background service is appropriate.

## Editing and reusable UI helpers

Initialize with `sheet_init(doc)`. `sheet_set(doc, row, col, kind, text, length)`
validates the complete cell source before replacing its destination. Failure
returns −1 and preserves the entire document. A source inside the document is
supported, including the destination cell itself: a small temporary cell makes
copying safe. A successful edit does not refresh other cached values; call
`sheet_recalculate(doc)` after an edit or batch of edits.

Stored kinds:

- `SHEET_EMPTY`: no source text; an absent cell
- `SHEET_TEXT`: exact literal text, including numeric-looking or `=...` text;
  explicit zero-length text is distinct from EMPTY in the native model
- `SHEET_NUMBER`: plain signed decimal syntax
- `SHEET_FORMULA`: source begins with `=`; invalid expressions remain editable
  formula sources with a visible calculation error

`SHEET_AUTO` is an input convenience, never a stored kind. Empty input clears a
cell; leading apostrophe forces TEXT and removes that one prefix; leading `=`
selects FORMULA; complete decimal syntax selects NUMBER; everything else is
TEXT. The 95-byte limit applies to input before removing an apostrophe. Explicit
TEXT mode can store the full 95 bytes and retain a literal leading apostrophe.
A future formula bar should preserve the kind while editing a TEXT cell, or add
an apostrophe when presenting it as AUTO input. Blindly passing its raw source
back through AUTO would reinterpret numeric-looking/formula-looking text.

Plain decimals accept optional `+`/`-`, an optional dot, and at least one digit.
Examples: `12`, `-1.25`, `.5`, `+2.`, `001.2000`. Whitespace, exponent notation,
thousands separators, currency symbols and percent notation are not numeric
syntax. Thus AUTO stores `1e3` or ` 12 ` as text. Valid decimal syntax outside the
range, or containing nonzero digits after the third decimal place, stays NUMBER
and displays an explicit error; it is never silently converted to text/clipped.

`sheet_cell` returns a const cell pointer for in-bounds coordinates, otherwise
NULL. `sheet_reference` parses complete case-insensitive A1..Z128 names;
`sheet_label` emits a label in a five-byte buffer. Labels reject leading row
zeroes, `$` markers, whitespace and columns beyond Z.

`sheet_format` emits cached values using plain decimal notation, at most three
fractional digits and no unnecessary zeroes. It emits literal text verbatim and
empty as empty; calculation errors use the labels below. Its output is NUL
terminated; `written` excludes NUL. `out=NULL, capacity=0` measures. Output and
count remain unchanged on failure. The output/count cannot alias each other or
the source cell. No rendering, column width, alignment or wrapping is imposed.

**Embedded CSV newlines are cell text, never grid row separators.** A UI must
clip/wrap or visibly replace TAB/CR/LF within the one cell's drawing rectangle;
it must not reinterpret these source bytes as extra sheet rows. A multiline
editor may retain the exact original bytes for native round-trip.

## Formula language

Arithmetic supports `+`, `-`, `*`, `/`, unary `+`/`-`, parentheses, numeric
literals and case-insensitive A1 references. Multiplication/division bind before
addition/subtraction; operators at the same level associate left-to-right.
Whitespace is accepted between tokens. Examples:

```text
=B3-C3
=(B3+C3)*2
=SUM(D2:D9)
=AVG(B2:B8, 12.5)
=MAX(B2:C8)-MIN(B2:C8)
=COUNT(A1:C12)
```

Functions are case-insensitive `SUM`, `AVG`, `MIN`, `MAX`, `COUNT`. Arguments
can be expressions, individual references, or rectangular ranges such as
`A1:C9`. Ranges are allowed only as complete function arguments. Their first
row/column must not exceed their last; reverse or out-of-grid ranges report
`#REF!`. There are no names, sheets, text literals, comparisons, string
concatenation, date types, absolute references, exponentiation, or automatic
reference rewriting for copy/fill/insert operations in this core.

For arithmetic references, EMPTY is zero; TEXT is `#VALUE!`. For a complete
function reference/range argument, EMPTY and TEXT are ignored. Numeric cells and
successful formula results count. An expression argument always supplies one
numeric result (`COUNT(A1+0)` therefore counts one even when A1 is empty).
Referenced errors propagate for every function, including COUNT.

SUM of no numeric values is zero. MIN/MAX of no numeric values is zero. COUNT
of no numeric values is zero. AVG of no numeric values is `#DIV/0!`. SUM and AVG
accumulate in a bounded signed 64-bit intermediate; SUM checks the final range,
so canceling terms may fit even when a partial sum would not. AVG divides that
exact sum by its count before narrowing. Ordinary arithmetic checks after each
operator; `2147483+1-1` reports overflow at the first addition. Products,
quotients and averages **truncate toward zero to three decimal places**;
`1/3` is `0.333`, `-1/3` is `-0.333`. There is no binary floating-point rounding.

Errors:

| Label | Meaning |
|---|---|
| `#SYNTAX!` | Incomplete/unknown expression, function or unsupported operator |
| `#REF!` | Invalid/out-of-grid A1 coordinate or invalid rectangular range |
| `#DIV/0!` | Division by zero or AVG without numeric arguments |
| `#OVERFLOW!` | Literal or result exceeds the fixed-point interval |
| `#PRECISION!` | Literal has nonzero digits beyond three decimal places |
| `#VALUE!` | Text used as an arithmetic operand |
| `#CYCLE!` | Circular dependency, or propagated circular-dependency error |
| `#DEPTH!` | More than 16 nested parenthesis/function levels |

Recalculation validates all cells before changing any cached result, resets
all calculation state, then evaluates roots in row-major order. A private
explicit depth-first stack handles missing dependencies. The parser yields on
an unseen reference, then reparses the formula after that dependency completes;
encountering an active dependency reports a cycle. No cell dependency calls the
C parser recursively. Formula failures preserve the exact source, store zero
as the unused numeric cache, and propagate the first encountered error. Editing
away a cycle and recalculating clears its old errors normally.

## Native `.bsh`: versioned, sparse and lossless

All integers are little-endian. Header, 16 bytes:

| Offset | Bytes | Value |
|---|---:|---|
| 0 | 4 | ASCII `BSH1` |
| 4 | 2 | Version 1 |
| 6 | 2 | Flags 0 |
| 8 | 2 | Rows 128 |
| 10 | 2 | Columns 26 |
| 12 | 4 | Record count, 0..3328 |

Each record has a four-byte prefix: unsigned 16-bit row-major index
`row*26+column`, unsigned 8-bit kind (1 TEXT, 2 NUMBER, 3 FORMULA), unsigned
8-bit byte length (0..95), then exactly that many source bytes without NUL.
Records must have strictly increasing indices. EMPTY cells are omitted; an
explicit empty TEXT has a zero-length record. There is no padding or trailer.

The format preserves kind, exact numeric spelling, formula source, ASCII
controls and all literal text. Cached results and private traversal state are
not serialized. Decode validates the complete header/record sequence, lengths,
source kinds and all bytes before clearing/replacing the destination, then
recalculates. Unrecognized versions, flags, dimensions, duplicates, invalid
indices, trailing bytes and invalid source bytes are rejected atomically.
Calculation errors are legitimate content and survive native save/reopen.

`sheet_native_encode` validates and measures before writing. Its bytes depend
only on source data; fresh caches are not required for native encoding. Empty
native sheets are exactly the 16-byte header. There is no checksum/authentication
field in version 1; the filesystem's storage protection is a separate layer.

## CSV interchange

Import supports comma-separated fields, LF or CRLF record terminators,
RFC4180-style quoted fields, doubled quotes, and embedded CR/LF inside quotes.
A closing quote must immediately precede a delimiter or EOF. Bare CR record
terminators, quotes inside unquoted fields, unclosed quotes and non-ASCII/BOM
input are rejected. Ragged rows are allowed. A final line terminator does not
add a phantom row; a final comma does add a final empty field. Empty input is an
empty sheet. All 128 rows and 26 fields per row count, even when empty.

CSV import treats complete decimal fields as NUMBER and every other nonempty
field as literal TEXT. `=...`, `+CMD`, `-CMD` and `@...` never execute in BaseOS
CSV import. Quotes provide CSV escaping only, not a type annotation. Apostrophes
are literal CSV text; they are not stripped by the importer. Empty fields,
including quoted empties, become EMPTY. Use native BSH1 when exact text/number
kind or explicit empty TEXT must survive reopening.

Export requires freshly recalculated caches. It writes values rather than
formula sources, using `sheet_format` for every cell, and emits the rectangular
area through the last nonempty kind. Interior/leading empty cells and rows keep
their positions. Every emitted row ends CRLF. Fields containing commas, quotes,
CR or LF are quoted, with embedded quotes doubled. An entirely empty grid emits
zero bytes. Errors export as their visible error labels; these import as text.

CSV does not carry types/formulas, so this is deliberately not a lossless native
save path. For example, literal text `0012` may import back as the number 12,
and successful formula `=2+3` exports as 5. **CSV quoting does not neutralize
formula-like literal text for other spreadsheet programs.** Such text is kept
verbatim; another application may interpret it as a formula when opening CSV.
BaseOS's conservative importer does not. This distinction should be visible in
the eventual CSV export UI/documentation rather than silently altering text.

## Atomicity and integration contract

All int APIs return 0 for accepted input and −1 for invalid input/capacity.
Calculation failures are per-cell errors, not API failure. Native decode and
CSV import validate in a first pass before changing the document; the second
pass commits known-valid cells and recalculates. Native and CSV outputs first
validate/measure in full. Too-small output buffers leave both buffer and
`written` unchanged. `out=NULL, capacity=0` queries exact bytes. Codec output is
not NUL terminated. `written` is mandatory and cannot alias the model/output;
import source cannot overlap the destination model. Input/output backing memory
must be stable and available for the declared lengths throughout each call.

The core's atomicity covers supplied memory. A desktop wrapper must still use
its filesystem's failure-atomic create/replace operation and identity checks;
it must not truncate an existing file before successful encoding and storage
admission. Native save and CSV export should remain separate actions, and CSV
export should not rebind a native document or clear its dirty state. The core
makes no external filesystem changes and imposes no file-ownership policy.

## Verification

```sh
TMPDIR=/workspace/shared/baseos-test-tmp ASAN_OPTIONS=detect_leaks=0 \
  python3 -m unittest discover -s tests -p 'test_sheet_*.py' -v
```

The ordinary-operation host suite runs with AddressSanitizer and
UndefinedBehaviorSanitizer. It covers a useful budget and updates, precedence,
signed boundary values, precision/overflow/divide-by-zero errors, function
coercion, every A1 coordinate, full 3,328-cell dependency chains and cycles,
cycle repair, expression depth, cell-copy aliases, exact native reopening,
quoted multiline CSV, CSV literal-formula import, maximum document/file sizes,
malformed format rejection, and import/export failure atomicity. Independent
Python `csv` and `struct` readers verify actual emitted interchange files.
There are no fuzzers or intentional memory-fault probes.

Freestanding i386 objects are compiled with the production flags and
`-fstack-usage`; unresolved dependencies are only `decimal_calculate`,
`platform_poll` and the documented model APIs. The model and codec add roughly
8.5 KiB of object text and zero mutable data/BSS before final-link alignment.
Measured own stack frames are at most 192 bytes; bounded expression recursion
uses roughly 4.5 KiB before platform callbacks in the production `-Os` build.
QEMU/desktop/file-binding verification belongs to the later integration and is
not claimed by these isolated host/object checks.

The included `tests/sheet_performance_host.c` also runs in the sanitizer suite.
A production-style `-Os` host run on 2026-10-04 measured:

| Ordinary workload | Host CPU time | Cooperative polls |
|---|---:|---:|
| Full 3,328-cell forward dependency chain | 0.403 ms | 6,783 |
| Full 3,328-cell circular dependency | 0.245 ms | 6,785 |
| 120-row ledger, 720 formulas, 72,000 range references | 0.567 ms | 80,228 |

These are small host measurements with a no-op device-service stub, not guest
responsiveness promises. The range workload's polls include range rows and
reparsing after forward references. Production integration must measure real
platform callbacks. No evaluation-work cutoff leaves stale results: every valid
cell reaches a value or explicit error during each complete recalculation.

The final isolated i386 build measured model text **5,649 bytes**, codec text
**2,923 bytes**, and zero mutable data/BSS in either object (8,572 bytes combined).
