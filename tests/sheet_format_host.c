#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sheet_codec.h"

/* Ordinary named display, editing and file-interchange cases only. */
static SheetDoc doc, reopened, before;
static unsigned char native[SHEET_NATIVE_MAX_SIZE + 1u];
static unsigned char saved[SHEET_NATIVE_MAX_SIZE + 1u];
static unsigned char csv[SHEET_CSV_MAX_SIZE];
static unsigned polls;
void platform_poll(void) { ++polls; }

static void set(unsigned row, unsigned col, SheetKind kind, const char *source) {
    assert(!sheet_set(&doc, row, col, kind, source, (unsigned)strlen(source)));
}

static void display(unsigned row, unsigned col, const char *expected) {
    char out[SHEET_TEXT_MAX + 1u];
    unsigned n = 999, expected_length = (unsigned)strlen(expected);
    assert(!sheet_format_display(&doc, row, col, NULL, 0, &n));
    assert(n == expected_length);
    assert(!sheet_format_display(&doc, row, col, out, sizeof(out), &n));
    assert(n == expected_length && !strcmp(out, expected));
    memset(out, 'x', sizeof(out));
    n = 999;
    assert(sheet_format_display(&doc, row, col, out, expected_length, &n) == -1);
    assert(n == 999);
    for (unsigned i = 0; i < sizeof(out); ++i) assert(out[i] == 'x');
    assert(!sheet_format_display(&doc, row, col, out, expected_length + 1u, &n));
    assert(n == expected_length && !strcmp(out, expected));
}

static void defaults(const SheetDoc *d) {
    SheetFormat format;
    unsigned width;
    assert(!sheet_validate(d));
    for (unsigned i = 0; i < SHEET_CELLS; ++i) {
        assert(!sheet_get_format(d, i / SHEET_COLS, i % SHEET_COLS, &format));
        assert(format == SHEET_FORMAT_GENERAL);
    }
    for (unsigned col = 0; col < SHEET_COLS; ++col) {
        assert(!sheet_get_column_width(d, col, &width));
        assert(width == SHEET_COLUMN_WIDTH_DEFAULT);
    }
}

static void equal_documents(void) {
    assert(!sheet_validate(&doc) && !sheet_validate(&reopened));
    for (unsigned i = 0; i < SHEET_CELLS; ++i) {
        const SheetCell *a = &doc.cells[i], *b = &reopened.cells[i];
        assert(a->kind == b->kind && a->length == b->length);
        assert(!memcmp(a->text, b->text, a->length + 1u));
        assert(a->value == b->value && a->error == b->error);
        assert(doc.formats[i] == reopened.formats[i]);
    }
    assert(!memcmp(doc.column_widths, reopened.column_widths, sizeof(doc.column_widths)));
}

static void test_display(void) {
    static const struct {
        const char *source, *general, *fixed, *currency, *percent;
    } cases[] = {
        {"0", "0", "0.00", "$0.00", "0.00%"},
        {"12", "12", "12.00", "$12.00", "1200.00%"},
        {"+001.2340", "1.234", "1.23", "$1.23", "123.40%"},
        {"1.235", "1.235", "1.24", "$1.24", "123.50%"},
        {"1.995", "1.995", "2.00", "$2.00", "199.50%"},
        {"-1.235", "-1.235", "-1.24", "-$1.24", "-123.50%"},
        {"-.004", "-0.004", "0.00", "$0.00", "-0.40%"},
        {"-.005", "-0.005", "-0.01", "-$0.01", "-0.50%"},
        {"-.001", "-0.001", "0.00", "$0.00", "-0.10%"},
        {".125", "0.125", "0.13", "$0.13", "12.50%"},
        {"-.125", "-0.125", "-0.13", "-$0.13", "-12.50%"},
        {"999.995", "999.995", "1000.00", "$1000.00", "99999.50%"},
        {"2147483.647", "2147483.647", "2147483.65", "$2147483.65", "214748364.70%"},
        {"-2147483.648", "-2147483.648", "-2147483.65", "-$2147483.65", "-214748364.80%"}
    };
    sheet_init(&doc);
    defaults(&doc);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        const char *expected[] = {cases[i].general, cases[i].fixed, cases[i].currency, cases[i].percent};
        set(0, 0, SHEET_NUMBER, cases[i].source);
        set(0, 1, SHEET_FORMULA, "=A1");
        assert(!sheet_recalculate(&doc));
        before = doc;
        for (unsigned f = SHEET_FORMAT_GENERAL; f <= SHEET_FORMAT_PERCENT; ++f) {
            assert(!sheet_set_format(&doc, 0, 0, (SheetFormat)f));
            assert(!sheet_set_format(&doc, 0, 1, (SheetFormat)f));
            display(0, 0, expected[f]);
            display(0, 1, expected[f]);
            assert(!memcmp(doc.cells, before.cells, sizeof(doc.cells)));
        }
        char raw[32]; unsigned n;
        assert(!sheet_format(&doc.cells[0], raw, sizeof(raw), &n));
        assert(!strcmp(raw, cases[i].general));
    }
    set(1, 0, SHEET_TEXT, "0012");
    set(1, 1, SHEET_TEXT, "");
    set(1, 2, SHEET_FORMULA, "=1/0");
    set(1, 3, SHEET_NUMBER, "0.0001");
    assert(!sheet_recalculate(&doc));
    for (unsigned f = SHEET_FORMAT_GENERAL; f <= SHEET_FORMAT_PERCENT; ++f) {
        for (unsigned col = 0; col < 5; ++col)
            assert(!sheet_set_format(&doc, 1, col, (SheetFormat)f));
        display(1, 0, "0012"); display(1, 1, ""); display(1, 2, "#DIV/0!");
        display(1, 3, "#PRECISION!"); display(1, 4, "");
    }
}

