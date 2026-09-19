#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "app.h"
static unsigned snapshot_index;
static int stuck, binary, twelve, invalid_bcd;
uint8_t rtc_test_read(uint8_t reg) {
    if (reg == 0x0a) return stuck ? 0x80 : 0;
    if (reg == 0x0b) return binary ? 6 : twelve ? 0 : 2;
    if (reg == 0) snapshot_index++;
    if (reg == 0) return invalid_bcd ? 0x1a : snapshot_index == 1 ? 0x59 : 0;
    if (reg == 2) return snapshot_index == 1 ? 0x59 : 0;
    if (reg == 4) return twelve ? 0x92 : binary ? 13 : snapshot_index == 1 ? 0x23 : 0;
    if (reg == 7) return snapshot_index == 1 ? 0x31 : 1;
    if (reg == 8) return snapshot_index == 1 ? 0x12 : 1;
    if (reg == 9) return binary ? 26 : snapshot_index == 1 ? 0x25 : 0x26;
    if (reg == 6) return 1;
    assert(0); return 0;
}
void platform_log(const char *s) { (void)s; }
int main(void) {
    RtcTime t;
    rtc_read(&t);
    assert(snapshot_index >= 4);
    assert(t.year == 2026 && t.month == 1 && t.day == 1 && t.hour == 0);
    stuck = 1; rtc_read(&t); assert(t.year == 2026 && t.day == 1);
    stuck = 0; binary = 1; rtc_read(&t); assert(t.hour == 13 && t.year == 2026);
    binary = 0; twelve = 1; rtc_read(&t); assert(t.hour == 12);
    invalid_bcd = 1; rtc_read(&t); assert(t.sec == 0 && t.hour == 12);
    assert(rtc_days_in_month(2024, 2) == 29);
    assert(rtc_days_in_month(2100, 2) == 28);
    assert(rtc_days_in_month(2026, 0) == 0);
    puts("RTC: rollover, bounded failure, last valid snapshot, binary, BCD, 12-hour conversion passed");
}
