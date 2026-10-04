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
/* Pixels use the active width as their tightly packed row stride. */
const unsigned char *term_canvas(void);
int term_canvas_width(void);
int term_canvas_height(void);

/* Scrollback, oldest first. */
int term_count(void);
const char *term_get(int i);

/* Live row: prompt text plus whatever has been typed so far. */
void term_prompt(char *out, int max);
const char *term_input(void);

void term_char(char c);
void term_backspace(void);
void term_enter(void);

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
    unsigned instance;           /* Changes on each accepted terminal task start. */
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
int term_task_poll(void);
int term_task_running(int slot);
int term_task_key(int slot, int key);
void term_task_stop(int slot);
void term_task_close(int slot);

#endif
