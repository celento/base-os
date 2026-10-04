/* Original bounded PDF serializer/layout. All lengths and coordinates fit 32 bits.
 * Font metric data below is derived from Adobe AFM files, with permission:
 * Copyright (c) 1985, 1987, 1989, 1990, 1993, 1997 Adobe Systems Incorporated.
 * All Rights Reserved. Times is a trademark of Linotype-Hell AG and/or its
 * subsidiaries. See third_party/adobe-core14/{NOTICE,readme}.txt.
 * MODIFIED: four 95-glyph WinAnsi advance-only tables; no AFM program/kerning.
 */
#include "writer_pdf.h"
#include "platform.h"

/* ASCII 32..126, 1/1000 em. Order is regular, bold, italic, bold italic.
 * PDF WinAnsiEncoding uses quotesingle at 39 and grave at 96. */
static const unsigned short widths[4][95] = {
    { /* Times-Roman */
        250, 333, 408, 500, 500, 833, 778, 180, 333, 333, 500, 564, 250, 333, 250, 278,
        500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 278, 278, 564, 564, 564, 444,
        921, 722, 667, 667, 722, 611, 556, 722, 722, 333, 389, 722, 611, 889, 722, 722,
        556, 722, 667, 556, 611, 722, 722, 944, 722, 722, 611, 333, 278, 333, 469, 500,
        333, 444, 500, 444, 500, 444, 333, 500, 500, 278, 278, 500, 278, 778, 500, 500,
        500, 500, 333, 389, 278, 500, 500, 722, 500, 500, 444, 480, 200, 480, 541
    },
    { /* Times-Bold */
        250, 333, 555, 500, 500, 1000, 833, 278, 333, 333, 500, 570, 250, 333, 250, 278,
        500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 333, 333, 570, 570, 570, 500,
        930, 722, 667, 722, 722, 667, 611, 778, 778, 389, 500, 778, 667, 944, 722, 778,
        611, 778, 722, 556, 667, 722, 722, 1000, 722, 722, 667, 333, 278, 333, 581, 500,
        333, 500, 556, 444, 556, 444, 333, 500, 556, 278, 333, 556, 278, 833, 556, 500,
        556, 556, 444, 389, 333, 556, 500, 722, 500, 500, 444, 394, 220, 394, 520
    },
    { /* Times-Italic */
        250, 333, 420, 500, 500, 833, 778, 214, 333, 333, 500, 675, 250, 333, 250, 278,
        500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 333, 333, 675, 675, 675, 500,
        920, 611, 611, 667, 722, 611, 611, 722, 722, 333, 444, 667, 556, 833, 667, 722,
        611, 722, 611, 500, 556, 722, 611, 833, 611, 556, 556, 389, 278, 389, 422, 500,
        333, 500, 500, 444, 500, 444, 278, 500, 500, 278, 278, 444, 278, 722, 500, 500,
        500, 500, 389, 389, 278, 500, 444, 667, 444, 444, 389, 400, 275, 400, 541
    },
    { /* Times-BoldItalic */
        250, 389, 555, 500, 500, 833, 778, 278, 333, 333, 500, 570, 250, 333, 250, 278,
        500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 333, 333, 570, 570, 570, 500,
        832, 667, 667, 667, 722, 667, 667, 722, 778, 389, 500, 667, 611, 889, 722, 722,
        611, 722, 667, 556, 611, 722, 667, 889, 667, 611, 611, 333, 278, 333, 570, 500,
        333, 500, 500, 444, 500, 444, 333, 500, 556, 278, 278, 500, 278, 778, 556, 500,
        500, 500, 389, 389, 278, 556, 444, 667, 500, 444, 389, 348, 220, 348, 570
    },
};

/* Geometry in thousandths of a point, preserving exact AFM advances without
 * floating point or rounding each character. PDF decimal output is exact. */
