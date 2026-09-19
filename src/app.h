#ifndef APP_H
#define APP_H

#include "gfx.h"
#include "font.h"

#define KEY_ESC        0x01
#define KEY_BACKSPACE  0x0E
#define KEY_TAB        0x0F
#define KEY_ENTER      0x1C
#define KEY_SPACE      0x39
#define KEY_LEFT       0x4B
#define KEY_UP         0x48
#define KEY_RIGHT      0x4D
#define KEY_DOWN       0x50

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

extern uint8_t app_accent;
extern uint8_t app_accent_dk;
extern uint8_t app_text;
extern uint8_t app_text_dim;
extern uint8_t app_chrome;
extern uint8_t app_chrome_dk;

#endif
