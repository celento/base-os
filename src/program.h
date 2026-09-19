#ifndef PROGRAM_H
#define PROGRAM_H
#include <stdint.h>
/* Callbacks operate only on the owning terminal's bounded canvas/output. */
typedef struct { void (*print)(const char *); void (*plot)(int,int,int); int (*key)(void); void (*present)(void); } ProgramIO;
int basic_run(const char *source, int length, const ProgramIO *io);
void process_init(void);
int process_run(const void *file, unsigned bytes, const ProgramIO *io);
int process_interrupt(uint32_t *registers);
int program_key(void);
void program_present(void);
#endif
