#include "sheet_codec.h"
#include "platform.h"

/* Small bounded scratch only: callers own all document and output storage. */
static void poll_at(unsigned position) {
    if (!(position & 4095u)) platform_poll();
}

static int overlaps(const void *a, unsigned an, const void *b, unsigned bn) {
    uintptr_t av = (uintptr_t)a, bv = (uintptr_t)b;
    if (!an || !bn) return 0;
    return av <= bv ? bv - av < an : av - bv < bn;
}

static int valid_text(unsigned char c) {
    return (c >= 32u && c <= 126u) || c == '\t' || c == '\r' || c == '\n';
}

/* Same plain decimal syntax accepted by the model, without evaluating it. */
static int decimal_source(const char *text, unsigned length) {
    unsigned i = 0, digits = 0;
    if (i < length && (text[i] == '+' || text[i] == '-')) ++i;
    while (i < length && text[i] >= '0' && text[i] <= '9') {
        ++i;
        ++digits;
    }
    if (i < length && text[i] == '.') {
        ++i;
        while (i < length && text[i] >= '0' && text[i] <= '9') {
            ++i;
            ++digits;
        }
    }
    return digits && i == length;
}

static int valid_source(unsigned kind, const unsigned char *text, unsigned length) {
    unsigned i;
    if (length > SHEET_TEXT_MAX || kind < SHEET_TEXT || kind > SHEET_FORMULA)
        return 0;
    for (i = 0; i < length; ++i) if (!valid_text(text[i])) return 0;
    if (kind == SHEET_NUMBER) return decimal_source((const char *)text, length);
    if (kind == SHEET_FORMULA) return length && text[0] == '=';
    return 1;
}

