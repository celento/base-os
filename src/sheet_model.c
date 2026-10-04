#include "sheet_model.h"
#include "decimal.h"
#include "platform.h"

/* No heap, physical address, document-sized stack, or recursive dependency
 * evaluation. The caller's work[] stack admits the longest possible chain. */
#define WAIT_DEPENDENCY 255u
#define UNSEEN 0u
#define ACTIVE 1u
#define READY 2u

static int overlaps(const void *a, unsigned an, const void *b, unsigned bn) {
    uintptr_t av = (uintptr_t)a, bv = (uintptr_t)b;
    if (!an || !bn) return 0;
    return av <= bv ? bv - av < an : av - bv < bn;
}
static int text_byte(unsigned char c) {
    return (c >= 32u && c <= 126u) || c == '\t' || c == '\r' || c == '\n';
}
static int digit(char c) { return c >= '0' && c <= '9'; }
static char upper(char c) { return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c; }
static int letter(char c) { c = upper(c); return c >= 'A' && c <= 'Z'; }
static int numeric_syntax(const char *s, unsigned n) {
    unsigned i = 0, digits = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
    while (i < n && digit(s[i])) { ++digits; ++i; }
    if (i < n && s[i] == '.') {
        ++i;
        while (i < n && digit(s[i])) { ++digits; ++i; }
    }
    return digits && i == n;
}
static unsigned number_value(const char *s, unsigned n, int negate, int32_t *value) {
    unsigned i = 0, fraction = 0, places = 0;
    uint32_t whole = 0, magnitude;
    int overflow = 0, precision = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        negate ^= s[i] == '-'; ++i;
    }
    while (i < n && digit(s[i])) {
        if (whole > 2147483u / 10u) overflow = 1;
        if (!overflow) {
            whole = whole * 10u + (unsigned)(s[i] - '0');
            if (whole > 2147483u) overflow = 1;
        }
        ++i;
    }
    if (i < n && s[i] == '.') {
        ++i;
        while (i < n && digit(s[i])) {
            if (places < 3u) fraction = fraction * 10u + (unsigned)(s[i] - '0');
            else if (s[i] != '0') precision = 1;
            ++places; ++i;
        }
    }
    if (overflow) return SHEET_ERR_OVERFLOW;
    if (precision) return SHEET_ERR_PRECISION;
    while (places++ < 3u) fraction *= 10u;
    magnitude = whole * SHEET_SCALE + fraction;
    if (magnitude > (negate ? 2147483648u : 2147483647u)) return SHEET_ERR_OVERFLOW;
    *value = negate ? (int32_t)(-(int64_t)magnitude) : (int32_t)magnitude;
    return SHEET_OK;
}