static void test_metadata_edits(void) {
    SheetFormat format = SHEET_FORMAT_GENERAL;
    unsigned width = 999;
    sheet_init(&doc);
    assert(!sheet_set_format(&doc, 127, 25, SHEET_FORMAT_CURRENCY));
    assert(!sheet_set_column_width(&doc, 0, SHEET_COLUMN_WIDTH_MIN));
    assert(!sheet_set_column_width(&doc, 25, SHEET_COLUMN_WIDTH_MAX));
    assert(!sheet_get_column_width(&doc, 0, &width) && width == 48);
    assert(!sheet_get_column_width(&doc, 25, &width) && width == 320);
    assert(!sheet_get_format(&doc, 127, 25, &format) && format == SHEET_FORMAT_CURRENCY);
    set(127, 25, SHEET_NUMBER, "1.235");
    assert(!sheet_recalculate(&doc)); display(127, 25, "$1.24");
    set(127, 25, SHEET_EMPTY, "");
    assert(!sheet_recalculate(&doc)); display(127, 25, "");
    assert(!sheet_get_format(&doc, 127, 25, &format) && format == SHEET_FORMAT_CURRENCY);
    before = doc;
    assert(sheet_set_format(&doc, 128, 0, SHEET_FORMAT_GENERAL) == -1);
    assert(sheet_set_format(&doc, 0, 26, SHEET_FORMAT_GENERAL) == -1);
    assert(sheet_set_format(&doc, 0, 0, (SheetFormat)4) == -1);
    assert(sheet_set_format(&doc, 0, 0, (SheetFormat)-1) == -1);
    assert(sheet_set_column_width(&doc, 26, 104) == -1);
    assert(sheet_set_column_width(&doc, 0, 47) == -1);
    assert(sheet_set_column_width(&doc, 0, 321) == -1);
    assert(sheet_get_column_width(&doc, 26, &width) == -1 && width == 320);
    assert(sheet_get_format(&doc, 128, 0, &format) == -1 && format == SHEET_FORMAT_CURRENCY);
    assert(!memcmp(&doc, &before, sizeof(doc)));
    sheet_init(&doc); defaults(&doc);
}

static void formatted_document(void) {
    sheet_init(&doc);
    set(0, 0, SHEET_NUMBER, "+001.2350");
    set(0, 1, SHEET_FORMULA, "=A1*2");
    set(0, 2, SHEET_NUMBER, ".125");
    set(1, 0, SHEET_TEXT, "0012");
    assert(!sheet_set_format(&doc, 0, 0, SHEET_FORMAT_FIXED2));
    assert(!sheet_set_format(&doc, 0, 1, SHEET_FORMAT_CURRENCY));
    assert(!sheet_set_format(&doc, 0, 2, SHEET_FORMAT_PERCENT));
    assert(!sheet_set_format(&doc, 1, 0, SHEET_FORMAT_CURRENCY));
    assert(!sheet_set_format(&doc, 127, 25, SHEET_FORMAT_FIXED2));
    assert(!sheet_set_column_width(&doc, 0, 160));
    assert(!sheet_set_column_width(&doc, 25, 48));
    assert(!sheet_recalculate(&doc));
}

