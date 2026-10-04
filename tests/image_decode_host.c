/* Ordinary reference-image decoding, with host ASan/UBSan instrumentation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image_decode.h"

int main(int argc, char **argv) {
    assert(argc == 3 || argc == 4);
    FILE *input = fopen(argv[1], "rb"); assert(input);
    assert(!fseek(input, 0, SEEK_END));
    long length = ftell(input); assert(length > 0 && length <= 4 * 1024 * 1024);
    rewind(input);
    unsigned char *source = malloc((size_t)length); assert(source);
    assert(fread(source, 1, (size_t)length, input) == (size_t)length);
    fclose(input);
    unsigned capacity = argc == 4 ? (unsigned)strtoul(argv[3], 0, 0) : 0x640000u;
    unsigned char *workspace = malloc(capacity); assert(workspace);
    ImageDecoded image;
    int result = image_decode(source, (unsigned)length, workspace, capacity, &image);
    FILE *output = fopen(argv[2], "wb"); assert(output);
    fprintf(output, "%d %u %u %u %u %u\n", result, image.width, image.height,
            image.channels, image.format, image.workspace_peak);
    if (result == IMAGE_OK) {
        unsigned bytes = image.width * image.height * (image.channels ? image.channels : 1);
        assert(image.pixels >= workspace && image.pixels + bytes <= workspace + capacity);
        unsigned char *reference = malloc(bytes); assert(reference);
        memcpy(reference, image.pixels, bytes);
        /* Mutating or compacting the source after an open cannot affect pixels. */
        memset(source, 0, (size_t)length);
        assert(!memcmp(reference, image.pixels, bytes));
        assert(fwrite(image.pixels, 1, bytes, output) == bytes);
        free(reference);
    }
    fclose(output);
    free(workspace); free(source);
    return 0;
}
