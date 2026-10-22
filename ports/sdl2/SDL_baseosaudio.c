/* SDL audio driver for BaseOS: plays through a kernel PCM stream
 * (BOS_SYS_AUDIO_*). AUDIO_WRITE waits for room, which paces SDL's audio
 * thread. Copied into SDL's src/audio/baseos by ports/build.py. */
#include "../../SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_BASEOS

#include "SDL_audio.h"
#include "../SDL_audio_c.h"
#include "../SDL_sysaudio.h"
#define _THIS SDL_AudioDevice *_this
#include <baseos_sys.h>

struct SDL_PrivateAudioData {
    int stream;
    Uint8 *mixbuf;
};

static long sys(long number, long a, long b, long c)
{
    long result;
    __asm__ volatile("int $0x81" : "=a"(result) : "a"(number), "b"(a), "c"(b), "d"(c) : "memory");
    return result;
}

static int BASEOS_OpenDevice(_THIS, const char *devname)
{
    (void)devname;
    _this->hidden = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*_this->hidden));
    if (!_this->hidden) {
        return SDL_OutOfMemory();
    }
    _this->spec.format = AUDIO_S16LSB;
    if (_this->spec.channels > 2) {
        _this->spec.channels = 2;
    }
    if (_this->spec.channels < 1) {
        _this->spec.channels = 1;
    }
    if (_this->spec.freq < BOS_AUDIO_RATE_MIN) {
        _this->spec.freq = BOS_AUDIO_RATE_MIN;
    }
    if (_this->spec.freq > BOS_AUDIO_RATE_MAX) {
        _this->spec.freq = BOS_AUDIO_RATE_MAX;
    }
    SDL_CalculateAudioSpec(&_this->spec);
    _this->hidden->mixbuf = (Uint8 *)SDL_malloc(_this->spec.size);
    if (!_this->hidden->mixbuf) {
        return SDL_OutOfMemory();
    }
    SDL_memset(_this->hidden->mixbuf, _this->spec.silence, _this->spec.size);
    _this->hidden->stream = (int)sys(BOS_SYS_AUDIO_OPEN, _this->spec.freq, _this->spec.channels, 0);
    if (_this->hidden->stream <= 0) {
        return SDL_SetError("BaseOS: cannot open an audio stream");
    }
    return 0;
}

static void BASEOS_PlayDevice(_THIS)
{
    long frames = _this->spec.size / (_this->spec.channels * 2);
    sys(BOS_SYS_AUDIO_WRITE, _this->hidden->stream, (long)_this->hidden->mixbuf, frames);
}

static Uint8 *BASEOS_GetDeviceBuf(_THIS)
{
    return _this->hidden->mixbuf;
}

static void BASEOS_CloseDevice(_THIS)
{
    if (_this->hidden->stream > 0) {
        sys(BOS_SYS_AUDIO_CLOSE, _this->hidden->stream, 0, 0);
    }
    SDL_free(_this->hidden->mixbuf);
    SDL_free(_this->hidden);
}

static SDL_bool BASEOS_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = BASEOS_OpenDevice;
    impl->PlayDevice = BASEOS_PlayDevice;
    impl->GetDeviceBuf = BASEOS_GetDeviceBuf;
    impl->CloseDevice = BASEOS_CloseDevice;
    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;
    return SDL_TRUE;
}

AudioBootStrap BASEOS_audio_bootstrap = {
    "baseos", "BaseOS audio streams", BASEOS_Init, SDL_FALSE
};

#endif /* SDL_AUDIO_DRIVER_BASEOS */
