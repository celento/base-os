#ifndef TERM_H
#define TERM_H

#define TERM_COLS  80
#define TERM_LINES 320 /* Holds one complete 256-node directory listing. */

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
/* Pixels use the returned width as their tightly packed row stride. Native
 * tasks expose only their last complete published frame, including dimensions,
 * even after timer preemption, stop or error. BASIC/exec draw synchronously. */
const unsigned char *term_canvas(void);
int term_canvas_width(void);
int term_canvas_height(void);
/* Visible frame dimensions for an explicit owner, without changing selection.
 * Returns 1 when present; otherwise returns 0 and clears both outputs. Native
 * working-frame changes stay hidden until publication, as in term_canvas(). */
int term_canvas_size(int slot, int *width, int *height);

/* Scrollback, oldest first. */
int term_count(void);
const char *term_get(int i);

/* Live row: prompt text plus whatever has been typed so far. */
void term_prompt(char *out, int max);
const char *term_input(void);

void term_char(char c);
void term_backspace(void);
/* Refuse an incomplete command after finite device-ingress loss; retain draft. */
void term_input_lost(int slot);
void term_enter(void);
/* Desktop input ownership around synchronous BASIC/exec only. */
void term_set_program_input(void (*hook)(int active));

/* Desktop-owned task integration. Poll once per desktop turn, never in IRQs. */
#define TERM_TASK_NAME_LEN 24
#define TERM_TASK_TITLE_LEN (TERM_TASK_NAME_LEN*2+3)
typedef struct {
    char name[TERM_TASK_NAME_LEN]; /* Copied basename, not a mutable filesystem pointer. */
    char document[TERM_TASK_NAME_LEN]; /* Copied startup basename, or empty. */
    int owner;                   /* Zero-based terminal/window slot. */
    int state;                   /* PROCESS_TASK_READY or PROCESS_TASK_SLEEPING. */
    unsigned started_ticks;      /* PIT time at the accepted start. */
    unsigned elapsed_sec;        /* Wall-clock lifetime, including sleep. */
    unsigned instance;           /* Full opaque process handle; recheck before UI actions. */
} TermTaskInfo;
/* Returns 1 for a live task, otherwise 0 and a cleared output. No selection change. */
int term_task_info(int slot, TermTaskInfo *out);
/* Copies the live program name, optionally followed by " - document". Returns
 * 1 if live; otherwise clears out and returns 0. Does not change selection. */
int term_task_title(int slot, char *out, int capacity);
/* Start from an identity-matching ordinary file, copying its image and name.
 * Returns 0 on success, -1 on failure; preserves selected terminal and rejects
 * live owners without clearing their canvas, input or metadata. */
int term_task_start_file(int slot, int file, unsigned identity);
int term_task_start_file_with_arg(int slot, int file, unsigned identity,
                                   const char *argument, unsigned argument_length);
enum {
    TERM_TASK_CANVAS = 1,    /* Published pixels; existing canvas layout is stable. */
    TERM_TASK_TEXT = 2,      /* Scrollback/live-row content. */
    TERM_TASK_LAYOUT = 4,    /* First canvas activation, reset or resize. */
    TERM_TASK_LIFECYCLE = 8  /* Start/end changes the window/taskbar title. */
};
typedef struct { int slot; unsigned flags; } TermTaskUpdate;
/* Runs at most one ready task slice, preserving the selected terminal. Pending
 * changes are consumed for that slot; slot is -1 when no task ran. */
TermTaskUpdate term_task_poll_update(void);
/* Compatibility for callers which always redraw the entire desktop. */
int term_task_poll(void);
int term_task_running(int slot);
int term_task_key(int slot, int key);
void term_task_stop(int slot);
/* Returns 0 while an active process is stopping; callers must not reuse/reset. */
int term_task_close(int slot);

#endif
