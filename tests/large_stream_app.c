/* An ordinary native app streams the whole 16 MiB file in bounded 4 KiB reads. */
#include "baseos.h"
static unsigned char buffer[4096];
static const char path[] = "/Documents/input-large.bin";
static void check(int ok, int code) { if (!ok) bos_exit(code); }
int main(void) {
    check(bos_file_size(path) == 16777216, 1);
    unsigned total = 0, sum = 0, chunks = 0;
    while (total < 16777216) {
        int count = bos_read_file_at(path, buffer, sizeof(buffer), total);
        check(count == sizeof(buffer), 2);
        for (unsigned i = 0; i < sizeof(buffer); ++i) {
            unsigned offset = total + i;
            check(buffer[i] == (unsigned char)(offset * 37 + (offset >> 16) + 19), 3);
            sum += buffer[i];
        }
        total += count; ++chunks;
        if (!(chunks & 31)) check(!bos_yield(), 4);
    }
    check(bos_read_file_at(path, buffer, sizeof(buffer), total) == 0, 5);
    unsigned result[] = { total, sum, chunks, 0x4c415247 };
    check(bos_replace_file("/Documents/large-stream.bin", result, sizeof(result)) == sizeof(result), 6);
    check(!bos_sync(), 7);
    return 0;
}
