#ifndef SYSMON_APP_H
#define SYSMON_APP_H

#include "term.h"

#define SYSMON_W 520
#define SYSMON_H 440
#define SYSMON_MAX_WIN 8
#define SYSMON_MAX_TASKS 8

enum { SYSMON_TAB_SYSTEM, SYSMON_TAB_WINDOWS, SYSMON_TAB_TASKS };
enum {
    SYSMON_ACTION_NONE,
    SYSMON_ACTION_REDRAW,
    SYSMON_ACTION_CLOSE_WINDOW,
    SYSMON_ACTION_SHOW_TERMINAL,
    SYSMON_ACTION_STOP_TASK
};
#define SYSMON_ACTION_SHOW_WINDOW SYSMON_ACTION_SHOW_TERMINAL
typedef struct {
    int kind;
    int owner;                    /* Window/terminal slot, only for an action. */
    unsigned task_instance;       /* Process handle, or window seq for Close. */
} SysmonAction;

typedef struct {
    unsigned uptime_sec;
    unsigned frames;              /* Actual compositor redraws, never PIT ticks. */
    int win_n;
    const char *win_name[SYSMON_MAX_WIN];
    char win_title[SYSMON_MAX_WIN][APP_VIEW_TITLE_LEN];
    int win_id[SYSMON_MAX_WIN];
    unsigned win_instance[SYSMON_MAX_WIN];
    int win_min[SYSMON_MAX_WIN];
    int task_n;
    TermTaskInfo tasks[SYSMON_MAX_TASKS];
    int fs_nodes;
    int fs_max;
    int fs_bytes;
    int fs_cap;
    int fb_w, fb_h, fb_bpp;
    int mem_mb;
    unsigned redraws;
} SysInfo;

/* A single-instance monitor. Reset is optional when opening a fresh window. */
void sysmon_reset(void);
int sysmon_tab(void);
void sysmon_draw(int bx, int by, int bw, int bh, const SysInfo *si);
/* Same client bounds as draw; controls produce actions, never mutate tasks/windows. */
SysmonAction sysmon_click(int bx, int by, int bw, int bh, int mx, int my, const SysInfo *si);

#endif
