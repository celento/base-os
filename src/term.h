#ifndef TERM_H
#define TERM_H

#define TERM_COLS  80
#define TERM_LINES 64

void term_select(int slot);
void term_reset(void);
void term_history(int direction);
void term_complete(void);
void term_set_rows(int rows);
void term_set_view(int rows,int total);
void term_scroll(int delta);
int term_scroll_offset(void);
int term_cwd(void);
void term_set_cwd(int id);
const unsigned char *term_canvas(void);

/* Scrollback, oldest first. */
int term_count(void);
const char *term_get(int i);

/* Live row: prompt text plus whatever has been typed so far. */
void term_prompt(char *out, int max);
const char *term_input(void);

void term_char(char c);
void term_backspace(void);
void term_enter(void);

#endif
