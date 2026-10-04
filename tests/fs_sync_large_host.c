/* Explicit incremental request on a full ordinary large-profile volume. */
#define main legacy_large_main
#include "large_volume_host.c"
#undef main
unsigned large_test_polls;
int main(void) {
    for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (char)(i * 17 + i / 4096);
    high_available = 1; mark();
    assert(fs_init() == 0 && fs_load_disk() == FS_LOAD_BLANK && fs_large_profile());
    assert(fs_empty_dir(0) == 0);
    int first = create("first", FS_LARGE_FILE_MAX);
    int second = create("second", 33543168 - FS_LARGE_FILE_MAX);
    while (fs_node_count() < FS_MAX_NODES) {
        char name[24]; snprintf(name, sizeof(name), "empty%d", fs_node_count());
        assert(fs_create(0, name) > 0);
    }
    assert(fs_used_bytes() == fs_capacity() && fs_capacity() == 33543168);
    memset(pool_arena, 0xd3, sizeof(pool_arena));
    memset(image_arena, 0xe4, sizeof(image_arena));
    FsSyncTicket ticket;
    assert(fs_sync_request(&ticket) == 0 && fs_sync_busy());
    unsigned steps = 0;
    while (fs_sync_busy()) {
        assert(fs_sync_result(ticket) == FS_SYNC_PENDING && fs_needs_sync());
        assert(fs_write(first, fs_data(second), fs_size(second)) == FS_ERR_BUSY);
        assert(fs_copy(first, 0) == FS_ERR_BUSY && fs_empty_dir(0) == FS_ERR_BUSY);
        assert(fs_sync_step() != FS_SYNC_IDLE);
        assert(++steps < 150000);
    }
    assert(steps > 40000 && fs_sync_result(ticket) == 0 && !fs_needs_sync());
    assert(fs_sync_release(ticket) == 0);
    /* Both the commit and direct mounted reload must preserve the lower
     * arenas: other selected-profile workspace owners are not borrowed. */
    assert(fs_load_disk() == 0 && fs_large_profile());
    for (unsigned i = 0; i < sizeof(pool_arena); ++i) assert(pool_arena[i] == 0xd3);
    for (unsigned i = 0; i < sizeof(image_arena); ++i) assert(image_arena[i] == 0xe4);
    verify(first, FS_LARGE_FILE_MAX, 0);
    verify(second, 33543168 - FS_LARGE_FILE_MAX, 0);
    assert(fs_node_count() == FS_MAX_NODES && fs_used_bytes() == fs_capacity());
    printf("incremental large FS: full 256-node/33,543,168-byte commit, bounded %u steps, lease and lower-arena ownership passed\n", steps);
    return 0;
}
