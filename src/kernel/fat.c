/* FAT12/16/32 volumes with long file names. See fat.h and docs/FAT.md.
 * References: Microsoft "FAT: General Overview of On-Disk Format" (fatgen103)
 * and the VFAT long-name layout it describes. */
#include "kernel/fat.h"

void *kmalloc(uint32_t bytes);
void kfree(void *pointer);
void kmemcpy(void *destination, const void *source, int length);
void kmemset(void *destination, int value, int length);

#define NONE 0xFFFFFFFFu
#define ENTRY 32u
#define SLOTS_PER_SECTOR (512u / ENTRY)
#define DIR_SLOT_MAX 65536u             /* a directory holds at most 2 MiB of entries */
#define FREE_SLOT 0xE5u
#define LFN_ATTRIBUTES 0x0Fu
#define LFN_LAST 0x40u
#define LFN_CHARS 13u
#define CASE_BASE 0x08u                 /* NT flags: base / extension stored lower case */
#define CASE_EXT 0x10u
#define FAT_INFO_LEAD 0x41615252u
#define FAT_INFO_STRUCT 0x61417272u
#define FAT_INFO_TRAIL 0xAA550000u

static const uint8_t zero_sector[512];

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static int dev_read(FatVolume *v, uint32_t lba, void *buffer, uint32_t sectors) {
    if (lba > v->sectors || sectors > v->sectors - lba) return FAT_ERR_CORRUPT;
    return v->device.read(v->device.context, v->start + lba, buffer, sectors) ? FAT_ERR_IO : FAT_OK;
}
static int dev_write(FatVolume *v, uint32_t lba, const void *buffer, uint32_t sectors) {
    if (lba > v->sectors || sectors > v->sectors - lba) return FAT_ERR_CORRUPT;
    return v->device.write(v->device.context, v->start + lba, buffer, sectors) ? FAT_ERR_IO : FAT_OK;
}

/* ---------- boot sector ---------- */

typedef struct {
    uint32_t total, reserved, fats, root_entries, fat_sectors, cluster_sectors;
    uint32_t root_sectors, data_start, clusters;
    int bits;
} Geometry;

/* Checks a boot sector against a volume of sectors sectors. */
static int geometry(const uint8_t *b, uint32_t sectors, Geometry *g) {
    if (b[510] != 0x55 || b[511] != 0xAA) return 0;
    if (b[0] != 0xEB && b[0] != 0xE9) return 0;
    if (le16(b + 11) != 512) return 0;
    g->cluster_sectors = b[13];
    if (!g->cluster_sectors || (g->cluster_sectors & (g->cluster_sectors - 1))) return 0;
    g->reserved = le16(b + 14);
    g->fats = b[16];
    g->root_entries = le16(b + 17);
    g->total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    g->fat_sectors = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    if (!g->reserved || !g->fats || g->fats > 8 || !g->fat_sectors || !g->total || g->total > sectors) return 0;
    g->root_sectors = (g->root_entries * ENTRY + 511u) / 512u;
    uint64_t data = (uint64_t)g->reserved + (uint64_t)g->fats * g->fat_sectors + g->root_sectors;
    if (data >= g->total) return 0;
    g->data_start = (uint32_t)data;
    g->clusters = (g->total - g->data_start) / g->cluster_sectors;
    if (!g->clusters) return 0;
    g->bits = g->clusters < 4085u ? 12 : g->clusters < 65525u ? 16 : 32;
    if (g->bits == 32) {
        if (g->root_entries || le16(b + 22) || le16(b + 42)) return 0;
        if (g->clusters > 0x0FFFFFF5u) return 0;
    } else if (!g->root_entries) {
        return 0;
    }
    return ((uint64_t)g->clusters + 2u) * (uint32_t)g->bits <= (uint64_t)g->fat_sectors * 512u * 8u;
}

int fat_find(const FatDevice *device, uint32_t *start, uint32_t *sectors) {
    uint8_t *b = kmalloc(512), *p = kmalloc(512);
    int result = FAT_ERR_NOMEM;
    if (!b || !p) goto done;
    result = FAT_ERR_IO;
    if (!device->sectors || device->read(device->context, 0, b, 1)) goto done;
    Geometry g;
    result = FAT_OK;
    if (geometry(b, device->sectors, &g)) {
        *start = 0; *sectors = device->sectors;
        goto done;
    }
    result = FAT_ERR_FORMAT;
    if (b[510] != 0x55 || b[511] != 0xAA) goto done;
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *entry = b + 446 + i * 16;
        uint8_t type = entry[4];
        if (type != 0x01 && type != 0x04 && type != 0x06 && type != 0x0B && type != 0x0C && type != 0x0E)
            continue;
        uint32_t first = le32(entry + 8), count = le32(entry + 12);
        if (!first || !count || first >= device->sectors || count > device->sectors - first) continue;
        if (device->read(device->context, first, p, 1)) { result = FAT_ERR_IO; goto done; }
        if (geometry(p, count, &g)) {
            *start = first; *sectors = count; result = FAT_OK;
            goto done;
        }
    }
done:
    kfree(b); kfree(p);
    return result;
}

/* ---------- the FAT ---------- */

static uint32_t end_mark(const FatVolume *v) { return v->bits == 12 ? 0xFFFu : v->bits == 16 ? 0xFFFFu : 0x0FFFFFFFu; }
static int is_end(const FatVolume *v, uint32_t value) {
    return value >= (v->bits == 12 ? 0xFF8u : v->bits == 16 ? 0xFFF8u : 0x0FFFFFF8u);
}
static int valid_cluster(const FatVolume *v, uint32_t cluster) { return cluster >= 2 && cluster - 2 < v->clusters; }
static uint32_t cluster_lba(const FatVolume *v, uint32_t cluster) {
    return v->data_start + (cluster - 2) * v->cluster_sectors;
}

static int window_flush(FatVolume *v) {
    if (!v->window_dirty) return FAT_OK;
    for (uint32_t copy = 0; copy < v->fat_count; ++copy) {
        if (v->fat_active != NONE && copy != v->fat_active) continue;
        int result = dev_write(v, v->fat_start + copy * v->fat_sectors + v->window_sector, v->window, v->window_count);
        if (result) return result;
    }
    v->window_dirty = 0;
    return FAT_OK;
}

/* Brings bytes [byte, byte + width) of the FAT into the window. */
static int window_at(FatVolume *v, uint32_t byte, uint32_t width, uint8_t **at) {
    uint32_t first = byte / 512u, last = (byte + width - 1) / 512u;
    if (last >= v->fat_sectors) return FAT_ERR_CORRUPT;
    if (v->window_sector == NONE || first < v->window_sector || last >= v->window_sector + v->window_count) {
        int result = window_flush(v);
        if (result) return result;
        uint32_t count = v->fat_sectors - first;
        if (count > FAT_WINDOW_SECTORS) count = FAT_WINDOW_SECTORS;
        uint32_t copy = v->fat_active == NONE ? 0 : v->fat_active;
        v->window_sector = NONE;
        if ((result = dev_read(v, v->fat_start + copy * v->fat_sectors + first, v->window, count))) return result;
        v->window_sector = first;
        v->window_count = count;
    }
    *at = v->window + (byte - v->window_sector * 512u);
    return FAT_OK;
}

