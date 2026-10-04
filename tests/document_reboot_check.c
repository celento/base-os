/* Ordinary SDK-only, read-only reboot acceptance client.  No kernel pointers,
 * model observations, privileged operations, intentional faults or writes. */
#include "baseos.h"

#define MANIFEST_MAGIC 0x31434442u
#define MANIFEST_LIMIT 4096u
#define ENTRY_BYTES 136u
#define PATH_BYTES 128u
static unsigned char manifest[MANIFEST_LIMIT], chunk[BOS_FILE_CHUNK_MAX];

static unsigned word(const unsigned char *p) {
    return (unsigned)p[0] | (unsigned)p[1] << 8 |
           (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}
static void number(unsigned value) {
    char reverse[11], text[12]; unsigned length = 0;
    do { reverse[length++] = (char)('0' + value % 10); value /= 10; } while (value);
    for (unsigned i = 0; i < length; ++i) text[i] = reverse[length - i - 1];
    text[length] = 0; bos_print(text);
}
static int failed(const char *reason, const char *path) {
    bos_print("DOCUMENT VERIFICATION FAILED: "); bos_print(reason);
    if (path) { bos_print(" "); bos_print(path); }
    bos_print("\n"); return 1;
}
int main(void) {
    const char *manifest_path = "/Documents/document-check.bin";
    int size = bos_file_size(manifest_path);
    if (size < 16 || size > (int)sizeof manifest)
        return failed("manifest size", 0);
    if (bos_read_file_at(manifest_path, manifest, (unsigned)size, 0) != size)
        return failed("manifest read", 0);
    unsigned count = word(manifest + 8);
    if (word(manifest) != MANIFEST_MAGIC || word(manifest + 4) != 1 ||
        word(manifest + 12) || !count || count > (MANIFEST_LIMIT - 16) / ENTRY_BYTES ||
        16 + count * ENTRY_BYTES != (unsigned)size)
        return failed("manifest format", 0);
    unsigned total = 0;
    for (unsigned index = 0; index < count; ++index) {
        const unsigned char *entry = manifest + 16 + index * ENTRY_BYTES;
        const char *path = (const char *)entry;
        unsigned length = word(entry + PATH_BYTES), expected = word(entry + PATH_BYTES + 4);
        unsigned offset = 0, hash = 2166136261u, path_length = 0;
        while (path_length < PATH_BYTES && entry[path_length]) {
            if (entry[path_length] < 32 || entry[path_length] > 126)
                return failed("manifest path character", 0);
            ++path_length;
        }
        if (path_length < 2 || path_length == PATH_BYTES || entry[0] != '/' ||
            length > 16777216u) return failed("manifest path or length", 0);
        if (bos_file_size(path) != (int)length) return failed("file size", path);
        while (offset < length) {
            unsigned wanted = length - offset;
            if (wanted > sizeof chunk) wanted = sizeof chunk;
            int got = bos_read_file_at(path, chunk, wanted, offset);
            if (got != (int)wanted) return failed("bounded read", path);
            for (unsigned i = 0; i < wanted; ++i) hash = (hash ^ chunk[i]) * 16777619u;
            offset += wanted;
            if (!(offset & 65535u) && bos_task_id() && bos_sleep(0) < 0)
                return failed("yield", path);
        }
        if (hash != expected) return failed("hash", path);
        if (bos_file_size(path) != (int)length || bos_read_file_at(path, chunk, 1, length) != 0)
            return failed("final size or EOF", path);
        total += length;
        bos_print("Verified "); number(index + 1); bos_print(" "); bos_print(path); bos_print("\n");
    }
    bos_print("DOCUMENT FILES VERIFIED\n");
    bos_print("Files: "); number(count); bos_print(" Bytes: "); number(total); bos_print("\n");
    return 0;
}
