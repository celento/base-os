#include "writer_codec.h"
#include "platform.h"

/* All work is bounded by WRITER_TEXT_MAX. No heap or document-sized stack. */
static void poll_at(unsigned position) {
    if (!(position & 1023u)) platform_poll();
}

static int overlaps(const void *a, unsigned an, const void *b, unsigned bn) {
    uintptr_t av = (uintptr_t)a, bv = (uintptr_t)b;
    if (!an || !bn) return 0;
    return av <= bv ? bv - av < an : av - bv < bn;
}

static int valid_text(unsigned char c) {
    return (c >= 32u && c <= 126u) || c == '\t' || c == '\n';
}

static int valid_arrays(const unsigned char *text, const unsigned char *style,
                        const unsigned char *paragraph, unsigned length) {
    unsigned i;
    for (i = 0; i <= length; ++i) {
        unsigned p = paragraph[i];
        poll_at(i);
        if (i < length && !valid_text(text[i])) return -1;
        if (style[i] & ~WRITER_STYLE_MASK) return -1;
        if ((p & ~WRITER_PARAGRAPH_MASK) ||
            (p & WRITER_ALIGN_MASK) == WRITER_ALIGN_MASK) return -1;
        if (i && text[i - 1u] != '\n' && p) return -1;
    }
    return 0;
}

void writer_doc_init(WriterDoc *doc) {
    unsigned i;
    if (!doc) return;
    doc->length = 0;
    for (i = 0; i <= WRITER_TEXT_MAX; ++i) {
        poll_at(i);
        if (i < WRITER_TEXT_MAX) doc->text[i] = 0;
        doc->style[i] = 0;
        doc->paragraph[i] = 0;
    }
}

int writer_doc_validate(const WriterDoc *doc) {
    if (!doc || doc->length > WRITER_TEXT_MAX) return -1;
    return valid_arrays(doc->text, doc->style, doc->paragraph, doc->length);
}

int writer_plain_import(WriterDoc *doc, const unsigned char *data, unsigned length) {
    unsigned i, n = 0;
    if (!doc || length > WRITER_PLAIN_MAX_INPUT || (!data && length) ||
        overlaps(doc, sizeof(*doc), data, length)) return -1;
    for (i = 0; i < length; ++i) {
        poll_at(n);
        if (data[i] == '\r') {
            if (i + 1u == length || data[i + 1u] != '\n') return -1;
            ++i;
        } else if (!valid_text(data[i])) return -1;
        if (n == WRITER_TEXT_MAX) return -1;
        ++n;
    }
    /* Commit only after every source byte has been accepted. */
    writer_doc_init(doc);
    n = 0;
    for (i = 0; i < length; ++i) {
        poll_at(n);
        if (data[i] == '\r') ++i;
        doc->text[n++] = data[i];
    }
    doc->length = n;
    return 0;
}

