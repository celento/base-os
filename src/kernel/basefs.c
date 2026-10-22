#include "kernel/basefs.h"
#include "kernel/kheap.h"
#include "kernel/fs.h"
#include "kernel/layout.h"

/* Block 0 holds the data-disk marker in its first sector; blocks 1 and 2 hold
 * superblock slots 0 and 1. Everything else is data or checkpoint. */
#define RESERVED_BLOCKS BFS_RESERVED_BLOCKS
#define SUPER_MAGIC 0x32534642u        /* "BFS2" */
#define CHECKPOINT_MAGIC 0x43534642u   /* "BFSC" */
#define FORMAT_VERSION 1u
#define SUPER_EXTENTS 48u
#define RECORD_HEADER 36u
#define EXTENT_BYTES 12u

typedef struct {
    uint32_t start, blocks;
} SuperExtent;

typedef struct {
    uint32_t magic, version, block_size, blocks;
    uint32_t generation_low, generation_high;
    uint32_t next_identity, next_revision;
    uint32_t checkpoint_bytes, checkpoint_crc, checkpoint_extents, node_count;
    SuperExtent extents[SUPER_EXTENTS];
    uint32_t reserved[19];
    uint32_t crc;                      /* of every byte before it */
} Super;
_Static_assert(sizeof(Super) == 512, "superblock is one sector");

/* ---------- helpers ---------- */

static uint32_t crc_table[256];

uint32_t bfs_crc32(uint32_t crc, const void *data, uint32_t length) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    const uint8_t *p = data;
    crc = ~crc;
    for (uint32_t i = 0; i < length; ++i) crc = crc_table[(crc ^ p[i]) & 255u] ^ (crc >> 8);
    return ~crc;
}

static int bfsi_bit(const uint8_t *map, uint32_t block) { return map[block >> 3] >> (block & 7) & 1; }
static void bfsi_set_bit(uint8_t *map, uint32_t block) { map[block >> 3] |= (uint8_t)(1u << (block & 7)); }
static void bfsi_clear_bit(uint8_t *map, uint32_t block) { map[block >> 3] &= (uint8_t)~(1u << (block & 7)); }

static int bfsi_read_blocks(const BfsDevice *device, uint32_t block, void *buffer, uint32_t count) {
    return device->read(device->context, block * BFS_SECTORS_PER_BLOCK, buffer,
                        count * BFS_SECTORS_PER_BLOCK) < 0 ? BFS_ERR_IO : BFS_OK;
}

static int bfsi_write_blocks(const BfsDevice *device, uint32_t block, const void *buffer, uint32_t count) {
    return device->write(device->context, block * BFS_SECTORS_PER_BLOCK, buffer,
                         count * BFS_SECTORS_PER_BLOCK) < 0 ? BFS_ERR_IO : BFS_OK;
}

static int bfsi_flush(const BfsDevice *device) {
    return device->flush(device->context) < 0 ? BFS_ERR_IO : BFS_OK;
}

static uint32_t bfsi_device_blocks(const BfsDevice *device) { return device->sectors / BFS_SECTORS_PER_BLOCK; }

static void bfsi_marker(uint32_t *sector, uint32_t sectors) {
    kmemset(sector, 0, 512);
    sector[0] = DATA_MARKER_MAGIC;
    sector[1] = BFS_MARKER_VERSION;
    sector[2] = sectors;
    sector[6] = bfs_crc32(0, sector, 24);
}

/* ---------- superblocks ---------- */

static int bfsi_read_super(const BfsDevice *device, int slot, Super *super) {
    if (device->read(device->context, (1u + (uint32_t)slot) * BFS_SECTORS_PER_BLOCK, super, 1) < 0)
        return BFS_ERR_IO;
    if (super->magic != SUPER_MAGIC || super->version != FORMAT_VERSION ||
        super->block_size != BFS_BLOCK || super->crc != bfs_crc32(0, super, sizeof *super - 4) ||
        super->blocks != bfsi_device_blocks(device) || !super->checkpoint_extents ||
        super->checkpoint_extents > SUPER_EXTENTS || !super->node_count)
        return BFS_ERR_CORRUPT;
    return BFS_OK;
}

/* ---------- checkpoints ---------- */

static uint32_t bfsi_name_length(const char *name) {
    uint32_t n = 0;
    while (name && name[n]) ++n;
    return n;
}

static uint32_t bfsi_record_bytes(const BfsRecord *record) {
    return RECORD_HEADER + record->extent_count * EXTENT_BYTES + ((bfsi_name_length(record->name) + 3u) & ~3u);
}

