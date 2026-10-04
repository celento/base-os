/* Two normal version-bound readers of the complete supported 16 MiB file. */
#define main legacy_large_main
#include "large_volume_host.c"
#undef main
#include "../src/native_files.c"
unsigned large_test_polls;
int main(void) {
    for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (char)(i * 17 + i / 4096);
    high_available = 1; mark();
    assert(fs_init() == 0 && fs_load_disk() == FS_LOAD_BLANK && fs_large_profile());
    assert(fs_empty_dir(0) == 0);
    int docs = fs_mkdir(0, "Documents"); assert(docs > 0);
    int id = fs_create(docs, "maximum.bin"); assert(id > 0);
    assert(fs_write(id, bytes, sizeof(bytes)) == (int)sizeof(bytes));
    assert(fs_file_limit() == FS_LARGE_FILE_MAX);
    native_files_init();
    BosFileInfo a, b;
    unsigned owner_a = BOS_HANDLE_TYPE_PROCESS | 1u, owner_b = BOS_HANDLE_TYPE_PROCESS | 2u;
    assert(native_file_open(owner_a, "/Documents/maximum.bin", BOS_FILE_OPEN_READ | BOS_FILE_OPEN_WRITE, &a) == BOS_OK);
    assert(native_file_open(owner_b, "/Documents/maximum.bin", BOS_FILE_OPEN_READ, &b) == BOS_OK);
    assert(a.size == sizeof(bytes) && b.size == a.size && a.revision == b.revision);
    char chunk[NATIVE_FILE_READ_MAX];
    for (unsigned offset = 0; offset < sizeof(bytes); offset += sizeof(chunk)) {
        assert(native_file_read_at(owner_a, a.handle, offset, chunk, sizeof(chunk)) == (int)sizeof(chunk));
        assert(!memcmp(chunk, bytes + offset, sizeof(chunk)));
        assert(native_file_read_at(owner_b, b.handle, offset, chunk, sizeof(chunk)) == (int)sizeof(chunk));
        assert(!memcmp(chunk, bytes + offset, sizeof(chunk)));
    }
    assert(native_file_read_at(owner_a, a.handle, sizeof(bytes), chunk, sizeof(chunk)) == 0);
    assert(native_file_replace(owner_a, a.handle, bytes, NATIVE_FILE_REPLACE_MAX, &a) == BOS_OK);
    assert(a.size == NATIVE_FILE_REPLACE_MAX && fs_size(id) == NATIVE_FILE_REPLACE_MAX);
    memset(chunk, 0x55, sizeof(chunk));
    assert(native_file_read_at(owner_b, b.handle, 0, chunk, sizeof(chunk)) == BOS_E_CHANGED);
    for (unsigned i = 0; i < sizeof(chunk); ++i) assert(chunk[i] == 0x55);
    native_files_release_owner(owner_a); native_files_release_owner(owner_b);
    puts("versioned files: two exact full 16 MiB large-profile streams, bounded replacement and coherent invalidation passed");
    return 0;
}
