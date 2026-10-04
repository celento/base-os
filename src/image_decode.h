#ifndef BASEOS_IMAGE_DECODE_H
#define BASEOS_IMAGE_DECODE_H

#include <stdint.h>

#define IMAGE_MAX_DIMENSION 1024
#define IMAGE_MAX_PIXELS (1024u * 768u)
#define IMAGE_MAX_FILE_BYTES (2u * 1024u * 1024u)

enum {
    IMAGE_OK = 0,
    IMAGE_BAD_FILE = -1,
    IMAGE_UNSUPPORTED = -2,
    IMAGE_TOO_LARGE = -3,
    IMAGE_NO_MEMORY = -4
};
enum {
    IMAGE_FORMAT_NONE, IMAGE_FORMAT_BOS1, IMAGE_FORMAT_JPEG,
    IMAGE_FORMAT_PNG, IMAGE_FORMAT_BMP, IMAGE_FORMAT_GIF
};

/* Pixels are owned by the supplied workspace. Channels 1/2/3/4 mean
 * gray/gray-alpha/RGB/RGBA; 0 means original BaseOS palette indices (BOS1).
 * A successful decode never keeps a pointer to the source file. */
typedef struct {
    uint8_t *pixels;
    unsigned width, height, channels, format;
    unsigned workspace_peak;
} ImageDecoded;

/* One synchronous decoder at a time. Workspace must remain alive until the
 * result is consumed; the next decode into it invalidates the earlier result.
 * No heap, FPU, filesystem calls, or global image-size allocations are used. */
int image_decode(const void *file, unsigned bytes, void *workspace,
                 unsigned workspace_bytes, ImageDecoded *result);
int image_probe(const void *file, unsigned bytes);
const char *image_format_name(unsigned format);
const char *image_error_string(int error);

#endif