static void bfsi_put32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}
static uint32_t bfsi_get32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t bfsi_encode(const BfsRecord *records, uint32_t count, uint8_t *out) {
    bfsi_put32(out, CHECKPOINT_MAGIC);
    bfsi_put32(out + 4, FORMAT_VERSION);
    bfsi_put32(out + 8, count);
    bfsi_put32(out + 12, 0);
    uint32_t at = 16;
    for (uint32_t i = 0; i < count; ++i) {
        const BfsRecord *r = &records[i];
        uint32_t length = bfsi_name_length(r->name);
        uint32_t fields[9] = {r->parent, r->flags, r->identity, r->revision, r->modified,
                              (uint32_t)r->size, (uint32_t)(r->size >> 32), r->extent_count, length};
        for (int k = 0; k < 9; ++k) bfsi_put32(out + at + 4u * (uint32_t)k, fields[k]);
        at += RECORD_HEADER;
        for (uint32_t e = 0; e < r->extent_count; ++e) {
            bfsi_put32(out + at, r->extents[e].start);
            bfsi_put32(out + at + 4, r->extents[e].blocks);
            bfsi_put32(out + at + 8, r->extents[e].crc);
            at += EXTENT_BYTES;
        }
        kmemset(out + at, 0, (int)((length + 3u) & ~3u));
        kmemcpy(out + at, r->name, (int)length);
        at += (length + 3u) & ~3u;
    }
    return at;
}

void bfs_free_records(BfsRecord *records, uint32_t count) {
    if (!records) return;
    for (uint32_t i = 0; i < count; ++i) {
        kfree(records[i].extents);
        kfree(records[i].name);
    }
    kfree(records);
}

static uint32_t bfsi_blocks_for(uint64_t size) { return (uint32_t)((size + BFS_BLOCK - 1) / BFS_BLOCK); }

/* Parses and checks a checkpoint: structure, the tree, and that every extent
 * lies on the device without overlapping another or the reserved blocks.
 * Marks the extents in used. */
static int bfsi_decode(const uint8_t *data, uint32_t bytes, uint32_t blocks, uint8_t *used,
                  BfsRecord **out, uint32_t *out_count) {
    if (bytes < 16 || bfsi_get32(data) != CHECKPOINT_MAGIC || bfsi_get32(data + 4) != FORMAT_VERSION) return BFS_ERR_CORRUPT;
    uint32_t count = bfsi_get32(data + 8);
    if (!count || count > bytes / RECORD_HEADER) return BFS_ERR_CORRUPT;
    BfsRecord *records = kmalloc(count * sizeof *records);
    if (!records) return BFS_ERR_NOMEM;
    kmemset(records, 0, (int)(count * sizeof *records));
    uint32_t at = 16;
    int result = BFS_OK;
    for (uint32_t i = 0; i < count && !result; ++i) {
        BfsRecord *r = &records[i];
        if (bytes - at < RECORD_HEADER) { result = BFS_ERR_CORRUPT; break; }
        const uint8_t *p = data + at;
        r->parent = bfsi_get32(p); r->flags = bfsi_get32(p + 4); r->identity = bfsi_get32(p + 8);
        r->revision = bfsi_get32(p + 12); r->modified = bfsi_get32(p + 16);
        r->size = bfsi_get32(p + 20) | (uint64_t)bfsi_get32(p + 24) << 32;
        r->extent_count = bfsi_get32(p + 28);
        uint32_t length = bfsi_get32(p + 32);
        at += RECORD_HEADER;
        if (length > BFS_NAME_MAX || r->extent_count > (bytes - at) / EXTENT_BYTES ||
            (r->flags & ~(BFS_DIRECTORY | BFS_APP)) ||
            ((r->flags & BFS_DIRECTORY) && (r->size || r->extent_count))) {
            result = BFS_ERR_CORRUPT;
            break;
        }
        r->extents = r->extent_count ? kmalloc(r->extent_count * sizeof *r->extents) : 0;
        r->name = kmalloc(length + 1);
        if ((r->extent_count && !r->extents) || !r->name) { result = BFS_ERR_NOMEM; break; }
        uint32_t covered = 0;
        for (uint32_t e = 0; e < r->extent_count; ++e, at += EXTENT_BYTES) {
            BfsExtent *x = &r->extents[e];
            x->start = bfsi_get32(data + at);
            uint32_t n = bfsi_get32(data + at + 4);
            x->blocks = (uint16_t)n;
            x->crc = bfsi_get32(data + at + 8);
            if (!n || n > BFS_EXTENT_BLOCKS || x->start < RESERVED_BLOCKS || x->start >= blocks ||
                n > blocks - x->start) { result = BFS_ERR_CORRUPT; break; }
            for (uint32_t b = x->start; b < x->start + n; ++b) {
                if (bfsi_bit(used, b)) { result = BFS_ERR_CORRUPT; break; }
                bfsi_set_bit(used, b);
            }
            covered += n;
        }
        if (result) break;
        if (covered != bfsi_blocks_for(r->size)) { result = BFS_ERR_CORRUPT; break; }
        uint32_t padded = (length + 3u) & ~3u;
        if (bytes - at < padded) { result = BFS_ERR_CORRUPT; break; }
        kmemcpy(r->name, data + at, (int)length);
        r->name[length] = 0;
        for (uint32_t k = 0; k < length; ++k)
            if (!r->name[k] || r->name[k] == '/') result = BFS_ERR_CORRUPT;
        at += padded;
        /* The root is record 0; every other record names an earlier-or-later
         * directory and has a name. */
        if (i == 0 ? r->parent != BFS_ROOT_PARENT || !(r->flags & BFS_DIRECTORY) || length
                   : r->parent >= count || r->parent == i || !length)
            result = BFS_ERR_CORRUPT;
    }
    for (uint32_t i = 1; i < count && !result; ++i) {
        if (!(records[records[i].parent].flags & BFS_DIRECTORY)) result = BFS_ERR_CORRUPT;
        /* Every chain reaches the root within count steps: no cycles. */
        uint32_t walk = i, steps = 0;
        while (walk && steps++ <= count) walk = records[walk].parent;
        if (walk) result = BFS_ERR_CORRUPT;
    }
    if (result) {
        bfs_free_records(records, count);
        return result;
    }
    *out = records;
    *out_count = count;
    return BFS_OK;
}

