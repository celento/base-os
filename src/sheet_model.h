#ifndef BASEOS_SHEET_MODEL_H
#define BASEOS_SHEET_MODEL_H
#include <stdint.h>

#define SHEET_COLS 26u
#define SHEET_ROWS 128u
#define SHEET_CELLS (SHEET_COLS * SHEET_ROWS)
#define SHEET_TEXT_MAX 95u
#define SHEET_SCALE 1000
#define SHEET_PARSE_DEPTH 16u

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
typedef struct {
    char text[SHEET_TEXT_MAX + 1u];
    int32_t value;                 /* Fixed point, three decimal places. */
    uint8_t length, kind, error, state;
} SheetCell;
typedef struct {
    SheetCell cells[SHEET_CELLS];  /* Row-major, A1 at index zero. */
    uint16_t work[SHEET_CELLS];    /* Private bounded dependency stack. */
} SheetDoc;

/* No allocation/global model; callers own and correctly align the document.
 * Initialization clears the complete object. All int operations return 0 on
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
 * Editing does not recalculate. Call sheet_recalculate before reading caches.
 */
void sheet_init(SheetDoc *doc);
int sheet_validate(const SheetDoc *doc);
int sheet_set(SheetDoc *doc, unsigned row, unsigned col, SheetKind kind,
              const char *text, unsigned length);
const SheetCell *sheet_cell(const SheetDoc *doc, unsigned row, unsigned col);
int sheet_recalculate(SheetDoc *doc);
const char *sheet_error_name(unsigned error);

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
