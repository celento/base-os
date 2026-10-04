#ifndef PROGRAM_H
#define PROGRAM_H
#include <stdint.h>
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
/* Bounded desktop-driven native tasks, one per terminal/window slot. */
#define PROCESS_TASKS 8
#define PROCESS_TASK_EMPTY 0
#define PROCESS_TASK_READY 1
#define PROCESS_TASK_SLEEPING 2
#define PROCESS_TASK_DONE 3
#define PROCESS_TASK_STOPPED (-4)
int process_task_start(int owner, const void *file, unsigned bytes, const ProgramIO *io);
/* One optional absolute printable-ASCII path, copied before returning. A zero
 * length means no argument. BEX1 images, entry points and stack ABI are unchanged. */
int process_task_start_with_arg(int owner, const void *file, unsigned bytes,
                                const ProgramIO *io, const char *argument,
                                unsigned argument_length);
/* Run at most one PIT tick of user code. Returns 1 if a slice ran. */
int process_task_step(int owner);
int process_task_status(int owner);
int process_task_result(int owner);
int process_task_key(int owner, int key);
void process_task_stop(int owner);
/* Forget a stopped/completed task, including queued input and saved state. */
void process_task_clear(int owner);
int program_key(void);
void program_present(void);
#endif