#define MARGIN 72000u
#define BODY_SIZE 12u
#define HEADING_SIZE 18u
#define BODY_LEADING 16000u
#define HEADING_LEADING 24000u

typedef struct { unsigned width, height; } Paper;
typedef struct { unsigned position, paragraph, done; } Layout;
typedef struct { unsigned start, end, width, paragraph; } Line;
typedef struct { unsigned char *out; unsigned length; } Sink;

static int overlaps(const void *a, unsigned an, const void *b, unsigned bn) {
    uintptr_t av = (uintptr_t)a, bv = (uintptr_t)b;
    if (!an || !bn) return 0;
    return av <= bv ? bv - av < an : av - bv < bn;
}
static int valid_output(const WriterDoc *doc, unsigned char *out, unsigned capacity,
                        unsigned *written, unsigned *pages) {
    return doc && written && pages && (out || !capacity) &&
           !overlaps(doc, sizeof(*doc), out, capacity) &&
           !overlaps(doc, sizeof(*doc), written, sizeof(*written)) &&
           !overlaps(doc, sizeof(*doc), pages, sizeof(*pages)) &&
           !overlaps(out, capacity, written, sizeof(*written)) &&
           !overlaps(out, capacity, pages, sizeof(*pages)) &&
           !overlaps(written, sizeof(*written), pages, sizeof(*pages));
}
static unsigned font_size(unsigned paragraph) {
    return paragraph & WRITER_PARAGRAPH_HEADING ? HEADING_SIZE : BODY_SIZE;
}
static unsigned leading(unsigned paragraph) {
    return paragraph & WRITER_PARAGRAPH_HEADING ? HEADING_LEADING : BODY_LEADING;
}
static unsigned advance(unsigned char ch, unsigned style, unsigned size, unsigned x) {
    if (ch == '\t') {
        unsigned stop = 4u * 250u * size;
        return stop - x % stop;
    }
    return widths[style & 3u][ch - 32u] * size;
}

/* Greedy whitespace wrapping; all input spaces are retained, including the
 * whitespace at a wrap. Long tokens hard-wrap without hyphenation. No trimming
 * or skipped paragraphs. Each successful call consumes bytes or final EOF.
 * Lookahead is at most one bounded line, so repeated wrapping remains linear. */
static Line next_line(const WriterDoc *doc, Paper paper, Layout *layout) {
    unsigned start = layout->position, at = start, width = 0;
    unsigned break_at = start, break_width = 0, nonspace = 0;
    if (!start || doc->text[start - 1u] == '\n')
        layout->paragraph = doc->paragraph[start];
    unsigned size = font_size(layout->paragraph), available = paper.width - 2u * MARGIN;
    while (at < doc->length && doc->text[at] != '\n') {
        unsigned add = advance(doc->text[at], doc->style[at], size, width);
        if (!(at & 127u)) platform_poll();
        if (width + add > available) {
            if (break_at > start) { at = break_at; width = break_width; }
            break;
        }
        width += add;
        if (doc->text[at] == ' ' || doc->text[at] == '\t') {
            if (nonspace) { break_at = at + 1u; break_width = width; }
        } else nonspace = 1;
        ++at;
    }
    Line line = {start, at, width, layout->paragraph};
    layout->position = at;
    if (at < doc->length && doc->text[at] == '\n') ++layout->position;
    else if (at == doc->length) layout->done = 1;
    return line;
}
static unsigned count_pages(const WriterDoc *doc, Paper paper) {
    Layout layout = {0, 0, 0};
    unsigned pages = 1, used = 0, available = paper.height - 2u * MARGIN;
    while (!layout.done) {
        Line line = next_line(doc, paper, &layout);
        unsigned height = leading(line.paragraph);
        if (used + height > available) { ++pages; used = 0; }
        used += height;
        platform_poll();
    }
    return pages;
}

