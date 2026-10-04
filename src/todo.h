#ifndef TODO_H
#define TODO_H

#define TODO_W       420
#define TODO_WIN_H   404
#define TODO_MAX     12
#define TODO_ROW_H   24
#define TODO_FOOT_H  60

/* Reopening preserves queued/failed items and unsubmitted field text. */
void todo_load(void);
/* Call after storage progress and before autosync, even without an open Todo
 * window. Returns nonzero when status changed and the UI needs repainting. */
int todo_tick(void);
const char *todo_status(void);
/* Explicit retry: 0 means durable, -2 means disk busy, -1 means failed. */
int todo_retry_save(void);
/* Drain a lease and stage all accepted items. 0 means ready in FS RAM only;
 * shutdown must still perform its final fs_sync and check that result. */
int todo_prepare_shutdown(void);

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
