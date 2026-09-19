#ifndef TODO_H
#define TODO_H

#define TODO_W       420
#define TODO_WIN_H   380
#define TODO_MAX     12
#define TODO_ROW_H   24
#define TODO_FOOT_H  36

void todo_load(void);

int todo_count(void);
int todo_done(int i);
const char *todo_text(int i);

int todo_toggle(int i);

/* New-item field. */
void todo_focus_field(void);
int todo_field_active(void);
const char *todo_field(void);
int todo_field_char(char c);
int todo_field_backspace(void);
int todo_field_enter(void);

#endif
