#include "decimal.h"

/* i386 freestanding builds have no compiler runtime for 64-bit division. */
static uint64_t divide(uint64_t numerator, uint32_t denominator) {
    uint64_t quotient = 0, remainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
        remainder = (remainder << 1) | ((numerator >> bit) & 1);
        if (remainder >= denominator) {
            remainder -= denominator;
            quotient |= (uint64_t)1 << bit;
        }
    }
    return quotient;
}

static int checked(int64_t value, int32_t *out) {
    if (value < INT32_MIN || value > INT32_MAX) return 0;
    *out = (int32_t)value;
    return 1;
}

int decimal_parse(const char *text, int32_t *value) {
    int negative = *text == '-';
    if (negative || *text == '+') ++text;
    uint32_t whole = 0, fraction = 0;
    int digits = 0, places = 0;
    while (*text >= '0' && *text <= '9') {
        if (whole > 2147483u / 10u) return 0;
        whole = whole * 10 + (unsigned)(*text++ - '0');
        if (whole > 2147483u) return 0;
        ++digits;
    }
    if (*text == '.') {
        ++text;
        while (*text >= '0' && *text <= '9') {
            if (places < 3) fraction = fraction * 10 + (unsigned)(*text - '0');
            else if (*text != '0') return 0;
            ++places; ++text; ++digits;
        }
    }
    if (*text || !digits) return 0;
    while (places++ < 3) fraction *= 10;
    int64_t result = (int64_t)whole * DECIMAL_SCALE + fraction;
    return checked(negative ? -result : result, value);
}

int decimal_calculate(int32_t left, int32_t right, char operation, int32_t *value) {
    int64_t result;
    if (operation == '+') result = (int64_t)left + right;
    else if (operation == '-') result = (int64_t)left - right;
    else if (operation == '*' || operation == '/') {
        if (operation == '/' && !right) return 0;
        int negative = (left < 0) != (right < 0);
        uint32_t a = left < 0 ? (uint32_t)(-(int64_t)left) : (uint32_t)left;
        uint32_t b = right < 0 ? (uint32_t)(-(int64_t)right) : (uint32_t)right;
        uint64_t magnitude = operation == '*'
            ? divide((uint64_t)a * b, DECIMAL_SCALE)
            : divide((uint64_t)a * DECIMAL_SCALE, b);
        if (magnitude > (negative ? 2147483648ULL : 2147483647ULL)) return 0;
        result = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    } else return 0;
    return checked(result, value);
}