/* ---------- allocation ---------- */

static int bfsi_free_block(const BfsVolume *volume, uint32_t block) {
    return !bfsi_bit(volume->used, block) && !bfsi_bit(volume->taken, block);
}

/* A run of up to want free blocks; returns its length, 0 when full. */
static uint32_t bfsi_allocate_run(BfsVolume *volume, uint32_t want, uint32_t *start) {
    for (uint32_t block = RESERVED_BLOCKS; block < volume->limit; ++block) {
        if (!bfsi_free_block(volume, block)) continue;
        uint32_t n = 0;
        while (n < want && block + n < volume->limit && bfsi_free_block(volume, block + n)) ++n;
        for (uint32_t k = 0; k < n; ++k) bfsi_set_bit(volume->taken, block + k);
        volume->free_blocks -= n;
        *start = block;
        return n;
    }
    return 0;
}

void bfs_discard(BfsVolume *volume, const BfsExtent *extents, uint32_t count) {
    for (uint32_t e = 0; e < count; ++e)
        for (uint32_t b = extents[e].start; b < extents[e].start + extents[e].blocks; ++b)
            if (b < volume->blocks && bfsi_bit(volume->taken, b) && !bfsi_bit(volume->used, b)) {
                bfsi_clear_bit(volume->taken, b);
                ++volume->free_blocks;
            }
}

uint32_t bfs_drop(BfsVolume *volume, const BfsExtent *extents, uint32_t count) {
    uint32_t committed = 0;
    for (uint32_t e = 0; e < count; ++e)
        for (uint32_t b = extents[e].start; b < extents[e].start + extents[e].blocks; ++b)
            if (b < volume->blocks && bfsi_bit(volume->used, b)) ++committed;
    bfs_discard(volume, extents, count);
    return committed;
}

int bfs_allocate(BfsVolume *volume, uint64_t size, BfsExtent **extents, uint32_t *count) {
    *extents = 0;
    *count = 0;
    if (size > (uint64_t)0xFFFFFFFFu * BFS_BLOCK) return BFS_ERR_NOSPACE;
    uint32_t total = bfsi_blocks_for(size);
    if (!total) return BFS_OK;
    if (total > volume->free_blocks) return BFS_ERR_NOSPACE;
    BfsExtent *list = kmalloc((total + BFS_EXTENT_BLOCKS - 1) / BFS_EXTENT_BLOCKS * sizeof *list + total * sizeof *list);
    if (!list) return BFS_ERR_NOMEM;
    uint32_t placed = 0, n = 0;
    while (placed < total) {
        uint32_t want = total - placed > BFS_EXTENT_BLOCKS ? BFS_EXTENT_BLOCKS : total - placed;
        uint32_t start, got = bfsi_allocate_run(volume, want, &start);
        if (!got) {
            bfs_discard(volume, list, n);
            kfree(list);
            return BFS_ERR_NOSPACE;
        }
        list[n++] = (BfsExtent){start, (uint16_t)got, 0, 0};
        placed += got;
    }
    *extents = list;
    *count = n;
    return BFS_OK;
}