static void test_native_versions(void) {
    static const unsigned char v1[] = {
        'B','S','H','1',1,0,0,0,128,0,26,0,1,0,0,0,
        0,0,SHEET_NUMBER,5,'1','.','2','3','5'
    };
    unsigned n, second;
    formatted_document();
    assert(!sheet_native_decode(&doc, v1, sizeof(v1)));
    defaults(&doc);
    assert(doc.cells[0].value == 1235);
    assert(!sheet_native_encode(&doc, native, sizeof(native), &n));
    assert(n == sizeof(v1) && !memcmp(native, v1, n));
    assert(!sheet_set_column_width(&doc, 4, 200));
    assert(!sheet_native_encode(&doc, native, sizeof(native), &n));
    assert(native[4] == 2 && n == 78);
    assert(!sheet_native_decode(&reopened, native, n)); equal_documents();
    assert(!sheet_set_column_width(&doc, 4, 104));
    assert(!sheet_native_encode(&doc, native, sizeof(native), &n));
    assert(n == sizeof(v1) && !memcmp(native, v1, n));

    formatted_document();
    assert(!sheet_native_encode(&doc, NULL, 0, &n));
    memset(native, 0xa5, sizeof(native)); second = 999;
    assert(sheet_native_encode(&doc, native, n - 1u, &second) == -1 && second == 999);
    for (unsigned i = 0; i < sizeof(native); ++i) assert(native[i] == 0xa5);
    assert(!sheet_native_encode(&doc, native, n, &second) && second == n);
    assert(native[4] == 2 && native[12] == 5 && native[n] == 0xa5);
    memcpy(saved, native, n);
    assert(!sheet_native_decode(&reopened, native, n)); equal_documents();
    assert(!sheet_native_encode(&reopened, native, sizeof(native), &second));
    assert(second == n && !memcmp(native, saved, n));
    assert(reopened.cells[1].value == 2470 && reopened.cells[3327].kind == SHEET_EMPTY);

    sheet_init(&doc);
    assert(!sheet_set_column_width(&doc, 12, 320));
    assert(!sheet_native_encode(&doc, native, sizeof(native), &n) && n == 68);
    assert(native[12] == 0);
    assert(!sheet_native_decode(&reopened, native, n)); equal_documents();
    assert(!sheet_set_format(&doc, 127, 25, SHEET_FORMAT_PERCENT));
    assert(!sheet_native_encode(&doc, native, sizeof(native), &n) && n == 73);
    assert(native[68] == 0xff && native[69] == 0x0c && native[70] == SHEET_EMPTY);
    assert(native[71] == 0 && native[72] == SHEET_FORMAT_PERCENT);
    assert(!sheet_native_decode(&reopened, native, n)); equal_documents();
    assert(!sheet_csv_export(&doc, csv, sizeof(csv), &n) && n == 0);

    /* Explicit empty TEXT stays distinct from a formatted EMPTY in v2. */
    sheet_init(&doc);
    set(0, 0, SHEET_TEXT, "");
    assert(!sheet_set_format(&doc, 0, 0, SHEET_FORMAT_CURRENCY));
    assert(!sheet_recalculate(&doc));
    assert(!sheet_native_encode(&doc, native, sizeof(native), &n) && n == 73);
    assert(native[70] == SHEET_TEXT && native[71] == 0);
    assert(!sheet_native_decode(&reopened, native, n)); equal_documents();
    assert(!sheet_set_format(&doc, 0, 0, SHEET_FORMAT_GENERAL));
    assert(!sheet_native_encode(&doc, native, sizeof(native), &n) && n == 20);
    assert(native[4] == 1 && native[18] == SHEET_TEXT);

    /* Readers admit v2 default metadata; the next save canonicalizes to v1. */
    sheet_init(&doc);
    assert(!sheet_set_column_width(&doc, 0, 160));
    assert(!sheet_native_encode(&doc, native, sizeof(native), &n) && n == 68);
    native[16] = SHEET_COLUMN_WIDTH_DEFAULT;
    assert(!sheet_native_decode(&reopened, native, n)); defaults(&reopened);
    assert(!sheet_native_encode(&reopened, native, sizeof(native), &n) && n == 16);
    assert(native[4] == 1 && native[12] == 0);
}

static void reject_file(unsigned n) {
    assert(sheet_native_decode(&reopened, native, n) == -1);
    assert(!memcmp(&reopened, &before, sizeof(before)));
}

