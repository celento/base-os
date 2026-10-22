#ifndef ENTROPY_H
#define ENTROPY_H
#include <stdint.h>

/* Kernel random numbers: a ChaCha20 generator keyed from TSC timing and
 * reseeded with the timing of device interrupts. */
void entropy_mix(uint32_t value);  /* interrupt safe and cheap */
void entropy_fill(void *out, uint32_t length);
#endif