int bfs_write_data(BfsVolume *volume, const void *data, uint64_t size, BfsExtent **extents, uint32_t *count) {
    BfsExtent *list;
    uint32_t n;
    int result = bfs_allocate(volume, size, &list, &n);
    *extents = 0;
    *count = 0;
    if (result || !n) return result;
    uint8_t *staging = kmalloc(BFS_EXTENT_BLOCKS * BFS_BLOCK);
    if (!staging) {
        bfs_discard(volume, list, n);
        kfree(list);
        return BFS_ERR_NOMEM;
    }
    const uint8_t *bytes = data;
    uint64_t offset = 0;
    for (uint32_t e = 0; e < n && !result; ++e) {
        uint32_t span = list[e].blocks * BFS_BLOCK;
        uint32_t length = size - offset < span ? (uint32_t)(size - offset) : span;
        kmemcpy(staging, bytes + offset, (int)length);
        kmemset(staging + length, 0, (int)(span - length));
        list[e].crc = bfs_crc32(0, staging, length);
        result = bfsi_write_blocks(&volume->device, list[e].start, staging, list[e].blocks);
        offset += length;
    }
    kfree(staging);
    if (result) {
        bfs_discard(volume, list, n);
        kfree(list);
        return result;
    }
    *extents = list;
    *count = n;
    return BFS_OK;
}

uint64_t bfs_free_bytes(const BfsVolume *volume) { return (uint64_t)volume->free_blocks * BFS_BLOCK; }

/* ---------- reading ---------- */

int bfs_read(BfsVolume *volume, const BfsRecord *record, uint64_t offset, void *buffer, uint32_t length) {
    if (offset > record->size || length > record->size - offset) return BFS_ERR_INVALID;
    uint8_t *staging = 0;
    uint8_t *out = buffer;
    uint64_t base = 0;
    int result = BFS_OK;
    for (uint32_t e = 0; e < record->extent_count && length; ++e) {
        const BfsExtent *x = &record->extents[e];
        uint64_t span = (uint64_t)x->blocks * BFS_BLOCK;
        if (offset >= base + span) { base += span; continue; }
        if (!staging && !(staging = kmalloc(BFS_EXTENT_BLOCKS * BFS_BLOCK))) return BFS_ERR_NOMEM;
        if ((result = bfsi_read_blocks(&volume->device, x->start, staging, x->blocks))) break;
        uint32_t valid = record->size - base < span ? (uint32_t)(record->size - base) : (uint32_t)span;
        if (bfs_crc32(0, staging, valid) != x->crc) { result = BFS_ERR_CORRUPT; break; }
        uint32_t from = (uint32_t)(offset - base), take = valid - from;
        if (take > length) take = length;
        kmemcpy(out, staging + from, (int)take);
        out += take;
        offset += take;
        length -= take;
        base += span;
    }
    kfree(staging);
    return result;
}

/* ---------- format, mount, commit ---------- */
/* Reserves blocks for a checkpoint of bytes in at most SUPER_EXTENTS runs and
 * records them in super. No I/O. */
static int bfsi_plan_checkpoint(BfsVolume *volume, uint32_t bytes, Super *super) {
    uint32_t need = bfsi_blocks_for(bytes), placed = 0, extents = 0;
    while (placed < need) {
        uint32_t want = need - placed > 0xFFFFu ? 0xFFFFu : need - placed;
        uint32_t start, got = extents < SUPER_EXTENTS ? bfsi_allocate_run(volume, want, &start) : 0;
        if (!got) {
            for (uint32_t e = 0; e < extents; ++e) {
                BfsExtent x = {super->extents[e].start, (uint16_t)super->extents[e].blocks, 0, 0};
                bfs_discard(volume, &x, 1);
            }
            return BFS_ERR_NOSPACE;
        }
        super->extents[extents++] = (SuperExtent){start, got};
        placed += got;
    }
    super->checkpoint_extents = extents;
    super->checkpoint_bytes = bytes;
    return BFS_OK;
}

