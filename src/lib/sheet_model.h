#ifndef BASEOS_SHEET_MODEL_H
#define BASEOS_SHEET_MODEL_H
#include <stdint.h>

#define SHEET_COLS 26u
#define SHEET_ROWS 128u
#define SHEET_CELLS (SHEET_COLS * SHEET_ROWS)
#define SHEET_TEXT_MAX 95u
#define SHEET_SCALE 1000
#define SHEET_PARSE_DEPTH 16u
#define SHEET_COLUMN_WIDTH_DEFAULT 104u
#define SHEET_COLUMN_WIDTH_MIN 48u
#define SHEET_COLUMN_WIDTH_MAX 320u

/* AUTO is accepted only by sheet_set; it is never stored. */
typedef enum {
    SHEET_EMPTY = 0, SHEET_TEXT = 1, SHEET_NUMBER = 2,
    SHEET_FORMULA = 3, SHEET_AUTO = 4
} SheetKind;
typedef enum {
    SHEET_OK = 0, SHEET_ERR_SYNTAX, SHEET_ERR_REF, SHEET_ERR_DIV0,
    SHEET_ERR_OVERFLOW, SHEET_ERR_PRECISION, SHEET_ERR_VALUE,
    SHEET_ERR_CYCLE, SHEET_ERR_DEPTH
} SheetError;
/* Display metadata never changes source, cached values or CSV interchange. */
typedef enum {
    SHEET_FORMAT_GENERAL = 0, SHEET_FORMAT_FIXED2 = 1,
    SHEET_FORMAT_CURRENCY = 2, SHEET_FORMAT_PERCENT = 3
} SheetFormat;
typedef struct {
    char text[SHEET_TEXT_MAX + 1u];
    int32_t value;                 /* Fixed point, three decimal places. */
    uint8_t length, kind, error, state;
} SheetCell;
typedef struct {
    SheetCell cells[SHEET_CELLS];  /* Row-major, A1 at index zero. */
    uint16_t work[SHEET_CELLS];    /* Private bounded dependency stack. */
    uint8_t formats[SHEET_CELLS];  /* SheetFormat, including empty cells. */
    uint16_t column_widths[SHEET_COLS]; /* Actual pixels, not zero sentinels. */
} SheetDoc;

/* No allocation/global model; callers own and correctly align the document.
 * Initialization clears the object and installs default column widths.
 * All int operations return 0 on
 * success and -1 on invalid input/capacity, preserving outputs on failure.
 * Source bytes are printable ASCII, TAB, CR or LF; NUL is not a text byte.
 * Source storage beyond length is ignored. Formula source includes '='.
 * Valid numbers use plain decimal notation; exponent notation is text in AUTO.
 * Numeric values outside [-2147483.648,2147483.647], or with nonzero digits
 * beyond three decimal places, become explicit calculation errors, not text.
 * AUTO: empty => EMPTY, leading apostrophe => TEXT with prefix removed,
 * leading '=' => FORMULA, valid decimal syntax => NUMBER, otherwise TEXT.
 * TEXT permits empty text and retains literal leading '=' or apostrophe.
 * set accepts source within doc (e.g. copying a cell); validates before writing.
 * Editing preserves cell format and column widths; it does not recalculate.
 * Call sheet_recalculate before reading caches.
 */
void sheet_init(SheetDoc *doc);
int sheet_validate(const SheetDoc *doc);
int sheet_set(SheetDoc *doc, unsigned row, unsigned col, SheetKind kind,
              const char *text, unsigned length);
const SheetCell *sheet_cell(const SheetDoc *doc, unsigned row, unsigned col);
int sheet_recalculate(SheetDoc *doc);
const char *sheet_error_name(unsigned error);

/* Metadata setters validate coordinates and new values, preserving the document
 * on failure. Getters also reject invalid stored values; output is required and
 * cannot overlap the document. Setters do not change content or recalculate.
 * A formatted empty cell retains its format when subsequently edited/cleared.
 */
int sheet_get_format(const SheetDoc *doc, unsigned row, unsigned col,
                     SheetFormat *format);
int sheet_set_format(SheetDoc *doc, unsigned row, unsigned col, SheetFormat format);
int sheet_get_column_width(const SheetDoc *doc, unsigned col, unsigned *width);
int sheet_set_column_width(SheetDoc *doc, unsigned col, unsigned width);

/* UI-only cached display, with the same output/measurement contract as
 * sheet_format below, but neither output may overlap any part of doc.
 * GENERAL delegates to sheet_format. FIXED2 and CURRENCY round to two decimals,
 * halfway away from zero, suppressing negative zero; CURRENCY uses -$1.23.
 * PERCENT displays value * 100 with exactly two decimals, e.g. 0.125 => 12.50%.
 * Text, empty cells and errors are unaffected by their retained format.
 * Stored values/source and calculation precision never change.
 */
int sheet_format_display(const SheetDoc *doc, unsigned row, unsigned col,
                         char *out, unsigned capacity, unsigned *written);

/* Formats a cached numeric/formula result, text verbatim, or empty as empty.
 * NUL-terminated output. written excludes NUL; required capacity includes it.
 * out=NULL,capacity=0 measures. written is required; out/written must not alias
 * each other or cell. Rendering text with TAB/CR/LF is the UI's responsibility.
 */
int sheet_format(const SheetCell *cell, char *out, unsigned capacity,
                 unsigned *written);
/* A1..Z128, case-insensitive, no '$' or whitespace; row/col zero-based. */
int sheet_reference(const char *text, unsigned length,
                    unsigned *row, unsigned *col);
/* NUL-terminated A1-style label; capacity at least five is always sufficient. */
int sheet_label(unsigned row, unsigned col, char *out, unsigned capacity);
#endif
