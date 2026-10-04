#ifndef PROGRAM_H
#define PROGRAM_H
#include <stdint.h>
/* Callbacks operate only on the owning terminal's bounded canvas/output. */
typedef struct { void (*print)(const char *); void (*plot)(int,int,int); int (*key)(void); void (*present)(void); } ProgramIO;
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