static int bfsi_read_checkpoint(const BfsDevice *device, const Super *super, uint8_t **out) {
    uint32_t need = bfsi_blocks_for(super->checkpoint_bytes), have = 0;
    for (uint32_t e = 0; e < super->checkpoint_extents; ++e) {
        const SuperExtent *x = &super->extents[e];
        if (x->start < RESERVED_BLOCKS || x->start >= super->blocks || !x->blocks ||
            x->blocks > super->blocks - x->start) return BFS_ERR_CORRUPT;
        have += x->blocks;
    }
    if (have != need) return BFS_ERR_CORRUPT;
    uint8_t *payload = kmalloc(need * BFS_BLOCK);
    if (!payload) return BFS_ERR_NOMEM;
    uint32_t at = 0;
    for (uint32_t e = 0; e < super->checkpoint_extents; ++e) {
        if (bfsi_read_blocks(device, super->extents[e].start, payload + at, super->extents[e].blocks)) {
            kfree(payload);
            return BFS_ERR_IO;
        }
        at += super->extents[e].blocks * BFS_BLOCK;
    }
    if (bfs_crc32(0, payload, super->checkpoint_bytes) != super->checkpoint_crc) {
        kfree(payload);
        return BFS_ERR_CORRUPT;
    }
    *out = payload;
    return BFS_OK;
}

int bfs_marker_matches(const void *sector, uint32_t sectors) {
    uint32_t expected[128];
    const uint32_t *words = sector;
    if (sectors < (RESERVED_BLOCKS + 2) * BFS_SECTORS_PER_BLOCK) return 0;
    bfsi_marker(expected, sectors);
    for (int i = 0; i < 128; ++i)
        if (words[i] != expected[i]) return 0;
    return 1;
}

int bfs_probe(const BfsDevice *device) {
    uint32_t sector[128];
    if (device->sectors < (RESERVED_BLOCKS + 2) * BFS_SECTORS_PER_BLOCK) return 0;
    if (device->read(device->context, 0, sector, 1) < 0) return 0;
    return bfs_marker_matches(sector, device->sectors);
}


void bfs_unmount(BfsVolume *volume) {
    kfree(volume->used);
    kfree(volume->taken);
    kfree(volume->checkpoint);
    kmemset(volume, 0, sizeof *volume);
}

/* Marks the reserved blocks, the checkpoint's blocks and every record's
 * extents. Returns nonzero on an overlap. */
static int bfsi_mark_owned(uint8_t *map, uint32_t blocks, const Super *super,
                      const BfsRecord *records, uint32_t count) {
    kmemset(map, 0, (int)((blocks + 7) / 8));
    for (uint32_t b = 0; b < RESERVED_BLOCKS; ++b) bfsi_set_bit(map, b);
    for (uint32_t e = 0; e < super->checkpoint_extents; ++e)
        for (uint32_t b = super->extents[e].start; b < super->extents[e].start + super->extents[e].blocks; ++b) {
            if (bfsi_bit(map, b)) return 1;
            bfsi_set_bit(map, b);
        }
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t e = 0; e < records[i].extent_count; ++e)
            for (uint32_t b = records[i].extents[e].start;
                 b < records[i].extents[e].start + records[i].extents[e].blocks; ++b) {
                if (b >= blocks || bfsi_bit(map, b)) return 1;
                bfsi_set_bit(map, b);
            }
    return 0;
}

static void bfsi_count_free(BfsVolume *volume) {
    volume->free_blocks = 0;
    for (uint32_t b = 0; b < volume->limit; ++b) volume->free_blocks += bfsi_free_block(volume, b);
}

static int bfsi_remember_checkpoint(BfsVolume *volume, const Super *super) {
    BfsExtent *list = kmalloc(super->checkpoint_extents * sizeof *list);
    if (!list) return BFS_ERR_NOMEM;
    for (uint32_t e = 0; e < super->checkpoint_extents; ++e)
        list[e] = (BfsExtent){super->extents[e].start, (uint16_t)super->extents[e].blocks, 0, 0};
    kfree(volume->checkpoint);
    volume->checkpoint = list;
    volume->checkpoint_extents = super->checkpoint_extents;
    return BFS_OK;
}

