#ifndef BASEOS_RTC_H
#define BASEOS_RTC_H
/* CMOS real-time clock and the small formatting helpers built on it. */
typedef struct {
    int year, month, day;
    int hour, min, sec;
    int wday;
} RtcTime;

void rtc_read(RtcTime *t);
int rtc_days_in_month(int year, int month);
int rtc_weekday(int year, int month, int day);
extern const char *const rtc_month_name[12];
extern const char *const rtc_day_name[7];

void fmt_uint(char *out, unsigned v);
void fmt_pad2(char *out, int v);
#endif
