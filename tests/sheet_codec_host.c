#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "sheet_codec.h"

/* Named editing/interchange cases only; no fuzzing or memory-fault probes. */
static SheetDoc doc, decoded, before;
static _Alignas(unsigned) unsigned char native[SHEET_NATIVE_MAX_SIZE + 8u];
static unsigned char native_saved[SHEET_NATIVE_MAX_SIZE + 8u];
static _Alignas(unsigned) unsigned char csv[SHEET_CSV_MAX_SIZE + 8u];
static unsigned char csv_saved[SHEET_CSV_MAX_SIZE + 8u];
static unsigned polls;
void platform_poll(void) { ++polls; }

static void set(unsigned row, unsigned col, SheetKind kind, const char *text) {
    assert(!sheet_set(&doc, row, col, kind, text, (unsigned)strlen(text)));
}

static void equal_source(const SheetDoc *a, const SheetDoc *b) {
    unsigned i;
    assert(!sheet_validate(a) && !sheet_validate(b));
    for (i = 0; i < SHEET_CELLS; ++i) {
        const SheetCell *x = &a->cells[i], *y = &b->cells[i];
        assert(x->kind == y->kind && x->length == y->length);
        assert(!memcmp(x->text, y->text, x->length));
        assert(x->value == y->value && x->error == y->error);
    }
}

static void cell(unsigned row, unsigned col, SheetKind kind, const char *text) {
    const SheetCell *c = sheet_cell(&decoded, row, col);
    assert(c && c->kind == kind && c->length == strlen(text));
    assert(!memcmp(c->text, text, c->length));
}

static void unchanged(void) { assert(!memcmp(&decoded, &before, sizeof(decoded))); }

static void preserve_destination(void) {
    sheet_init(&decoded);
    assert(!sheet_set(&decoded, 8, 9, SHEET_FORMULA, "=42*2", 5));
    assert(!sheet_set(&decoded, 12, 0, SHEET_TEXT, "Keep me", 7));
    assert(!sheet_recalculate(&decoded));
    memcpy(&before, &decoded, sizeof(before));
}

static void reject_csv(const unsigned char *data, unsigned length) {
    assert(sheet_csv_import(&decoded, data, length) == -1);
    unchanged();
}

static void reject_native(unsigned length) {
    assert(sheet_native_decode(&decoded, native, length) == -1);
    unchanged();
}

static void test_empty(void) {
    static const unsigned char header[] = {
        'B', 'S', 'H', '1', 1, 0, 0, 0, 128, 0, 26, 0, 0, 0, 0, 0
    };
    unsigned written = 999;
    sheet_init(&doc);
    assert(!sheet_recalculate(&doc));
    assert(!sheet_native_encode(&doc, NULL, 0, &written) && written == 16);
    memset(native, 0xa5, sizeof(native));
    assert(!sheet_native_encode(&doc, native, 16, &written) && written == 16);
    assert(!memcmp(native, header, sizeof(header)) && native[16] == 0xa5);
    assert(!sheet_native_decode(&decoded, native, written));
    equal_source(&doc, &decoded);
    assert(!sheet_csv_export(&doc, NULL, 0, &written) && written == 0);
    csv[0] = 0xa5;
    assert(!sheet_csv_export(&doc, csv, 0, &written) && written == 0 && csv[0] == 0xa5);
    preserve_destination();
    assert(!sheet_csv_import(&decoded, NULL, 0));
    equal_source(&doc, &decoded);
    assert(!sheet_csv_import(&decoded, (const unsigned char *)"", 0));
    equal_source(&doc, &decoded);
    /* Explicit empty text occupies a sparse native record and CSV rectangle. */
    set(1, 2, SHEET_TEXT, "");
    assert(!sheet_recalculate(&doc));
    assert(!sheet_native_encode(&doc, native, sizeof(native), &written) && written == 20);
    assert(native[12] == 1 && native[16] == 28 && native[18] == SHEET_TEXT && !native[19]);
    assert(!sheet_native_decode(&decoded, native, written));
    equal_source(&doc, &decoded);
    assert(!sheet_csv_export(&doc, csv, sizeof(csv), &written));
    assert(written == 8 && !memcmp(csv, ",,\r\n,,\r\n", written));
    assert(!sheet_csv_import(&decoded, csv, written));
    cell(1, 2, SHEET_EMPTY, "");
}

