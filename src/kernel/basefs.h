#ifndef BASEFS_H
#define BASEFS_H
#include <stdint.h>

/* BaseFS 2: the block-based native volume format (docs/BASEFS2.md).
 *
 * This layer owns the on-disk structures only: marker, the two superblocks,
 * checkpoints (the complete node table) and file data extents, plus the free
 * space map. It never keeps file contents; kernel/fs.c decides what stays in
 * memory. Every write goes to blocks the committed checkpoint does not use,
 * and a commit becomes current only when its superblock is written after a
 * flush, so a crash at any point leaves the previous commit intact.
 *
 * Memory comes from kmalloc/kfree (host tests provide them). Not reentrant. */

#define BFS_BLOCK 4096u
#define BFS_SECTORS_PER_BLOCK (BFS_BLOCK / 512u)
#define BFS_EXTENT_BLOCKS 16u          /* at most 64 KiB per extent, one CRC each */
#define BFS_NAME_MAX 255u
#define BFS_MARKER_VERSION 3u          /* data-disk marker version for BaseFS 2 */
#define BFS_ROOT_PARENT 0xFFFFFFFFu

enum {
    BFS_OK = 0,
    BFS_ERR_IO = -1,        /* device error */
    BFS_ERR_CORRUPT = -2,   /* a checksum or structure check failed */
    BFS_ERR_NOSPACE = -3,
    BFS_ERR_NOMEM = -4,
    BFS_ERR_FORMAT = -5,    /* not a BaseFS 2 volume */
    BFS_ERR_INVALID = -6
};

typedef struct {
    int (*read)(void *context, uint32_t lba, void *buffer, uint32_t sectors);
    int (*write)(void *context, uint32_t lba, const void *buffer, uint32_t sectors);
    int (*flush)(void *context);
    void *context;
    uint32_t sectors;
} BfsDevice;

typedef struct {
    uint32_t start;         /* first block */
    uint16_t blocks;
    uint16_t reserved;
    uint32_t crc;           /* CRC32 of the extent's bytes that belong to the file */
} BfsExtent;

#define BFS_DIRECTORY 1u
#define BFS_APP 2u

/* One node of a checkpoint. Index 0 is the root directory; parent is an
 * index (BFS_ROOT_PARENT for the root). */
typedef struct {
    uint32_t parent;
    uint32_t flags;
    uint32_t identity, revision;
    uint32_t modified;      /* seconds since 2000-01-01 UTC, 0 unknown */
    uint64_t size;
    uint32_t extent_count;
    BfsExtent *extents;
    char *name;             /* NUL-terminated, at most BFS_NAME_MAX bytes */
} BfsRecord;

typedef struct {
    BfsDevice device;
    uint32_t blocks;
    uint32_t limit;                 /* allocation stays below this block */
    uint64_t generation;
    uint32_t next_identity, next_revision;
    int slot;                       /* superblock slot of the current commit */
    uint8_t *used;                  /* bitmap: blocks the current commit owns */
    uint8_t *taken;                 /* bitmap: blocks allocated since */
    BfsExtent *checkpoint;          /* where the current checkpoint lives */
    uint32_t checkpoint_extents;
    uint32_t free_blocks;
} BfsVolume;

/* Writes the marker, an empty root and both superblocks. */
int bfs_format(const BfsDevice *device, uint32_t identity_seed);
/* An empty volume in memory, ready for writes and a first commit, for
 * converting a disk in place: it clears both superblock slots but leaves
 * sector 0 alone, and allocates only below block limit (0: the whole disk)
 * until it is mounted again. Write the marker after the first commit. */
int bfs_create(BfsVolume *volume, const BfsDevice *device, uint32_t limit);
int bfs_write_marker(const BfsDevice *device);
/* Nonzero when sector 0 holds a BaseFS 2 marker for this device size. */
int bfs_probe(const BfsDevice *device);
/* Loads the newest valid commit. *records (count *count) and their extents
 * and names belong to the caller: free with bfs_free_records. */
int bfs_mount(BfsVolume *volume, const BfsDevice *device, BfsRecord **records, uint32_t *count);
void bfs_unmount(BfsVolume *volume);
void bfs_free_records(BfsRecord *records, uint32_t count);

/* Reads length bytes at offset of a file, checking each extent it touches. */
int bfs_read(BfsVolume *volume, const BfsRecord *record, uint64_t offset, void *buffer, uint32_t length);
/* Reserves blocks for size bytes in extents of at most BFS_EXTENT_BLOCKS;
 * *extents is kmalloc'd. The caller writes each extent, zero padded to whole
 * blocks, and sets its crc to bfs_crc32(0, bytes of the file in it). */
int bfs_allocate(BfsVolume *volume, uint64_t size, BfsExtent **extents, uint32_t *count);
/* bfs_allocate plus the writes. */
int bfs_write_data(BfsVolume *volume, const void *data, uint64_t size, BfsExtent **extents, uint32_t *count);
/* Gives back blocks reserved since the last commit that no commit will use. */
void bfs_discard(BfsVolume *volume, const BfsExtent *extents, uint32_t count);

/* Makes records the current state: writes the checkpoint, flushes, reads it
 * back, then writes the other superblock, flushes and reads it back. Blocks
 * only the old commit used become free afterwards. On failure the previous
 * commit stays current. */
int bfs_commit(BfsVolume *volume, const BfsRecord *records, uint32_t count,
               uint32_t next_identity, uint32_t next_revision);

/* The same commit in steps, for callers that drive the device themselves:
 *   bfs_commit_begin   validate and encode, reserve checkpoint blocks (no I/O)
 *   write each run of checkpoint (bfs_commit_runs/run) in order, flush, read
 *   it back and compare
 *   write the block super_block (super, zero padded), flush, read its first
 *   sector back and compare
 *   bfs_commit_end     with the outcome
 * records must not change in between. */
typedef struct {
    uint8_t *checkpoint;            /* checkpoint_blocks whole blocks */
    uint32_t checkpoint_blocks;
    uint32_t super_block;
    uint8_t super[512] __attribute__((aligned(4)));
} BfsCommit;
enum {
    BFS_COMMIT_DONE,        /* the superblock read back intact */
    BFS_COMMIT_FAILED,      /* failed before the superblock write was issued */
    BFS_COMMIT_UNCERTAIN    /* the superblock write was issued but not verified */
};
int bfs_commit_begin(BfsVolume *volume, const BfsRecord *records, uint32_t count,
                     uint32_t next_identity, uint32_t next_revision, BfsCommit *commit);
uint32_t bfs_commit_runs(const BfsCommit *commit);
/* Start block of run index; *blocks its length. */
uint32_t bfs_commit_run(const BfsCommit *commit, uint32_t index, uint32_t *blocks);
/* Drops a begun commit before any write. */
void bfs_commit_cancel(BfsVolume *volume, BfsCommit *commit);
int bfs_commit_end(BfsVolume *volume, BfsCommit *commit, const BfsRecord *records, uint32_t count,
                   int outcome);

uint64_t bfs_free_bytes(const BfsVolume *volume);
uint32_t bfs_crc32(uint32_t crc, const void *data, uint32_t length);
#endif
