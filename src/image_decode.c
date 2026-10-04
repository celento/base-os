/* Integer-only, bounded freestanding wrapper around the pinned stb decoder.
 * The small coalescing arena is private to one synchronous decode. */
#include "image_decode.h"
#include <stddef.h>

static ImagePollHook poll_hook;
static unsigned poll_work;
ImagePollHook image_decode_set_poll_hook(ImagePollHook hook) {
    ImagePollHook previous = poll_hook;
    poll_hook = hook; poll_work = 0;
    return previous;
}
static void image_progress(unsigned work) {
    if (!poll_hook) return;
    poll_work += work;
    if (poll_work >= 4096) { poll_work = 0; poll_hook(); }
}

static void *image_copy(void *to, const void *from, size_t bytes) {
    uint8_t *d = to; const uint8_t *s = from;
    while (bytes) {
        unsigned chunk = bytes > 4096 ? 4096 : (unsigned)bytes;
        for (unsigned i = 0; i < chunk; i++) *d++ = *s++;
        bytes -= chunk; image_progress(chunk);
    }
    return to;
}
static void *image_set(void *to, int value, size_t bytes) {
    uint8_t *d = to;
    while (bytes) {
        unsigned chunk = bytes > 4096 ? 4096 : (unsigned)bytes;
        for (unsigned i = 0; i < chunk; i++) *d++ = (uint8_t)value;
        bytes -= chunk; image_progress(chunk);
    }
    return to;
}
static int image_compare(const void *left, const void *right, size_t bytes) {
    const uint8_t *a = left, *b = right;
    while (bytes--) { if (*a != *b) return (int)*a - (int)*b; a++; b++; }
    return 0;
}
static int image_abs(int value) {
    /* A BMP's signed height is rejected later if outside our dimension cap. */
    if (value == INT32_MIN) return INT32_MAX;
    return value < 0 ? -value : value;
}

#define ARENA_END UINT32_MAX
typedef struct { uint32_t size, previous, next, available; } ImageBlock;
_Static_assert(sizeof(ImageBlock) == 16, "image allocation alignment");
static struct {
    uint8_t *memory;
    unsigned capacity, used, peak;
    int exhausted;
} arena;

static ImageBlock *block_at(unsigned offset) {
    return (ImageBlock *)(arena.memory + offset);
}
static void block_split(ImageBlock *block, unsigned bytes) {
    if (block->size < bytes + sizeof(ImageBlock) + 16) return;
    unsigned offset = (unsigned)((uint8_t *)block - arena.memory);
    unsigned next = offset + sizeof(ImageBlock) + bytes;
    ImageBlock *tail = block_at(next);
    *tail = (ImageBlock){block->size - bytes - sizeof(ImageBlock), offset,
                         block->next, 1};
    if (tail->next != ARENA_END) block_at(tail->next)->previous = next;
    block->next = next;
    block->size = bytes;
}
static void block_join_next(ImageBlock *block) {
    if (block->next == ARENA_END) return;
    ImageBlock *next = block_at(block->next);
    if (!next->available) return;
    block->size += sizeof(ImageBlock) + next->size;
    block->next = next->next;
    if (block->next != ARENA_END)
        block_at(block->next)->previous = (unsigned)((uint8_t *)block - arena.memory);
}
static void *image_allocate(size_t requested) {
    if (!requested) requested = 1;
    if (requested > arena.capacity || requested > UINT32_MAX - 15) {
        arena.exhausted = 1; return NULL;
    }
    unsigned bytes = ((unsigned)requested + 15u) & ~15u;
    for (unsigned offset = 0; offset != ARENA_END; ) {
        ImageBlock *block = block_at(offset);
        if (block->available && block->size >= bytes) {
            /* Large decoded planes grow from the opposite end to compressed
             * file-sized chunks. Once PNG releases its IDAT bytes, the front
             * stays contiguous for its second full-size pixel plane. */
            if (bytes > IMAGE_MAX_FILE_BYTES && block->size >= bytes + sizeof(ImageBlock) + 16) {
                block_split(block, block->size - bytes - sizeof(ImageBlock));
                block = block_at(block->next);
            } else block_split(block, bytes);
            block->available = 0;
            arena.used += block->size + sizeof(ImageBlock);
            if (arena.used > arena.peak) arena.peak = arena.used;
            return block + 1;
        }
        offset = block->next;
    }
    arena.exhausted = 1;
    return NULL;
}
static void image_free(void *pointer) {
    if (!pointer) return;
    ImageBlock *block = (ImageBlock *)pointer - 1;
    arena.used -= block->size + sizeof(ImageBlock);
    block->available = 1;
    block_join_next(block);
    if (block->previous != ARENA_END) {
        ImageBlock *previous = block_at(block->previous);
        if (previous->available) block_join_next(previous);
    }
}
static void *image_resize(void *pointer, size_t old_bytes, size_t new_bytes) {
    if (!pointer) return image_allocate(new_bytes);
    if (!new_bytes) { image_free(pointer); return NULL; }
    ImageBlock *block = (ImageBlock *)pointer - 1;
    unsigned original = block->size;
    if (new_bytes <= original) return pointer;
    if (new_bytes > arena.capacity || new_bytes > UINT32_MAX - 15) {
        arena.exhausted = 1; return NULL;
    }
    unsigned needed = ((unsigned)new_bytes + 15u) & ~15u;
    if (block->next != ARENA_END) {
        ImageBlock *next = block_at(block->next);
        if (next->available && original + sizeof(ImageBlock) + next->size >= needed) {
            block_join_next(block);
            block_split(block, needed);
            arena.used += block->size - original;
            if (arena.used > arena.peak) arena.peak = arena.used;
            return pointer;
        }
    }
    void *replacement = image_allocate(new_bytes);
    if (!replacement) return NULL;
    if (old_bytes > original) old_bytes = original;
    image_copy(replacement, pointer, old_bytes);
    image_free(pointer);
    return replacement;
}