static void budget(void) {
    sheet_init(&doc);
    set(0, 0, SHEET_TEXT, "Item");
    set(0, 1, SHEET_TEXT, "Cost");
    set(0, 2, SHEET_TEXT, "Quantity");
    set(0, 3, SHEET_TEXT, "Total");
    set(1, 0, SHEET_TEXT, "Rent");
    set(1, 1, SHEET_NUMBER, "+001250.5000");
    set(1, 2, SHEET_NUMBER, "1");
    set(1, 3, SHEET_FORMULA, "=B2*C2");
    set(2, 0, SHEET_TEXT, "Supplies");
    set(2, 1, SHEET_NUMBER, "19.995");
    set(2, 2, SHEET_NUMBER, "3");
    set(2, 3, SHEET_FORMULA, "=B3*C3");
    set(3, 0, SHEET_TEXT, "Coffee");
    set(3, 1, SHEET_NUMBER, "2.5");
    set(3, 2, SHEET_NUMBER, "2");
    set(3, 3, SHEET_FORMULA, "=B4*C4");
    set(4, 0, SHEET_TEXT, "Budget total");
    set(4, 3, SHEET_FORMULA, "=SUM(D2:D4)");
    set(5, 0, SHEET_TEXT, "Notes, \"review\"\r\nline\tend");
    set(5, 2, SHEET_TEXT, "=2+3");
    set(5, 3, SHEET_TEXT, "'retained apostrophe");
    set(6, 5, SHEET_TEXT, "");
    assert(!sheet_recalculate(&doc));
    assert(doc.cells[4u * SHEET_COLS + 3u].value == 1315485);
}

static void test_native_roundtrip(void) {
    unsigned written, second;
    budget();
    assert(!sheet_native_encode(&doc, native, sizeof(native), &written));
    memcpy(native_saved, native, written);
    assert(!sheet_native_decode(&decoded, native, written));
    equal_source(&doc, &decoded);
    assert(!memcmp(native, native_saved, written));
    assert(!sheet_native_encode(&decoded, csv, sizeof(csv), &second));
    assert(second == written && !memcmp(csv, native, written));
    cell(1, 1, SHEET_NUMBER, "+001250.5000");
    cell(4, 3, SHEET_FORMULA, "=SUM(D2:D4)");
    cell(5, 2, SHEET_TEXT, "=2+3");
    cell(6, 5, SHEET_TEXT, "");
    /* Invalid formulas are source data and survive native save/load exactly. */
    set(7, 0, SHEET_FORMULA, "=1+");
    set(7, 1, SHEET_NUMBER, "0.0001");
    set(7, 2, SHEET_NUMBER, "2147483.648");
    assert(!sheet_recalculate(&doc));
    assert(!sheet_native_encode(&doc, native, sizeof(native), &written));
    assert(!sheet_native_decode(&decoded, native, written));
    equal_source(&doc, &decoded);
}

static void simple_native(void) {
    unsigned written;
    sheet_init(&doc);
    set(0, 0, SHEET_TEXT, "a");
    set(0, 2, SHEET_NUMBER, "1.5");
    set(1, 3, SHEET_FORMULA, "=1+2");
    assert(!sheet_recalculate(&doc));
    assert(!sheet_native_encode(&doc, native, sizeof(native), &written));
    assert(written == 36);
    memcpy(native_saved, native, sizeof(native));
}

static void test_native_invalid(void) {
    unsigned i;
    const unsigned truncations[] = { 0, 3, 15, 16, 18, 20, 21, 24, 27, 31, 35 };
    preserve_destination();
    simple_native();
    for (i = 0; i < sizeof(truncations) / sizeof(*truncations); ++i) reject_native(truncations[i]);
#define RESTORE() memcpy(native, native_saved, sizeof(native))
    native[36] = 'x'; reject_native(37); RESTORE();
    native[0] = 'X'; reject_native(36); RESTORE();
    native[4] = 2; reject_native(36); RESTORE();
    native[6] = 1; reject_native(36); RESTORE();
    native[8] = 127; reject_native(36); RESTORE();
    native[10] = 27; reject_native(36); RESTORE();
    native[12] = 2; reject_native(36); RESTORE();
    native[12] = 4; reject_native(36); RESTORE();
    native[12] = 1; native[13] = 13; reject_native(36); RESTORE(); /* 3329 records. */
    native[16] = 0; native[17] = 13; reject_native(36); RESTORE(); /* Index 3328. */
    native[21] = 0; reject_native(36); RESTORE(); /* Duplicate index. */
    native[16] = 3; reject_native(36); RESTORE(); /* Descending indices. */
    native[18] = SHEET_EMPTY; reject_native(36); RESTORE();
    native[18] = SHEET_AUTO; reject_native(36); RESTORE();
    native[19] = 96; reject_native(36); RESTORE();
    native[20] = 0; reject_native(36); RESTORE();
    native[20] = 0x80; reject_native(36); RESTORE();
    native[25] = 'x'; reject_native(36); RESTORE(); /* Invalid NUMBER source. */
    native[32] = '1'; reject_native(36); RESTORE(); /* FORMULA needs '='. */
    /* A complete overlong record must fail even though all bytes are present. */
    native[12] = 1; native[19] = 96; memset(native + 20, 'x', 96);
    reject_native(116); RESTORE();
    assert(sheet_native_decode(&decoded, NULL, 0) == -1); unchanged();
    assert(sheet_native_decode(NULL, native, 36) == -1);
    assert(sheet_native_decode(&decoded, (const unsigned char *)&decoded, 16) == -1);
    unchanged();
    reject_native(SHEET_NATIVE_MAX_SIZE + 1u);
#undef RESTORE
}

