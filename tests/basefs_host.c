/* BaseFS 2 core on a RAM disk: format, mount, data round trips, free space,
 * corruption detection and every crash point of a commit. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void kmemcpy(void *d, const void *s, int n) { memmove(d, s, (size_t)n); }
void kmemset(void *d, int v, int n) { memset(d, v, (size_t)n); }
static long live;
void *kmalloc(uint32_t bytes) { void *p = calloc(1, bytes ? bytes : 1); if (p) ++live; return p; }
void kfree(void *p) { if (p) { --live; free(p); } }

#include "../src/kernel/basefs.c"

typedef struct {
    uint8_t *data;
    uint32_t sectors;
    long budget;        /* sector writes left before the "power cut"; <0 unlimited */
    int cut;
} Disk;

static int disk_read(void *context, uint32_t lba, void *buffer, uint32_t sectors) {
    Disk *d = context;
    if (lba > d->sectors || sectors > d->sectors - lba) return -1;
    memcpy(buffer, d->data + (size_t)lba * 512, (size_t)sectors * 512);
    return 0;
}
static int disk_write(void *context, uint32_t lba, const void *buffer, uint32_t sectors) {
    Disk *d = context;
    if (lba > d->sectors || sectors > d->sectors - lba) return -1;
    for (uint32_t i = 0; i < sectors; ++i) {
        if (d->budget == 0) { d->cut = 1; return -1; }
        if (d->budget > 0) --d->budget;
        memcpy(d->data + (size_t)(lba + i) * 512, (const uint8_t *)buffer + (size_t)i * 512, 512);
    }
    return 0;
}
static int disk_flush(void *context) { return ((Disk *)context)->cut ? -1 : 0; }

static BfsDevice device_of(Disk *d) { return (BfsDevice){disk_read, disk_write, disk_flush, d, d->sectors}; }

static uint8_t *pattern(uint64_t size, unsigned seed) {
    uint8_t *p = malloc(size ? size : 1);
    uint32_t x = seed * 2654435761u + 1;
    for (uint64_t i = 0; i < size; ++i) { x = x * 1103515245u + 12345u; p[i] = (uint8_t)(x >> 16); }
    return p;
}

static char *name_of(const char *text) { char *n = kmalloc((uint32_t)strlen(text) + 1); strcpy(n, text); return n; }

/* Builds root + /docs + files of the given sizes in /docs, writing data.
 * Returns 0 when a data write fails (a power cut before the commit). */
static BfsRecord *build(BfsVolume *v, const uint64_t *sizes, unsigned files, unsigned seed, uint32_t *count) {
    BfsRecord *r = kmalloc((files + 2) * sizeof *r);
    r[0] = (BfsRecord){BFS_ROOT_PARENT, BFS_DIRECTORY, 1, 0, 0, 0, 0, 0, name_of("")};
    r[1] = (BfsRecord){0, BFS_DIRECTORY, 2, 0, 77, 0, 0, 0, name_of("docs with a long name that old volumes could not hold")};
    for (unsigned i = 0; i < files; ++i) {
        char name[32];
        snprintf(name, sizeof name, "file-%u.bin", i);
        BfsRecord *f = &r[2 + i];
        *f = (BfsRecord){1, i == 0 ? BFS_APP : 0, 10 + i, seed, 1000 + i, sizes[i], 0, 0, name_of(name)};
        uint8_t *data = pattern(sizes[i], seed + i);
        int result = bfs_write_data(v, data, sizes[i], &f->extents, &f->extent_count);
        free(data);
        if (result) {
            assert(result == BFS_ERR_IO);
            bfs_free_records(r, i + 3);
            return 0;
        }
    }
    *count = files + 2;
    return r;
}

static void verify(BfsVolume *v, const BfsRecord *r, uint32_t count, const uint64_t *sizes, unsigned seed) {
    assert(count >= 2 && !strcmp(r[1].name, "docs with a long name that old volumes could not hold"));
    for (uint32_t i = 2; i < count; ++i) {
        uint64_t size = sizes[i - 2];
        assert(r[i].size == size && r[i].parent == 1 && r[i].modified == 1000 + i - 2);
        uint8_t *want = pattern(size, seed + i - 2), *got = malloc(size ? size : 1);
        assert(bfs_read(v, &r[i], 0, got, (uint32_t)size) == BFS_OK);
        assert(!memcmp(want, got, size));
        if (size > 5000) {
            /* A read in the middle crosses an extent boundary. */
            assert(bfs_read(v, &r[i], 65530, got, 20) == (size >= 65550 ? BFS_OK : BFS_ERR_INVALID));
            if (size >= 65550) assert(!memcmp(want + 65530, got, 20));
        }
        free(want);
        free(got);
    }
}