static void byte(Sink *sink, unsigned char ch) {
    if (sink->out) sink->out[sink->length] = ch;
    ++sink->length;
    if (!(sink->length & 4095u)) platform_poll();
}
static void string(Sink *sink, const char *text) {
    while (*text) byte(sink, (unsigned char)*text++);
}
static void number(Sink *sink, unsigned value) {
    char digits[10]; unsigned count = 0;
    do { digits[count++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (count) byte(sink, (unsigned char)digits[--count]);
}
static void decimal(Sink *sink, unsigned value) {
    number(sink, value / 1000u);
    if (value % 1000u) {
        byte(sink, '.');
        byte(sink, (unsigned char)('0' + value / 100u % 10u));
        byte(sink, (unsigned char)('0' + value / 10u % 10u));
        byte(sink, (unsigned char)('0' + value % 10u));
    }
}
static void reference(Sink *sink, unsigned object) {
    number(sink, object); string(sink, " 0 R");
}
static void xref_entry(Sink *sink, unsigned offset) {
    unsigned divisor = 1000000000u;
    do {
        byte(sink, (unsigned char)('0' + offset / divisor % 10u));
        divisor /= 10u;
    } while (divisor);
    string(sink, " 00000 n \n"); /* Exactly 20 bytes, including trailing space/LF. */
}
static void object(Sink *sink, unsigned id, Sink *xref) {
    if (xref) xref_entry(xref, sink->length);
    number(sink, id); string(sink, " 0 obj\n");
}
static void underline(Sink *sink, unsigned x, unsigned end, unsigned baseline, unsigned size) {
    decimal(sink, size * 50u); string(sink, " w ");
    decimal(sink, x); byte(sink, ' '); decimal(sink, baseline - size * 100u);
    string(sink, " m "); decimal(sink, end); byte(sink, ' ');
    decimal(sink, baseline - size * 100u); string(sink, " l S\n");
}
static void render_line(Sink *sink, const WriterDoc *doc, Paper paper, Line line, unsigned used) {
    unsigned at = line.start, x = 0, left = MARGIN;
    unsigned size = font_size(line.paragraph), baseline = paper.height - MARGIN - used - size * 1000u;
    unsigned align = line.paragraph & WRITER_ALIGN_MASK;
    unsigned spare = paper.width - 2u * MARGIN - line.width;
    if (align == WRITER_ALIGN_CENTER) left += spare / 2u;
    if (align == WRITER_ALIGN_RIGHT) left += spare;
    while (at < line.end) {
        unsigned style = doc->style[at], begin = x;
        if (doc->text[at] == '\t') {
            x += advance('\t', style, size, x);
            ++at;
        } else {
            string(sink, "BT /F"); number(sink, 1u + (style & 3u)); byte(sink, ' ');
            number(sink, size); string(sink, " Tf 1 0 0 1 ");
            decimal(sink, left + x); byte(sink, ' '); decimal(sink, baseline);
            string(sink, " Tm (");
            do {
                unsigned char ch = doc->text[at];
                if (ch == '(' || ch == ')' || ch == '\\') byte(sink, '\\');
                byte(sink, ch);
                x += advance(ch, style, size, x);
                ++at;
            } while (at < line.end && doc->style[at] == style && doc->text[at] != '\t');
            string(sink, ") Tj ET\n");
        }
        if (style & WRITER_STYLE_UNDERLINE)
            underline(sink, left + begin, left + x, baseline, size);
    }
}

/* Each stream length is an indirect integer, eliminating backpatches or a
 * per-page array. Objects are emitted in ascending order. For the xref pass,
 * sink counts the exact same body while xref receives each offset directly;
 * this is ONE linear pass, not a restart/rescan for each object or page. */
static void body(Sink *sink, const WriterDoc *doc, Paper paper, unsigned pages, Sink *xref) {
    static const char *const fonts[4] = {"Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic"};
    string(sink, "%PDF-1.4\n%BaseOS Writer\n");
    object(sink, 1, xref);
    string(sink, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    object(sink, 2, xref);
    string(sink, "<< /Type /Pages /Count "); number(sink, pages); string(sink, " /Kids [");
    for (unsigned i = 0; i < pages; ++i) { reference(sink, 7u + 3u * i); byte(sink, ' '); }
    string(sink, "] /MediaBox [0 0 "); decimal(sink, paper.width); byte(sink, ' ');
    decimal(sink, paper.height); string(sink, "] /Resources << /Font << ");
    for (unsigned i = 0; i < 4; ++i) {
        string(sink, "/F"); number(sink, i + 1u); byte(sink, ' '); reference(sink, i + 3u); byte(sink, ' ');
    }
    string(sink, ">> >> >>\nendobj\n");
    for (unsigned i = 0; i < 4; ++i) {
        object(sink, i + 3u, xref);
        string(sink, "<< /Type /Font /Subtype /Type1 /BaseFont /"); string(sink, fonts[i]);
        string(sink, " /Encoding /WinAnsiEncoding >>\nendobj\n");
    }
    Layout layout = {0, 0, 0};
    Line line = next_line(doc, paper, &layout);
    for (unsigned page = 0; page < pages; ++page) {
        unsigned id = 7u + 3u * page, used = 0;
        object(sink, id, xref);
        string(sink, "<< /Type /Page /Parent 2 0 R /Contents "); reference(sink, id + 1u);
        string(sink, " >>\nendobj\n");
        object(sink, id + 1u, xref);
        string(sink, "<< /Length "); reference(sink, id + 2u); string(sink, " >>\nstream\n");
        unsigned content_start = sink->length;
        string(sink, "q\n0 g 0 G\n");
        for (;;) {
            render_line(sink, doc, paper, line, used);
            used += leading(line.paragraph);
            platform_poll();
            if (layout.done) break;
            line = next_line(doc, paper, &layout);
            if (used + leading(line.paragraph) > paper.height - 2u * MARGIN) break;
        }
        string(sink, "Q\n");
        unsigned content_length = sink->length - content_start;
        string(sink, "endstream\nendobj\n");
        object(sink, id + 2u, xref); number(sink, content_length); string(sink, "\nendobj\n");
    }
}
static void xref_header(Sink *sink, unsigned pages) {
    string(sink, "xref\n0 "); number(sink, 7u + 3u * pages);
    string(sink, "\n0000000000 65535 f \n");
}
static void trailer(Sink *sink, unsigned pages, unsigned xref) {
    string(sink, "trailer\n<< /Size "); number(sink, 7u + 3u * pages);
    string(sink, " /Root 1 0 R >>\nstartxref\n"); number(sink, xref); string(sink, "\n%%EOF\n");
}
int writer_pdf_export(const WriterDoc *doc, unsigned paper_kind,
                      unsigned char *out, unsigned capacity,
                      unsigned *written, unsigned *page_count) {
    if (!valid_output(doc, out, capacity, written, page_count) ||
        paper_kind > WRITER_PDF_A4 || writer_doc_validate(doc)) return WRITER_PDF_INVALID;
    Paper paper = paper_kind == WRITER_PDF_A4 ? (Paper){595276u, 841890u} : (Paper){612000u, 792000u};
    unsigned pages = count_pages(doc, paper);
    Sink measure = {0, 0};
    body(&measure, doc, paper, pages, 0);
    unsigned xref_offset = measure.length;
    xref_header(&measure, pages);
    /* Fixed-width entries need no saved offsets to measure. */
    measure.length += 20u * (6u + 3u * pages);
    trailer(&measure, pages, xref_offset);
    if (out) {
        if (capacity < measure.length) return WRITER_PDF_CAPACITY;
        Sink sink = {out, 0}, offsets = {0, 0};
        body(&sink, doc, paper, pages, 0);
        xref_header(&sink, pages);
        body(&offsets, doc, paper, pages, &sink);
        trailer(&sink, pages, xref_offset);
    }
    *written = measure.length;
    *page_count = pages;
    return WRITER_PDF_OK;
}