static void test_csv_import(void) {
    static const unsigned char source[] =
        "Item,Cost,Formula\r\n\"Rent, office\",\"1250.5000\",=2+3\n"
        "\"one \"\"quote\"\"\r\nand\nlines\rend\",-.5,'plain\r\n"
        ",+2.,\r\n\"\",1e3, \n";
    unsigned written;
    assert(!sheet_csv_import(&decoded, source, sizeof(source) - 1u));
    cell(0, 0, SHEET_TEXT, "Item");
    cell(1, 0, SHEET_TEXT, "Rent, office");
    cell(1, 1, SHEET_NUMBER, "1250.5000");
    cell(1, 2, SHEET_TEXT, "=2+3");
    cell(2, 0, SHEET_TEXT, "one \"quote\"\r\nand\nlines\rend");
    cell(2, 1, SHEET_NUMBER, "-.5");
    cell(2, 2, SHEET_TEXT, "'plain");
    cell(3, 0, SHEET_EMPTY, "");
    cell(3, 1, SHEET_NUMBER, "+2.");
    cell(3, 2, SHEET_EMPTY, "");
    cell(4, 0, SHEET_EMPTY, "");
    cell(4, 1, SHEET_TEXT, "1e3");
    cell(4, 2, SHEET_TEXT, " ");
    cell(5, 0, SHEET_EMPTY, "");
    assert(!sheet_csv_export(&decoded, csv, sizeof(csv), &written));
    memcpy(csv_saved, csv, written);
    assert(!sheet_csv_import(&doc, csv, written));
    assert(!memcmp(csv, csv_saved, written));
    assert(!sheet_csv_export(&doc, native, sizeof(native), &written));
    assert(!memcmp(native, csv_saved, written));
    /* Ragged rows, a trailing field, and a final terminator retain positions. */
    assert(!sheet_csv_import(&decoded, (const unsigned char *)"a,b,c\n\nlast,", 12));
    cell(0, 2, SHEET_TEXT, "c"); cell(1, 0, SHEET_EMPTY, "");
    cell(2, 0, SHEET_TEXT, "last"); cell(2, 1, SHEET_EMPTY, "");
    assert(!sheet_csv_import(&decoded, (const unsigned char *)"\r\n", 2));
    assert(!sheet_csv_export(&decoded, csv, sizeof(csv), &written) && written == 0);
    /* Numeric errors stay numeric while CSV formula text never executes. */
    assert(!sheet_csv_import(&decoded,
        (const unsigned char *)"0.0001,2147483.648,=1/0,+CMD,-CMD,@SUM(A1)", 42));
    cell(0, 0, SHEET_NUMBER, "0.0001");
    cell(0, 1, SHEET_NUMBER, "2147483.648");
    cell(0, 2, SHEET_TEXT, "=1/0");
    assert(decoded.cells[0].error == SHEET_ERR_PRECISION);
    assert(decoded.cells[1].error == SHEET_ERR_OVERFLOW);
}

