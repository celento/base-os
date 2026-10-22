/* SDL 2 on BaseOS: a software-rendered window, keyboard and mouse events,
 * and a square-wave tone through SDL audio. Escape or closing the window
 * quits. Build: i686-baseos-gcc sdl_demo.c $(pkg-config --static --cflags --libs sdl2) */
#include <SDL.h>
#include <stdio.h>
#include <string.h>

static void say(const char *text) {
    long result;
    __asm__ volatile("int $0x81" : "=a"(result) : "a"(2), "b"(text), "c"(strlen(text)) : "memory");
    (void)result;
}

static unsigned phase;
static void tone(void *user, Uint8 *stream, int length) {
    (void)user;
    Sint16 *samples = (Sint16 *)stream;
    for (int i = 0; i < length / 4; i++, phase++) {
        samples[i * 2] = (phase % 100) < 50 ? 8000 : -8000;
        samples[i * 2 + 1] = (phase % 50) < 25 ? 6000 : -6000;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char line[160];
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        snprintf(line, sizeof line, "SDL init failed: %s", SDL_GetError());
        say(line);
        return 1;
    }
    snprintf(line, sizeof line, "SDL drivers: video %s, audio %s", SDL_GetCurrentVideoDriver(),
             SDL_GetCurrentAudioDriver());
    say(line);
    SDL_Window *window = SDL_CreateWindow("SDL demo", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                          320, 240, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE) : NULL;
    if (!renderer) {
        snprintf(line, sizeof line, "SDL window failed: %s", SDL_GetError());
        say(line);
        return 1;
    }
    SDL_AudioSpec want = {0}, have;
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = tone;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);
    snprintf(line, sizeof line, "SDL ready: audio %s at %d Hz", audio ? "open" : "missing", have.freq);
    say(line);
    int running = 1, frame = 0;
    Uint32 start = SDL_GetTicks();
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                say("SDL quit");
                running = 0;
                break;
            case SDL_KEYDOWN:
                snprintf(line, sizeof line, "SDL key %s", SDL_GetKeyName(event.key.keysym.sym));
                say(line);
                if (event.key.keysym.sym == SDLK_ESCAPE) running = 0;
                break;
            case SDL_TEXTINPUT:
                snprintf(line, sizeof line, "SDL text %s", event.text.text);
                say(line);
                break;
            case SDL_MOUSEBUTTONDOWN:
                snprintf(line, sizeof line, "SDL click %d at %d,%d", event.button.button, event.button.x,
                         event.button.y);
                say(line);
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    snprintf(line, sizeof line, "SDL resized %d %d", event.window.data1, event.window.data2);
                    say(line);
                }
                break;
            }
        }
        int w, h;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        SDL_SetRenderDrawColor(renderer, 0x12, 0x34, 0x56, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawColor(renderer, 0xE0, 0x70, 0x10, 255);
        SDL_Rect box = {frame % (w - 20 > 0 ? w - 20 : 1), h / 2 - 10, 20, 20};
        SDL_RenderFillRect(renderer, &box);
        SDL_RenderPresent(renderer);
        frame++;
        SDL_Delay(16);
    }
    snprintf(line, sizeof line, "SDL frames %d in %u ms", frame, (unsigned)(SDL_GetTicks() - start));
    say(line);
    if (audio) SDL_CloseAudioDevice(audio);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
