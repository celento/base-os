#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "../src/decimal.c"
static int32_t parse(const char *s) { int32_t v; assert(decimal_parse(s, &v)); return v; }
int main(void) {
    int32_t v;
    assert(decimal_calculate(parse("1000000"), parse("2000000"), '/', &v) && v == 500);
    assert(decimal_calculate(parse("0.999"), parse("2000000"), '*', &v) && v == 1998000000);
    assert(decimal_calculate(parse("-10"), parse("4"), '/', &v) && v == -2500);
    assert(decimal_calculate(INT32_MIN, 1000, '*', &v) && v == INT32_MIN);
    assert(!decimal_parse("2147484", &v));
    assert(!decimal_calculate(INT32_MAX, 1, '+', &v));
    assert(!decimal_calculate(INT32_MIN, 1, '-', &v));
    assert(!decimal_calculate(parse("2000000"), parse("2000000"), '*', &v));
    assert(!decimal_calculate(1000, 0, '/', &v));
    assert(!decimal_parse("1.0001", &v));
    assert(decimal_parse("1.2300", &v) && v == 1230);
    assert(decimal_parse("-2147483.648", &v) && v == INT32_MIN);
    puts("decimal: exact large intermediate arithmetic, signed limits and explicit errors passed");
}
