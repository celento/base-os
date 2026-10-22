#ifndef FAT_H
#define FAT_H
#include <stdint.h>

/* FAT12, FAT16 and FAT32 volumes (docs/FAT.md), for disks and disk images
 * shared with other systems. Long file names (VFAT) are read and written;
 * names are UTF-8 in this API.
 *
 * FAT has no journal, so writes are ordered to keep a crash repairable by a
 * disk checker without losing data a finished call wrote: new clusters are
 * linked in the FAT before data lands in them, and the directory entry that
 * points at them is written last. Freed clusters are released only after the
 * directory entry stops pointing at them. A crash can leave lost clusters,
 * never a file pointing at a free or shared cluster.
 *
 * The volume is marked dirty on disk (FAT[1]) at its first change and clean
 * again by fat_sync, as other systems expect.
 *
 * Memory comes from kmalloc/kfree (host tests provide them). Not reentrant. */

enum {
    FAT_OK = 0,
    FAT_ERR_IO = -1,            /* device error */
    FAT_ERR_CORRUPT = -2,       /* a structure on disk is inconsistent */
    FAT_ERR_NOSPACE = -3,
    FAT_ERR_NOMEM = -4,
    FAT_ERR_FORMAT = -5,        /* not a FAT volume this code handles */
    FAT_ERR_INVALID = -6,       /* bad argument or name */
    FAT_ERR_NOTFOUND = -7,
    FAT_ERR_EXISTS = -8,
    FAT_ERR_NOTDIR = -9,
    FAT_ERR_ISDIR = -10,
    FAT_ERR_NOTEMPTY = -11,
    FAT_ERR_NAMETOOLONG = -12,
    FAT_ERR_FBIG = -13,         /* past the 4 GiB - 1 file size limit */
    FAT_ERR_READONLY = -14,       /* the volume takes no changes */
    FAT_ERR_ACCESS = -15          /* the file has the read-only attribute */
};

#define FAT_NAME_MAX 255u               /* UTF-16 units, as VFAT counts them */
#define FAT_NAME_BYTES (FAT_NAME_MAX * 3u + 1u)
#define FAT_FILE_MAX 0xFFFFFFFFu

#define FAT_ATTR_READ_ONLY 0x01u
#define FAT_ATTR_HIDDEN 0x02u
#define FAT_ATTR_SYSTEM 0x04u
#define FAT_ATTR_VOLUME 0x08u
#define FAT_ATTR_DIRECTORY 0x10u
#define FAT_ATTR_ARCHIVE 0x20u

typedef struct {
    int (*read)(void *context, uint32_t lba, void *buffer, uint32_t sectors);
    int (*write)(void *context, uint32_t lba, const void *buffer, uint32_t sectors);
    int (*flush)(void *context);
    void *context;
    uint32_t sectors;
} FatDevice;

#define FAT_WINDOW_SECTORS 8u

typedef struct {
    FatDevice device;
    uint32_t start;                 /* first sector of the volume on the device */
    uint32_t sectors;               /* sectors in the volume */
    int bits;                       /* 12, 16 or 32 */
    uint32_t cluster_sectors, cluster_bytes;
    uint32_t fat_start, fat_sectors, fat_count;   /* relative to start */
    uint32_t fat_active;            /* the FAT read; -1u: all copies are mirrors */
    uint32_t root_start, root_entries;            /* FAT12/16 fixed root */
    uint32_t data_start;
    uint32_t clusters;              /* data clusters, numbered 2 .. clusters + 1 */
    uint32_t root_cluster;          /* FAT32 */
    uint32_t fsinfo;                /* FAT32 FSInfo sector, 0 none */
    uint32_t free_count;            /* 0xFFFFFFFF until counted */
    uint32_t next_free;
    int marked_dirty;               /* FAT[1] says the volume is in use */
    int was_dirty;                  /* it said so at mount: another system did not finish */
    int changed;                    /* written since the last fat_sync */
    int unflushed;                  /* written since the last device flush */
    int read_only;
    uint32_t (*now)(void);          /* seconds since 2000-01-01 UTC, 0 unknown */
    char label[12];
    /* FAT window: FAT_WINDOW_SECTORS sectors of the first FAT, written back
     * to every FAT copy before anything that depends on it. */
    uint32_t window_sector;         /* 0xFFFFFFFF none */
    uint32_t window_count;
    int window_dirty;
    uint8_t *window;
    /* Last directory sector read. */
    uint32_t dir_lba;               /* 0xFFFFFFFF none */
    uint8_t *dir;
    uint8_t *sector;                /* scratch for partial data sectors */
    /* Last cluster-chain step looked up for a directory. */
    uint32_t walk_first, walk_index, walk_cluster;
} FatVolume;

/* A file or directory. Copies are independent; after a call that changes a
 * node through one copy, other copies of it are stale. */
typedef struct {
    uint32_t parent;                /* first cluster of the containing directory, 0 the FAT12/16 root */
    uint32_t slot;                  /* index of the short entry in it (32-byte slots) */
    uint32_t lfn_slots;             /* long-name slots just before it */
    uint32_t cluster;               /* first cluster, 0 none */
    uint32_t size;
    uint8_t attributes;
    uint8_t is_root;
    uint32_t modified;              /* seconds since 2000-01-01, 0 unknown */
    char name[FAT_NAME_BYTES];      /* UTF-8 */
    /* Position cache for sequential access: cluster number at index. */
    uint32_t at_index, at_cluster;
} FatNode;

/* Finds a FAT volume on a device: a volume starting at sector 0, or the first
 * FAT partition of an MBR partition table. */
int fat_find(const FatDevice *device, uint32_t *start, uint32_t *sectors);
int fat_mount(FatVolume *volume, const FatDevice *device, uint32_t start, uint32_t sectors);
/* fat_sync, then frees the volume's memory. */
int fat_unmount(FatVolume *volume);
/* Writes FSInfo, flushes, and marks the volume clean. */
int fat_sync(FatVolume *volume);

void fat_root(const FatVolume *volume, FatNode *root);
/* Next entry of a directory from *cursor (start at 0), skipping ".", "..",
 * the volume label and deleted entries. 1 found, 0 at the end. */
int fat_next(FatVolume *volume, const FatNode *directory, uint32_t *cursor, FatNode *out);
/* A child by name, ignoring ASCII case as other systems do. */
int fat_lookup(FatVolume *volume, const FatNode *directory, const char *name, FatNode *out);
/* A '/'-separated path from the root; "" and "/" are the root. */
int fat_resolve(FatVolume *volume, const char *path, FatNode *out);

/* Bytes read, short only at the end of the file. */
int fat_read(FatVolume *volume, FatNode *file, uint32_t offset, void *buffer, uint32_t length);
/* Writes all of length or fails; a gap past the end reads as zeros. */
int fat_write(FatVolume *volume, FatNode *file, uint32_t offset, const void *data, uint32_t length);
int fat_truncate(FatVolume *volume, FatNode *file, uint32_t size);
int fat_create(FatVolume *volume, const FatNode *directory, const char *name, int is_directory, FatNode *out);
/* Removes a file or an empty directory. */
int fat_remove(FatVolume *volume, const FatNode *node);
/* Moves node to name in directory. An existing target is not replaced. */
int fat_rename(FatVolume *volume, FatNode *node, const FatNode *directory, const char *name);
/* 1 when a directory has no entries besides "." and "..". */
int fat_is_empty(FatVolume *volume, const FatNode *directory);
int fat_set_modified(FatVolume *volume, FatNode *node, uint32_t modified);

uint32_t fat_free_clusters(FatVolume *volume);
#endif
