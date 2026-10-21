/* A C program for the BaseOS ELF runtime, built with the cross toolchain:
 *   build/toolchain/bin/i686-baseos-gcc -O2 hello.c -o hello -lpthread
 * It uses stdio, the heap, time and POSIX threads. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long total;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *work(void *argument) {
    long id = (long)argument;
    for (int i = 0; i < 100000; ++i) {
        pthread_mutex_lock(&lock);
        total += id;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int main(int argc, char **argv) {
    printf("Hello from C on BaseOS (%d argument%s)\n", argc, argc == 1 ? "" : "s");
    size_t bytes = 32u << 20;
    unsigned char *block = malloc(bytes);
    if (!block) {
        puts("malloc failed");
        return 1;
    }
    memset(block, 0xA5, bytes);
    printf("malloc and memset of %zu MiB ok\n", bytes >> 20);
    free(block);

    pthread_t threads[4];
    for (long i = 0; i < 4; ++i) pthread_create(&threads[i], NULL, work, (void *)(i + 1));
    for (int i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
    printf("threads total %ld (expected %ld)\n", total, 100000L * (1 + 2 + 3 + 4));

    time_t now = time(NULL);
    struct tm parts;
    gmtime_r(&now, &parts);
    printf("UTC year %d\n", parts.tm_year + 1900);
    (void)argv;
    return total == 1000000 ? 0 : 2;
}