#define memcpy image_copy
#define memset image_set
#define memcmp image_compare
#define abs image_abs
#define STBI_MALLOC(bytes) image_allocate(bytes)
#define STBI_REALLOC_SIZED(pointer, old_bytes, new_bytes) image_resize(pointer, old_bytes, new_bytes)
#define STBI_FREE(pointer) image_free(pointer)
#define STBI_ASSERT(expression) ((void)0)
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_FAILURE_STRINGS
#define STBI_BASEOS_PROGRESS(work) image_progress((unsigned)(work))
#define STB_IMAGE_STATIC
#define STBIDEF static __attribute__((unused))
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
/* stb declares three disabled thread-local entry points as static. GCC emits
 * their unused warnings at end-of-translation-unit, after an include-local pop. */
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../third_party/stb/stb_image.h"
#undef memcpy
#undef memset
#undef memcmp
#undef abs

int image_probe(const void *file, unsigned bytes) {
    const uint8_t *p = file;
    if (!p) return IMAGE_FORMAT_NONE;
    if (bytes >= 4 && p[0] == 'B' && p[1] == 'O' && p[2] == 'S' && p[3] == '1')
        return IMAGE_FORMAT_BOS1;
    if (bytes >= 3 && p[0] == 255 && p[1] == 216 && p[2] == 255)
        return IMAGE_FORMAT_JPEG;
    if (bytes >= 8 && !image_compare(p, "\211PNG\r\n\032\n", 8))
        return IMAGE_FORMAT_PNG;
    if (bytes >= 2 && p[0] == 'B' && p[1] == 'M') return IMAGE_FORMAT_BMP;
    if (bytes >= 6 && (!image_compare(p, "GIF87a", 6) || !image_compare(p, "GIF89a", 6)))
        return IMAGE_FORMAT_GIF;
    return IMAGE_FORMAT_NONE;
}