static void test_invalid_metadata_files(void) {
    unsigned n;
    formatted_document();
    assert(!sheet_native_encode(&doc, native, sizeof(native), &n));
    memcpy(saved, native, n);
    reopened = doc; before = reopened;
#define RESTORE() memcpy(native, saved, n)
    reject_file(16); reject_file(67); reject_file(n - 1u);
    native[16] = 47; native[17] = 0; reject_file(n); RESTORE();
    native[16] = 65; native[17] = 1; reject_file(n); RESTORE(); /* 321 pixels */
    native[72] = 4; reject_file(n); RESTORE();
    native[70] = SHEET_EMPTY; reject_file(n); RESTORE(); /* EMPTY with source */
    native[n - 1u] = SHEET_FORMAT_GENERAL; reject_file(n); RESTORE();
    native[4] = 3; reject_file(n); RESTORE();
    native[n] = 'x'; reject_file(n + 1u); RESTORE();
    /* Rejection preserved both source and nondefault metadata. */
    assert(!sheet_native_decode(&reopened, native, n)); equal_documents();
#undef RESTORE
}

static void test_csv_values(void) {
    static const char expected[] = "1.235,2.47,0.125\r\n0012,,\r\n";
    unsigned n, second;
    formatted_document();
    before = doc;
    assert(!sheet_csv_export(&doc, csv, sizeof(csv), &n));
    assert(n == sizeof(expected) - 1u && !memcmp(csv, expected, n));
    assert(!memcmp(&doc, &before, sizeof(doc)));
    for (unsigned i = 0; i < SHEET_CELLS; ++i)
        assert(!sheet_set_format(&doc, i / SHEET_COLS, i % SHEET_COLS, SHEET_FORMAT_PERCENT));
    assert(!sheet_csv_export(&doc, native, sizeof(native), &second));
    assert(second == n && !memcmp(native, csv, n));
    assert(!sheet_recalculate(&doc));
    assert(!memcmp(doc.cells, before.cells, sizeof(doc.cells)));
    reopened = doc;
    assert(!sheet_csv_import(&reopened, csv, n)); defaults(&reopened);
    assert(reopened.cells[0].value == 1235 && reopened.cells[1].value == 2470);
    assert(reopened.cells[1].kind == SHEET_NUMBER);
    assert(reopened.cells[26].value == 12000);
}

static void test_maximum_v2(void) {
    char source[SHEET_TEXT_MAX + 1u]; unsigned n;
    memset(source, 'x', SHEET_TEXT_MAX); source[SHEET_TEXT_MAX] = 0;
    sheet_init(&doc);
    for (unsigned i = 0; i < SHEET_CELLS; ++i) {
        set(i / SHEET_COLS, i % SHEET_COLS, SHEET_TEXT, source);
        assert(!sheet_set_format(&doc, i / SHEET_COLS, i % SHEET_COLS, (SheetFormat)(i % 4u)));
    }
    assert(!sheet_set_column_width(&doc, 25, 320));
    assert(!sheet_recalculate(&doc));
    assert(!sheet_native_encode(&doc, NULL, 0, &n) && n == SHEET_NATIVE_MAX_SIZE);
    native[n] = 0xa5;
    assert(!sheet_native_encode(&doc, native, n, &n));
    assert(n == 332868 && native[n] == 0xa5);
    assert(!sheet_native_decode(&reopened, native, n)); equal_documents();
    printf("Maximum spreadsheet native v2: %u bytes\n", n);
}

static void fixture(const char *directory, const char *name,
                    const unsigned char *data, unsigned n) {
    char path[1024]; FILE *file;
    assert(snprintf(path, sizeof(path), "%s/%s", directory, name) < (int)sizeof(path));
    file = fopen(path, "wb"); assert(file);
    assert(fwrite(data, 1, n, file) == n); assert(!fclose(file));
}

int main(int argc, char **argv) {
    unsigned n;
    assert(argc == 2);
    test_display(); test_metadata_edits(); test_native_versions();
    test_invalid_metadata_files(); test_csv_values(); test_maximum_v2();
    formatted_document();
    assert(!sheet_native_encode(&doc, native, sizeof(native), &n));
    fixture(argv[1], "formatted.bsh", native, n);
    assert(!sheet_csv_export(&doc, csv, sizeof(csv), &n));
    fixture(argv[1], "formatted.csv", csv, n);
    assert(polls > 0);
    puts("Spreadsheet format tests passed: display, metadata, v1/v2, unchanged CSV");
    return 0;
}