static int entry_get(FatVolume *v, uint32_t cluster, uint32_t *value) {
    uint8_t *at;
    uint32_t byte = v->bits == 12 ? cluster + cluster / 2 : cluster * (uint32_t)(v->bits / 8);
    int result = window_at(v, byte, v->bits == 32 ? 4 : 2, &at);
    if (result) return result;
    if (v->bits == 32) *value = le32(at) & 0x0FFFFFFFu;
    else if (v->bits == 16) *value = le16(at);
    else *value = cluster & 1 ? le16(at) >> 4 : le16(at) & 0xFFFu;
    return FAT_OK;
}

static int entry_set(FatVolume *v, uint32_t cluster, uint32_t value) {
    uint8_t *at;
    uint32_t byte = v->bits == 12 ? cluster + cluster / 2 : cluster * (uint32_t)(v->bits / 8);
    int result = window_at(v, byte, v->bits == 32 ? 4 : 2, &at);
    if (result) return result;
    if (v->bits == 32) put32(at, (le32(at) & 0xF0000000u) | (value & 0x0FFFFFFFu));
    else if (v->bits == 16) put16(at, value);
    else if (cluster & 1) put16(at, (le16(at) & 0x000Fu) | (value & 0xFFFu) << 4);
    else put16(at, (le16(at) & 0xF000u) | (value & 0xFFFu));
    v->window_dirty = 1;
    return FAT_OK;
}

/* The cluster after cluster: 0 at the end of the chain. */
static int chain_next(FatVolume *v, uint32_t cluster, uint32_t *next) {
    uint32_t value;
    int result = entry_get(v, cluster, &value);
    if (result) return result;
    if (is_end(v, value)) { *next = 0; return FAT_OK; }
    if (!valid_cluster(v, value)) return FAT_ERR_CORRUPT;
    *next = value;
    return FAT_OK;
}

static int same_text(const char *a, const char *b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

static uint32_t clean_bit(const FatVolume *v) { return v->bits == 32 ? 0x08000000u : v->bits == 16 ? 0x8000u : 0; }

/* Marks the volume in use before its first change. */
static int begin_change(FatVolume *v) {
    if (v->read_only) return FAT_ERR_READONLY;
    v->changed = 1;
    if (v->marked_dirty) return FAT_OK;
    if (clean_bit(v)) {
        uint32_t value;
        int result = entry_get(v, 1, &value);
        if (!result) result = entry_set(v, 1, value & ~clean_bit(v));
        if (!result) result = window_flush(v);
        if (result) return result;
    }
    v->marked_dirty = 1;
    return FAT_OK;
}

/* Takes one free cluster and marks it as the end of a chain. */
static int take_cluster(FatVolume *v, uint32_t *out) {
    if (v->free_count == 0) return FAT_ERR_NOSPACE;
    uint32_t start = valid_cluster(v, v->next_free) ? v->next_free : 2;
    for (uint32_t i = 0; i < v->clusters; ++i) {
        uint32_t cluster = 2 + (start - 2 + i) % v->clusters, value;
        int result = entry_get(v, cluster, &value);
        if (result) return result;
        if (value) continue;
        if ((result = entry_set(v, cluster, end_mark(v)))) return result;
        v->next_free = cluster + 1;
        if (v->free_count != NONE) --v->free_count;
        *out = cluster;
        return FAT_OK;
    }
    v->free_count = 0;
    return FAT_ERR_NOSPACE;
}

static int free_chain(FatVolume *v, uint32_t cluster) {
    v->walk_first = NONE;
    for (uint32_t steps = 0; cluster; ++steps) {
        if (steps >= v->clusters || !valid_cluster(v, cluster)) return FAT_ERR_CORRUPT;
        uint32_t next;
        int result = chain_next(v, cluster, &next);
        if (!result) result = entry_set(v, cluster, 0);
        if (result) return result;
        if (v->free_count != NONE) ++v->free_count;
        if (cluster < v->next_free) v->next_free = cluster;
        cluster = next;
    }
    return FAT_OK;
}

uint32_t fat_free_clusters(FatVolume *v) {
    if (v->free_count != NONE) return v->free_count;
    uint32_t count = 0;
    for (uint32_t cluster = 2; cluster - 2 < v->clusters; ++cluster) {
        uint32_t value;
        if (entry_get(v, cluster, &value)) return 0;
        if (!value) ++count;
    }
    v->free_count = count;
    return count;
}

static int zero_cluster(FatVolume *v, uint32_t cluster) {
    uint32_t lba = cluster_lba(v, cluster);
    for (uint32_t i = 0; i < v->cluster_sectors; ++i) {
        int result = dev_write(v, lba + i, zero_sector, 1);
        if (result) return result;
    }
    return FAT_OK;
}

/* ---------- mount ---------- */

int fat_mount(FatVolume *v, const FatDevice *device, uint32_t start, uint32_t sectors) {
    kmemset(v, 0, sizeof *v);
    if (start > device->sectors || sectors > device->sectors - start || !sectors) return FAT_ERR_INVALID;
    v->device = *device;
    v->start = start;
    v->sectors = sectors;
    v->window = kmalloc(FAT_WINDOW_SECTORS * 512u);
    v->dir = kmalloc(512);
    v->sector = kmalloc(512);
    v->window_sector = v->dir_lba = v->walk_first = NONE;
    if (!v->window || !v->dir || !v->sector) { fat_unmount(v); return FAT_ERR_NOMEM; }
    uint8_t *b = v->sector;
    int result = dev_read(v, 0, b, 1);
    Geometry g;
    if (!result && !geometry(b, sectors, &g)) result = FAT_ERR_FORMAT;
    if (result) { fat_unmount(v); return result; }
    v->sectors = g.total;
    v->bits = g.bits;
    v->cluster_sectors = g.cluster_sectors;
    v->cluster_bytes = g.cluster_sectors * 512u;
    v->fat_start = g.reserved;
    v->fat_sectors = g.fat_sectors;
    v->fat_count = g.fats;
    v->fat_active = NONE;
    v->root_start = g.reserved + g.fats * g.fat_sectors;
    v->root_entries = g.root_entries;
    v->data_start = g.data_start;
    v->clusters = g.clusters;
    v->free_count = NONE;
    v->next_free = 2;
    const uint8_t *label = b + 43;
    if (v->bits == 32) {
        uint32_t flags = le16(b + 40);
        if (flags & 0x80u) {
            v->fat_active = flags & 0x0Fu;
            if (v->fat_active >= v->fat_count) { fat_unmount(v); return FAT_ERR_CORRUPT; }
        }
        v->root_cluster = le32(b + 44);
        v->fsinfo = le16(b + 48);
        if (v->fsinfo == 0xFFFFu || v->fsinfo >= g.reserved) v->fsinfo = 0;
        label = b + 71;
        if (!valid_cluster(v, v->root_cluster)) { fat_unmount(v); return FAT_ERR_CORRUPT; }
    }
    if (label[-5] == 0x29) {
        kmemcpy(v->label, label, 11);
        int length = 11;
        while (length && v->label[length - 1] == ' ') --length;
        v->label[length] = 0;
        if (same_text(v->label, "NO NAME")) v->label[0] = 0;
    }
    uint32_t mark;
    if ((result = entry_get(v, 1, &mark))) { fat_unmount(v); return result; }
    if (clean_bit(v) && !(mark & clean_bit(v))) v->was_dirty = v->marked_dirty = 1;
    if (v->fsinfo && !v->was_dirty) {
        if ((result = dev_read(v, v->fsinfo, b, 1))) { fat_unmount(v); return result; }
        if (le32(b) == FAT_INFO_LEAD && le32(b + 484) == FAT_INFO_STRUCT && le32(b + 508) == FAT_INFO_TRAIL) {
            uint32_t free = le32(b + 488), next = le32(b + 492);
            if (free <= v->clusters) v->free_count = free;
            if (valid_cluster(v, next)) v->next_free = next;
        }
    }
    return FAT_OK;
}

int fat_sync(FatVolume *v) {
    if (!v->window) return FAT_OK;
    int result = window_flush(v);
    if (result || !v->changed) return result;
    if (v->fsinfo) {
        uint8_t *b = v->sector;
        if ((result = dev_read(v, v->fsinfo, b, 1))) return result;
        if (le32(b) == FAT_INFO_LEAD && le32(b + 484) == FAT_INFO_STRUCT && le32(b + 508) == FAT_INFO_TRAIL) {
            put32(b + 488, v->free_count);
            put32(b + 492, v->next_free);
            if ((result = dev_write(v, v->fsinfo, b, 1))) return result;
        }
    }
    if (v->device.flush(v->device.context)) return FAT_ERR_IO;
    if (clean_bit(v)) {
        uint32_t value;
        if ((result = entry_get(v, 1, &value))) return result;
        if ((result = entry_set(v, 1, value | clean_bit(v)))) return result;
        if ((result = window_flush(v))) return result;
        if (v->device.flush(v->device.context)) return FAT_ERR_IO;
    }
    v->changed = v->marked_dirty = v->was_dirty = 0;
    return FAT_OK;
}

int fat_unmount(FatVolume *v) {
    int result = fat_sync(v);
    kfree(v->window); kfree(v->dir); kfree(v->sector);
    v->window = v->dir = v->sector = 0;
    return result;
}

/* ---------- names and times ---------- */

static const uint16_t cp437_high[128] = {
    0xc7, 0xfc, 0xe9, 0xe2, 0xe4, 0xe0, 0xe5, 0xe7,
    0xea, 0xeb, 0xe8, 0xef, 0xee, 0xec, 0xc4, 0xc5,
    0xc9, 0xe6, 0xc6, 0xf4, 0xf6, 0xf2, 0xfb, 0xf9,
    0xff, 0xd6, 0xdc, 0xa2, 0xa3, 0xa5, 0x20a7, 0x192,
    0xe1, 0xed, 0xf3, 0xfa, 0xf1, 0xd1, 0xaa, 0xba,
    0xbf, 0x2310, 0xac, 0xbd, 0xbc, 0xa1, 0xab, 0xbb,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
    0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
    0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
    0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
    0x3b1, 0xdf, 0x393, 0x3c0, 0x3a3, 0x3c3, 0xb5, 0x3c4,
    0x3a6, 0x398, 0x3a9, 0x3b4, 0x221e, 0x3c6, 0x3b5, 0x2229,
    0x2261, 0xb1, 0x2265, 0x2264, 0x2320, 0x2321, 0xf7, 0x2248,
    0xb0, 0x2219, 0xb7, 0x221a, 0x207f, 0xb2, 0x25a0, 0xa0,
};

static unsigned put_utf8(char *out, uint32_t c) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | c >> 6); out[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | c >> 12); out[1] = (char)(0x80 | (c >> 6 & 63)); out[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    out[0] = (char)(0xF0 | c >> 18); out[1] = (char)(0x80 | (c >> 12 & 63));
    out[2] = (char)(0x80 | (c >> 6 & 63)); out[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* UTF-16 to UTF-8; an unpaired surrogate becomes U+FFFD. At most 255 units
 * fit FAT_NAME_BYTES: a pair takes 4 bytes for 2 units, others 3 per unit. */
static void utf16_text(const uint16_t *u, unsigned n, char *out) {
    unsigned length = 0;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD;
        }
        length += put_utf8(out + length, c);
    }
    out[length] = 0;
}