int main(void) {
    Disk disk = {calloc(1, 8u << 20), (8u << 20) / 512, -1, 0};
    BfsDevice device = device_of(&disk);
    assert(!bfs_probe(&device));
    assert(bfs_format(&device, 500) == BFS_OK && bfs_probe(&device));

    BfsVolume v;
    BfsRecord *r;
    uint32_t n;
    assert(bfs_mount(&v, &device, &r, &n) == BFS_OK && n == 1 && (r[0].flags & BFS_DIRECTORY));
    assert(v.next_identity == 501);
    uint64_t empty_free = bfs_free_bytes(&v);
    bfs_free_records(r, n);

    /* Files of every shape, including empty and several extents. */
    uint64_t sizes[] = {0, 1, 4096, 4097, 65536, 70000, 1u << 20};
    unsigned files = sizeof sizes / sizeof *sizes;
    r = build(&v, sizes, files, 7, &n);
    assert(r);
    assert(bfs_commit(&v, r, n, 600, 50) == BFS_OK);
    bfs_free_records(r, n);
    bfs_unmount(&v);
    assert(bfs_mount(&v, &device, &r, &n) == BFS_OK && n == files + 2 && v.next_revision == 50);
    verify(&v, r, n, sizes, 7);
    uint64_t used_once = empty_free - bfs_free_bytes(&v);

    /* Rewriting everything many times reuses space: no leak. */
    for (unsigned round = 0; round < 20; ++round) {
        for (uint32_t i = 2; i < n; ++i) {
            BfsExtent *old = r[i].extents;
            /* Record i holds pattern seed 100 + round + i, which verify() reads
             * as seed base 102 + round. */
            uint8_t *data = pattern(sizes[i - 2], 100 + round + i);
            assert(bfs_write_data(&v, data, sizes[i - 2], &r[i].extents, &r[i].extent_count) == BFS_OK);
            free(data);
            kfree(old);
        }
        assert(bfs_commit(&v, r, n, 600, 51 + round) == BFS_OK);
    }
    assert(empty_free - bfs_free_bytes(&v) == used_once);
    bfs_free_records(r, n);
    bfs_unmount(&v);

    /* A bad table is refused before anything is written. */
    assert(bfs_mount(&v, &device, &r, &n) == BFS_OK);
    uint32_t parent = r[1].parent;
    r[1].parent = 2;   /* a file as a parent */
    assert(bfs_commit(&v, r, n, 600, 99) == BFS_ERR_INVALID);
    r[1].parent = parent;
    BfsExtent shared = r[3].extents[0];
    r[3].extents[0] = r[4].extents[0];   /* two files on one block */
    assert(bfs_commit(&v, r, n, 600, 99) == BFS_ERR_INVALID);
    r[3].extents[0] = shared;
    bfs_free_records(r, n);
    bfs_unmount(&v);

    /* Power cut at every sector of a commit: mount sees the old or the new
     * state, never anything else. */
    uint8_t *before = malloc((size_t)disk.sectors * 512);
    memcpy(before, disk.data, (size_t)disk.sectors * 512);
    uint64_t new_sizes[] = {3, 9000, 0, 123456, 4096, 1, 70001};
    unsigned outcomes[2] = {0, 0};
    for (long cut = 0;; ++cut) {
        memcpy(disk.data, before, (size_t)disk.sectors * 512);
        disk.budget = -1;
        disk.cut = 0;
        assert(bfs_mount(&v, &device, &r, &n) == BFS_OK);
        bfs_free_records(r, n);
        disk.budget = cut;
        BfsRecord *next = build(&v, new_sizes, files, 900, &n);
        int result = BFS_ERR_IO;
        if (next) {
            result = bfs_commit(&v, next, n, 700, 200);
            bfs_free_records(next, n);
        }
        bfs_unmount(&v);
        disk.budget = -1;
        disk.cut = 0;
        assert(bfs_mount(&v, &device, &r, &n) == BFS_OK);
        int is_new = v.next_revision == 200;
        verify(&v, r, n, is_new ? new_sizes : sizes, is_new ? 900 : 100 + 19 + 2);
        ++outcomes[is_new];
        if (result == BFS_OK) assert(is_new);
        bfs_free_records(r, n);
        bfs_unmount(&v);
        if (result == BFS_OK) break;
    }
    assert(outcomes[0] > 100 && outcomes[1] >= 1);

    /* A damaged extent is reported, not returned as data. */
    assert(bfs_mount(&v, &device, &r, &n) == BFS_OK);
    disk.data[(size_t)r[3].extents[0].start * BFS_BLOCK + 10] ^= 0x40;
    uint8_t buffer[9000];
    assert(bfs_read(&v, &r[3], 0, buffer, 9000) == BFS_ERR_CORRUPT);
    disk.data[(size_t)r[3].extents[0].start * BFS_BLOCK + 10] ^= 0x40;
    /* A damaged newest checkpoint falls back to the previous commit. */
    uint64_t generation = v.generation;
    uint32_t checkpoint = v.checkpoint[0].start;
    bfs_free_records(r, n);
    bfs_unmount(&v);
    disk.data[(size_t)checkpoint * BFS_BLOCK + 40] ^= 1;
    assert(bfs_mount(&v, &device, &r, &n) == BFS_OK && v.generation == generation - 1);
    bfs_free_records(r, n);
    bfs_unmount(&v);

    /* Filling the volume reports no space and leaves the commit usable. */
    disk.data[(size_t)checkpoint * BFS_BLOCK + 40] ^= 1;
    assert(bfs_mount(&v, &device, &r, &n) == BFS_OK && v.generation == generation);
    uint8_t *big = pattern(16u << 20, 3);
    BfsExtent *extents;
    uint32_t extent_count;
    uint64_t free_before = bfs_free_bytes(&v);
    assert(bfs_write_data(&v, big, 16u << 20, &extents, &extent_count) == BFS_ERR_NOSPACE);
    assert(bfs_free_bytes(&v) == free_before);
    free(big);
    assert(bfs_commit(&v, r, n, 800, 300) == BFS_OK);
    bfs_free_records(r, n);
    bfs_unmount(&v);
    assert(live == 0);
    free(before);
    free(disk.data);
    printf("basefs: format, mount, %u file shapes, reuse, refused tables, %u+%u crash points, "
           "corruption and full volume passed\n", files, outcomes[0], outcomes[1]);
    return 0;
}
