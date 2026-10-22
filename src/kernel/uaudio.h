#ifndef UAUDIO_H
#define UAUDIO_H
#include <stdint.h>

/* Audio streams of ELF processes (BOS_SYS_AUDIO_*), played through the
 * kernel's PCM stream (kernel/audio.h). Serialized kernel context only. */
struct Uproc;
int uaudio_open(struct Uproc *process, uint32_t rate, uint32_t channels);
int uaudio_write(struct Uproc *process, uint32_t handle, uint32_t frames, uint32_t count);
int uaudio_queued(struct Uproc *process, uint32_t handle);
int uaudio_close(struct Uproc *process, uint32_t handle);
/* The process is ending: its streams close. */
void uaudio_release_owner(uint32_t pid);
#endif