/* A name from this API as UTF-16, checked for what FAT allows. */
static int name_units(const char *name, uint16_t *u, unsigned *count) {
    const uint8_t *p = (const uint8_t *)name;
    unsigned n = 0;
    while (*p) {
        uint32_t c = *p++;
        unsigned more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 9;
        if (more == 9) return FAT_ERR_INVALID;
        if (more) c &= 0x3Fu >> more;
        for (unsigned i = 0; i < more; ++i, ++p) {
            if ((*p & 0xC0) != 0x80) return FAT_ERR_INVALID;
            c = c << 6 | (*p & 63u);
        }
        if ((more == 1 && c < 0x80) || (more == 2 && c < 0x800) || (more == 3 && c < 0x10000) ||
            c > 0x10FFFF || (c >= 0xD800 && c < 0xE000))
            return FAT_ERR_INVALID;
        if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' ||
            c == '?' || c == '\\' || c == '|' || c == 0x7F)
            return FAT_ERR_INVALID;
        if (n + (c >= 0x10000 ? 2 : 1) > FAT_NAME_MAX) return FAT_ERR_NAMETOOLONG;
        if (c >= 0x10000) {
            u[n++] = (uint16_t)(0xD800 + ((c - 0x10000) >> 10));
            u[n++] = (uint16_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
        } else {
            u[n++] = (uint16_t)c;
        }
    }
    if (!n || u[n - 1] == ' ' || u[n - 1] == '.') return FAT_ERR_INVALID;
    *count = n;
    return FAT_OK;
}