int bfs_mount(BfsVolume *volume, const BfsDevice *device, BfsRecord **records, uint32_t *count) {
    kmemset(volume, 0, sizeof *volume);
    if (!bfs_probe(device)) return BFS_ERR_FORMAT;
    uint32_t blocks = bfsi_device_blocks(device);
    Super supers[2];
    int valid[2];
    /* Only a slot that reads back corrupt may be passed over. An unreadable
     * one could hold the newest commit, and mounting the older one would let
     * the next commit overwrite it and reuse its blocks. */
    for (int s = 0; s < 2; ++s) {
        int read = bfsi_read_super(device, s, &supers[s]);
        if (read == BFS_ERR_IO) return BFS_ERR_IO;
        valid[s] = read == BFS_OK;
    }
    int order[2] = {0, 1};
    uint64_t g0 = supers[0].generation_low | (uint64_t)supers[0].generation_high << 32;
    uint64_t g1 = supers[1].generation_low | (uint64_t)supers[1].generation_high << 32;
    if (valid[1] && (!valid[0] || g1 > g0)) { order[0] = 1; order[1] = 0; }
    volume->device = *device;
    volume->blocks = blocks;
    volume->limit = blocks;
    volume->used = kmalloc((blocks + 7) / 8);
    volume->taken = kmalloc((blocks + 7) / 8);
    if (!volume->used || !volume->taken) {
        bfs_unmount(volume);
        return BFS_ERR_NOMEM;
    }
    int result = BFS_ERR_CORRUPT;
    for (int k = 0; k < 2; ++k) {
        int s = order[k];
        if (!valid[s]) continue;
        uint8_t *payload;
        if ((result = bfsi_read_checkpoint(device, &supers[s], &payload))) {
            if (result == BFS_ERR_CORRUPT) continue;
            break;
        }
        kmemset(volume->used, 0, (int)((blocks + 7) / 8));
        BfsRecord *list;
        uint32_t n;
        result = bfsi_decode(payload, supers[s].checkpoint_bytes, blocks, volume->used, &list, &n);
        kfree(payload);
        if (result == BFS_ERR_CORRUPT) continue;
        if (result) break;
        if (n != supers[s].node_count || bfsi_mark_owned(volume->used, blocks, &supers[s], list, n) ||
            bfsi_remember_checkpoint(volume, &supers[s])) {
            bfs_free_records(list, n);
            result = BFS_ERR_CORRUPT;
            continue;
        }
        kmemset(volume->taken, 0, (int)((blocks + 7) / 8));
        volume->slot = s;
        volume->generation = s ? g1 : g0;
        volume->next_identity = supers[s].next_identity;
        volume->next_revision = supers[s].next_revision;
        bfsi_count_free(volume);
        *records = list;
        *count = n;
        return BFS_OK;
    }
    bfs_unmount(volume);
    return result;
}
int bfs_create(BfsVolume *volume, const BfsDevice *device, uint32_t limit) {
    kmemset(volume, 0, sizeof *volume);
    uint32_t blocks = bfsi_device_blocks(device);
    if (blocks < RESERVED_BLOCKS + 2 || limit > blocks || (limit && limit < RESERVED_BLOCKS + 2))
        return BFS_ERR_INVALID;
    volume->device = *device;
    volume->blocks = blocks;
    volume->limit = limit ? limit : blocks;
    volume->used = kmalloc((blocks + 7) / 8);
    volume->taken = kmalloc((blocks + 7) / 8);
    uint8_t *zero = kmalloc(BFS_BLOCK);
    int result = volume->used && volume->taken && zero ? BFS_OK : BFS_ERR_NOMEM;
    if (!result) {
        kmemset(volume->used, 0, (int)((blocks + 7) / 8));
        kmemset(volume->taken, 0, (int)((blocks + 7) / 8));
        for (uint32_t b = 0; b < RESERVED_BLOCKS; ++b) bfsi_set_bit(volume->used, b);
        bfsi_count_free(volume);
        volume->slot = 1;   /* the first commit goes to slot 0 */
        /* Old superblocks must not outlive the new volume's first commit. */
        kmemset(zero, 0, BFS_BLOCK);
        for (uint32_t b = 1; b < RESERVED_BLOCKS && !result; ++b) result = bfsi_write_blocks(device, b, zero, 1);
        if (!result) result = bfsi_flush(device);
    }
    kfree(zero);
    if (result) bfs_unmount(volume);
    return result;
}

int bfs_write_marker(const BfsDevice *device) {
    uint32_t sector[128];
    bfsi_marker(sector, device->sectors);
    if (device->write(device->context, 0, sector, 1) < 0 || device->flush(device->context) < 0)
        return BFS_ERR_IO;
    return BFS_OK;
}

