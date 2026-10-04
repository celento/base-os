#ifndef DECIMAL_H
#define DECIMAL_H
#include <stdint.h>
#define DECIMAL_SCALE 1000
/* Signed fixed-point values, with three decimal places. */
int decimal_parse(const char *text, int32_t *value);
int decimal_calculate(int32_t left, int32_t right, char operation, int32_t *value);
#endif