void sheet_init(SheetDoc *doc) {
    if (!doc) return;
    unsigned char *p = (unsigned char *)doc;
    for (unsigned i = 0; i < sizeof(*doc); ++i) {
        p[i] = 0;
        if (!(i & 4095u)) platform_poll();
    }
}
static int cell_valid(const SheetCell *c) {
    if (c->length > SHEET_TEXT_MAX || c->kind > SHEET_FORMULA ||
        c->text[c->length] != 0 || c->error > SHEET_ERR_DEPTH) return 0;
    for (unsigned i = 0; i < c->length; ++i)
        if (!text_byte((unsigned char)c->text[i])) return 0;
    if (c->kind == SHEET_EMPTY) return !c->length;
    if (c->kind == SHEET_NUMBER) return numeric_syntax(c->text, c->length);
    if (c->kind == SHEET_FORMULA) return c->length && c->text[0] == '=';
    return 1;
}
int sheet_validate(const SheetDoc *doc) {
    if (!doc) return -1;
    for (unsigned i = 0; i < SHEET_CELLS; ++i) {
        if (!(i & 127u)) platform_poll();
        if (!cell_valid(&doc->cells[i])) return -1;
    }
    return 0;
}
int sheet_set(SheetDoc *doc, unsigned row, unsigned col, SheetKind kind,
              const char *text, unsigned length) {
    SheetCell next = {{0}, 0, 0, 0, 0, 0};
    if (!doc || row >= SHEET_ROWS || col >= SHEET_COLS || kind > SHEET_AUTO ||
        (int)kind < 0 || length > SHEET_TEXT_MAX || (!text && length)) return -1;
    for (unsigned i = 0; i < length; ++i)
        if (!text_byte((unsigned char)text[i])) return -1;
    if (kind == SHEET_AUTO) {
        if (!length) kind = SHEET_EMPTY;
        else if (*text == '\'') { kind = SHEET_TEXT; ++text; --length; }
        else if (*text == '=') kind = SHEET_FORMULA;
        else kind = numeric_syntax(text, length) ? SHEET_NUMBER : SHEET_TEXT;
    }
    next.kind = (uint8_t)kind;
    next.length = (uint8_t)length;
    for (unsigned i = 0; i < length; ++i) next.text[i] = text[i];
    if (!cell_valid(&next)) return -1;
    doc->cells[row * SHEET_COLS + col] = next;
    return 0;
}
const SheetCell *sheet_cell(const SheetDoc *doc, unsigned row, unsigned col) {
    return doc && row < SHEET_ROWS && col < SHEET_COLS
        ? &doc->cells[row * SHEET_COLS + col] : (const SheetCell *)0;
}

int sheet_reference(const char *text, unsigned length, unsigned *row, unsigned *col) {
    unsigned r = 0;
    if (!text || !row || !col || row == col || length < 2u || length > 4u ||
        !letter(text[0]) || text[1] < '1' || text[1] > '9' ||
        overlaps(text, length, row, sizeof(*row)) ||
        overlaps(text, length, col, sizeof(*col)) ||
        overlaps(row, sizeof(*row), col, sizeof(*col))) return -1;
    for (unsigned i = 1; i < length; ++i) {
        if (!digit(text[i])) return -1;
        r = r * 10u + (unsigned)(text[i] - '0');
    }
    if (!r || r > SHEET_ROWS) return -1;
    *row = r - 1u; *col = (unsigned)(upper(text[0]) - 'A');
    return 0;
}
int sheet_label(unsigned row, unsigned col, char *out, unsigned capacity) {
    char s[5];
    unsigned n = 1;
    if (row >= SHEET_ROWS || col >= SHEET_COLS || !out) return -1;
    s[0] = (char)('A' + col); ++row;
    if (row >= 100u) s[n++] = (char)('0' + row / 100u);
    if (row >= 10u) s[n++] = (char)('0' + row / 10u % 10u);
    s[n++] = (char)('0' + row % 10u); s[n] = 0;
    if (capacity <= n) return -1;
    for (unsigned i = 0; i <= n; ++i) out[i] = s[i];
    return 0;
}

typedef struct {
    SheetDoc *doc;
    const char *text;
    unsigned length, position, depth, error, pending;
    unsigned *poll_work;
} Parser;
/* One shared counter spans reparses and the explicit dependency stack. Counting
 * cells/references, rather than range rows, avoids a device callback per item. */
