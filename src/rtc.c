#include "app.h"
#include "platform.h"

const char *const rtc_month_name[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

const char *const rtc_day_name[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

#ifdef RTC_HOST_TEST
extern uint8_t rtc_test_read(uint8_t reg);
#define cmos_read rtc_test_read
#else
static uint8_t cmos_read(uint8_t reg) {
    uint8_t v;
    asm volatile("outb %0, %1" : : "a"(reg), "Nd"((uint16_t)0x70));
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"((uint16_t)0x71));
    return v;
}

#endif

static int updating(void) {
    return cmos_read(0x0A) & 0x80;
}

static int bcd(int v) {
    return (v & 0x0F) + ((v >> 4) & 0x0F) * 10;
}

static int snapshot(uint8_t raw[8]) {
    for (unsigned wait = 0; updating(); ++wait)
        if (wait == 1000) return 0;
    static const uint8_t regs[8] = {0, 2, 4, 7, 8, 9, 6, 0x0b};
    for (unsigned i = 0; i < 8; ++i) raw[i] = cmos_read(regs[i]);
    return !updating();
}

void rtc_read(RtcTime *t) {
    static RtcTime last = {2000, 1, 1, 0, 0, 0, 6};
    static int warned;
    uint8_t raw[8], other[8];
    int stable = 0;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (!snapshot(raw) || !snapshot(other)) continue;
        stable = 1;
        for (unsigned i = 0; i < 8; ++i)
            if (raw[i] != other[i]) stable = 0;
        if (stable) break;
    }
    if (!stable) goto unavailable;
    uint8_t b = raw[7];
    int binary = b & 0x04;
    int h24 = b & 0x02;
    if (!binary) {
        for (unsigned i = 0; i < 6; ++i) {
            unsigned value = i == 2 ? raw[i] & 0x7f : raw[i];
            if ((value & 15) > 9 || (value >> 4) > 9) goto unavailable;
        }
    }

    int sec = binary ? raw[0] : bcd(raw[0]);
    int min = binary ? raw[1] : bcd(raw[1]);
    int hr_raw = raw[2];
    int pm = hr_raw & 0x80;
    hr_raw &= 0x7F;
    int hour = binary ? hr_raw : bcd(hr_raw);
    if ((h24 && pm) || (!h24 && (hour < 1 || hour > 12))) goto unavailable;
    if (!h24) {
        if (hour == 12)
            hour = 0;
        if (pm)
            hour += 12;
    }
    int day = binary ? raw[3] : bcd(raw[3]);
    int month = binary ? raw[4] : bcd(raw[4]);
    int year = binary ? raw[5] : bcd(raw[5]);
    year += 2000;

    if (month < 1 || month > 12 || day < 1 ||
        day > rtc_days_in_month(year, month) || hour > 23 || min > 59 || sec > 59)
        goto unavailable;

    t->year = year;
    t->month = month;
    t->day = day;
    t->hour = hour;
    t->min = min;
    t->sec = sec;
    t->wday = rtc_weekday(year, month, day);
    last = *t;
    warned = 0;
    return;
unavailable:
    *t = last;
    if (!warned) platform_log("RTC unavailable; retaining last valid time\n");
    warned = 1;
}

int rtc_days_in_month(int year, int month) {
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 0;
    if (month == 2) {
        int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29 : 28;
    }
    return dm[month - 1];
}

int rtc_weekday(int year, int month, int day) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 1 || month > 12 || day < 1 || day > rtc_days_in_month(year, month))
        return 0;
    if (month < 3)
        year--;
    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

void fmt_uint(char *out, unsigned v) {
    char tmp[12];
    int n = 0;
    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

void fmt_pad2(char *out, int v) {
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
    out[2] = 0;
}

#ifndef RTC_HOST_TEST
/* UTC seconds since 2000-01-01; zero means unknown in legacy volumes. */
unsigned fs_clock(void) {
    RtcTime t; rtc_read(&t);
    unsigned days = 0;
    for (int y = 2000; y < t.year; y++) days += rtc_days_in_month(y, 2) == 29 ? 366 : 365;
    for (int m = 1; m < t.month; m++) days += rtc_days_in_month(t.year, m);
    days += t.day - 1;
    return ((days * 24 + t.hour) * 60 + t.min) * 60 + t.sec;
}
#endif
