/* Two seconds of stereo square waves through an audio stream: 441 Hz on
 * the left, 882 Hz on the right. Build with i686-baseos-gcc. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "sdk/baseos_sys.h"

static long sys(long number, long a, long b, long c) {
    long result;
    __asm__ volatile("int $0x81" : "=a"(result) : "a"(number), "b"(a), "c"(b), "d"(c) : "memory");
    return result;
}

int main(void) {
    enum { RATE = 44100, CHUNK = 1470 };
    static short frames[CHUNK * 2];
    int stream = (int)sys(BOS_SYS_AUDIO_OPEN, RATE, 2, 0);
    if (stream <= 0) { printf("tone: open failed %d\n", stream); return 1; }
    long total = 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int chunk = 0; chunk < 2 * RATE / CHUNK; chunk++) {
        for (int i = 0; i < CHUNK; i++) {
            long frame = total + i;
            frames[i * 2] = (frame % 100) < 50 ? 12000 : -12000;
            frames[i * 2 + 1] = (frame % 50) < 25 ? 9000 : -9000;
        }
        long written = sys(BOS_SYS_AUDIO_WRITE, stream, (long)frames, CHUNK);
        if (written != CHUNK) { printf("tone: write returned %ld\n", written); return 1; }
        total += CHUNK;
    }
    while (sys(BOS_SYS_AUDIO_QUEUED, stream, 0, 0) > 0) {
        struct timespec pause = {0, 5000000};
        nanosleep(&pause, 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    long ms = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000;
    struct timespec tail = {0, 400000000};
    nanosleep(&tail, 0);
    sys(BOS_SYS_AUDIO_CLOSE, stream, 0, 0);
    printf("tone ok: %ld frames in %ld ms\n", total, ms);
    return 0;
}