static void work_poll(unsigned *work) {
    if (!(++*work & 255u)) platform_poll();
}
static void spaces(Parser *p) {
    while (p->position < p->length) {
        char c = p->text[p->position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++p->position;
    }
}
static char peek(Parser *p) {
    spaces(p);
    return p->position < p->length ? p->text[p->position] : 0;
}
static int32_t expression(Parser *p);
static int32_t reference_value(Parser *p, unsigned index, int aggregate, int *numeric) {
    SheetCell *cell = &p->doc->cells[index];
    work_poll(p->poll_work);
    if (cell->state == UNSEEN) {
        p->pending = index; p->error = WAIT_DEPENDENCY; return 0;
    }
    if (cell->state == ACTIVE) { p->error = SHEET_ERR_CYCLE; return 0; }
    if (cell->error) { p->error = cell->error; return 0; }
    *numeric = cell->kind == SHEET_NUMBER || cell->kind == SHEET_FORMULA;
    if (cell->kind == SHEET_TEXT && !aggregate) p->error = SHEET_ERR_VALUE;
    return *numeric ? cell->value : 0;
}
/* Tokenizes all letters/digits so AA1 and A129 produce #REF!, not truncation. */
static int read_reference(Parser *p, unsigned *index) {
    unsigned start = p->position, row, col;
    while (p->position < p->length && letter(p->text[p->position])) ++p->position;
    while (p->position < p->length && digit(p->text[p->position])) ++p->position;
    if (sheet_reference(p->text + start, p->position - start, &row, &col)) {
        p->error = SHEET_ERR_REF; return 0;
    }
    *index = row * SHEET_COLS + col;
    return 1;
}
static uint64_t divide_u64(uint64_t n, uint32_t d) {
    uint64_t q = 0, r = 0;
    for (int bit = 63; bit >= 0; --bit) {
        r = (r << 1) | ((n >> bit) & 1u);
        if (r >= d) { r -= d; q |= (uint64_t)1u << bit; }
    }
    return q;
}
typedef struct { int64_t sum; int32_t min, max; unsigned count; } Aggregate;
static void accumulate(Aggregate *a, int32_t v) {
    if (!a->count || v < a->min) a->min = v;
    if (!a->count || v > a->max) a->max = v;
    a->sum += v; ++a->count;
}
static int name_equal(const char *s, unsigned n, const char *name) {
    unsigned i = 0;
    while (name[i]) { if (i == n || upper(s[i]) != name[i]) return 0; ++i; }
    return i == n;
}
static int32_t function(Parser *p, unsigned start, unsigned length) {
    Aggregate a = {0, 0, 0, 0};
    unsigned which = 0;
    if (name_equal(p->text + start, length, "SUM")) which = 1;
    else if (name_equal(p->text + start, length, "AVG")) which = 2;
    else if (name_equal(p->text + start, length, "MIN")) which = 3;
    else if (name_equal(p->text + start, length, "MAX")) which = 4;
    else if (name_equal(p->text + start, length, "COUNT")) which = 5;
    if (!which) { p->error = SHEET_ERR_SYNTAX; return 0; }
    ++p->position; /* '(' */
    if (peek(p) != ')') for (;;) {
        unsigned first = 0, last = 0, saved = p->position;
        int ref_arg = 0;
        /* Range or standalone reference arguments ignore blanks and text.
         * References inside expressions retain arithmetic coercion rules. */
        if (letter(peek(p))) {
            unsigned scan = p->position;
            while (scan < p->length && letter(p->text[scan])) ++scan;
            if (scan < p->length && digit(p->text[scan])) {
                if (!read_reference(p, &first)) return 0;
                char c = peek(p);
                if (c == ':') {
                    ++p->position; spaces(p);
                    if (!read_reference(p, &last)) return 0;
                    ref_arg = 1;
                } else if (c == ',' || c == ')') { last = first; ref_arg = 1; }
            }
        }
        if (ref_arg) {
            unsigned r0 = first / SHEET_COLS, c0 = first % SHEET_COLS;
            unsigned r1 = last / SHEET_COLS, c1 = last % SHEET_COLS;
            if (r0 > r1 || c0 > c1) { p->error = SHEET_ERR_REF; return 0; }
            for (unsigned r = r0; r <= r1; ++r) {
                for (unsigned c = c0; c <= c1; ++c) {
                    int numeric = 0;
                    int32_t v = reference_value(p, r * SHEET_COLS + c, 1, &numeric);
                    if (p->error) return 0;
                    if (numeric) accumulate(&a, v);
                }
            }
        } else {
            p->position = saved;
            int32_t v = expression(p);
            if (p->error) return 0;
            accumulate(&a, v);
        }
        char c = peek(p);
        if (c == ')') break;
        if (c != ',') { p->error = SHEET_ERR_SYNTAX; return 0; }
        ++p->position;
    }
    if (peek(p) != ')') { p->error = SHEET_ERR_SYNTAX; return 0; }
    ++p->position;
    if (which == 5) return (int32_t)(a.count * SHEET_SCALE);
    if (which == 3) return a.count ? a.min : 0;
    if (which == 4) return a.count ? a.max : 0;
    if (which == 2) {
        if (!a.count) { p->error = SHEET_ERR_DIV0; return 0; }
        uint64_t magnitude = divide_u64((uint64_t)(a.sum < 0 ? -a.sum : a.sum), a.count);
        return a.sum < 0 ? (int32_t)(-(int64_t)magnitude) : (int32_t)magnitude;
    }
    if (a.sum < INT32_MIN || a.sum > INT32_MAX) { p->error = SHEET_ERR_OVERFLOW; return 0; }
    return (int32_t)a.sum;
}
static int32_t primary(Parser *p) {
    int32_t value = 0;
    char c = peek(p);
    if (c == '(') {
        if (p->depth == SHEET_PARSE_DEPTH) { p->error = SHEET_ERR_DEPTH; return 0; }
        ++p->depth; ++p->position;
        value = expression(p);
        if (!p->error) {
            if (peek(p) != ')') p->error = SHEET_ERR_SYNTAX;
            else ++p->position;
        }
        --p->depth;
    } else if (letter(c)) {
        unsigned start = p->position, index;
        while (p->position < p->length && letter(p->text[p->position])) ++p->position;
        unsigned end = p->position;
        if (peek(p) == '(') {
            if (p->depth == SHEET_PARSE_DEPTH) { p->error = SHEET_ERR_DEPTH; return 0; }
            ++p->depth;
            value = function(p, start, end - start);
            --p->depth;
        }
        else {
            p->position = start;
            if (read_reference(p, &index)) {
                int numeric = 0;
                value = reference_value(p, index, 0, &numeric);
            }
        }
    } else p->error = SHEET_ERR_SYNTAX;
    return value;
}
static int32_t unary(Parser *p) {
    int negative = 0;
    char c = peek(p);
    while (c == '+' || c == '-') {
        negative ^= c == '-'; ++p->position; c = peek(p);
    }
    if (digit(c) || c == '.') {
        unsigned start = p->position, digits = 0;
        while (p->position < p->length && digit(p->text[p->position])) { ++p->position; ++digits; }
        if (p->position < p->length && p->text[p->position] == '.') {
            ++p->position;
            while (p->position < p->length && digit(p->text[p->position])) { ++p->position; ++digits; }
        }
        int32_t value = 0;
        p->error = digits ? number_value(p->text + start, p->position - start, negative, &value)
                          : SHEET_ERR_SYNTAX;
        return value;
    }
    int32_t value = primary(p);
    if (!p->error && negative) {
        if (value == INT32_MIN) p->error = SHEET_ERR_OVERFLOW;
        else value = -value;
    }
    return value;
}
static int32_t calculate(Parser *p, int32_t a, int32_t b, char op) {
    int32_t result = 0;
    if (op == '/' && !b) p->error = SHEET_ERR_DIV0;
    else if (!decimal_calculate(a, b, op, &result)) p->error = SHEET_ERR_OVERFLOW;
    return result;
}
static int32_t term(Parser *p) {
    int32_t value = unary(p);
    while (!p->error) {
        char op = peek(p);
        if (op != '*' && op != '/') break;
        ++p->position;
        int32_t right = unary(p);
        if (!p->error) value = calculate(p, value, right, op);
    }
    return value;
}
static int32_t expression(Parser *p) {
    int32_t value = term(p);
    while (!p->error) {
        char op = peek(p);
        if (op != '+' && op != '-') break;
        ++p->position;
        int32_t right = term(p);
        if (!p->error) value = calculate(p, value, right, op);
    }
    return value;
}
int sheet_recalculate(SheetDoc *doc) {
    unsigned poll_work = 0;
    platform_poll();
    if (sheet_validate(doc)) { platform_poll(); return -1; }
    for (unsigned i = 0; i < SHEET_CELLS; ++i) {
        work_poll(&poll_work);
        SheetCell *cell = &doc->cells[i];
        cell->value = 0; cell->error = 0;
        cell->state = cell->kind == SHEET_FORMULA ? UNSEEN : READY;
        if (cell->kind == SHEET_NUMBER)
            cell->error = (uint8_t)number_value(cell->text, cell->length, 0, &cell->value);
    }
    for (unsigned root = 0; root < SHEET_CELLS; ++root) {
        work_poll(&poll_work);
        if (doc->cells[root].state == READY) continue;
        unsigned depth = 1;
        doc->work[0] = (uint16_t)root;
        doc->cells[root].state = ACTIVE;
        while (depth) {
            SheetCell *cell = &doc->cells[doc->work[depth - 1u]];
            Parser parser = {doc, cell->text, cell->length, 1, 0, 0, 0, &poll_work};
            int32_t value = expression(&parser);
            if (parser.error == WAIT_DEPENDENCY) {
                /* Each push is an UNSEEN cell, so no push exceeds SHEET_CELLS. */
                doc->work[depth++] = (uint16_t)parser.pending;
                doc->cells[parser.pending].state = ACTIVE;
            } else {
                if (!parser.error && peek(&parser)) parser.error = SHEET_ERR_SYNTAX;
                cell->value = parser.error ? 0 : value;
                cell->error = (uint8_t)parser.error;
                cell->state = READY;
                --depth;
            }
            work_poll(&poll_work);
        }
    }
    platform_poll();
    return 0;
}
const char *sheet_error_name(unsigned error) {
    static const char *const errors[] = {
        "", "#SYNTAX!", "#REF!", "#DIV/0!", "#OVERFLOW!", "#PRECISION!",
        "#VALUE!", "#CYCLE!", "#DEPTH!"
    };
    return error <= SHEET_ERR_DEPTH ? errors[error] : "#INVALID!";
}
int sheet_format(const SheetCell *cell, char *out, unsigned capacity, unsigned *written) {
    char number[16];
    const char *s;
    unsigned n = 0;
    if (!cell || !written || (!out && capacity) || !cell_valid(cell) ||
        overlaps(cell, sizeof(*cell), out, capacity) ||
        overlaps(cell, sizeof(*cell), written, sizeof(*written)) ||
        overlaps(out, capacity, written, sizeof(*written))) return -1;
    if (cell->error) {
        s = sheet_error_name(cell->error);
        while (s[n]) ++n;
    } else if (cell->kind == SHEET_TEXT || cell->kind == SHEET_EMPTY) {
        s = cell->text; n = cell->length;
    } else {
        uint32_t magnitude = cell->value < 0 ? (uint32_t)(-(int64_t)cell->value) : (uint32_t)cell->value;
        uint32_t whole = magnitude / SHEET_SCALE, fraction = magnitude % SHEET_SCALE;
        char reversed[10]; unsigned digits = 0;
        if (cell->value < 0) number[n++] = '-';
        do { reversed[digits++] = (char)('0' + whole % 10u); whole /= 10u; } while (whole);
        while (digits) number[n++] = reversed[--digits];
        if (fraction) {
            number[n++] = '.';
            number[n++] = (char)('0' + fraction / 100u);
            number[n++] = (char)('0' + fraction / 10u % 10u);
            number[n++] = (char)('0' + fraction % 10u);
            while (number[n - 1u] == '0') --n;
        }
        number[n] = 0; s = number;
    }
    if (out) {
        if (capacity <= n) return -1;
        for (unsigned i = 0; i < n; ++i) out[i] = s[i];
        out[n] = 0;
    }
    *written = n;
    return 0;
}