static void test_csv_invalid(void) {
    static const char *bad[] = {
        "\"unfinished", "a,\"unfinished", "\"a\"x", "\"a\" \n", "a\"b", "a,\"x\"b",
        "\r", "a\r", "a\rb", "\"a\"\r", "\"a\"\rX", "\"\"\"", "ok\nlast,\"bad"
    };
    unsigned i, n;
    preserve_destination();
    for (i = 0; i < sizeof(bad) / sizeof(*bad); ++i)
        reject_csv((const unsigned char *)bad[i], (unsigned)strlen(bad[i]));
    csv[0] = 'a'; csv[1] = ','; csv[2] = 0; reject_csv(csv, 3);
    csv[2] = 0x7f; reject_csv(csv, 3);
    csv[2] = 0x80; reject_csv(csv, 3);
    csv[0] = 0xef; csv[1] = 0xbb; csv[2] = 0xbf; reject_csv(csv, 3);
    memset(csv, 'x', 96); reject_csv(csv, 96);
    csv[0] = '"'; memset(csv + 1, 'x', 96); csv[97] = '"'; reject_csv(csv, 98);
    /* Twenty-seven empty fields are not truncated, even on a later row. */
    memset(csv, ',', 26); reject_csv(csv, 26);
    memcpy(csv, "accepted\n", 9); memset(csv + 9, ',', 26); reject_csv(csv, 35);
    /* 128 empty rows are permitted; a 129th row is rejected. */
    memset(csv, '\n', 128);
    assert(!sheet_csv_import(&doc, csv, 128));
    csv[128] = '\n'; reject_csv(csv, 129);
    memset(csv, '\n', 127); csv[127] = 'x'; csv[128] = '\n';
    assert(!sheet_csv_import(&doc, csv, 129));
    csv[129] = ','; reject_csv(csv, 130);
    /* A full final row with and without CRLF uses exactly the same 26 cells. */
    for (n = 0, i = 0; i < 26; ++i) {
        if (i) csv[n++] = ',';
        csv[n++] = 'x';
    }
    assert(!sheet_csv_import(&doc, csv, n));
    csv[n++] = '\r'; csv[n++] = '\n'; assert(!sheet_csv_import(&doc, csv, n));
    reject_csv(NULL, 1);
    reject_csv((const unsigned char *)&decoded, 16);
    reject_csv(csv, SHEET_CSV_MAX_SIZE + 1u);
    assert(sheet_csv_import(NULL, csv, 1) == -1);
}

static void test_output_contract(void) {
    int (*encoders[])(const SheetDoc *, unsigned char *, unsigned, unsigned *) = {
        sheet_native_encode, sheet_csv_export
    };
    unsigned k, needed, written, i;
    budget();
    memcpy(&before, &doc, sizeof(before));
    for (k = 0; k < 2; ++k) {
        assert(!encoders[k](&doc, NULL, 0, &needed));
        memset(csv, 0xa5, sizeof(csv));
        memcpy(csv_saved, csv, sizeof(csv));
        written = 4321;
        assert(encoders[k](&doc, csv, needed - 1u, &written) == -1);
        assert(written == 4321 && !memcmp(csv, csv_saved, sizeof(csv)));
        assert(encoders[k](&doc, NULL, 1, &written) == -1 && written == 4321);
        assert(encoders[k](&doc, csv, 0, &written) == -1 && written == 4321);
        assert(encoders[k](&doc, csv, sizeof(csv), NULL) == -1);
        assert(encoders[k](NULL, csv, sizeof(csv), &written) == -1);
        assert(encoders[k](&doc, (unsigned char *)&doc, 20, &written) == -1 && written == 4321);
        assert(encoders[k](&doc, NULL, 0, (unsigned *)(void *)&doc.cells[0].value) == -1);
        assert(encoders[k](&doc, csv, sizeof(csv), (unsigned *)(void *)csv) == -1);
        assert(!memcmp(csv, csv_saved, sizeof(csv)));
        assert(!encoders[k](&doc, csv, needed, &written) && written == needed);
        for (i = needed; i < sizeof(csv); ++i) assert(csv[i] == 0xa5);
        /* A malformed late cell must not expose partial output or a new size. */
        doc.cells[SHEET_CELLS - 1u].kind = SHEET_AUTO;
        memset(csv, 0xa5, sizeof(csv));
        written = 4321;
        assert(encoders[k](&doc, csv, sizeof(csv), &written) == -1 && written == 4321);
        assert(!memcmp(csv, csv_saved, sizeof(csv)));
        assert(encoders[k](&doc, NULL, 0, &written) == -1 && written == 4321);
        doc.cells[SHEET_CELLS - 1u].kind = SHEET_EMPTY;
        assert(!memcmp(&doc, &before, sizeof(doc)));
    }
}

