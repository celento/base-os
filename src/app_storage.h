#ifndef APP_STORAGE_H
#define APP_STORAGE_H
#include <stddef.h>
#include "layout.h"
#include "term.h"
/* Storage only: Terminal command state does not own process identity or pixels.
 * All fields and arrays remain in the original reserved Terminal container. */
typedef struct {
    char lines[TERM_LINES][TERM_COLS+1],input[TERM_COLS+1];
    char history[16][TERM_COLS+1],draft[TERM_COLS+1];
    int head,count,len,cwd,hcount,hpos,scroll,rows,view_count,input_overflow;
    int input_invalid,draft_invalid;
    unsigned cwd_identity;
} TerminalText;
typedef struct {
    ProcessBinding binding;
    unsigned task_dirty;
    char task_name[APP_VIEW_NAME_LEN],task_document[APP_VIEW_NAME_LEN];
    unsigned task_started;
    AppViewOutput output;
    AppViewBackend backend;
    AppViewResult result;
    unsigned executable_identity;
    int result_valid,close_pending,output_visible;
    AppCanvas canvas;
} AppView;
/* Each print callback is already a normalized, <=80-column row from the
 * unchanged process write syscall. The newest 48 rows survive overflow. */
typedef struct {
    char lines[APP_VIEW_OUTPUT_ROWS][APP_VIEW_OUTPUT_COLS+1];
    unsigned head,count,dropped;
} AppViewLog;
typedef struct {
    TerminalText terminals[PROCESS_TASKS];
    AppView views[PROCESS_TASKS];
    AppViewLog logs[PROCESS_TASKS];
} AppStorage;
#ifndef TERM_MEMORY
#define TERM_MEMORY (APPS_BASE+0x300000)
#endif
#ifndef NATIVE_CANVAS_MEMORY
#define NATIVE_CANVAS_MEMORY NATIVE_CANVAS_BASE
#endif
#define APP_STORAGE_CAPACITY 0xC0000u
_Static_assert(sizeof(TerminalText)<=28672,"terminal text budget overflow");
_Static_assert(offsetof(AppView,canvas)<=256,"app view metadata budget overflow");
_Static_assert(sizeof(AppViewLog)<=4096,"app view output log budget overflow");
_Static_assert(sizeof(AppStorage)<=APP_STORAGE_CAPACITY,"app-view arena overflow");
_Static_assert(PROCESS_TASKS*APP_CANVAS_PIXELS<=NATIVE_CANVAS_CAPACITY,"published canvas arena overflow");
void app_storage_init(void);
#endif
