#ifndef PROGRAM_H
#define PROGRAM_H
#include <stdint.h>
#include "address_space.h"
#include "executable.h"
/* Source-identified enabled verification candidate. Release support requires
 * ordinary private-space/mixed-format gates; see docs/ADDRESS_SPACES.md. */
#ifndef BASEOS_BEX2_ENABLED
#define BASEOS_BEX2_ENABLED 1
#endif
#define PROCESS_PRIVATE_PAGE_LIMIT 1024u
/* Callbacks operate only on the owning terminal's bounded canvas/output. */
#define PROCESS_IMAGE_LIMIT 49152u
#define PROCESS_FILE_CHUNK_MAX 4096u
#define PROCESS_DOCUMENT_MAX 32768u
#define PROCESS_ARGUMENT_MAX 128u
#define PROGRAM_CANVAS_DEFAULT_WIDTH 160u
#define PROGRAM_CANVAS_DEFAULT_HEIGHT 100u
#define PROGRAM_CANVAS_MAX_WIDTH 320u
#define PROGRAM_CANVAS_MAX_HEIGHT 200u
typedef struct {
    void (*print)(const char *);
    void (*plot)(int,int,int);
    int (*key)(void);
    void (*present)(void);
    /* Optional; a successful resize explicitly clears and activates the canvas. */
    int (*resize)(int width,int height);
    /* Optional; same clipped pixels/dirty state as plot, without publication.
     * Width/height are positive and bounded by the active canvas. */
    void (*rect)(int x,int y,int width,int height,int color);
} ProgramIO;
int basic_run(const char *source, int length, const ProgramIO *io);
void process_init(void);
int process_run(const void *file, unsigned bytes, const ProgramIO *io);
int process_interrupt(uint32_t *registers);
/* Internal process identity is the existing opaque owner handle, never a
 * record index or display slot. BEX1 call 12 retains an explicit display ID. */
typedef uint32_t ProcessHandle;
#define PROCESS_TASKS 8
#define PROCESS_TASK_EMPTY 0
#define PROCESS_TASK_READY 1
#define PROCESS_TASK_SLEEPING 2
#define PROCESS_TASK_DONE 3
#define PROCESS_TASK_CREATED 4
#define PROCESS_TASK_EXITING 5
#define PROCESS_TASK_CREATING 6
#define PROCESS_TASK_STOPPED (-4)
enum { PROCESS_EXIT_NONE, PROCESS_EXIT_APP, PROCESS_EXIT_ERROR, PROCESS_EXIT_STOP };
typedef struct { int value; unsigned reason; } ProcessResult;
typedef struct {
    ProcessHandle process;
    unsigned slot, generation;
} ProcessBinding;
/* This table and its bounded context are copied; no borrowed Terminal/user
 * pointer survives launch. Callbacks validate the complete attachment tuple. */
typedef struct {
    ProcessBinding binding;
    void (*print)(const ProcessBinding *,const char *);
    void (*plot)(const ProcessBinding *,int,int,int);
    void (*present)(const ProcessBinding *);
    int (*resize)(const ProcessBinding *,int,int);
    void (*rect)(const ProcessBinding *,int,int,int,int,int);
} ProcessIO;
typedef struct {
    unsigned records, created, live, exiting, done, owned;
} ProcessCounts;
/* Private creation result: backing capacity/readiness unavailable. The public
 * BEX1 syscall ABI is unchanged. Creation failure leaves out_process untouched. */
#define PROCESS_CREATE_MEMORY (-3)
#define PROCESS_CREATE_UNSUPPORTED (-5)
#define PROCESS_CREATE_LAYOUT (-6)
/* Creation is not runnable. BEX1 owns16 backing pages; enabled BEX2 owns its
 * complete declared commitment plus a private directory/table before publish. */
int process_create(const void *file,unsigned bytes,const char *argument,
                   unsigned argument_length,ProcessHandle *out_process);
int process_bind(ProcessHandle process,const ProcessIO *io);
/* Only an unscheduled CREATED record can bind/unbind. Unbind owns no cleanup. */
int process_unbind(ProcessHandle process);
int process_start(ProcessHandle process);
int process_status(ProcessHandle process);
/* Complete copied binding and immediate stopping state, for owned services. */
int process_binding_live(const ProcessBinding *binding);
int process_get_result(ProcessHandle process,ProcessResult *out);
int process_key(ProcessHandle process,int key);
/* Returns 1 once stopped/inactive, 0 if its active slice must first return. */
int process_request_stop(ProcessHandle process);
/* Consume DONE, then reap. Repeated/stale handles cannot affect another record. */
int process_reap(ProcessHandle process);
/* Run at most one PIT tick; scheduling rotates process records, not displays. */
int process_step(ProcessHandle process);
ProcessHandle process_schedule_one(void);
void process_counts(ProcessCounts *out);
int program_key(void);
void program_present(void);
#endif
