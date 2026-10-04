#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "writer_pdf.h"

/* Named valid boundary/capacity cases, not memory-fault or fuzz probing. These
 * large fixture buffers exist only in this host test, never in the exporter. */
static WriterDoc doc, before;
static unsigned char output[4u * 1024u * 1024u];
static unsigned char native[WRITER_NATIVE_MAX_SIZE];
static unsigned polls;
void platform_poll(void) { ++polls; }

static void unchanged(void) { assert(!memcmp(&doc, &before, sizeof(doc))); }
static void set_text(const char *text) {
    assert(!writer_plain_import(&doc, (const unsigned char *)text, (unsigned)strlen(text)));
}
static void mark(const char *text, unsigned style) {
    unsigned length = (unsigned)strlen(text);
    for (unsigned i = 0; i + length <= doc.length; ++i) {
        if (!memcmp(doc.text + i, text, length)) {
            memset(doc.style + i, (int)style, length); return;
        }
    }
    assert(!"fixture text was not found");
}
static void save(const char *directory, const char *name, const char *extension,
                 const unsigned char *data, unsigned length) {
    char path[1024];
    assert(snprintf(path, sizeof(path), "%s/%s.%s", directory, name, extension) < (int)sizeof(path));
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(data, 1, length, file) == length); assert(!fclose(file));
}
static void export_fixture(const char *directory, const char *name, unsigned paper) {
    unsigned needed = 0, pages = 0, size = 0, actual_pages = 0;
    memcpy(&before, &doc, sizeof(doc));
    assert(!writer_pdf_export(&doc, paper, NULL, 0, &needed, &pages));
    assert(pages && needed <= sizeof(output)); unchanged();
    memset(output, 0xa5, sizeof(output));
    assert(!writer_pdf_export(&doc, paper, output, needed, &size, &actual_pages));
    assert(size == needed && actual_pages == pages && output[size] == 0xa5); unchanged();
    assert(!memcmp(output, "%PDF-1.4\n", 9));
    assert(!memcmp(output + size - 6, "%%EOF\n", 6));
    save(directory, name, "pdf", output, size);
    assert(!writer_native_encode(&doc, native, sizeof(native), &size));
    save(directory, name, "bwr", native, size);
    save(directory, name, "txt", doc.text, doc.length);
    printf("%s: %u bytes, %u pages\n", name, needed, pages);
}
static void test_contract(void) {
    unsigned size = 999, pages = 999, needed, count;
    set_text("Preserve (this) \\ native draft.\n");
    mark("native", WRITER_STYLE_BOLD | WRITER_STYLE_ITALIC);
    memcpy(&before, &doc, sizeof(doc));
    assert(!writer_pdf_export(&doc, WRITER_PDF_LETTER, NULL, 0, &needed, &count));
    memset(output, 0xa5, sizeof(output));
    assert(writer_pdf_export(&doc, WRITER_PDF_LETTER, output, needed - 1u, &size, &pages) == WRITER_PDF_CAPACITY);
    for (unsigned i = 0; i < sizeof(output); ++i) assert(output[i] == 0xa5);
    assert(size == 999 && pages == 999); unchanged();
    assert(writer_pdf_export(&doc, 99, output, sizeof(output), &size, &pages) == WRITER_PDF_INVALID);
    assert(writer_pdf_export(&doc, 0, NULL, 1, &size, &pages) == WRITER_PDF_INVALID);
    assert(writer_pdf_export(&doc, 0, output, sizeof(output), NULL, &pages) == WRITER_PDF_INVALID);
    assert(writer_pdf_export(&doc, 0, output, sizeof(output), &size, NULL) == WRITER_PDF_INVALID);
    assert(writer_pdf_export(&doc, 0, output, sizeof(output), &size, &size) == WRITER_PDF_INVALID);
    assert(writer_pdf_export(&doc, 0, doc.text, sizeof(doc.text), &size, &pages) == WRITER_PDF_INVALID);
    unchanged();
    doc.style[doc.length - 1u] = 8;
    memcpy(&before, &doc, sizeof(doc));
    assert(writer_pdf_export(&doc, 0, output, sizeof(output), &size, &pages) == WRITER_PDF_INVALID);
    unchanged();
    for (unsigned i = 0; i < sizeof(output); ++i) assert(output[i] == 0xa5);
    assert(size == 999 && pages == 999);
}
static void styled(const char *directory) {
    set_text("BaseOS Writer: printable documents\n"
             "A practical page from a small, independent operating system.\n"
             "\n"
             "Bold, italic, underline, and all three together.\n"
             "Literal punctuation: (parentheses) \\ backslash {braces} [brackets].\n"
             "Straight ASCII quotes: 'single' and \"double\"; grave ` and tilde ~.\n"
             "Centered body text\n"
             "Right-aligned body text\n"
             "\n"
             "A second heading\n"
             "These pages use Times standard PDF fonts at 12 and 18 points. Long paragraphs "
             "wrap at spaces, with a hard wrap only when a word cannot fit on a complete "
             "line. The layout is independent of the screen's font and window size. "
             "All input remains in the native document for editing.\n"
             "\n"
             "Tabs:\tOne\tTwo\tThree\n"
             "The exported file can be opened or printed in a host PDF application.\n");
    doc.paragraph[0] = WRITER_PARAGRAPH_HEADING | WRITER_ALIGN_CENTER;
    mark("BaseOS Writer", WRITER_STYLE_BOLD);
    mark("Bold", WRITER_STYLE_BOLD); mark("italic", WRITER_STYLE_ITALIC);
    mark("underline", WRITER_STYLE_UNDERLINE); mark("all three together", 7);
    for (unsigned i = 0; i < doc.length; ++i) if (!i || doc.text[i - 1] == '\n') {
        if (!memcmp(doc.text + i, "Centered", 8)) doc.paragraph[i] = WRITER_ALIGN_CENTER;
        if (!memcmp(doc.text + i, "Right-", 6)) doc.paragraph[i] = WRITER_ALIGN_RIGHT;
        if (!memcmp(doc.text + i, "A second", 8)) doc.paragraph[i] = WRITER_PARAGRAPH_HEADING;
    }
    export_fixture(directory, "styled-letter", WRITER_PDF_LETTER);
    export_fixture(directory, "styled-a4", WRITER_PDF_A4);
}
static void metrics(const char *directory) {
    writer_doc_init(&doc);
    for (unsigned style = 0; style < 8; ++style) {
        for (unsigned ch = 32; ch <= 126; ++ch) {
            doc.text[doc.length] = (unsigned char)ch;
            doc.style[doc.length++] = (unsigned char)style;
        }
        doc.text[doc.length++] = '\n';
    }
    export_fixture(directory, "ascii-styles", WRITER_PDF_LETTER);
    writer_doc_init(&doc);
    for (unsigned a = 0; a < 8; ++a) for (unsigned b = 0; b < 8; ++b) {
        doc.text[doc.length] = 'A'; doc.style[doc.length++] = (unsigned char)a;
        doc.text[doc.length] = 'z'; doc.style[doc.length++] = (unsigned char)b;
    }
    export_fixture(directory, "style-transitions", WRITER_PDF_A4);
    writer_doc_init(&doc);
    for (unsigned heading = 0; heading < 2; ++heading) for (unsigned font = 0; font < 4; ++font) {
        doc.paragraph[doc.length] = (unsigned char)(heading ? WRITER_PARAGRAPH_HEADING : 0);
        for (unsigned ch = 32; ch <= 126; ++ch) {
            doc.text[doc.length] = (unsigned char)ch;
            /* Same font, alternating underline, so each glyph starts a run. */
            doc.style[doc.length++] = (unsigned char)(font | ((ch & 1u) ? WRITER_STYLE_UNDERLINE : 0));
        }
        doc.text[doc.length++] = '\n';
    }
    export_fixture(directory, "metrics-grid", WRITER_PDF_LETTER);
}
static void aligned(const char *directory) {
    writer_doc_init(&doc);
    for (unsigned heading = 0; heading < 2; ++heading) for (unsigned align = 0; align < 3; ++align) {
        const char *text = "Aligned words and punctuation ('`\\)";
        unsigned start = doc.length;
        doc.paragraph[start] = (unsigned char)(align | (heading ? WRITER_PARAGRAPH_HEADING : 0));
        while (*text) {
            doc.text[doc.length] = (unsigned char)*text++;
            doc.style[doc.length] = (unsigned char)(doc.length % 8u);
            ++doc.length;
        }
        doc.text[doc.length++] = '\n';
    }
    export_fixture(directory, "alignment", WRITER_PDF_LETTER);
    set_text("A\tB\tC\nA\tB\tC\nA\tB\tC");
    doc.paragraph[6] = WRITER_ALIGN_CENTER;
    doc.paragraph[12] = WRITER_ALIGN_RIGHT | WRITER_PARAGRAPH_HEADING;
    memset(doc.style, WRITER_STYLE_UNDERLINE, doc.length);
    export_fixture(directory, "tabs", WRITER_PDF_A4);
}
static void multipage(const char *directory) {
    writer_doc_init(&doc);
    for (unsigned i = 0; i < 100; ++i) {
        char text[200];
        int length = snprintf(text, sizeof(text), "Paragraph %03u: The quick brown fox jumps over the lazy dog. "
                              "This complete paragraph wraps normally and continues onto following pages when needed.\n", i + 1u);
        assert(length > 0 && doc.length + (unsigned)length < WRITER_TEXT_MAX);
        doc.paragraph[doc.length] = (unsigned char)(i % 11u == 0 ? WRITER_PARAGRAPH_HEADING | WRITER_ALIGN_CENTER : 0);
        memcpy(doc.text + doc.length, text, (unsigned)length);
        memset(doc.style + doc.length, i % 8u, (unsigned)length);
        doc.length += (unsigned)length;
    }
    export_fixture(directory, "multipage-letter", WRITER_PDF_LETTER);
    export_fixture(directory, "multipage-a4", WRITER_PDF_A4);
}
static void page_edges(const char *directory) {
    writer_doc_init(&doc);
    for (unsigned i = 0; i < 40; ++i) {
        doc.text[doc.length++] = 'X';
        if (i < 39) doc.text[doc.length++] = '\n';
    }
    unsigned size, pages;
    assert(!writer_pdf_export(&doc, 0, NULL, 0, &size, &pages) && pages == 1);
    export_fixture(directory, "forty-lines", WRITER_PDF_LETTER);
    doc.text[doc.length++] = '\n';
    assert(!writer_pdf_export(&doc, 0, NULL, 0, &size, &pages) && pages == 2);
    export_fixture(directory, "final-empty-page", WRITER_PDF_LETTER);
    set_text("\n\n\n");
    doc.paragraph[0] = WRITER_PARAGRAPH_HEADING;
    doc.paragraph[2] = WRITER_PARAGRAPH_HEADING;
    export_fixture(directory, "empty-paragraphs", WRITER_PDF_LETTER);
    writer_doc_init(&doc);
    export_fixture(directory, "empty", WRITER_PDF_A4);
    set_text("  AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA word");
    export_fixture(directory, "indented-long-word", WRITER_PDF_LETTER);
}
static void maximum(const char *directory) {
    unsigned size, pages, old_size = 123, old_pages = 456;
    writer_doc_init(&doc); doc.length = WRITER_TEXT_MAX;
    for (unsigned i = 0; i < doc.length; ++i) {
        doc.text[i] = (const unsigned char []){'W', 'g', '(', ')', '\\'}[i % 5u];
        doc.style[i] = (unsigned char)(i % 8u);
    }
    doc.paragraph[0] = WRITER_PARAGRAPH_HEADING;
    memcpy(&before, &doc, sizeof(doc));
    assert(!writer_pdf_export(&doc, 0, NULL, 0, &size, &pages));
    assert(size > 524288u);
    memset(output, 0xa5, sizeof(output));
    assert(writer_pdf_export(&doc, 0, output, 524288u, &old_size, &old_pages) == WRITER_PDF_CAPACITY);
    assert(old_size == 123 && old_pages == 456); unchanged();
    for (unsigned i = 0; i < sizeof(output); ++i) assert(output[i] == 0xa5);
    export_fixture(directory, "maximum-styles", WRITER_PDF_LETTER);
    writer_doc_init(&doc); doc.length = WRITER_TEXT_MAX;
    for (unsigned i = 0; i < doc.length; ++i) {
        doc.text[i] = '\n';
        doc.style[i] = (unsigned char)(i % 8u);
        doc.paragraph[i] = (unsigned char)(WRITER_PARAGRAPH_HEADING | (i % 3u));
    }
    doc.paragraph[doc.length] = WRITER_PARAGRAPH_HEADING;
    assert(!writer_pdf_export(&doc, 0, NULL, 0, &size, &pages));
    assert(pages == 1214);
    export_fixture(directory, "maximum-newlines", WRITER_PDF_LETTER);
    assert(!writer_pdf_export(&doc, 1, NULL, 0, &size, &pages));
    assert(pages == 1130);
    for (unsigned i = 0; i < doc.length; ++i) {
        doc.text[i] = 'W'; doc.style[i] = 0; doc.paragraph[i] = 0;
    }
    doc.paragraph[doc.length] = 0;
    export_fixture(directory, "maximum-token", WRITER_PDF_A4);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    test_contract(); styled(argv[1]); metrics(argv[1]); aligned(argv[1]);
    multipage(argv[1]); page_edges(argv[1]); maximum(argv[1]);
    assert(polls > 1000);
    printf("Writer PDF tests passed; %u cooperative polls\n", polls);
    return 0;
}
