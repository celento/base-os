/* Host check: decode a raw LZMA1 payload with the boot decoder and compare. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/boot_lzma.c"

static unsigned char *load(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(*size ? *size : 1);
    if (fread(data, 1, *size, f) != (size_t)*size) { fclose(f); free(data); return 0; }
    fclose(f);
    return data;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    long packed_size, raw_size;
    unsigned char *packed = load(argv[1], &packed_size), *raw = load(argv[2], &raw_size);
    if (!packed || !raw) return 2;
    unsigned char *out = malloc(raw_size ? raw_size : 1);
    BootLzmaProbs *probs = malloc(sizeof *probs);
    int status = boot_lzma(packed, (uint32_t)packed_size, out, (uint32_t)raw_size, probs);
    if (status) { printf("decode failed: %d\n", status); return 1; }
    if (memcmp(out, raw, raw_size)) { printf("mismatch\n"); return 1; }
    /* Truncated payloads never read past their end (ASan checks this on a
     * separately allocated copy). Dropping only trailing range-coder flush
     * bytes can still decode correctly; anything else must report failure. */
    for (long cut = 0; cut < packed_size; cut += cut < 64 ? 1 : packed_size / 16 + 1) {
        unsigned char *copy = malloc(cut ? cut : 1);
        memcpy(copy, packed, cut);
        if (!boot_lzma(copy, (uint32_t)cut, out, (uint32_t)raw_size, probs) &&
            memcmp(out, raw, raw_size)) {
            printf("truncated payload produced wrong output at %ld\n", cut); return 1;
        }
        free(copy);
    }
    printf("boot LZMA decoder: %ld -> %ld bytes OK\n", packed_size, raw_size);
    free(out); free(probs); free(packed); free(raw);
    return 0;
}