static unsigned read_le16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned read_le32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static void write_le32(unsigned char *p, unsigned value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

int writer_native_decode(WriterDoc *doc, const unsigned char *data, unsigned length) {
    const unsigned char *text, *style, *paragraph;
    unsigned n, i;
    if (!doc || !data || length < WRITER_NATIVE_HEADER_SIZE ||
        overlaps(doc, sizeof(*doc), data, length)) return -1;
    if (data[0] != 'B' || data[1] != 'W' || data[2] != 'R' || data[3] != '1' ||
        read_le16(data + 4) != 1u || read_le16(data + 6) != 0u ||
        read_le32(data + 12) != 0u) return -1;
    n = read_le32(data + 8);
    if (n > WRITER_TEXT_MAX || length != WRITER_NATIVE_HEADER_SIZE + 3u * n + 2u)
        return -1;
    text = data + WRITER_NATIVE_HEADER_SIZE;
    style = text + n;
    paragraph = style + n + 1u;
    if (valid_arrays(text, style, paragraph, n)) return -1;
    writer_doc_init(doc);
    for (i = 0; i <= n; ++i) {
        poll_at(i);
        if (i < n) doc->text[i] = text[i];
        doc->style[i] = style[i];
        doc->paragraph[i] = paragraph[i];
    }
    doc->length = n;
    return 0;
}

static int output_valid(const WriterDoc *doc, unsigned char *out,
                        unsigned capacity, unsigned *written) {
    if (!doc || !written || (!out && capacity) ||
        overlaps(doc, sizeof(*doc), written, sizeof(*written)) ||
        overlaps(doc, sizeof(*doc), out, capacity) ||
        overlaps(out, capacity, written, sizeof(*written))) return 0;
    return 1;
}

int writer_native_encode(const WriterDoc *doc, unsigned char *out,
                         unsigned capacity, unsigned *written) {
    unsigned n, needed, i;
    unsigned char *text, *style, *paragraph;
    if (!output_valid(doc, out, capacity, written) || writer_doc_validate(doc)) return -1;
    n = doc->length;
    needed = WRITER_NATIVE_HEADER_SIZE + 3u * n + 2u;
    if (!out) {
        *written = needed;
        return 0;
    }
    if (capacity < needed) return -1;
    out[0] = 'B'; out[1] = 'W'; out[2] = 'R'; out[3] = '1';
    out[4] = 1; out[5] = 0; out[6] = 0; out[7] = 0;
    write_le32(out + 8, n);
    write_le32(out + 12, 0);
    text = out + WRITER_NATIVE_HEADER_SIZE;
    style = text + n;
    paragraph = style + n + 1u;
    for (i = 0; i <= n; ++i) {
        poll_at(i);
        if (i < n) text[i] = doc->text[i];
        style[i] = doc->style[i];
        paragraph[i] = doc->paragraph[i];
    }
    *written = needed;
    return 0;
}

typedef struct {
    unsigned char *out;
    unsigned length;
} RtfSink;

static void rtf_char(RtfSink *sink, unsigned char c) {
    if (sink->out) sink->out[sink->length] = c;
    ++sink->length;
}

static void rtf_string(RtfSink *sink, const char *s) {
    while (*s) rtf_char(sink, (unsigned char)*s++);
}

/* Uses only RTF 1.9.1 basic paragraph/character controls; see WRITER_FORMAT.md.
 * A control word delimiter is emitted once after the last changed property. */
static unsigned rtf_write(const WriterDoc *doc, unsigned char *out) {
    RtfSink sink = { out, 0 };
    unsigned i, prior_style = ~0u, prior_paragraph = ~0u;
    rtf_string(&sink, "{\\rtf1\\ansi\\ansicpg1252\\deff0"
                     "{\\fonttbl{\\f0\\fswiss Arial;}}\\pard\\plain\\f0\n");
    for (i = 0; i <= doc->length; ++i) {
        unsigned style = doc->style[i], changed = 0;
        poll_at(i);
        if (!i || doc->text[i - 1u] == '\n') {
            unsigned paragraph = doc->paragraph[i];
            if (paragraph != prior_paragraph) {
                unsigned alignment = paragraph & WRITER_ALIGN_MASK;
                rtf_string(&sink, "\\pard");
                rtf_string(&sink, alignment == WRITER_ALIGN_CENTER ? "\\qc" :
                                  alignment == WRITER_ALIGN_RIGHT ? "\\qr" : "\\ql");
                rtf_string(&sink, paragraph & WRITER_PARAGRAPH_HEADING ? "\\fs36" : "\\fs24");
                prior_paragraph = paragraph;
                changed = 1;
            }
        }
        if (!i || ((style ^ prior_style) & WRITER_STYLE_BOLD)) {
            rtf_string(&sink, style & WRITER_STYLE_BOLD ? "\\b" : "\\b0");
            changed = 1;
        }
        if (!i || ((style ^ prior_style) & WRITER_STYLE_ITALIC)) {
            rtf_string(&sink, style & WRITER_STYLE_ITALIC ? "\\i" : "\\i0");
            changed = 1;
        }
        if (!i || ((style ^ prior_style) & WRITER_STYLE_UNDERLINE)) {
            rtf_string(&sink, style & WRITER_STYLE_UNDERLINE ? "\\ul" : "\\ul0");
            changed = 1;
        }
        prior_style = style;
        if (changed) rtf_char(&sink, ' ');
        if (i == doc->length) break;
        if (doc->text[i] == '\n') rtf_string(&sink, "\\par\n");
        else if (doc->text[i] == '\t') rtf_string(&sink, "\\tab ");
        else {
            if (doc->text[i] == '\\' || doc->text[i] == '{' || doc->text[i] == '}')
                rtf_char(&sink, '\\');
            rtf_char(&sink, doc->text[i]);
        }
    }
    rtf_char(&sink, '}');
    return sink.length;
}

int writer_rtf_export(const WriterDoc *doc, unsigned char *out,
                      unsigned capacity, unsigned *written) {
    unsigned needed;
    if (!output_valid(doc, out, capacity, written) || writer_doc_validate(doc)) return -1;
    /* Each input position emits at most 29 bytes; all counters fit unsigned. */
    needed = rtf_write(doc, 0);
    if (!out) {
        *written = needed;
        return 0;
    }
    if (capacity < needed) return -1;
    rtf_write(doc, out);
    *written = needed;
    return 0;
}