static unsigned read_le16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned read_le32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static void write_le16(unsigned char *p, unsigned value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void write_le32(unsigned char *p, unsigned value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

int sheet_native_decode(SheetDoc *doc, const unsigned char *data, unsigned length) {
    unsigned count, position, record, prior = 0, version, prefix, start;
    if (!doc || !data || length < SHEET_NATIVE_HEADER_SIZE ||
        length > SHEET_NATIVE_MAX_SIZE || overlaps(doc, sizeof(*doc), data, length))
        return -1;
    version = read_le16(data + 4);
    if (data[0] != 'B' || data[1] != 'S' || data[2] != 'H' || data[3] != '1' ||
        (version != 1u && version != 2u) || read_le16(data + 6) != 0u ||
        read_le16(data + 8) != SHEET_ROWS || read_le16(data + 10) != SHEET_COLS)
        return -1;
    count = read_le32(data + 12);
    if (count > SHEET_CELLS) return -1;
    position = SHEET_NATIVE_HEADER_SIZE;
    prefix = version == 1u ? 4u : 5u;
    if (version == 2u) {
        if (length - position < SHEET_NATIVE_WIDTH_TABLE_SIZE) return -1;
        for (unsigned col = 0; col < SHEET_COLS; ++col) {
            unsigned width = read_le16(data + position + 2u * col);
            if (width < SHEET_COLUMN_WIDTH_MIN || width > SHEET_COLUMN_WIDTH_MAX)
                return -1;
        }
        position += SHEET_NATIVE_WIDTH_TABLE_SIZE;
    }
    start = position;
    for (record = 0; record < count; ++record) {
        unsigned index, kind, size, format;
        if (!(record & 31u)) platform_poll();
        if (length - position < prefix) return -1;
        index = read_le16(data + position);
        kind = data[position + 2u];
        size = data[position + 3u];
        format = version == 1u ? SHEET_FORMAT_GENERAL : data[position + 4u];
        position += prefix;
        if (index >= SHEET_CELLS || (record && index <= prior) ||
            size > length - position || format > SHEET_FORMAT_PERCENT) return -1;
        if (kind == SHEET_EMPTY) {
            /* Version 1 never stores EMPTY. Version 2 needs an EMPTY record
             * only when there is a nondefault format to preserve. */
            if (version == 1u || size || format == SHEET_FORMAT_GENERAL) return -1;
        } else if (!valid_source(kind, data + position, size)) return -1;
        position += size;
        prior = index;
    }
    if (position != length) return -1;

    /* Every setter below is guaranteed by the complete validation pass. */
    sheet_init(doc);
    if (version == 2u)
        for (unsigned col = 0; col < SHEET_COLS; ++col)
            doc->column_widths[col] = (uint16_t)read_le16(data + SHEET_NATIVE_HEADER_SIZE + 2u * col);
    position = start;
    for (record = 0; record < count; ++record) {
        unsigned index = read_le16(data + position);
        unsigned kind = data[position + 2u], size = data[position + 3u];
        if (!(record & 31u)) platform_poll();
        if (version == 2u) doc->formats[index] = data[position + 4u];
        position += prefix;
        (void)sheet_set(doc, index / SHEET_COLS, index % SHEET_COLS,
                        (SheetKind)kind, (const char *)data + position, size);
        position += size;
    }
    (void)sheet_recalculate(doc);
    return 0;
}

static int output_valid(const SheetDoc *doc, unsigned char *out,
                        unsigned capacity, unsigned *written) {
    return doc && written && !(!out && capacity) &&
        !overlaps(doc, sizeof(*doc), written, sizeof(*written)) &&
        !overlaps(doc, sizeof(*doc), out, capacity) &&
        !overlaps(out, capacity, written, sizeof(*written));
}

int sheet_native_encode(const SheetDoc *doc, unsigned char *out,
                        unsigned capacity, unsigned *written) {
    unsigned i, j, count = 0, needed = SHEET_NATIVE_HEADER_SIZE, position;
    unsigned version = 1u, prefix;
    if (!output_valid(doc, out, capacity, written) || sheet_validate(doc)) return -1;
    for (i = 0; i < SHEET_COLS; ++i)
        if (doc->column_widths[i] != SHEET_COLUMN_WIDTH_DEFAULT) version = 2u;
    for (i = 0; i < SHEET_CELLS; ++i) {
        if (!(i & 31u)) platform_poll();
        if (doc->formats[i] != SHEET_FORMAT_GENERAL) version = 2u;
        if (doc->cells[i].kind != SHEET_EMPTY || doc->formats[i] != SHEET_FORMAT_GENERAL) {
            needed += doc->cells[i].length;
            ++count;
        }
    }
    prefix = version == 1u ? 4u : 5u;
    needed += prefix * count;
    if (version == 2u) needed += SHEET_NATIVE_WIDTH_TABLE_SIZE;
    if (!out) {
        *written = needed;
        return 0;
    }
    if (capacity < needed) return -1;
    out[0] = 'B'; out[1] = 'S'; out[2] = 'H'; out[3] = '1';
    write_le16(out + 4, version);
    write_le16(out + 6, 0u);
    write_le16(out + 8, SHEET_ROWS);
    write_le16(out + 10, SHEET_COLS);
    write_le32(out + 12, count);
    position = SHEET_NATIVE_HEADER_SIZE;
    if (version == 2u) {
        for (i = 0; i < SHEET_COLS; ++i)
            write_le16(out + position + 2u * i, doc->column_widths[i]);
        position += SHEET_NATIVE_WIDTH_TABLE_SIZE;
    }
    for (i = 0; i < SHEET_CELLS; ++i) {
        const SheetCell *cell = &doc->cells[i];
        if (!(i & 31u)) platform_poll();
        if (cell->kind == SHEET_EMPTY && doc->formats[i] == SHEET_FORMAT_GENERAL) continue;
        write_le16(out + position, i);
        out[position + 2u] = cell->kind;
        out[position + 3u] = cell->length;
        if (version == 2u) out[position + 4u] = doc->formats[i];
        position += prefix;
        for (j = 0; j < cell->length; ++j) out[position++] = (unsigned char)cell->text[j];
    }
    *written = needed;
    return 0;
}

/* Read one field into at most 96 bytes. A closing quote must be immediately
 * followed by a delimiter or EOF. Embedded CR, LF and CRLF remain exact text. */
static int csv_field(const unsigned char *data, unsigned length, unsigned *position,
                     char *text, unsigned *size, unsigned *delimiter) {
    unsigned p = *position, n = 0, quoted = p < length && data[p] == '"';
    if (quoted) ++p;
    for (;;) {
        unsigned char c;
        if (p == length) {
            if (quoted) return -1;
            break;
        }
        poll_at(p);
        c = data[p];
        if (quoted && c == '"') {
            ++p;
            if (p < length && data[p] == '"') c = data[p++];
            else {
                quoted = 0;
                if (p < length && data[p] != ',' && data[p] != '\r' && data[p] != '\n')
                    return -1;
                break;
            }
        } else {
            if (!quoted && (c == ',' || c == '\r' || c == '\n')) break;
            if (!quoted && c == '"') return -1;
            ++p;
        }
        if (!valid_text(c) || n == SHEET_TEXT_MAX) return -1;
        text[n++] = (char)c;
    }
    *delimiter = 0;
    if (p < length) {
        if (data[p] == ',') {
            *delimiter = 1u;
            ++p;
        } else {
            if (data[p] == '\r') {
                ++p;
                if (p == length || data[p] != '\n') return -1;
            }
            ++p;
            *delimiter = 2u;
        }
    }
    text[n] = 0;
    *size = n;
    *position = p;
    return 0;
}

static int csv_read(SheetDoc *doc, const unsigned char *data, unsigned length) {
    char text[SHEET_TEXT_MAX + 1u];
    unsigned position = 0, row = 0, col = 0;
    if (!length) return 0;
    for (;;) {
        unsigned size, delimiter;
        if (row >= SHEET_ROWS || col >= SHEET_COLS ||
            csv_field(data, length, &position, text, &size, &delimiter)) return -1;
        if (doc && size) {
            SheetKind kind = decimal_source(text, size) ? SHEET_NUMBER : SHEET_TEXT;
            (void)sheet_set(doc, row, col, kind, text, size);
        }
        if (!delimiter) return 0;
        if (delimiter == 1u) ++col;
        else {
            if (position == length) return 0;
            ++row;
            col = 0;
        }
    }
}

int sheet_csv_import(SheetDoc *doc, const unsigned char *data, unsigned length) {
    if (!doc || (!data && length) || length > SHEET_CSV_MAX_SIZE ||
        overlaps(doc, sizeof(*doc), data, length) || csv_read(0, data, length)) return -1;
    sheet_init(doc);
    (void)csv_read(doc, data, length);
    (void)sheet_recalculate(doc);
    return 0;
}

typedef struct {
    unsigned char *out;
    unsigned length;
} CsvSink;

static void csv_char(CsvSink *sink, unsigned char c) {
    if (sink->out) sink->out[sink->length] = c;
    ++sink->length;
}

static int csv_write(const SheetDoc *doc, unsigned rows, unsigned cols,
                     unsigned char *out, unsigned *written) {
    CsvSink sink = { out, 0 };
    char text[SHEET_TEXT_MAX + 1u];
    unsigned row, col;
    for (row = 0; row < rows; ++row) {
        platform_poll();
        for (col = 0; col < cols; ++col) {
            const SheetCell *cell = &doc->cells[row * SHEET_COLS + col];
            unsigned size, i, quoted = 0;
            if (sheet_format(cell, text, sizeof(text), &size)) return -1;
            for (i = 0; i < size; ++i)
                if (text[i] == '"' || text[i] == ',' || text[i] == '\r' || text[i] == '\n')
                    quoted = 1;
            if (col) csv_char(&sink, ',');
            if (quoted) csv_char(&sink, '"');
            for (i = 0; i < size; ++i) {
                if (text[i] == '"') csv_char(&sink, '"');
                csv_char(&sink, (unsigned char)text[i]);
            }
            if (quoted) csv_char(&sink, '"');
        }
        csv_char(&sink, '\r');
        csv_char(&sink, '\n');
    }
    *written = sink.length;
    return 0;
}

int sheet_csv_export(const SheetDoc *doc, unsigned char *out,
                     unsigned capacity, unsigned *written) {
    unsigned i, rows = 0, cols = 0, needed;
    if (!output_valid(doc, out, capacity, written) || sheet_validate(doc)) return -1;
    for (i = 0; i < SHEET_CELLS; ++i) {
        if (!(i & 31u)) platform_poll();
        if (doc->cells[i].kind != SHEET_EMPTY) {
            unsigned row = i / SHEET_COLS + 1u, col = i % SHEET_COLS + 1u;
            if (row > rows) rows = row;
            if (col > cols) cols = col;
        }
    }
    /* Complete formatting/measurement precedes any output writes. Cache
     * freshness is the caller's responsibility, as documented in the API. */
    if (csv_write(doc, rows, cols, 0, &needed)) return -1;
    if (!out) {
        *written = needed;
        return 0;
    }
    if (capacity < needed) return -1;
    (void)csv_write(doc, rows, cols, out, &needed);
    *written = needed;
    return 0;
}