int image_decode(const void *file, unsigned bytes, void *workspace,
                 unsigned workspace_bytes, ImageDecoded *result) {
    if (!result) return IMAGE_BAD_FILE;
    image_set(result, 0, sizeof *result);
    if (!file || !bytes) return IMAGE_BAD_FILE;
    if (bytes > IMAGE_MAX_FILE_BYTES) return IMAGE_TOO_LARGE;
    int format = image_probe(file, bytes);
    if (!format) return IMAGE_UNSUPPORTED;
    uintptr_t start = (uintptr_t)workspace;
    unsigned skip = (unsigned)((16u - (start & 15u)) & 15u);
    if (!workspace || workspace_bytes < skip + sizeof(ImageBlock) + 16)
        return IMAGE_NO_MEMORY;
    arena.memory = (uint8_t *)workspace + skip;
    arena.capacity = (workspace_bytes - skip) & ~15u;
    arena.used = arena.peak = 0;
    arena.exhausted = 0;
    *block_at(0) = (ImageBlock){arena.capacity - sizeof(ImageBlock), ARENA_END, ARENA_END, 1};
    int width = 0, height = 0, channels = 0;
    uint8_t *pixels;
    if (format == IMAGE_FORMAT_BOS1) {
        const uint8_t *p = file;
        if (bytes < 8) return IMAGE_BAD_FILE;
        width = p[4] | (p[5] << 8); height = p[6] | (p[7] << 8);
        if (width < 1 || height < 1) return IMAGE_BAD_FILE;
        if (width > IMAGE_MAX_DIMENSION || height > IMAGE_MAX_DIMENSION ||
            (unsigned)width * (unsigned)height > IMAGE_MAX_PIXELS) return IMAGE_TOO_LARGE;
        unsigned count = (unsigned)width * (unsigned)height;
        if (count > bytes - 8) return IMAGE_BAD_FILE;
        pixels = image_allocate(count);
        if (!pixels) return IMAGE_NO_MEMORY;
        image_copy(pixels, p + 8, count);
    } else {
        if (!stbi_info_from_memory(file, (int)bytes, &width, &height, &channels))
            return arena.exhausted ? IMAGE_NO_MEMORY : IMAGE_BAD_FILE;
        if (width < 1 || height < 1) return IMAGE_BAD_FILE;
        if (width > IMAGE_MAX_DIMENSION || height > IMAGE_MAX_DIMENSION ||
            (unsigned)width * (unsigned)height > IMAGE_MAX_PIXELS) return IMAGE_TOO_LARGE;
        /* Keep native channels: requesting RGBA adds unnecessary allocations
         * for grayscale and RGB PNG/JPEG and increases the bounded peak. */
        pixels = stbi_load_from_memory(file, (int)bytes, &width, &height, &channels, 0);
        if (!pixels) return arena.exhausted ? IMAGE_NO_MEMORY : IMAGE_BAD_FILE;
        if (width < 1 || height < 1 || channels < 1 || channels > 4 ||
            width > IMAGE_MAX_DIMENSION || height > IMAGE_MAX_DIMENSION ||
            (unsigned)width * (unsigned)height > IMAGE_MAX_PIXELS) {
            image_free(pixels); return IMAGE_BAD_FILE;
        }
    }
    *result = (ImageDecoded){pixels, (unsigned)width, (unsigned)height,
                            (unsigned)channels, (unsigned)format, arena.peak};
    return IMAGE_OK;
}

const char *image_format_name(unsigned format) {
    switch (format) {
    case IMAGE_FORMAT_BOS1: return "BOS1";
    case IMAGE_FORMAT_JPEG: return "JPEG";
    case IMAGE_FORMAT_PNG: return "PNG";
    case IMAGE_FORMAT_BMP: return "BMP";
    case IMAGE_FORMAT_GIF: return "GIF (first frame)";
    default: return "Unknown";
    }
}
const char *image_error_string(int error) {
    switch (error) {
    case IMAGE_OK: return "Ready";
    case IMAGE_BAD_FILE: return "Cannot decode this image or image variant";
    case IMAGE_UNSUPPORTED: return "Supported: JPEG, PNG, BMP, GIF and BaseOS Paint";
    case IMAGE_TOO_LARGE: return "Limit: 2 MiB, 1024 px per side, 786432 pixels";
    case IMAGE_NO_MEMORY: return "Image needs too much decoder memory; try a smaller copy";
    default: return "Image could not be opened";
    }
}