static void test_recalculated_export(void) {
    unsigned written;
    sheet_init(&doc);
    set(0, 0, SHEET_NUMBER, "2");
    set(0, 1, SHEET_FORMULA, "=A1*3");
    assert(!sheet_recalculate(&doc));
    assert(!sheet_csv_export(&doc, csv, sizeof(csv), &written));
    assert(written == 5 && !memcmp(csv, "2,6\r\n", written));
    set(0, 0, SHEET_NUMBER, "4");
    /* Edits deliberately leave dependent caches stale. Callers recalculate
     * before export; neither format promises automatic cache invalidation. */
    assert(doc.cells[1].value == 6000);
    assert(!sheet_recalculate(&doc));
    assert(!sheet_csv_export(&doc, csv, sizeof(csv), &written));
    assert(written == 6 && !memcmp(csv, "4,12\r\n", written));
    set(1, 0, SHEET_FORMULA, "=1/0");
    assert(!sheet_recalculate(&doc));
    assert(!sheet_csv_export(&doc, csv, sizeof(csv), &written));
    assert(!sheet_csv_import(&decoded, csv, written));
    cell(1, 0, SHEET_TEXT, sheet_error_name(SHEET_ERR_DIV0));
    assert(!sheet_native_encode(&doc, native, sizeof(native), &written));
    assert(!sheet_native_decode(&decoded, native, written));
    equal_source(&doc, &decoded);
}

static void test_maximum(void) {
    char text[SHEET_TEXT_MAX + 1u];
    unsigned i, needed, written;
    memset(text, '"', SHEET_TEXT_MAX); text[SHEET_TEXT_MAX] = 0;
    sheet_init(&doc);
    for (i = 0; i < SHEET_CELLS; ++i)
        assert(!sheet_set(&doc, i / SHEET_COLS, i % SHEET_COLS, SHEET_TEXT,
                          text, SHEET_TEXT_MAX));
    assert(!sheet_recalculate(&doc));
    assert(!sheet_native_encode(&doc, NULL, 0, &needed) && needed == SHEET_NATIVE_V1_MAX_SIZE);
    memset(native, 0xa5, sizeof(native));
    assert(!sheet_native_encode(&doc, native, needed, &written) && written == needed);
    assert(native[needed] == 0xa5 && native[12] == 0 && native[13] == 13);
    assert(!sheet_native_decode(&decoded, native, written));
    equal_source(&doc, &decoded);
    assert(!sheet_csv_export(&doc, NULL, 0, &needed) && needed == SHEET_CSV_MAX_SIZE);
    memset(csv, 0xa5, sizeof(csv));
    written = 999;
    assert(sheet_csv_export(&doc, csv, needed - 1u, &written) == -1 && written == 999);
    for (i = 0; i < sizeof(csv); ++i) assert(csv[i] == 0xa5);
    assert(!sheet_csv_export(&doc, csv, needed, &written) && written == needed);
    assert(csv[needed] == 0xa5);
    assert(!sheet_csv_import(&decoded, csv, written));
    equal_source(&doc, &decoded);
    printf("Maximum spreadsheet native v1: %u bytes; CSV: %u bytes\n",
           (unsigned)SHEET_NATIVE_V1_MAX_SIZE, written);
    /* The furthest populated cell preserves all preceding empty fields/rows. */
    sheet_init(&doc); set(127, 25, SHEET_TEXT, "end");
    assert(!sheet_recalculate(&doc));
    assert(!sheet_csv_export(&doc, csv, sizeof(csv), &written));
    assert(written == 128u * 27u + 3u);
    assert(!sheet_csv_import(&decoded, csv, written));
    equal_source(&doc, &decoded);
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

static void fixtures(const char *directory) {
    unsigned written;
    budget();
    assert(!sheet_native_encode(&doc, native, sizeof(native), &written));
    write_fixture(directory, "budget.bsh", native, written);
    assert(!sheet_csv_export(&doc, csv, sizeof(csv), &written));
    write_fixture(directory, "budget.csv", csv, written);
    /* Every supported source byte, including CSV metacharacters and controls. */
    sheet_init(&doc);
    set(0, 0, SHEET_TEXT,
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~");
    set(0, 1, SHEET_TEXT, "\t\r\n");
    set(1, 1, SHEET_TEXT, "tail");
    assert(!sheet_recalculate(&doc));
    assert(!sheet_csv_export(&doc, csv, sizeof(csv), &written));
    write_fixture(directory, "ascii.csv", csv, written);
    assert(!sheet_csv_import(&decoded, csv, written));
    equal_source(&doc, &decoded);
    sheet_init(&doc); set(127, 25, SHEET_TEXT, "corner");
    assert(!sheet_recalculate(&doc));
    assert(!sheet_native_encode(&doc, native, sizeof(native), &written));
    write_fixture(directory, "corner.bsh", native, written);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    test_empty();
    test_native_roundtrip();
    test_native_invalid();
    test_csv_import();
    test_csv_invalid();
    test_output_contract();
    test_recalculated_export();
    test_maximum();
    fixtures(argv[1]);
    assert(polls > 0);
    puts("Spreadsheet codec tests passed");
    return 0;
}