int bfs_format(const BfsDevice *device, uint32_t identity_seed) {
    /* Invalidate the marker first, so a half-formatted disk is never mistaken
     * for a volume. */
    uint32_t zero[128];
    kmemset(zero, 0, sizeof zero);
    if (bfsi_device_blocks(device) < RESERVED_BLOCKS + 2) return BFS_ERR_INVALID;
    if (device->write(device->context, 0, zero, 1) < 0 || device->flush(device->context) < 0)
        return BFS_ERR_IO;
    BfsVolume volume;
    int result = bfs_create(&volume, device, 0);
    if (result) return result;
    char empty[1] = {0};
    BfsRecord root = {BFS_ROOT_PARENT, BFS_DIRECTORY, identity_seed, 0, 0, 0, 0, 0, empty};
    result = bfs_commit(&volume, &root, 1, identity_seed + 1, 1);
    if (!result) result = bfs_write_marker(device);
    bfs_unmount(&volume);
    return result;
}

int bfs_commit_begin(BfsVolume *volume, const BfsRecord *records, uint32_t count,
                     uint32_t next_identity, uint32_t next_revision, BfsCommit *commit) {
    kmemset(commit, 0, sizeof *commit);
    if (!count || records[0].parent != BFS_ROOT_PARENT) return BFS_ERR_INVALID;
    uint32_t bytes = 16;
    for (uint32_t i = 0; i < count; ++i) bytes += bfsi_record_bytes(&records[i]);
    uint32_t blocks = bfsi_blocks_for(bytes);
    uint8_t *payload = kmalloc(blocks * BFS_BLOCK);
    uint8_t *check_map = kmalloc((volume->blocks + 7) / 8);
    if (!payload || !check_map) {
        kfree(payload);
        kfree(check_map);
        return BFS_ERR_NOMEM;
    }
    kmemset(payload, 0, (int)(blocks * BFS_BLOCK));
    bfsi_encode(records, count, payload);
    /* Refuse a table that mount would reject: check it the same way. */
    kmemset(check_map, 0, (int)((volume->blocks + 7) / 8));
    for (uint32_t b = 0; b < RESERVED_BLOCKS; ++b) bfsi_set_bit(check_map, b);
    BfsRecord *decoded;
    uint32_t decoded_count;
    int valid = bfsi_decode(payload, bytes, volume->blocks, check_map, &decoded, &decoded_count);
    kfree(check_map);
    if (valid) {
        kfree(payload);
        return valid == BFS_ERR_NOMEM ? valid : BFS_ERR_INVALID;
    }
    bfs_free_records(decoded, decoded_count);
    Super *super = (Super *)commit->super;
    super->magic = SUPER_MAGIC;
    super->version = FORMAT_VERSION;
    super->block_size = BFS_BLOCK;
    super->blocks = volume->blocks;
    uint64_t generation = volume->generation + 1;
    super->generation_low = (uint32_t)generation;
    super->generation_high = (uint32_t)(generation >> 32);
    super->next_identity = next_identity;
    super->next_revision = next_revision;
    super->node_count = count;
    super->checkpoint_crc = bfs_crc32(0, payload, bytes);
    int result = bfsi_plan_checkpoint(volume, bytes, super);
    if (result) {
        kfree(payload);
        return result;
    }
    super->crc = bfs_crc32(0, super, sizeof *super - 4);
    commit->checkpoint = payload;
    commit->checkpoint_blocks = blocks;
    commit->super_block = 1u + (uint32_t)(volume->slot ^ 1);
    return BFS_OK;
}

uint32_t bfs_commit_runs(const BfsCommit *commit) {
    return ((const Super *)commit->super)->checkpoint_extents;
}

uint32_t bfs_commit_run(const BfsCommit *commit, uint32_t index, uint32_t *blocks) {
    const Super *super = (const Super *)commit->super;
    *blocks = super->extents[index].blocks;
    return super->extents[index].start;
}

void bfs_commit_cancel(BfsVolume *volume, BfsCommit *commit) {
    const Super *super = (const Super *)commit->super;
    for (uint32_t e = 0; e < super->checkpoint_extents; ++e) {
        BfsExtent x = {super->extents[e].start, (uint16_t)super->extents[e].blocks, 0, 0};
        bfs_discard(volume, &x, 1);
    }
    kfree(commit->checkpoint);
    kmemset(commit, 0, sizeof *commit);
}

