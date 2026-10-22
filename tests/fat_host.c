/* FAT core on a disk image in memory, driven by tests/test_fat.py:
 *   fat_host info IMAGE
 *   fat_host ls IMAGE PATH            one "d|f size modified name" line per entry
 *   fat_host cat IMAGE PATH           the file's bytes on stdout
 *   fat_host script IMAGE [BUDGET]    tab-separated operations from stdin
 *   fat_host self                     unit checks on a RAM disk
 * Script operations (paths from the root, '/'-separated):
 *   put PATH SEED SIZE OFFSET   write SIZE pattern bytes at OFFSET, creating the file
 *   trunc PATH SIZE | mkdir PATH | rm PATH | mv FROM TO | sync
 * BUDGET limits sector writes, as if power failed after that many; the image
 * is saved as the disk holds it then. A failed operation prints its error and
 * the script goes on. */
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

#include "../src/kernel/fat.c"

typedef struct {
    uint8_t *data;
    uint32_t sectors;
    long budget;            /* sector writes left; <0 unlimited */
    long writes;
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
        ++d->writes;
        memcpy(d->data + (size_t)(lba + i) * 512, (const uint8_t *)buffer + (size_t)i * 512, 512);
    }
    return 0;
}
static int disk_flush(void *context) { return ((Disk *)context)->cut ? -1 : 0; }
static FatDevice device_of(Disk *d) { return (FatDevice){disk_read, disk_write, disk_flush, d, d->sectors}; }

static uint32_t clock_value = 24u * 365u * 86400u + 12345u;
static uint32_t test_clock(void) { return clock_value; }

static uint8_t pattern(uint32_t seed, uint32_t i) { return (uint8_t)(seed * 31u + i * 7u + (i >> 8)); }

static Disk load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    Disk d = {malloc((size_t)size), (uint32_t)(size / 512), -1, 0, 0};
    if (fread(d.data, 1, (size_t)size, f) != (size_t)size) exit(2);
    fclose(f);
    return d;
}
static void save(const char *path, const Disk *d) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(d->data, 512, d->sectors, f) != d->sectors) exit(2);
    fclose(f);
}

static void mount(FatVolume *v, Disk *d) {
    FatDevice device = device_of(d);
    uint32_t start, sectors;
    int result = fat_find(&device, &start, &sectors);
    if (!result) result = fat_mount(v, &device, start, sectors);
    if (result) { fprintf(stderr, "mount: %d\n", result); exit(3); }
    v->now = test_clock;
}

/* Parent node and final name of a path. */
static int split(FatVolume *v, const char *path, FatNode *parent, const char **name) {
    static char copy[4096];
    snprintf(copy, sizeof copy, "%s", path);
    char *slash = strrchr(copy, '/');
    *name = path + (slash ? slash - copy + 1 : 0);
    if (slash) *slash = 0;
    return fat_resolve(v, slash ? copy : "", parent);
}

static int put(FatVolume *v, const char *path, uint32_t seed, uint32_t size, uint32_t offset) {
    FatNode node, parent;
    const char *name;
    int result = fat_resolve(v, path, &node);
    if (result == FAT_ERR_NOTFOUND) {
        if ((result = split(v, path, &parent, &name))) return result;
        result = fat_create(v, &parent, name, 0, &node);
    }
    if (result) return result;
    uint8_t *data = malloc(size ? size : 1);
    for (uint32_t i = 0; i < size; ++i) data[i] = pattern(seed, i);
    /* Pieces of odd sizes exercise partial sectors and the position cache. */
    uint32_t done = 0;
    while (!result && done < size) {
        uint32_t piece = size - done < 70001u ? size - done : 70001u;
        result = fat_write(v, &node, offset + done, data + done, piece);
        done += piece;
    }
    free(data);
    return result;
}

static int run_script(FatVolume *v) {
    char line[8192];
    int failures = 0;
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        char *field[6] = {0};
        int count = 0;
        for (char *p = line; p && count < 6; ++count) {
            field[count] = p;
            p = strchr(p, '\t');
            if (p) *p++ = 0;
        }
        if (!count || !field[0][0]) continue;
        int result = 0;
        FatNode node, parent;
        const char *name;
        if (!strcmp(field[0], "put")) {
            result = put(v, field[1], (uint32_t)strtoul(field[2], 0, 0), (uint32_t)strtoul(field[3], 0, 0),
                         (uint32_t)strtoul(field[4], 0, 0));
        } else if (!strcmp(field[0], "trunc")) {
            result = fat_resolve(v, field[1], &node);
            if (!result) result = fat_truncate(v, &node, (uint32_t)strtoul(field[2], 0, 0));
        } else if (!strcmp(field[0], "mkdir")) {
            result = split(v, field[1], &parent, &name);
            if (!result) result = fat_create(v, &parent, name, 1, &node);
        } else if (!strcmp(field[0], "rm")) {
            result = fat_resolve(v, field[1], &node);
            if (!result) result = fat_remove(v, &node);
        } else if (!strcmp(field[0], "mv")) {
            result = fat_resolve(v, field[1], &node);
            if (!result) result = split(v, field[2], &parent, &name);
            if (!result) result = fat_rename(v, &node, &parent, name);
        } else if (!strcmp(field[0], "sync")) {
            result = fat_sync(v);
        } else {
            fprintf(stderr, "unknown operation %s\n", field[0]);
            exit(2);
        }
        clock_value += 7;
        if (result) { printf("error %d %s\n", result, line); ++failures; }
    }
    return failures;
}