static int fold(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int same_name(const char *a, const char *b) {
    while (*a && fold((uint8_t)*a) == fold((uint8_t)*b)) ++a, ++b;
    return *a == *b;
}

/* The 8.3 name of a short entry as text, in NT case if flagged. */
static void short_text(const uint8_t *e, char *out) {
    unsigned length = 0;
    for (unsigned i = 0; i < 11; ++i) {
        if (i == 8) {
            if (e[8] == ' ') break;
            out[length++] = '.';
        }
        uint8_t c = i == 0 && e[0] == 0x05 ? FREE_SLOT : e[i];
        if (c == ' ') { if (i < 8) { i = 7; } continue; }
        if (c >= 'A' && c <= 'Z' && (e[12] & (i < 8 ? CASE_BASE : CASE_EXT))) c += 32;
        length += put_utf8(out + length, c < 0x80 ? c : cp437_high[c - 0x80]);
    }
    out[length] = 0;
}

static int short_char(uint32_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    for (const char *p = "$%'-_@~`!(){}^#&"; *p; ++p)
        if ((uint32_t)*p == c) return 1;
    return 0;
}

/* name as an 8.3 entry name: 1 when it fits and its base and extension are
 * each in one case (no long name needed), 2 when it fits in mixed case (the
 * short name is still the name itself), 0 when it does not fit. */
static int plain_short(const uint16_t *u, unsigned n, uint8_t *name11, uint8_t *flags) {
    unsigned dot = n;
    for (unsigned i = 0; i < n; ++i)
        if (u[i] == '.') { if (dot != n) return 0; dot = i; }
    if (dot == 0 || dot > 8 || (dot < n && n - dot - 1 > 3)) return 0;
    kmemset(name11, ' ', 11);
    *flags = 0;
    int lower[2] = {0, 0}, upper[2] = {0, 0};
    for (unsigned i = 0, at = 0; i < n; ++i) {
        if (i == dot) { at = 8; continue; }
        uint32_t c = u[i];
        int part = i > dot;
        if (c >= 'a' && c <= 'z') { lower[part] = 1; c -= 32; }
        else if (c >= 'A' && c <= 'Z') upper[part] = 1;
        if (!short_char(c)) return 0;
        name11[at++] = (uint8_t)c;
    }
    if ((lower[0] && upper[0]) || (lower[1] && upper[1])) return 2;
    *flags = (uint8_t)((lower[0] ? CASE_BASE : 0) | (lower[1] ? CASE_EXT : 0));
    return 1;
}

/* The basis of a generated short name (fatgen103): upper case, no spaces or
 * leading dots, other characters as '_', extension from the last dot.
 * Returns the base length (up to 8). */
static unsigned short_basis(const uint16_t *u, unsigned n, uint8_t *name11) {
    kmemset(name11, ' ', 11);
    unsigned last = n;
    for (unsigned i = 0; i < n; ++i)
        if (u[i] == '.') last = i;
    unsigned base = 0, ext = 0, i = 0;
    while (i < n && u[i] == '.') ++i;
    if (last < i) last = n;
    for (; i < n && base < 8; ++i) {
        if (i == last) break;
        uint32_t c = u[i];
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        name11[base++] = (uint8_t)(short_char(c) ? c : '_');
    }
    for (i = last + 1; last < n && i < n && ext < 3; ++i) {
        uint32_t c = u[i];
        if (c == ' ') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        name11[8 + ext++] = (uint8_t)(short_char(c) ? c : '_');
    }
    if (!base) name11[base++] = '_';
    return base;
}

/* The basis with tail "~number". */
static void short_tail(const uint8_t *basis, unsigned base, uint32_t number, uint8_t *name11) {
    char digits[8];
    unsigned count = 0;
    do { digits[count++] = (char)('0' + number % 10); number /= 10; } while (number);
    unsigned keep = 8 - 1 - count;
    if (keep > base) keep = base;
    kmemcpy(name11, basis, 11);
    for (unsigned i = keep; i < 8; ++i) name11[i] = ' ';
    name11[keep] = '~';
    for (unsigned i = 0; i < count; ++i) name11[keep + 1 + i] = (uint8_t)digits[count - 1 - i];
}

static uint8_t lfn_checksum(const uint8_t *name11) {
    uint8_t sum = 0;
    for (unsigned i = 0; i < 11; ++i) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

static const uint8_t lfn_offsets[LFN_CHARS] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

static int leap(uint32_t year) { return (year % 4 == 0 && year % 100) || year % 400 == 0; }
static uint32_t month_days(uint32_t year, uint32_t month) {
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return days[month - 1] + (month == 2 && leap(year));
}

/* Seconds since 2000-01-01 as a DOS date and time (2-second steps). */
static void dos_time(uint32_t seconds, uint32_t *date, uint32_t *time) {
    if (!seconds) { *date = 0x21; *time = 0; return; }   /* 1980-01-01 */
    uint32_t days = seconds / 86400u, rest = seconds % 86400u, year = 2000, month = 1;
    while (days >= 365u + leap(year)) days -= 365u + leap(year++);
    while (days >= month_days(year, month)) days -= month_days(year, month++);
    if (year > 2107) { *date = (127u << 9) | (12u << 5) | 31u; *time = (23u << 11) | (59u << 5) | 29u; return; }
    *date = (year - 1980) << 9 | month << 5 | (days + 1);
    *time = (rest / 3600u) << 11 | (rest / 60u % 60u) << 5 | (rest % 60u) / 2u;
}

/* A DOS date and time as seconds since 2000-01-01; 0 before 2000 or invalid. */
static uint32_t from_dos(uint32_t date, uint32_t time) {
    uint32_t year = 1980 + (date >> 9), month = date >> 5 & 15, day = date & 31;
    uint32_t hour = time >> 11, minute = time >> 5 & 63, second = (time & 31) * 2;
    if (year < 2000 || month < 1 || month > 12 || day < 1 || day > month_days(year, month) ||
        hour > 23 || minute > 59 || second > 59)
        return 0;
    uint32_t days = day - 1;
    for (uint32_t y = 2000; y < year; ++y) days += 365u + leap(y);
    for (uint32_t m = 1; m < month; ++m) days += month_days(year, m);
    return days * 86400u + hour * 3600u + minute * 60u + second;
}

static uint32_t now_of(const FatVolume *v) { return v->now ? v->now() : 0; }

/* ---------- directories ---------- */

static uint32_t entry_cluster(const FatVolume *v, const uint8_t *e) {
    return le16(e + 26) | (v->bits == 32 ? le16(e + 20) << 16 : 0);
}
static void set_entry_cluster(const FatVolume *v, uint8_t *e, uint32_t cluster) {
    put16(e + 26, cluster);
    put16(e + 20, v->bits == 32 ? cluster >> 16 : 0);
}

/* First cluster of a directory's entries; 0 is the FAT12/16 fixed root. */
static int dir_first(const FatVolume *v, const FatNode *d, uint32_t *first) {
    if (d->is_root) { *first = v->bits == 32 ? v->root_cluster : 0; return FAT_OK; }
    if (!(d->attributes & FAT_ATTR_DIRECTORY)) return FAT_ERR_NOTDIR;
    if (!valid_cluster(v, d->cluster)) return FAT_ERR_CORRUPT;
    *first = d->cluster;
    return FAT_OK;
}
static int is_root_first(const FatVolume *v, uint32_t first) { return v->bits == 32 ? first == v->root_cluster : !first; }

/* The sector holding a slot of the directory starting at first. 1 when the
 * slot is past the end; with grow, a cluster directory gets new zeroed
 * clusters instead. */
static int slot_lba(FatVolume *v, uint32_t first, uint32_t slot, uint32_t *lba, int grow) {
    if (!first) {
        if (slot >= v->root_entries) return grow ? FAT_ERR_NOSPACE : 1;
        *lba = v->root_start + slot / SLOTS_PER_SECTOR;
        return FAT_OK;
    }
    if (slot >= DIR_SLOT_MAX) return grow ? FAT_ERR_NOSPACE : 1;
    uint32_t per = v->cluster_bytes / ENTRY, index = slot / per;
    if (v->walk_first != first || v->walk_index > index) {
        v->walk_first = first; v->walk_index = 0; v->walk_cluster = first;
    }
    while (v->walk_index < index) {
        uint32_t next;
        int result = chain_next(v, v->walk_cluster, &next);
        if (result) return result;
        if (!next) {
            if (!grow) return 1;
            if ((result = take_cluster(v, &next))) return result;
            int linked = 0;
            if (!(result = zero_cluster(v, next)) && !(result = entry_set(v, v->walk_cluster, next))) {
                linked = 1;
                result = window_flush(v);
            }
            if (result) {
                /* The old tail ends the chain again before the new cluster
                 * is free, so no later flush can leave it pointing there. */
                if (linked) entry_set(v, v->walk_cluster, end_mark(v));
                entry_set(v, next, 0);
                if (v->free_count != NONE) ++v->free_count;
                return result;
            }
        }
        v->walk_cluster = next;
        ++v->walk_index;
    }
    *lba = cluster_lba(v, v->walk_cluster) + (slot % per) / SLOTS_PER_SECTOR;
    return FAT_OK;
}

/* The 32 bytes of a slot, in v->dir; 1 past the end. */
static int slot_entry(FatVolume *v, uint32_t first, uint32_t slot, uint8_t **e, int grow) {
    uint32_t lba;
    int result = slot_lba(v, first, slot, &lba, grow);
    if (result) return result;
    if (v->dir_lba != lba) {
        v->dir_lba = NONE;
        if ((result = dev_read(v, lba, v->dir, 1))) return result;
        v->dir_lba = lba;
    }
    *e = v->dir + slot % SLOTS_PER_SECTOR * ENTRY;
    return FAT_OK;
}
/* Writes back the cached directory sector. On failure the cache is dropped,
 * so a later write of the same sector cannot publish the failed change. */
static int dir_store(FatVolume *v) {
    int result = dev_write(v, v->dir_lba, v->dir, 1);
    if (result) v->dir_lba = NONE;
    return result;
}

static int is_dot_entry(const uint8_t *e) {
    return e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '));
}

/* The next live entry from *cursor; short_name gets its 8.3 name when given. */
static int next_entry(FatVolume *v, uint32_t first, uint32_t *cursor, FatNode *out, char *short_name) {
    uint16_t units[20 * LFN_CHARS];
    unsigned total = 0, expect = 0, start = 0;
    uint8_t checksum = 0;
    for (uint32_t slot = *cursor;; ++slot) {
        uint8_t *e;
        int result = slot_entry(v, first, slot, &e, 0);
        if (result < 0) return result;
        if (result == 1 || !e[0]) { *cursor = slot; return 0; }
        if (e[0] == FREE_SLOT) { total = expect = 0; continue; }
        if ((e[11] & 0x3F) == LFN_ATTRIBUTES) {
            unsigned order = e[0] & 0x1Fu;
            if (e[0] & LFN_LAST) {
                total = expect = 0;
                if (!order || order > 20) continue;
                total = expect = order; checksum = e[13]; start = slot;
            } else if (!expect || order != expect || e[13] != checksum) {
                total = expect = 0;
                continue;
            }
            for (unsigned i = 0; i < LFN_CHARS; ++i)
                units[(order - 1) * LFN_CHARS + i] = (uint16_t)le16(e + lfn_offsets[i]);
            --expect;
            continue;
        }
        int long_name = total && !expect && lfn_checksum(e) == checksum && slot - start == total;
        if ((e[11] & FAT_ATTR_VOLUME) || is_dot_entry(e)) { total = expect = 0; continue; }
        kmemset(out, 0, sizeof *out);
        out->parent = first;
        out->slot = slot;
        out->cluster = entry_cluster(v, e);
        out->size = le32(e + 28);
        out->attributes = e[11];
        out->modified = from_dos(le16(e + 24), le16(e + 22));
        if (out->attributes & FAT_ATTR_DIRECTORY) out->size = 0;
        if (short_name) short_text(e, short_name);
        unsigned length = 0;
        if (long_name) {
            while (length < total * LFN_CHARS && units[length]) ++length;
            if (length > FAT_NAME_MAX) long_name = 0;
        }
        if (long_name && length) {
            out->lfn_slots = total;
            utf16_text(units, length, out->name);
        } else {
            short_text(e, out->name);
        }
        *cursor = slot + 1;
        return 1;
    }
}

void fat_root(const FatVolume *v, FatNode *root) {
    kmemset(root, 0, sizeof *root);
    root->is_root = 1;
    root->attributes = FAT_ATTR_DIRECTORY;
    root->cluster = v->bits == 32 ? v->root_cluster : 0;
}

int fat_next(FatVolume *v, const FatNode *directory, uint32_t *cursor, FatNode *out) {
    uint32_t first;
    int result = dir_first(v, directory, &first);
    return result ? result : next_entry(v, first, cursor, out, 0);
}

int fat_lookup(FatVolume *v, const FatNode *directory, const char *name, FatNode *out) {
    uint32_t first, cursor = 0;
    int result = dir_first(v, directory, &first);
    if (result) return result;
    char short_name[40];
    while ((result = next_entry(v, first, &cursor, out, short_name)) == 1)
        if (same_name(out->name, name) || same_name(short_name, name)) return FAT_OK;
    return result ? result : FAT_ERR_NOTFOUND;
}

int fat_is_empty(FatVolume *v, const FatNode *directory) {
    FatNode node;
    uint32_t cursor = 0;
    int result = fat_next(v, directory, &cursor, &node);
    return result < 0 ? result : !result;
}

int fat_resolve(FatVolume *v, const char *path, FatNode *out) {
    unsigned length = 0;
    while (path[length]) ++length;
    char *copy = kmalloc(length + 1);
    const char **parts = kmalloc((length / 2 + 1) * sizeof *parts);
    int result = FAT_ERR_NOMEM;
    if (!copy || !parts) goto done;
    kmemcpy(copy, path, (int)length + 1);
    unsigned count = 0;
    for (char *p = copy; *p;) {
        while (*p == '/') *p++ = 0;
        if (!*p) break;
        char *part = p;
        while (*p && *p != '/') ++p;
        if (*p) *p++ = 0;
        if (part[0] == '.' && !part[1]) continue;
        if (part[0] == '.' && part[1] == '.' && !part[2]) { if (count) --count; continue; }
        parts[count++] = part;
    }
    fat_root(v, out);
    result = FAT_OK;
    for (unsigned i = 0; i < count && !result; ++i) {
        if (!(out->attributes & FAT_ATTR_DIRECTORY)) { result = FAT_ERR_NOTDIR; break; }
        FatNode parent = *out;
        result = fat_lookup(v, &parent, parts[i], out);
    }
done:
    kfree(copy); kfree(parts);
    return result;
}

/* Whether a live short entry of the directory has name11, other than the
 * entry at slot skip. */
static int short_used(FatVolume *v, uint32_t first, const uint8_t *name11, uint32_t skip) {
    for (uint32_t slot = 0;; ++slot) {
        uint8_t *e;
        int result = slot_entry(v, first, slot, &e, 0);
        if (result < 0) return result;
        if (result == 1 || !e[0]) return 0;
        if (e[0] == FREE_SLOT || (e[11] & 0x3F) == LFN_ATTRIBUTES || slot == skip) continue;
        unsigned i = 0;
        while (i < 11 && e[i] == name11[i]) ++i;
        if (i == 11) return 1;
    }
}

/* A short name for a new entry: the name itself when it is a plain 8.3 name
 * no other entry uses, else a "~number" alias. *long_name says whether the
 * entry needs long-name slots. */
static int choose_short(FatVolume *v, uint32_t first, const uint16_t *u, unsigned n, uint32_t skip,
                        uint8_t *name11, uint8_t *flags, int *long_name) {
    int result, fits = plain_short(u, n, name11, flags);
    if (fits && (result = short_used(v, first, name11, skip)) <= 0) {
        *long_name = fits == 2;
        if (fits == 2) *flags = 0;
        return result;
    }
    *flags = 0;
    *long_name = 1;
    uint8_t basis[11];
    unsigned base = short_basis(u, n, basis);
    /* One pass marks which tails are taken. */
    enum { TAILS = 65536 };
    uint8_t *taken = kmalloc(TAILS / 8);
    if (!taken) return FAT_ERR_NOMEM;
    kmemset(taken, 0, TAILS / 8);
    result = FAT_ERR_NOSPACE;
    for (uint32_t slot = 0;; ++slot) {
        uint8_t *e;
        int found = slot_entry(v, first, slot, &e, 0);
        if (found < 0) { result = found; goto done; }
        if (found == 1 || !e[0]) break;
        if (e[0] == FREE_SLOT || (e[11] & 0x3F) == LFN_ATTRIBUTES || slot == skip) continue;
        /* The tail is after the last tilde: the basis may hold one too. */
        unsigned tilde = 8;
        for (unsigned k = 0; k < 8; ++k)
            if (e[k] == '~') tilde = k;
        uint32_t number = 0;
        unsigned i = tilde + 1;
        while (i < 8 && e[i] >= '0' && e[i] <= '9' && number < TAILS) number = number * 10 + (e[i++] - '0');
        if (tilde == 8 || !number || number >= TAILS) continue;
        uint8_t candidate[11];
        short_tail(basis, base, number, candidate);
        unsigned same = 0;
        while (same < 11 && candidate[same] == e[same]) ++same;
        if (same == 11) taken[number / 8] |= (uint8_t)(1u << number % 8);
    }
    for (uint32_t number = 1; number < TAILS; ++number) {
        if (taken[number / 8] & (1u << number % 8)) continue;
        short_tail(basis, base, number, name11);
        /* Belt and braces: an entry another system named oddly may still
         * hold this alias. */
        int used = short_used(v, first, name11, skip);
        if (used < 0) { result = used; break; }
        if (used) continue;
        result = FAT_OK;
        break;
    }
done:
    kfree(taken);
    return result;
}

/* Finds count free slots in a row, past the end if need be; *past_end says
 * the run reaches the end marker. */
static int free_run(FatVolume *v, uint32_t first, uint32_t count, uint32_t *start, int *past_end) {
    uint32_t run = 0;
    *past_end = 0;
    for (uint32_t slot = 0;; ++slot) {
        uint8_t *e;
        int result = slot_entry(v, first, slot, &e, 0);
        if (result < 0) return result;
        if (result == 1 || !e[0]) {
            if (!run) *start = slot;
            *past_end = 1;
            uint32_t limit = first ? DIR_SLOT_MAX : v->root_entries;
            return *start + count <= limit ? FAT_OK : FAT_ERR_NOSPACE;
        }
        if (e[0] == FREE_SLOT) {
            if (!run++) *start = slot;
            if (run == count) return FAT_OK;
        } else {
            run = 0;
        }
    }
}

/* Writes long-name slots (if any) and the short entry. */
static int write_entries(FatVolume *v, uint32_t first, const uint16_t *u, unsigned n, int long_name,
                         const uint8_t *short_entry, uint32_t *slot, uint32_t *lfn_slots) {
    uint32_t parts = long_name ? (n + LFN_CHARS - 1) / LFN_CHARS : 0, start;
    int past_end, result = free_run(v, first, parts + 1, &start, &past_end);
    if (result) return result;
    if (past_end) {
        /* Slots past the end marker may hold stale entries: a new marker
         * after the run goes first, so they never become visible. */
        uint8_t *e;
        result = slot_entry(v, first, start + parts + 1, &e, 0);
        if (result < 0) return result;
        if (!result && e[0]) {
            e[0] = 0;
            if ((result = dir_store(v))) return result;
        }
    }
    uint8_t checksum = lfn_checksum(short_entry);
    for (uint32_t i = 0; i <= parts; ++i) {
        uint8_t *e;
        if ((result = slot_entry(v, first, start + i, &e, 1))) return result < 0 ? result : FAT_ERR_CORRUPT;
        if (i == parts) {
            kmemcpy(e, short_entry, ENTRY);
        } else {
            uint32_t order = parts - i;
            kmemset(e, 0, ENTRY);
            e[0] = (uint8_t)(order | (i == 0 ? LFN_LAST : 0));
            e[11] = LFN_ATTRIBUTES;
            e[13] = checksum;
            for (unsigned k = 0; k < LFN_CHARS; ++k) {
                unsigned at = (order - 1) * LFN_CHARS + k;
                put16(e + lfn_offsets[k], at < n ? u[at] : at == n ? 0 : 0xFFFF);
            }
        }
        if ((result = dir_store(v))) return result;
    }
    *slot = start + parts;
    *lfn_slots = parts;
    return FAT_OK;
}

static void make_short(const FatVolume *v, uint8_t *e, const uint8_t *name11, uint8_t flags,
                       uint8_t attributes, uint32_t cluster, uint32_t when) {
    uint32_t date, time;
    dos_time(when, &date, &time);
    kmemset(e, 0, ENTRY);
    kmemcpy(e, name11, 11);
    e[11] = attributes;
    e[12] = flags;
    put16(e + 14, time); put16(e + 16, date); put16(e + 18, date);
    put16(e + 22, time); put16(e + 24, date);
    set_entry_cluster(v, e, cluster);
}

/* The short entry of a node, checked to still be the node's. */
static int node_entry(FatVolume *v, const FatNode *node, uint8_t **e) {
    if (node->is_root) return FAT_ERR_INVALID;
    int result = slot_entry(v, node->parent, node->slot, e, 0);
    if (result) return result < 0 ? result : FAT_ERR_NOTFOUND;
    uint8_t *x = *e;
    if (!x[0] || x[0] == FREE_SLOT || (x[11] & 0x3F) == LFN_ATTRIBUTES || entry_cluster(v, x) != node->cluster)
        return FAT_ERR_NOTFOUND;
    return FAT_OK;
}

/* Writes a node's cluster, size, attributes and time to its entry. */
static int update_entry(FatVolume *v, const FatNode *node) {
    uint8_t *e;
    int result = slot_entry(v, node->parent, node->slot, &e, 0);
    if (result) return result < 0 ? result : FAT_ERR_NOTFOUND;
    if (!e[0] || e[0] == FREE_SLOT) return FAT_ERR_NOTFOUND;
    uint32_t date, time;
    dos_time(node->modified, &date, &time);
    set_entry_cluster(v, e, node->cluster);
    put32(e + 28, node->attributes & FAT_ATTR_DIRECTORY ? 0 : node->size);
    e[11] = node->attributes;
    put16(e + 18, date); put16(e + 22, time); put16(e + 24, date);
    return dir_store(v);
}

/* Marks a node's slots deleted. */
static int erase_entries(FatVolume *v, uint32_t first, uint32_t slot, uint32_t lfn_slots) {
    for (uint32_t s = slot - lfn_slots; s <= slot; ++s) {
        uint8_t *e;
        int result = slot_entry(v, first, s, &e, 0);
        if (result) return result < 0 ? result : FAT_ERR_CORRUPT;
        if (s < slot && (e[11] & 0x3F) != LFN_ATTRIBUTES) continue;
        e[0] = FREE_SLOT;
        if ((result = dir_store(v))) return result;
    }
    return FAT_OK;
}

int fat_create(FatVolume *v, const FatNode *directory, const char *name, int is_directory, FatNode *out) {
    uint32_t first;
    int result = dir_first(v, directory, &first);
    if (result) return result;
    if (v->read_only) return FAT_ERR_READONLY;
    uint16_t u[FAT_NAME_MAX];
    unsigned n;
    if ((result = name_units(name, u, &n))) return result;
    FatNode existing;
    result = fat_lookup(v, directory, name, &existing);
    if (!result) return FAT_ERR_EXISTS;
    if (result != FAT_ERR_NOTFOUND) return result;
    if ((result = begin_change(v))) return result;
    uint8_t name11[11], flags;
    int long_name;
    if ((result = choose_short(v, first, u, n, NONE, name11, &flags, &long_name))) return result;
    uint32_t when = now_of(v), date, time, cluster = 0;
    dos_time(when, &date, &time);
    uint8_t entry[ENTRY];
    if (is_directory) {
        if ((result = take_cluster(v, &cluster))) return result;
        if (!(result = zero_cluster(v, cluster))) {
            uint8_t *dots = v->sector;
            kmemset(dots, 0, 512);
            static const uint8_t dot[11] = {'.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
            static const uint8_t dotdot[11] = {'.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
            make_short(v, dots, dot, 0, FAT_ATTR_DIRECTORY, cluster, when);
            make_short(v, dots + ENTRY, dotdot, 0, FAT_ATTR_DIRECTORY, is_root_first(v, first) ? 0 : first, when);
            result = dev_write(v, cluster_lba(v, cluster), dots, 1);
        }
        if (!result) result = window_flush(v);
        if (result) { free_chain(v, cluster); window_flush(v); return result; }
    }
    uint8_t attributes = is_directory ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
    make_short(v, entry, name11, flags, attributes, cluster, when);
    uint32_t slot, lfn_slots;
    result = write_entries(v, first, u, n, long_name, entry, &slot, &lfn_slots);
    if (!result) result = window_flush(v);
    if (result) {
        if (cluster) { free_chain(v, cluster); window_flush(v); }
        return result;
    }
    kmemset(out, 0, sizeof *out);
    out->parent = first;
    out->slot = slot;
    out->lfn_slots = lfn_slots;
    out->cluster = cluster;
    out->attributes = attributes;
    out->modified = from_dos(date, time);
    if (long_name) utf16_text(u, n, out->name);
    else short_text(entry, out->name);
    return FAT_OK;
}

int fat_remove(FatVolume *v, const FatNode *node) {
    if (node->is_root) return FAT_ERR_INVALID;
    if (v->read_only) return FAT_ERR_READONLY;
    uint8_t *e;
    int result = node_entry(v, node, &e);
    if (result) return result;
    if (node->attributes & FAT_ATTR_DIRECTORY) {
        if ((result = fat_is_empty(v, node)) <= 0) return result < 0 ? result : FAT_ERR_NOTEMPTY;
    }
    if ((result = begin_change(v))) return result;
    if ((result = erase_entries(v, node->parent, node->slot, node->lfn_slots))) return result;
    if (node->cluster && (result = free_chain(v, node->cluster))) return result;
    return window_flush(v);
}

int fat_rename(FatVolume *v, FatNode *node, const FatNode *directory, const char *name) {
    if (node->is_root) return FAT_ERR_INVALID;
    if (v->read_only) return FAT_ERR_READONLY;
    uint32_t first;
    int result = dir_first(v, directory, &first);
    if (result) return result;
    uint16_t u[FAT_NAME_MAX];
    unsigned n;
    if ((result = name_units(name, u, &n))) return result;
    FatNode existing;
    result = fat_lookup(v, directory, name, &existing);
    if (!result && (existing.parent != node->parent || existing.slot != node->slot)) return FAT_ERR_EXISTS;
    if (result && result != FAT_ERR_NOTFOUND) return result;
    int is_directory = (node->attributes & FAT_ATTR_DIRECTORY) != 0;
    if (is_directory) {
        /* The target must not be the node or inside it. */
        uint32_t walk = first;
        for (uint32_t steps = 0; !is_root_first(v, walk); ++steps) {
            if (walk == node->cluster || steps > v->clusters) return FAT_ERR_INVALID;
            uint8_t *e;
            if ((result = slot_entry(v, walk, 1, &e, 0))) return result < 0 ? result : FAT_ERR_CORRUPT;
            if (e[0] != '.' || e[1] != '.') return FAT_ERR_CORRUPT;
            walk = entry_cluster(v, e);
            if (!walk && v->bits == 32) walk = v->root_cluster;
        }
    }
    uint8_t *e, old[ENTRY];
    if ((result = node_entry(v, node, &e))) return result;
    kmemcpy(old, e, ENTRY);
    if ((result = begin_change(v))) return result;
    uint8_t name11[11], flags;
    int long_name;
    uint32_t skip = first == node->parent ? node->slot : NONE;
    if ((result = choose_short(v, first, u, n, skip, name11, &flags, &long_name))) return result;
    kmemcpy(old, name11, 11);
    old[12] = flags;
    uint32_t slot, lfn_slots;
    /* The new entry first: a crash in between leaves two names, not none. */
    if ((result = write_entries(v, first, u, n, long_name, old, &slot, &lfn_slots))) return result;
    if ((result = erase_entries(v, node->parent, node->slot, node->lfn_slots))) {
        /* The old short entry is erased last, so it still stands: take the
         * new name back, or the two names would share one chain. If that
         * fails too, stop changing the volume until it is checked. */
        if (erase_entries(v, first, slot, lfn_slots)) v->read_only = 1;
        return result;
    }
    if (is_directory && first != node->parent) {
        /* Moves trust "..", so a stale one could let a later move put a
         * folder inside itself. If it cannot be fixed, stop changing the
         * volume until it is checked. */
        result = slot_entry(v, node->cluster, 1, &e, 0);
        if (!result && e[0] == '.' && e[1] == '.') {
            set_entry_cluster(v, e, is_root_first(v, first) ? 0 : first);
            result = dir_store(v);
        }
        if (result) {
            v->read_only = 1;
            return result < 0 ? result : FAT_ERR_CORRUPT;
        }
    }
    node->parent = first;
    node->slot = slot;
    node->lfn_slots = lfn_slots;
    if (long_name) utf16_text(u, n, node->name);
    else short_text(old, node->name);
    return FAT_OK;
}

int fat_set_modified(FatVolume *v, FatNode *node, uint32_t modified) {
    uint8_t *e;
    int result = node_entry(v, node, &e);
    if (!result) result = begin_change(v);
    if (result) return result;
    uint32_t date, time;
    dos_time(modified, &date, &time);
    node->modified = from_dos(date, time);
    return update_entry(v, node);
}

/* ---------- file data ---------- */

static uint32_t clusters_for(const FatVolume *v, uint32_t size) {
    return size / v->cluster_bytes + (size % v->cluster_bytes != 0);
}

/* Cluster number index of a file's chain; 0 when the chain is shorter. */
static int cluster_at(FatVolume *v, FatNode *f, uint32_t index, uint32_t *out) {
    uint32_t at = 0, cluster = f->cluster;
    if (f->at_cluster && f->at_index <= index) { at = f->at_index; cluster = f->at_cluster; }
    if (!cluster) { *out = 0; return FAT_OK; }
    if (!valid_cluster(v, cluster)) return FAT_ERR_CORRUPT;
    while (at < index) {
        uint32_t next;
        int result = chain_next(v, cluster, &next);
        if (result) return result;
        if (!next) { *out = 0; return FAT_OK; }
        cluster = next;
        ++at;
    }
    f->at_index = at;
    f->at_cluster = cluster;
    *out = cluster;
    return FAT_OK;
}

/* Keeps the first keep clusters of a file's chain and frees the rest. */
static int cut_chain(FatVolume *v, FatNode *f, uint32_t keep) {
    if (f->at_index >= keep) f->at_cluster = 0;
    if (!keep) {
        uint32_t old = f->cluster;
        f->cluster = 0;
        return old ? free_chain(v, old) : FAT_OK;
    }
    uint32_t last, rest;
    int result = cluster_at(v, f, keep - 1, &last);
    if (result || !last) return result;
    if ((result = chain_next(v, last, &rest)) || !rest) return result;
    if ((result = entry_set(v, last, end_mark(v)))) return result;
    return free_chain(v, rest);
}

/* Makes a file's chain at least count clusters long. */
static int ensure_clusters(FatVolume *v, FatNode *f, uint32_t count) {
    if (!count) return FAT_OK;
    int result;
    if (!f->cluster) {
        if ((result = take_cluster(v, &f->cluster))) return result;
        f->at_index = 0;
        f->at_cluster = f->cluster;
    }
    uint32_t at = 0, cluster = f->cluster;
    if (f->at_cluster && f->at_index < count) { at = f->at_index; cluster = f->at_cluster; }
    if (!valid_cluster(v, cluster)) return FAT_ERR_CORRUPT;
    while (at + 1 < count) {
        uint32_t next;
        if ((result = chain_next(v, cluster, &next))) return result;
        if (!next) {
            if ((result = take_cluster(v, &next))) return result;
            if ((result = entry_set(v, cluster, next))) return result;
        }
        cluster = next;
        ++at;
    }
    f->at_index = at;
    f->at_cluster = cluster;
    return FAT_OK;
}

/* Reads or writes bytes [position, position + length) of a file's clusters,
 * which must exist. A write with data 0 writes zeros. */
static int transfer(FatVolume *v, FatNode *f, uint32_t position, uint8_t *in, const uint8_t *data,
                    uint32_t length, int writing) {
    while (length) {
        uint32_t index = position / v->cluster_bytes, within = position % v->cluster_bytes, cluster;
        int result = cluster_at(v, f, index, &cluster);
        if (result) return result;
        if (!cluster) return FAT_ERR_CORRUPT;
        uint32_t lba = cluster_lba(v, cluster) + within / 512u, offset = position % 512u, piece;
        if (offset || length < 512u) {
            piece = 512u - offset;
            if (piece > length) piece = length;
            if ((result = dev_read(v, lba, v->sector, 1))) return result;
            if (!writing) {
                kmemcpy(in, v->sector + offset, (int)piece);
            } else {
                if (data) kmemcpy(v->sector + offset, data, (int)piece);
                else kmemset(v->sector + offset, 0, (int)piece);
                if ((result = dev_write(v, lba, v->sector, 1))) return result;
            }
        } else {
            uint32_t sectors = length / 512u, room = (v->cluster_bytes - within) / 512u;
            if (sectors > room) sectors = room;
            piece = sectors * 512u;
            if (!writing) result = dev_read(v, lba, in, sectors);
            else if (data) result = dev_write(v, lba, data, sectors);
            else for (uint32_t i = 0; i < sectors && !result; ++i) result = dev_write(v, lba + i, zero_sector, 1);
            if (result) return result;
        }
        position += piece;
        length -= piece;
        if (in) in += piece;
        if (data) data += piece;
    }
    return FAT_OK;
}

int fat_read(FatVolume *v, FatNode *f, uint32_t offset, void *buffer, uint32_t length) {
    if (f->attributes & FAT_ATTR_DIRECTORY) return FAT_ERR_ISDIR;
    if (offset >= f->size) return 0;
    if (length > f->size - offset) length = f->size - offset;
    if (length > 0x7FFFFFFFu) length = 0x7FFFFFFFu;
    int result = transfer(v, f, offset, buffer, 0, length, 0);
    return result ? result : (int)length;
}

/* Grows a file to size, zero filling from its end, and writes data at
 * offset; the entry is written last. On failure the chain goes back to
 * what the entry describes. */
static int grow_and_write(FatVolume *v, FatNode *f, uint32_t size, uint32_t offset, const void *data,
                          uint32_t length) {
    uint32_t old_cluster = f->cluster, old_size = f->size;
    uint32_t need = clusters_for(v, size), have = clusters_for(v, old_size);
    if (need > have && v->free_count != NONE && need - have > v->free_count) return FAT_ERR_NOSPACE;
    int result = ensure_clusters(v, f, need);
    if (!result) result = window_flush(v);
    if (!result && offset > old_size) result = transfer(v, f, old_size, 0, 0, offset - old_size, 1);
    if (!result && length) result = transfer(v, f, offset, 0, data, length, 1);
    if (!result) {
        if (size > f->size) f->size = size;
        f->modified = now_of(v);
        uint32_t date, time;
        dos_time(f->modified, &date, &time);
        f->modified = from_dos(date, time);
        f->attributes |= FAT_ATTR_ARCHIVE;
        result = update_entry(v, f);
    }
    if (result) {
        /* The entry still names the old chain and size. */
        if (!old_cluster) cut_chain(v, f, 0);
        else cut_chain(v, f, have ? have : 1);
        f->cluster = old_cluster;
        f->size = old_size;
        f->at_cluster = 0;
    }
    int flushed = window_flush(v);
    return result ? result : flushed;
}

int fat_write(FatVolume *v, FatNode *f, uint32_t offset, const void *data, uint32_t length) {
    if (f->attributes & FAT_ATTR_DIRECTORY) return FAT_ERR_ISDIR;
    if (v->read_only) return FAT_ERR_READONLY;
    if (!length) return FAT_OK;
    if ((uint64_t)offset + length > FAT_FILE_MAX) return FAT_ERR_FBIG;
    uint8_t *e;
    int result = node_entry(v, f, &e);
    if (!result) result = begin_change(v);
    if (result) return result;
    uint32_t end = offset + length;
    return grow_and_write(v, f, end > f->size ? end : f->size, offset, data, length);
}

int fat_truncate(FatVolume *v, FatNode *f, uint32_t size) {
    if (f->attributes & FAT_ATTR_DIRECTORY) return FAT_ERR_ISDIR;
    if (v->read_only) return FAT_ERR_READONLY;
    uint8_t *e;
    int result = node_entry(v, f, &e);
    if (result || size == f->size) return result;
    if ((result = begin_change(v))) return result;
    if (size > f->size) return grow_and_write(v, f, size, size, 0, 0);
    /* The entry stops naming the clusters before they are freed. */
    uint32_t keep = clusters_for(v, size), old_cluster = f->cluster;
    FatNode shorter = *f;
    shorter.size = size;
    shorter.modified = now_of(v);
    uint32_t date, time;
    dos_time(shorter.modified, &date, &time);
    shorter.modified = from_dos(date, time);
    shorter.attributes |= FAT_ATTR_ARCHIVE;
    if (!keep) shorter.cluster = 0;
    if ((result = update_entry(v, &shorter))) return result;
    *f = shorter;
    f->cluster = old_cluster;
    result = cut_chain(v, f, keep);
    int flushed = window_flush(v);
    return result ? result : flushed;
}