int bfs_commit_end(BfsVolume *volume, BfsCommit *commit, const BfsRecord *records, uint32_t count,
                   int outcome) {
    const Super *super = (const Super *)commit->super;
    if (outcome == BFS_COMMIT_FAILED) {
        /* Checkpoint blocks of the attempt are free again; data the caller
         * wrote stays taken for the next try. */
        bfs_commit_cancel(volume, commit);
        return BFS_ERR_IO;
    }
    kfree(commit->checkpoint);
    commit->checkpoint = 0;
    if (outcome == BFS_COMMIT_UNCERTAIN) {
        /* The superblock may have reached the disk: keep the checkpoint's
         * blocks out of use until the next commit or mount settles it. */
        kmemset(commit, 0, sizeof *commit);
        return BFS_ERR_IO;
    }
    /* The new commit is current: only its blocks stay owned. Blocks still
     * taken but not referenced (abandoned writes) become free too. */
    uint8_t *owned = kmalloc((volume->blocks + 7) / 8);
    if (!owned || bfsi_mark_owned(owned, volume->blocks, super, records, count) ||
        bfsi_remember_checkpoint(volume, super)) {
        /* Durable, but the in-memory map is unusable: refuse further work. */
        kfree(owned);
        volume->free_blocks = 0;
        volume->limit = 0;
        kmemset(commit, 0, sizeof *commit);
        return BFS_ERR_NOMEM;
    }
    kfree(volume->used);
    volume->used = owned;
    kmemset(volume->taken, 0, (int)((volume->blocks + 7) / 8));
    volume->slot = (int)commit->super_block - 1;
    volume->generation = super->generation_low | (uint64_t)super->generation_high << 32;
    volume->next_identity = super->next_identity;
    volume->next_revision = super->next_revision;
    bfsi_count_free(volume);
    kmemset(commit, 0, sizeof *commit);
    return BFS_OK;
}

static int bfsi_same(const uint8_t *a, const uint8_t *b, uint32_t length) {
    for (uint32_t i = 0; i < length; ++i)
        if (a[i] != b[i]) return 0;
    return 1;
}

int bfs_commit(BfsVolume *volume, const BfsRecord *records, uint32_t count,
               uint32_t next_identity, uint32_t next_revision) {
    BfsCommit commit;
    int result = bfs_commit_begin(volume, records, count, next_identity, next_revision, &commit);
    if (result) return result;
    const BfsDevice *device = &volume->device;
    uint8_t *check = kmalloc(commit.checkpoint_blocks * BFS_BLOCK);
    if (!check) {
        bfs_commit_cancel(volume, &commit);
        return BFS_ERR_NOMEM;
    }
    /* The checkpoint must read back intact before anything points at it. */
    uint32_t at = 0, runs = bfs_commit_runs(&commit);
    for (uint32_t e = 0; e < runs && !result; ++e) {
        uint32_t blocks, start = bfs_commit_run(&commit, e, &blocks);
        result = bfsi_write_blocks(device, start, commit.checkpoint + at, blocks);
        at += blocks * BFS_BLOCK;
    }
    if (!result) result = bfsi_flush(device);
    at = 0;
    for (uint32_t e = 0; e < runs && !result; ++e) {
        uint32_t blocks, start = bfs_commit_run(&commit, e, &blocks);
        result = bfsi_read_blocks(device, start, check + at, blocks);
        at += blocks * BFS_BLOCK;
    }
    if (!result && !bfsi_same(check, commit.checkpoint, commit.checkpoint_blocks * BFS_BLOCK))
        result = BFS_ERR_IO;
    kfree(check);
    if (result) {
        bfs_commit_end(volume, &commit, records, count, BFS_COMMIT_FAILED);
        return result;
    }
    /* From here a failure leaves the outcome uncertain. */
    uint8_t block[BFS_BLOCK];
    kmemset(block, 0, sizeof block);
    kmemcpy(block, commit.super, sizeof commit.super);
    result = bfsi_write_blocks(device, commit.super_block, block, 1);
    if (!result) result = bfsi_flush(device);
    if (!result && device->read(device->context, commit.super_block * BFS_SECTORS_PER_BLOCK, block, 1) < 0)
        result = BFS_ERR_IO;
    if (!result && !bfsi_same(block, commit.super, sizeof commit.super)) result = BFS_ERR_IO;
    if (result) {
        bfs_commit_end(volume, &commit, records, count, BFS_COMMIT_UNCERTAIN);
        return result;
    }
    return bfs_commit_end(volume, &commit, records, count, BFS_COMMIT_DONE);
}