static void list(FatVolume *v, const char *path) {
    FatNode dir, node;
    int result = fat_resolve(v, path, &dir);
    uint32_t cursor = 0;
    if (result) { fprintf(stderr, "resolve: %d\n", result); exit(4); }
    while ((result = fat_next(v, &dir, &cursor, &node)) == 1)
        printf("%c %u %u %s\n", node.attributes & FAT_ATTR_DIRECTORY ? 'd' : 'f', node.size, node.modified, node.name);
    if (result) { fprintf(stderr, "next: %d\n", result); exit(4); }
}

static void cat(FatVolume *v, const char *path) {
    FatNode node;
    int result = fat_resolve(v, path, &node);
    if (result) { fprintf(stderr, "resolve: %d\n", result); exit(4); }
    static uint8_t buffer[65536 + 3];
    for (uint32_t at = 0;;) {
        int got = fat_read(v, &node, at, buffer, sizeof buffer);
        if (got < 0) { fprintf(stderr, "read: %d\n", got); exit(4); }
        if (!got) break;
        fwrite(buffer, 1, (size_t)got, stdout);
        at += (uint32_t)got;
    }
}

/* ---------- unit checks ---------- */

static void self_names(void) {
    uint16_t u[FAT_NAME_MAX];
    unsigned n;
    uint8_t name11[11], flags;
    assert(!name_units("readme.txt", u, &n) && n == 10);
    assert(plain_short(u, n, name11, &flags) && !memcmp(name11, "README  TXT", 11) && flags == (CASE_BASE | CASE_EXT));
    assert(!name_units("Readme.txt", u, &n) && plain_short(u, n, name11, &flags) == 2);
    assert(!memcmp(name11, "README  TXT", 11));
    assert(!name_units("A.B.C", u, &n) && !plain_short(u, n, name11, &flags));
    assert(short_basis(u, n, name11) == 2 && !memcmp(name11, "AB      C  ", 11));
    assert(!name_units(".bashrc", u, &n) && short_basis(u, n, name11) == 6 && !memcmp(name11, "BASHRC     ", 11));
    assert(!name_units("long file name.html", u, &n) && short_basis(u, n, name11) == 8);
    short_tail(name11, 8, 1, name11);
    assert(!memcmp(name11, "LONGFI~1HTM", 11));
    assert(name_units("a/b", u, &n) == FAT_ERR_INVALID && name_units("x.", u, &n) == FAT_ERR_INVALID);
    assert(name_units("", u, &n) == FAT_ERR_INVALID && name_units("\xff", u, &n) == FAT_ERR_INVALID);
    char big[300];
    memset(big, 'a', 256); big[256] = 0;
    assert(name_units(big, u, &n) == FAT_ERR_NAMETOOLONG);
    big[255] = 0;
    assert(!name_units(big, u, &n) && n == 255);
    assert(!name_units("\xc3\xa9t\xc3\xa9 \xf0\x9f\x98\x80", u, &n) && n == 6);
    char text[FAT_NAME_BYTES];
    utf16_text(u, n, text);
    assert(!strcmp(text, "\xc3\xa9t\xc3\xa9 \xf0\x9f\x98\x80"));
    /* Times round trip in 2-second steps; before 2000 reads as unknown. */
    uint32_t date, time;
    dos_time(clock_value, &date, &time);
    assert(from_dos(date, time) == clock_value - clock_value % 2);
    dos_time(0, &date, &time);
    assert(from_dos(date, time) == 0);
    dos_time(86400u * 59u, &date, &time);
    assert(date == ((20u << 9) | (2u << 5) | 29u));
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "self")) {
        self_names();
        assert(!live);
        puts("ok");
        return 0;
    }
    if (argc < 3) return 2;
    Disk disk = load(argv[2]);
    FatVolume v;
    mount(&v, &disk);
    int status = 0;
    if (!strcmp(argv[1], "info")) {
        printf("bits %d clusters %u free %u cluster_bytes %u label '%s' dirty %d\n", v.bits, v.clusters,
               fat_free_clusters(&v), v.cluster_bytes, v.label, v.was_dirty);
    } else if (!strcmp(argv[1], "ls") && argc == 4) {
        list(&v, argv[3]);
    } else if (!strcmp(argv[1], "cat") && argc == 4) {
        cat(&v, argv[3]);
    } else if (!strcmp(argv[1], "script")) {
        if (argc == 4) disk.budget = strtol(argv[3], 0, 0);
        status = run_script(&v) ? 1 : 0;
        if (fat_unmount(&v) && !disk.cut) { fprintf(stderr, "unmount failed\n"); status = 5; }
        save(argv[2], &disk);
        fprintf(stderr, "writes %ld\n", disk.writes);
        free(disk.data);
        assert(!live);
        return disk.cut ? 0 : status;
    } else {
        return 2;
    }
    fat_unmount(&v);
    free(disk.data);
    assert(!live);
    return status;
}
