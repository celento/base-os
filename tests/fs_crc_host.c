/* The actual filesystem implementation, with its ordinary in-memory disks.
 * None of the legacy fault-injection test entry point is executed here. */
#define main unused_legacy_suite
#include "fs_host.c"
#undef main

extern unsigned crc_test_polls;
static unsigned char input[FS_IMG_CAPACITY];

static void snapshot(const char *path) {
    reset();
    unsigned marker[] = {DATA_MARKER_MAGIC, DATA_MARKER_VERSION,
                         DATA_DISK_SECTORS, DATA_SLOT_SECTORS,
                         DATA_FIRST_LBA, DATA_SECOND_LBA, 0};
    marker[6] = crc32(marker, 24);
    memcpy(data_disk, marker, sizeof(marker));
    data_present = 1;
    fs_init(); assert(fs_load_disk() == FS_LOAD_BLANK);
    fs_empty_dir(0);
    for (int i = 1; i < FS_MAX_NODES; ++i) {
        char name[FS_NAME_LEN]; snprintf(name, sizeof(name), "file%d", i);
        assert(fs_create(0, name) == i);
    }
    for (unsigned i = 0; i < FS_FILE_MAX; ++i)
        input[i] = (unsigned char)(i * 37u + (i >> 16) + 19u);
    for (int id = 1; id <= 4; ++id) {
        unsigned n = id < 4 ? FS_FILE_MAX : fs_capacity() - 3 * FS_FILE_MAX;
        assert(fs_write(id, (const char *)input, n) == (int)n);
    }
    assert(fs_used_bytes() == fs_capacity() && fs_capacity() == 8377344u);
    assert(fs_sync() == 0);
    fs_init(); assert(fs_load_disk() == 0 && fs_node_count() == FS_MAX_NODES);
    /* A second ordinary full-capacity commit, preserving the first snapshot. */
    assert(fs_write(1, fs_data(1), fs_size(1)) == FS_FILE_MAX);
    assert(fs_sync() == 0);
    FILE *out = fopen(path, "wb"); assert(out);
    assert(fwrite(data_disk, 1, sizeof(data_disk), out) == sizeof(data_disk));
    assert(fclose(out) == 0);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--snapshot")) {
        snapshot(argv[2]); return 0;
    }
    assert(argc == 1);
    unsigned char size[4];
    while (fread(size, 1, sizeof(size), stdin) == sizeof(size)) {
        unsigned n = (unsigned)size[0] | (unsigned)size[1] << 8 |
                     (unsigned)size[2] << 16 | (unsigned)size[3] << 24;
        assert(n <= sizeof(input));
        assert(fread(input, 1, n, stdin) == n);
        crc_test_polls = 0;
        unsigned sum = crc32(input, n);
        printf("%08x %u\n", sum, crc_test_polls);
    }
    assert(feof(stdin));
    return 0;
}
