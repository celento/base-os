#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "writer_codec.h"

/* Fixed, named boundary/format cases, not a fuzz or fault-probe harness. */
static WriterDoc doc, decoded, before;
static unsigned char native[WRITER_NATIVE_MAX_SIZE + 16u];
static unsigned char saved[WRITER_NATIVE_MAX_SIZE + 16u];
static unsigned char plain[WRITER_TEXT_MAX + 1u];
static unsigned char windows_text[WRITER_PLAIN_MAX_INPUT + 1u];
static unsigned char rtf[32u * WRITER_TEXT_MAX + 256u];
static unsigned polls;
void platform_poll(void) { ++polls; }

static void same_doc(const WriterDoc *a, const WriterDoc *b) {
    assert(a->length == b->length);
    assert(!memcmp(a->text, b->text, a->length));
    assert(!memcmp(a->style, b->style, a->length + 1u));
    assert(!memcmp(a->paragraph, b->paragraph, a->length + 1u));
}

static void unchanged(void) { assert(!memcmp(&decoded, &before, sizeof(decoded))); }

static void reject_native(unsigned length) {
    assert(writer_native_decode(&decoded, native, length) == -1);
    unchanged();
}

static void test_empty(void) {
    static const unsigned char expected[18] = {
        'B', 'W', 'R', '1', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    unsigned written = UINT_MAX, i;
    memset(&doc, 0xa5, sizeof(doc));
    writer_doc_init(&doc);
    assert(doc.length == 0 && !writer_doc_validate(&doc));
    for (i = 0; i <= WRITER_TEXT_MAX; ++i) {
        if (i < WRITER_TEXT_MAX) assert(doc.text[i] == 0);
        assert(doc.style[i] == 0 && doc.paragraph[i] == 0);
    }
    assert(!writer_native_encode(&doc, NULL, 0, &written) && written == 18);
    memset(native, 0xa5, sizeof(native));
    assert(!writer_native_encode(&doc, native, 18, &written));
    assert(written == 18 && native[18] == 0xa5);
    assert(!memcmp(native, expected, 18));
    assert(!writer_native_decode(&decoded, native, written));
    same_doc(&doc, &decoded);
    doc.style[0] = WRITER_STYLE_MASK;
    doc.paragraph[0] = WRITER_ALIGN_CENTER | WRITER_PARAGRAPH_HEADING;
    assert(!writer_native_encode(&doc, native, sizeof(native), &written));
    assert(!writer_native_decode(&decoded, native, written));
    same_doc(&doc, &decoded);
    assert(!writer_plain_import(&doc, NULL, 0));
    assert(doc.length == 0 && doc.style[0] == 0 && doc.paragraph[0] == 0);
}

static void test_plain(void) {
    unsigned i, n = 0;
    for (i = 32; i <= 126; ++i) plain[n++] = (unsigned char)i;
    plain[n++] = '\t'; plain[n++] = '\n';
    assert(!writer_plain_import(&doc, plain, n));
    assert(!writer_doc_validate(&doc) && doc.length == n);
    assert(!memcmp(plain, doc.text, n));
    for (i = 0; i <= n; ++i) assert(!doc.style[i] && !doc.paragraph[i]);
    assert(!writer_plain_import(&decoded, (const unsigned char *)"Keep me", 7));
    decoded.style[0] = 3;
    decoded.paragraph[0] = 6;
    memcpy(&before, &decoded, sizeof(decoded));
    /* Every unsupported byte is rejected, including NUL, CR, DEL and UTF-8. */
    for (i = 0; i <= 255; ++i) {
        if ((i >= 32 && i <= 126) || i == '\t' || i == '\n') continue;
        plain[0] = 'a'; plain[1] = (unsigned char)i; plain[2] = 'z';
        assert(writer_plain_import(&decoded, plain, 3) == -1);
        unchanged();
    }
    assert(!writer_plain_import(&doc, (const unsigned char *)"a\r\nb", 4));
    assert(doc.length == 3 && !memcmp(doc.text, "a\nb", 3));
    memset(plain, 'x', sizeof(plain));
    plain[WRITER_TEXT_MAX - 1u] = '\r';
    assert(writer_plain_import(&decoded, plain, WRITER_TEXT_MAX) == -1);
    unchanged();
    plain[WRITER_TEXT_MAX - 1u] = 'x';
    assert(writer_plain_import(&decoded, plain, WRITER_TEXT_MAX + 1u) == -1);
    unchanged();
    assert(writer_plain_import(&decoded, NULL, 1) == -1); unchanged();
    assert(writer_plain_import(&decoded, decoded.text, decoded.length) == -1); unchanged();
    assert(writer_plain_import(NULL, plain, 1) == -1);
    assert(!writer_plain_import(&doc, plain, WRITER_TEXT_MAX));
    assert(doc.length == WRITER_TEXT_MAX && !writer_doc_validate(&doc));
}

static void test_crlf(void) {
    static const unsigned char mixed[] = "\r\nA\nB\r\nC\tD\r\n";
    static const unsigned char normalized[] = "\nA\nB\nC\tD\n";
    static const unsigned char *bad[] = {
        (const unsigned char *)"\r", (const unsigned char *)"a\r",
        (const unsigned char *)"\ra", (const unsigned char *)"a\r\rb",
        (const unsigned char *)"\r\r\n", (const unsigned char *)"a\r\nb\r"
    };
    unsigned i, written;
    assert(!writer_plain_import(&doc, mixed, sizeof(mixed) - 1u));
    assert(doc.length == sizeof(normalized) - 1u);
    assert(!memcmp(doc.text, normalized, doc.length));
    assert(!memcmp(mixed, "\r\nA\nB\r\nC\tD\r\n", sizeof(mixed)));
    assert(!writer_native_encode(&doc, native, sizeof(native), &written));
    assert(!writer_native_decode(&decoded, native, written));
    same_doc(&doc, &decoded);
    memcpy(&before, &decoded, sizeof(decoded));
    for (i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        assert(writer_plain_import(&decoded, bad[i], (unsigned)strlen((const char *)bad[i])) == -1);
        unchanged();
    }
    for (i = 0; i < WRITER_PLAIN_MAX_INPUT; i += 2u) {
        windows_text[i] = '\r'; windows_text[i + 1u] = '\n';
    }
    assert(!writer_plain_import(&doc, windows_text, WRITER_PLAIN_MAX_INPUT));
    assert(doc.length == WRITER_TEXT_MAX && !writer_doc_validate(&doc));
    for (i = 0; i < WRITER_TEXT_MAX; ++i) assert(doc.text[i] == '\n');
    for (i = 0; i < WRITER_PLAIN_MAX_INPUT; ++i)
        assert(windows_text[i] == (i & 1u ? '\n' : '\r'));
    /* A full normalized document may have an odd raw source length. */
    windows_text[WRITER_PLAIN_MAX_INPUT - 2u] = 'x';
    assert(!writer_plain_import(&doc, windows_text, WRITER_PLAIN_MAX_INPUT - 1u));
    assert(doc.length == WRITER_TEXT_MAX && doc.text[doc.length - 1u] == 'x');
    /* Two unpaired LF bytes push only normalized length over capacity. */
    windows_text[WRITER_PLAIN_MAX_INPUT - 2u] = '\n';
    assert(writer_plain_import(&decoded, windows_text, WRITER_PLAIN_MAX_INPUT) == -1);
    unchanged();
    windows_text[WRITER_PLAIN_MAX_INPUT - 2u] = '\r';
    assert(writer_plain_import(&decoded, windows_text, WRITER_PLAIN_MAX_INPUT - 1u) == -1);
    unchanged();
    windows_text[WRITER_PLAIN_MAX_INPUT - 1u] = 0x80;
    assert(writer_plain_import(&decoded, windows_text, WRITER_PLAIN_MAX_INPUT) == -1);
    unchanged();
    windows_text[WRITER_PLAIN_MAX_INPUT - 1u] = '\n';
    windows_text[WRITER_PLAIN_MAX_INPUT] = 'x';
    assert(writer_plain_import(&decoded, windows_text, WRITER_PLAIN_MAX_INPUT + 1u) == -1);
    unchanged();
}

static void fixture(void) {
    static const unsigned char content[] =
        "Bold plain Italic Underline All\nCentered heading\nRight aligned\nLeft \\ { } 123\tTAB\n\n";
    unsigned i, p = 0;
    assert(!writer_plain_import(&doc, content, sizeof(content) - 1u));
    for (i = 0; i < 4; ++i) doc.style[i] = WRITER_STYLE_BOLD;
    for (i = 11; i < 17; ++i) doc.style[i] = WRITER_STYLE_ITALIC;
    for (i = 18; i < 27; ++i) doc.style[i] = WRITER_STYLE_UNDERLINE;
    for (i = 28; i < 31; ++i) doc.style[i] = WRITER_STYLE_MASK;
    for (i = 0; i <= doc.length; ++i) {
        if (!i || doc.text[i - 1] == '\n') {
            if (p == 1) doc.paragraph[i] = WRITER_ALIGN_CENTER | WRITER_PARAGRAPH_HEADING;
            if (p == 2) doc.paragraph[i] = WRITER_ALIGN_RIGHT;
            if (p == 4) doc.paragraph[i] = WRITER_ALIGN_CENTER;
            if (p == 5) doc.paragraph[i] = WRITER_ALIGN_RIGHT | WRITER_PARAGRAPH_HEADING;
            ++p;
        }
    }
    doc.style[doc.length] = WRITER_STYLE_BOLD | WRITER_STYLE_UNDERLINE;
    assert(!writer_doc_validate(&doc));
}

static void test_native(void) {
    unsigned size, i, pos;
    fixture();
    assert(!writer_native_encode(&doc, native, sizeof(native), &size));
    assert(size == WRITER_NATIVE_HEADER_SIZE + 3u * doc.length + 2u);
    assert(!memcmp(native + 16, doc.text, doc.length));
    assert(!memcmp(native + 16 + doc.length, doc.style, doc.length + 1));
    assert(!memcmp(native + 17 + 2 * doc.length, doc.paragraph, doc.length + 1));
    assert(!writer_native_decode(&decoded, native, size));
    same_doc(&doc, &decoded);
    memcpy(&before, &decoded, sizeof(decoded));
    memcpy(saved, native, sizeof(native));
    for (i = 0; i < size; ++i) reject_native(i); /* Every truncation. */
    native[size] = 0; reject_native(size + 1); /* No trailing junk accepted. */
    for (pos = 0; pos < 16; ++pos) {
        memcpy(native, saved, sizeof(native));
        native[pos] ^= 0x80;
        reject_native(size);
    }
    memcpy(native, saved, sizeof(native));
    memset(native + 8, 255, 4); reject_native(size);
    memcpy(native, saved, sizeof(native));
    native[8] = 1; native[9] = 128; native[10] = native[11] = 0; reject_native(size);
    memcpy(native, saved, sizeof(native));
    native[16 + doc.length - 1u] = 0; reject_native(size);
    memcpy(native, saved, sizeof(native));
    native[16 + 2 * doc.length] = 8; reject_native(size); /* Insertion style. */
    memcpy(native, saved, sizeof(native));
    native[17 + 2 * doc.length] = 3; reject_native(size); /* Invalid alignment. */
    memcpy(native, saved, sizeof(native));
    native[18 + 2 * doc.length] = 1; reject_native(size); /* Non-start attribute. */
    memcpy(native, saved, sizeof(native));
    native[size - 1] = 8; reject_native(size); /* Final empty paragraph. */
    assert(writer_native_decode(&decoded, NULL, size) == -1); unchanged();
    assert(writer_native_decode(&decoded, decoded.text, 18) == -1); unchanged();
    assert(writer_native_decode(NULL, native, size) == -1);
    /* Length byte order is independently checked at the maximum boundary. */
    memset(plain, 'x', sizeof(plain));
    assert(!writer_plain_import(&doc, plain, WRITER_TEXT_MAX));
    doc.style[doc.length] = 7;
    assert(!writer_native_encode(&doc, native, WRITER_NATIVE_MAX_SIZE, &size));
    assert(size == WRITER_NATIVE_MAX_SIZE);
    assert(native[8] == 0 && native[9] == 128 && native[10] == 0 && native[11] == 0);
    assert(!writer_native_decode(&decoded, native, size));
    same_doc(&doc, &decoded);
}

static void reject_doc(void) {
    unsigned written = 1234;
    assert(writer_doc_validate(&doc) == -1);
    memset(native, 0xa5, sizeof(native));
    memcpy(saved, native, sizeof(native));
    assert(writer_native_encode(&doc, native, sizeof(native), &written) == -1);
    assert(written == 1234 && !memcmp(native, saved, sizeof(native)));
    memset(rtf, 0xa5, sizeof(rtf));
    assert(writer_rtf_export(&doc, rtf, sizeof(rtf), &written) == -1);
    assert(written == 1234 && rtf[0] == 0xa5 && rtf[sizeof(rtf) - 1u] == 0xa5);
}

static void test_invalid_docs(void) {
    fixture(); doc.length = WRITER_TEXT_MAX + 1u; reject_doc();
    fixture(); doc.text[doc.length - 1u] = 127; reject_doc();
    fixture(); doc.style[0] = 8; reject_doc();
    fixture(); doc.style[doc.length] = 128; reject_doc();
    fixture(); doc.paragraph[0] = 3; reject_doc();
    fixture(); doc.paragraph[0] = 7; reject_doc();
    fixture(); doc.paragraph[0] = 8; reject_doc();
    fixture(); doc.paragraph[1] = 1; reject_doc();
    fixture(); doc.text[doc.length - 1u] = 'x'; reject_doc();
    /* Storage after length is not part of the document and is never exported. */
    fixture();
    doc.text[doc.length] = 255;
    doc.style[doc.length + 1u] = 255;
    doc.paragraph[doc.length + 1u] = 255;
    assert(!writer_doc_validate(&doc));
    assert(writer_doc_validate(NULL) == -1);
}

static void test_output_contract(void) {
    int (*encoders[])(const WriterDoc *, unsigned char *, unsigned, unsigned *) = {
        writer_native_encode, writer_rtf_export
    };
    unsigned k, needed, written, i;
    fixture();
    for (k = 0; k < 2; ++k) {
        assert(!encoders[k](&doc, NULL, 0, &needed));
        memset(native, 0xa5, sizeof(native));
        memcpy(saved, native, sizeof(native));
        written = 4321;
        assert(encoders[k](&doc, native, needed - 1u, &written) == -1);
        assert(written == 4321 && !memcmp(native, saved, sizeof(native)));
        assert(encoders[k](&doc, NULL, 1, &written) == -1 && written == 4321);
        assert(encoders[k](&doc, native, 0, &written) == -1 && written == 4321);
        assert(encoders[k](&doc, native, sizeof(native), NULL) == -1);
        assert(encoders[k](NULL, native, sizeof(native), &written) == -1);
        assert(encoders[k](&doc, doc.text, 20, &written) == -1 && written == 4321);
        assert(encoders[k](&doc, NULL, 0, &doc.length) == -1);
        assert(encoders[k](&doc, native, 20, (unsigned *)(void *)native) == -1);
        assert(!encoders[k](&doc, native, needed, &written) && written == needed);
        assert(native[needed] == 0xa5); /* Exactly sized output has no extra NUL. */
        for (i = needed; i < sizeof(native); ++i) assert(native[i] == 0xa5);
    }
}

static void test_rtf_boundaries(void) {
    unsigned needed, written = 99, i;
    fixture();
    assert(!writer_rtf_export(&doc, rtf, sizeof(rtf), &written));
    assert(!memcmp(rtf, "{\\rtf1\\ansi", 11));
    for (i = 0; i < written; ++i) assert((rtf[i] >= 32 && rtf[i] <= 126) || rtf[i] == '\n');
    rtf[written] = 0; /* Host-only convenience, not supplied by exporter. */
    assert(strstr((char *)rtf, "\\\\ \\{ \\} 123\\tab TAB"));
    assert(strstr((char *)rtf, "\\pard\\qc\\fs36"));
    assert(strstr((char *)rtf, "\\pard\\qr\\fs24"));
    assert(strstr((char *)rtf, "\\b0") && strstr((char *)rtf, "\\i0") && strstr((char *)rtf, "\\ul0"));
    /* Maximum text with every character ending and changing a paragraph. */
    memset(plain, '\n', WRITER_TEXT_MAX);
    assert(!writer_plain_import(&doc, plain, WRITER_TEXT_MAX));
    for (i = 0; i <= doc.length; ++i) {
        doc.style[i] = i & 1u ? 7 : 0;
        doc.paragraph[i] = i & 1u ? 5 : 2;
    }
    assert(!writer_rtf_export(&doc, NULL, 0, &needed));
    assert(needed > 524288u && needed < sizeof(rtf));
    memset(rtf, 0xa5, sizeof(rtf));
    written = 99;
    assert(writer_rtf_export(&doc, rtf, 524288u, &written) == -1 && written == 99);
    for (i = 0; i < sizeof(rtf); ++i) assert(rtf[i] == 0xa5);
    assert(!writer_rtf_export(&doc, rtf, needed, &written) && written == needed);
    assert(rtf[needed] == 0xa5);
    printf("Maximum alternating-paragraph RTF: %u bytes\n", written);
}

static void write_fixture(const char *directory, const char *name,
                          const unsigned char *data, unsigned length) {
    char path[1024];
    FILE *file;
    assert(snprintf(path, sizeof(path), "%s/%s", directory, name) < (int)sizeof(path));
    file = fopen(path, "wb"); assert(file);
    assert(fwrite(data, 1, length, file) == length);
    assert(!fclose(file));
}

static void write_fixtures(const char *directory) {
    unsigned written, i, j;
    fixture();
    assert(!writer_rtf_export(&doc, rtf, sizeof(rtf), &written));
    write_fixture(directory, "styled.rtf", rtf, written);
    assert(!writer_native_encode(&doc, native, sizeof(native), &written));
    write_fixture(directory, "styled.bwr", native, written);
    write_fixture(directory, "styled.txt", doc.text, doc.length);
    writer_doc_init(&doc);
    doc.style[0] = 7; doc.paragraph[0] = 5;
    assert(!writer_rtf_export(&doc, rtf, sizeof(rtf), &written));
    write_fixture(directory, "empty.rtf", rtf, written);
    assert(!writer_native_encode(&doc, native, sizeof(native), &written));
    write_fixture(directory, "empty.bwr", native, written);
    writer_doc_init(&doc);
    for (i = 0; i < 8; ++i) for (j = 0; j < 8; ++j) {
        doc.text[doc.length] = 'a'; doc.style[doc.length++] = (unsigned char)i;
        doc.text[doc.length] = 'b'; doc.style[doc.length++] = (unsigned char)j;
    }
    doc.style[doc.length] = 7;
    assert(!writer_rtf_export(&doc, rtf, sizeof(rtf), &written));
    write_fixture(directory, "transitions.rtf", rtf, written);
    assert(!writer_native_encode(&doc, native, sizeof(native), &written));
    write_fixture(directory, "transitions.bwr", native, written);
    writer_doc_init(&doc);
    for (i = 32; i <= 126; ++i) {
        doc.text[doc.length] = (unsigned char)i;
        doc.style[doc.length++] = (unsigned char)(i % 8);
    }
    assert(!writer_rtf_export(&doc, rtf, sizeof(rtf), &written));
    write_fixture(directory, "ascii.rtf", rtf, written);
    assert(!writer_native_encode(&doc, native, sizeof(native), &written));
    write_fixture(directory, "ascii.bwr", native, written);
}

int main(int argc, char **argv) {
    test_empty();
    test_plain();
    test_crlf();
    test_native();
    test_invalid_docs();
    test_output_contract();
    test_rtf_boundaries();
    assert(polls > 200); /* Long validation/copy/export loops stay cooperative. */
    if (argc == 2) write_fixtures(argv[1]);
    puts("Writer codec tests passed");
    return 0;
}
