#ifndef APP_H
#define APP_H

#include "kernel/gfx.h"
#include "kernel/font.h"
#include "kernel/rtc.h"

#define KEY_ESC        0x01
#define KEY_BACKSPACE  0x0E
#define KEY_TAB        0x0F
#define KEY_ENTER      0x1C
#define KEY_SPACE      0x39
#define KEY_LEFT       0x4B
#define KEY_UP         0x48
#define KEY_RIGHT      0x4D
#define KEY_DOWN       0x50

/* Theme tokens shared by every app component (ui/theme.c). The active shell
 * assigns them from its own palette before drawing anything. */
extern uint8_t app_accent;
extern uint8_t app_accent_dk;
extern uint8_t app_text;
extern uint8_t app_text_dim;
extern uint8_t app_chrome;
extern uint8_t app_chrome_dk;
/* Dark UI surfaces shared by every app: window body, input fields, document
 * paper, borders, and the ink used on accent-filled controls. */
extern uint8_t app_body;
extern uint8_t app_field;
extern uint8_t app_paper;
extern uint8_t app_border;
extern uint8_t app_on_accent;

#endif
