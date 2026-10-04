#ifndef BASEOS_SHEET_CODEC_H
#define BASEOS_SHEET_CODEC_H
#include "sheet_model.h"
#define SHEET_NATIVE_HEADER_SIZE 16u
#define SHEET_NATIVE_V1_MAX_SIZE (SHEET_NATIVE_HEADER_SIZE + SHEET_CELLS * (4u + SHEET_TEXT_MAX))
#define SHEET_NATIVE_WIDTH_TABLE_SIZE (2u * SHEET_COLS)
#define SHEET_NATIVE_MAX_SIZE (SHEET_NATIVE_HEADER_SIZE + SHEET_NATIVE_WIDTH_TABLE_SIZE + \
                               SHEET_CELLS * (5u + SHEET_TEXT_MAX))
#define SHEET_CSV_MAX_SIZE (SHEET_CELLS * (2u * SHEET_TEXT_MAX + 3u) + SHEET_ROWS)
/* Sparse, lossless BSH1 native format retains source/kinds and display metadata.
 * Version 1 is emitted byte-for-byte as before when metadata is all default.
 * Version 2 adds a complete column-width table and a format byte per record;
 * formatted empty cells are retained. Both versions are accepted. See
 * docs/SHEET_FORMAT.md for the exact validated wire layout and size bounds.
 * CSV is value interchange: export unformatted calculated values/text, import
 * plain decimal fields as NUMBER and every other nonempty field as TEXT.
 * Formula-like CSV fields are literal text, never executed; quotes do not
 * force numbers to text (CSV has no type information). Empty fields => EMPTY.
 * Import accepts CRLF or LF record terminators, comma separators, RFC4180-style
 * quoted fields, doubled quotes and embedded CR/LF. Bare CR record terminators
 * and quotes inside unquoted fields are rejected. No UTF-8/BOM support.
 * Import accepts ragged rows but never silently truncates cells/rows/columns.
 * A final record terminator does not create an extra row. Empty input is empty.
 * Complete validation precedes mutation. On failure document/output/written
 * are unchanged. Decode/import recalculate the accepted document.
 * Source and destination ranges must not overlap, including written; overlap
 * is rejected. All inputs must remain stable until the operation returns.
 * NULL input is accepted only for zero-length CSV. NULL out + zero capacity
 * measures exact output bytes. written is required; output has no NUL byte.
 * CSV export requires recalculated caches (call sheet_recalculate after edits).
 * Native encoding uses only source/kind/metadata and needs no fresh caches.
 * CSV ignores display formats/widths; CSV import installs default metadata.
 */
int sheet_native_decode(SheetDoc *doc, const unsigned char *data, unsigned length);
int sheet_native_encode(const SheetDoc *doc, unsigned char *out,
                        unsigned capacity, unsigned *written);
int sheet_csv_import(SheetDoc *doc, const unsigned char *data, unsigned length);
int sheet_csv_export(const SheetDoc *doc, unsigned char *out,
                     unsigned capacity, unsigned *written);
#endif
