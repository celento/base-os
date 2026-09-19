#include "fs.h"
#include "persist.h"

#include "platform.h"
#define FS_SECTOR_SIZE SECTOR_SIZE

#define FS_DISK_MAGIC   0x46534F42u   /* "BOSF" */
#define FS_DISK_VERSION 3

typedef struct {
    char name[FS_NAME_LEN];
    int parent;
    int is_dir;
    int is_app;
    int size;
    int used;
    unsigned modified;
    char data[FS_MAX_SIZE];
} FsNode;

/* One serialized node: fixed header, then `size` bytes of data. */
typedef struct {
    unsigned short id;
    short parent;
    unsigned char is_dir;
    unsigned char is_app;
    unsigned short pad;
    unsigned int size;
    char name[FS_NAME_LEN];
    unsigned modified;
} DiskNode;

typedef struct {
    unsigned int magic;
    unsigned int version;
    unsigned int count;
    unsigned int bytes;   /* payload length after the header sector */
    unsigned int sum;
    unsigned int generation;
    unsigned int header_sum;
} DiskHeader;

static FsNode *nodes;
static int fs_touched = 0;
static unsigned identities[FS_MAX_NODES], next_identity;
unsigned fs_identity(int id){return fs_valid(id)?identities[id]:0;}
static int writable;
static int active_slot = -1;
static unsigned generation;
static unsigned sync_failures;
static unsigned last_sync_attempt;
static int save_failed;

_Static_assert(sizeof(FsNode) * FS_MAX_NODES <= FS_CAPACITY, "FS arena overflow");
_Static_assert(sizeof(DiskHeader) == 28 && sizeof(DiskNode) == 40, "disk ABI changed");
_Static_assert(FS_SECTOR_SIZE + FS_MAX_NODES * (sizeof(DiskNode) + FS_MAX_SIZE - 1)
               <= FS_DISK_SECTORS * FS_SECTOR_SIZE, "snapshot capacity too small");
_Static_assert(FS_DISK_SECTORS * FS_SECTOR_SIZE <= FS_IMG_CAPACITY, "staging overflow");
_Static_assert(FS_SECOND_LBA >= FS_DISK_LBA + FS_DISK_SECTORS &&
               FS_SECOND_LBA + FS_DISK_SECTORS <= DISK_SECTORS, "disk layout overlap");

__attribute__((weak)) unsigned fs_clock(void) { return 0; }
unsigned fs_modified(int id) { return fs_valid(id) ? nodes[id].modified : 0; }
unsigned fs_capacity(void) { return (FS_MAX_NODES - 1) * (FS_MAX_SIZE - 1); }
unsigned fs_used_bytes(void) {
    unsigned n = 0;
    for (int i = 0; i < FS_MAX_NODES; ++i) if (nodes[i].used) n += nodes[i].size;
    return n;
}

int kstrlen(const char *s) {
    int n = 0;
    while (s[n])
        n++;
    return n;
}

int kstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

void kstrcpy(char *d, const char *s) {
    while (*s)
        *d++ = *s++;
    *d = 0;
}

void kmemcpy(void *d, const void *s, int n) {
    if(n<=0)return;
    unsigned char *dd=d;const unsigned char *ss=s;
#ifdef __i386__
    unsigned words=(unsigned)n/4;
    __asm__ volatile("rep movsl" : "+D"(dd), "+S"(ss), "+c"(words) :: "memory");
    n&=3;
#endif
    for(int i=0;i<n;i++)dd[i]=ss[i];
}
void kmemset(void *d, int v, int n) {
    if(n<=0)return;
    unsigned char *dd=d;
#ifdef __i386__
    unsigned words=(unsigned)n/4,pattern=(unsigned char)v*0x01010101u;
    __asm__ volatile("rep stosl" : "+D"(dd), "+c"(words) : "a"(pattern) : "memory");
    n&=3;
#endif
    for(int i=0;i<n;i++)dd[i]=(unsigned char)v;
}

static int valid_name(const char *name) {
    if (!name || !*name || !kstrcmp(name, ".") || !kstrcmp(name, "..")) return 0;
    int n = 0;
    while (n < FS_NAME_LEN && name[n]) {
        if (name[n] == '/') return 0;
        n++;
    }
    return n < FS_NAME_LEN;
}

static int alloc_node(int parent, const char *name, int is_dir, int is_app) {
    if (!valid_name(name)) return -1;
    if (parent < 0 || parent >= FS_MAX_NODES || !nodes[parent].used)
        return -1;
    if (!nodes[parent].is_dir)
        return -1;
    if (fs_find_child(parent, name) >= 0)
        return -1;
    if (kstrlen(name) <= 0 || kstrlen(name) >= FS_NAME_LEN)
        return -1;

    for (int i = 0; i < FS_MAX_NODES; i++) {
        if (!nodes[i].used) {
            kmemset(&nodes[i], 0, (int)sizeof(FsNode));
            kstrcpy(nodes[i].name, name);
            nodes[i].parent = parent;
            nodes[i].is_dir = is_dir;
            nodes[i].is_app = is_app;
            nodes[i].size = 0;
            nodes[i].used = 1;
            identities[i]=++next_identity;
            nodes[i].modified = fs_clock();
            fs_touched = 1;
            return i;
        }
    }
    return -1;
}

void fs_init(void) {
    writable = 0;
    active_slot = -1;
    generation = sync_failures = last_sync_attempt = 0;
    save_failed = 0;
    nodes = (FsNode *)FS_BASE;
    kmemset(nodes, 0, (int)sizeof(FsNode) * FS_MAX_NODES);

    nodes[0].used = 1;
    nodes[0].is_dir = 1;
    nodes[0].parent = -1;
    nodes[0].name[0] = 0;

    int docs = fs_mkdir(0, "docs");
    fs_mkdir(0, "trash");
    fs_mkdir(0, "Documents");
    fs_mkdir(0, "Pictures");
    fs_mkdir(0, "prefs");

    const char *readme =
        "Welcome to BaseOS!\n"
        "Double-click icons or\n"
        "use the File menu.\n"
        "Save stores bytes in RAM.";
    int r = fs_create(0, "readme.txt");
    fs_write(r, readme, kstrlen(readme));

    /* Hello lives in the disk window, not the system menu. */
    int hello = fs_create(0, "Hello");
    const char *hw = "HELLO WORLD";
    fs_write(hello, hw, kstrlen(hw));

    const char *hello_txt = "Hello from /docs/hello.txt\nThis file lives in RAM.";
    int h = fs_create(docs, "hello.txt");
    fs_write(h, hello_txt, kstrlen(hello_txt));

    const char *tips = "Use the mouse to click.\nArrows and Enter still work.";
    int t = fs_create(docs, "tips.txt");
    fs_write(t, tips, kstrlen(tips));

    /* App nodes in the volume — opened by name, not the editor. */
    fs_create_app(0, "Calculator");
    fs_create_app(0, "Paint");
    fs_create_app(0, "Image Viewer");
    fs_create_app(0, "Snake");
    fs_create_app(0, "Wordle");
    fs_create_app(0, "Terminal");
    fs_create_app(0, "Todo");
    fs_create_app(0, "Clock");
    fs_create_app(0, "Calendar");
    fs_create_app(0, "Minesweeper");
    fs_create_app(0, "2048");
    fs_create_app(0, "Breakout");
    fs_create_app(0, "System Monitor");
}

int fs_root(void) { return 0; }

int fs_valid(int id) {
    return id >= 0 && id < FS_MAX_NODES && nodes[id].used;
}

int fs_is_dir(int id) { return fs_valid(id) && nodes[id].is_dir; }
int fs_is_app(int id) { return fs_valid(id) && nodes[id].is_app; }

int fs_node_count(void) {
    int n = 0;
    for (int i = 0; i < FS_MAX_NODES; i++)
        if (fs_valid(i))
            n++;
    return n;
}
int fs_parent(int id) { return fs_valid(id) ? nodes[id].parent : -1; }
int fs_size(int id) { return fs_valid(id) ? nodes[id].size : 0; }
const char *fs_name(int id) { return fs_valid(id) ? nodes[id].name : ""; }
const char *fs_data(int id) { return fs_valid(id) ? nodes[id].data : ""; }

int fs_find_child(int parent, const char *name) {
    for (int i = 0; i < FS_MAX_NODES; i++) {
        if (nodes[i].used && nodes[i].parent == parent &&
            kstrcmp(nodes[i].name, name) == 0)
            return i;
    }
    return -1;
}

int fs_mkdir(int parent, const char *name) {
    return alloc_node(parent, name, 1, 0);
}

int fs_create(int parent, const char *name) {
    return alloc_node(parent, name, 0, 0);
}

int fs_create_app(int parent, const char *name) {
    return alloc_node(parent, name, 0, 1);
}

int fs_write(int id, const char *data, int len) {
    if (!fs_valid(id) || nodes[id].is_dir || nodes[id].is_app)
        return -1;
    if (len < 0)
        len = 0;
    if (len > FS_MAX_SIZE - 1)
        len = FS_MAX_SIZE - 1;
    kmemcpy(nodes[id].data, data, len);
    nodes[id].data[len] = 0;
    nodes[id].size = len;
    nodes[id].modified = fs_clock();
    fs_touched = 1;
    return len;
}

int fs_read(int id, char *out, int max) {
    if (!fs_valid(id) || nodes[id].is_dir || nodes[id].is_app)
        return -1;
    int n = nodes[id].size;
    if (n > max - 1)
        n = max - 1;
    kmemcpy(out, nodes[id].data, n);
    out[n] = 0;
    return n;
}

int fs_list(int parent, int *ids, int max) {
    int n = 0;
    if (!fs_is_dir(parent))
        return 0;
    /* Directories first, then files, in node order. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < FS_MAX_NODES; i++) {
            if (!nodes[i].used || nodes[i].parent != parent)
                continue;
            if (pass == 0 && !nodes[i].is_dir)
                continue;
            if (pass == 1 && nodes[i].is_dir)
                continue;
            /* Desktop Trash is the trash; /prefs is Settings state.
             * Do not list either on the volume. */
            if (parent == 0 && (kstrcmp(nodes[i].name, "trash") == 0 ||
                                kstrcmp(nodes[i].name, "prefs") == 0))
                continue;
            if (n < max)
                ids[n++] = i;
        }
    }
    return n;
}

int fs_list_files(int *ids, int max) {
    int n = 0;
    for (int i = 0; i < FS_MAX_NODES; i++) {
        if (nodes[i].used && !nodes[i].is_dir) {
            if (n < max)
                ids[n++] = i;
        }
    }
    return n;
}

void fs_path(int id, char *out, int max) {
    if (!fs_valid(id) || max <= 1) {
        if (max > 0)
            out[0] = 0;
        return;
    }
    if (id == 0) {
        out[0] = '/';
        out[1] = 0;
        return;
    }

    int parts[FS_MAX_NODES];
    int n = 0;
    int cur = id;
    while (cur > 0 && n < FS_MAX_NODES) {
        parts[n++] = cur;
        cur = nodes[cur].parent;
    }

    int pos = 0;
    out[pos++] = '/';
    for (int i = n - 1; i >= 0; i--) {
        const char *nm = nodes[parts[i]].name;
        int j = 0;
        while (nm[j] && pos < max - 1)
            out[pos++] = nm[j++];
        if (i > 0 && pos < max - 1)
            out[pos++] = '/';
    }
    out[pos] = 0;
}

int fs_unique_file(int parent, char *out) {
    /* new1.txt, new2.txt, ... */
    for (int n = 1; n < 100; n++) {
        char name[FS_NAME_LEN];
        int pos = 0;
        name[pos++] = 'n';
        name[pos++] = 'e';
        name[pos++] = 'w';
        int x = n;
        char digs[4];
        int nd = 0;
        do {
            digs[nd++] = (char)('0' + (x % 10));
            x /= 10;
        } while (x && nd < 4);
        while (nd--)
            name[pos++] = digs[nd];
        name[pos++] = '.';
        name[pos++] = 't';
        name[pos++] = 'x';
        name[pos++] = 't';
        name[pos] = 0;
        if (fs_find_child(parent, name) < 0) {
            kstrcpy(out, name);
            return 0;
        }
    }
    return -1;
}

static void numbered_name(char *out, const char *base, int n) {
    int i = 0;
    while (base[i] && i < FS_NAME_LEN - 1) {
        out[i] = base[i];
        i++;
    }
    if (n <= 1) {
        out[i] = 0;
        return;
    }
    if (i > FS_NAME_LEN - 4)
        i = FS_NAME_LEN - 4;
    out[i++] = ' ';
    char digs[4];
    int nd = 0;
    int x = n;
    do {
        digs[nd++] = (char)('0' + (x % 10));
        x /= 10;
    } while (x && nd < 4);
    while (nd-- && i < FS_NAME_LEN - 1)
        out[i++] = digs[nd];
    out[i] = 0;
}

int fs_unique_dir(int parent, char *out) {
    const char *base = "untitled folder";
    for (int n = 1; n < 100; n++) {
        char name[FS_NAME_LEN];
        numbered_name(name, base, n);
        if (fs_find_child(parent, name) < 0) {
            kstrcpy(out, name);
            return 0;
        }
    }
    return -1;
}

int fs_child_count(int parent) {
    int n = 0;
    if (!fs_is_dir(parent))
        return 0;
    for (int i = 0; i < FS_MAX_NODES; i++) {
        if (nodes[i].used && nodes[i].parent == parent)
            n++;
    }
    return n;
}

int fs_delete(int id) {
    if (!fs_valid(id) || id == 0)
        return -1;
    if (nodes[id].is_dir) {
        for (int i = 0; i < FS_MAX_NODES; i++) {
            if (nodes[i].used && nodes[i].parent == id)
                fs_delete(i);
        }
    }
    nodes[id].used = 0;
    fs_touched = 1;
    return 0;
}

int fs_move(int id, int new_parent) {
    if (!fs_valid(id) || id == 0)
        return -1;
    if (!fs_is_dir(new_parent))
        return -1;
    if (id == new_parent)
        return -1;
    int walk = new_parent;
    while (walk >= 0) {
        if (walk == id)
            return -1;
        walk = nodes[walk].parent;
    }
    if (nodes[id].parent == new_parent)
        return 0;
    int clash = fs_find_child(new_parent, nodes[id].name);
    if (clash >= 0 && clash != id) {
        char name[FS_NAME_LEN];
        int found = 0;
        for (int n = 2; n < 100; n++) {
            numbered_name(name, nodes[id].name, n);
            if (fs_find_child(new_parent, name) < 0) {
                kstrcpy(nodes[id].name, name);
                found = 1;
                break;
            }
        }
        if (!found)
            return -1;
    }
    nodes[id].parent = new_parent;
    nodes[id].modified = fs_clock();
    fs_touched = 1;
    return 0;
}

void fs_empty_dir(int parent) {
    if (!fs_is_dir(parent))
        return;
    for (int i = 0; i < FS_MAX_NODES; i++) {
        if (nodes[i].used && nodes[i].parent == parent)
            fs_delete(i);
    }
}

static void make_copy_name(char *out, const char *base, int n) {
    /* n==1: "Hello copy"; n>=2: "Hello copy 2". Fit in FS_NAME_LEN. */
    char suffix[16];
    int sl = 0;
    suffix[sl++] = ' ';
    suffix[sl++] = 'c';
    suffix[sl++] = 'o';
    suffix[sl++] = 'p';
    suffix[sl++] = 'y';
    if (n >= 2) {
        suffix[sl++] = ' ';
        char digs[4];
        int nd = 0;
        int x = n;
        do {
            digs[nd++] = (char)('0' + (x % 10));
            x /= 10;
        } while (x && nd < 4);
        while (nd--)
            suffix[sl++] = digs[nd];
    }
    suffix[sl] = 0;

    int max_base = FS_NAME_LEN - 1 - sl;
    if (max_base < 1)
        max_base = 1;
    int i = 0;
    while (base[i] && i < max_base) {
        out[i] = base[i];
        i++;
    }
    int j = 0;
    while (suffix[j] && i < FS_NAME_LEN - 1)
        out[i++] = suffix[j++];
    out[i] = 0;
}

static int copy_into(int id, int parent, const char *name) {
    if (!fs_valid(id) || !fs_is_dir(parent))
        return -1;
    int dst;
    if (nodes[id].is_dir)
        dst = alloc_node(parent, name, 1, 0);
    else if (nodes[id].is_app)
        dst = alloc_node(parent, name, 0, 1);
    else
        dst = alloc_node(parent, name, 0, 0);
    if (dst < 0)
        return -1;
    if (nodes[id].is_dir) {
        for (int i = 0; i < FS_MAX_NODES; i++) {
            if (nodes[i].used && nodes[i].parent == id &&
                copy_into(i, dst, nodes[i].name) < 0) {
                fs_delete(dst);
                return -1;
            }
        }
    } else if (!nodes[id].is_app && nodes[id].size > 0) {
        fs_write(dst, nodes[id].data, nodes[id].size);
    }
    return dst;
}

int fs_unique_copy(int parent, const char *src, char *out) {
    if (!src || !out)
        return -1;
    for (int n = 1; n < 100; n++) {
        make_copy_name(out, src, n);
        if (fs_find_child(parent, out) < 0)
            return 0;
    }
    return -1;
}

int fs_copy(int id, int parent) {
    if (!fs_valid(id) || id == 0)
        return -1;
    if (!fs_is_dir(parent))
        return -1;
    int walk = parent;
    while (walk >= 0) {
        if (walk == id)
            return -1;
        walk = nodes[walk].parent;
    }
    char name[FS_NAME_LEN];
    if (fs_unique_copy(parent, nodes[id].name, name) < 0)
        return -1;
    int touched_before = fs_touched;
    int result = copy_into(id, parent, name);
    if (result < 0) fs_touched = touched_before;
    return result;
}

int fs_rename(int id, const char *name) {
    if (!fs_valid(id) || id == 0 || !valid_name(name))
        return -1;
    int len = kstrlen(name);
    if (len <= 0 || len >= FS_NAME_LEN)
        return -1;
    int clash = fs_find_child(nodes[id].parent, name);
    if (clash >= 0 && clash != id)
        return -1;
    kstrcpy(nodes[id].name, name);
    nodes[id].modified = fs_clock();
    fs_touched = 1;
    return 0;
}

/* ---------- Disk image ---------- */

static unsigned int checksum(const unsigned char *p, unsigned int n) {
    unsigned int s = n;
    for (unsigned int i = 0; i < n; i++)
        s = (s << 1) + (s >> 31) + p[i];
    return s;
}

/* CRC protects both metadata and payload; v1's rolling checksum is read only
 * for migration. A header is the commit record and is always written last. */
static unsigned crc32(const void *data, unsigned n) {
    const unsigned char *p = data;
    unsigned crc = ~0u;
    for (unsigned i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static unsigned slot_lba(int slot) { return slot ? FS_SECOND_LBA : FS_DISK_LBA; }

static unsigned disk_node_size(const DiskHeader *h) { return h->version >= 3 ? 40 : 36; }
static void decode_node(DiskNode *d, const unsigned char *p, const DiskHeader *h) {
    kmemset(d, 0, sizeof(*d));
    kmemcpy(d, p, disk_node_size(h));
}

/* Validate the complete graph before changing the live node table. */
static int validate_payload(const DiskHeader *h, const unsigned char *img) {
    unsigned ds = disk_node_size(h);
    unsigned offsets[FS_MAX_NODES] = {0};
    unsigned pos = FS_SECTOR_SIZE, end = pos + h->bytes;
    for (unsigned n = 0; n < h->count; ++n) {
        DiskNode d;
        if (end - pos < ds) return -1;
        decode_node(&d, img + pos, h);
        if (d.id >= FS_MAX_NODES || offsets[d.id] || d.parent < -1 ||
            d.parent >= FS_MAX_NODES || d.is_dir > 1 || d.is_app > 1 ||
            (d.is_dir && d.is_app) || d.size >= FS_MAX_SIZE ||
            d.size > end - pos - ds ||
            ((d.is_dir || d.is_app) && d.size)) return -1;
        int length = 0;
        while (length < FS_NAME_LEN && d.name[length]) {
            if (d.name[length] == '/') return -1;
            length++;
        }
        if (length == FS_NAME_LEN || (d.id && !length)) return -1;
        if (d.id && (kstrcmp(d.name, ".") == 0 || kstrcmp(d.name, "..") == 0))
            return -1;
        if (!d.id && (d.parent != -1 || !d.is_dir || length)) return -1;
        offsets[d.id] = pos;
        pos += ds + d.size;
    }
    if (pos != end || !offsets[0]) return -1;
    for (int id = 1; id < FS_MAX_NODES; ++id) {
        if (!offsets[id]) continue;
        DiskNode node;
        decode_node(&node, img + offsets[id], h);
        int walk = id;
        for (unsigned steps = 0; walk != 0; ++steps) {
            if (steps >= FS_MAX_NODES) return -1;
            DiskNode d, parent;
            decode_node(&d, img + offsets[walk], h);
            walk = d.parent;
            if (walk < 0 || !offsets[walk]) return -1;
            decode_node(&parent, img + offsets[walk], h);
            if (!parent.is_dir) return -1;
        }
        for (int other = 1; other < id; ++other) {
            if (!offsets[other]) continue;
            DiskNode d;
            decode_node(&d, img + offsets[other], h);
            if (d.parent == node.parent && !kstrcmp(d.name, node.name)) return -1;
        }
    }
    return 0;
}

static int all_zero(const unsigned char *p, unsigned n) {
    for (unsigned i = 0; i < n; ++i) if (p[i]) return 0;
    return 1;
}

/* 0 valid, 1 wholly blank, -1 corrupt, -2 I/O error. Never modifies nodes. */
static int read_slot(int slot, DiskHeader *h) {
    unsigned char *img = (unsigned char *)FS_IMG_BASE;
    unsigned lba = slot_lba(slot);
    if (lba + FS_DISK_SECTORS > disk_sector_count()) return -2;
    if (disk_read(lba, img, 1) < 0) return -2;
    if (all_zero(img, FS_SECTOR_SIZE)) {
        if (disk_read(lba + 1, img + FS_SECTOR_SIZE, FS_DISK_SECTORS - 1) < 0)
            return -2;
        return all_zero(img + FS_SECTOR_SIZE, (FS_DISK_SECTORS - 1) * FS_SECTOR_SIZE)
               ? 1 : -1;
    }
    kmemcpy(h, img, sizeof(*h));
    if (h->magic != FS_DISK_MAGIC || !h->count || h->count > FS_MAX_NODES ||
        h->bytes > (FS_DISK_SECTORS - 1) * FS_SECTOR_SIZE ||
        h->bytes < h->count * disk_node_size(h)) return -1;
    if (h->version == 1 && slot == 0) {
        h->generation = 0;
    } else if ((h->version != 2 && h->version != FS_DISK_VERSION) ||
               h->header_sum != crc32(h, 24)) return -1;
    unsigned sectors = (h->bytes + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    if (disk_read(lba + 1, img + FS_SECTOR_SIZE, sectors) < 0) return -2;
    unsigned sum = h->version == 1 ? checksum(img + FS_SECTOR_SIZE, h->bytes)
                                  : crc32(img + FS_SECTOR_SIZE, h->bytes);
    if (sum != h->sum || validate_payload(h, img) < 0) return -1;
    return 0;
}

static void import_payload(const DiskHeader *h) {
    const unsigned char *img = (const unsigned char *)FS_IMG_BASE;
    unsigned pos = FS_SECTOR_SIZE;
    kmemset(nodes, 0, sizeof(FsNode) * FS_MAX_NODES);
    for (unsigned n = 0; n < h->count; ++n) {
        DiskNode d;
        decode_node(&d, img + pos, h);
        pos += disk_node_size(h);
        FsNode *nd = &nodes[d.id];
        kmemcpy(nd->name, d.name, FS_NAME_LEN);
        nd->parent = d.parent;
        nd->is_dir = d.is_dir;
        nd->is_app = d.is_app;
        nd->size = d.size;
        nd->used = 1;
        identities[d.id]=++next_identity;
        nd->modified = d.modified;
        kmemcpy(nd->data, img + pos, d.size);
        pos += d.size;
    }
}

int fs_load_disk(void) {
    DiskHeader h[2];
    int state[2];
    writable = 0;
    active_slot = -1;
    state[0] = read_slot(0, &h[0]);
    state[1] = read_slot(1, &h[1]);
    int first = state[0] == 0 ? 0 : 1;
    if (state[0] == 0 && state[1] == 0 &&
        (int)(h[1].generation - h[0].generation) > 0) first = 1;
    for (int attempt = 0; attempt < 2; ++attempt) {
        int slot = first ^ attempt;
        if (state[slot] != 0) continue;
        DiskHeader current;
        int rc = read_slot(slot, &current);
        if (rc != 0) { state[slot] = rc; continue; }
        import_payload(&current);
        active_slot = slot;
        generation = current.generation;
        fs_touched = sync_failures = save_failed = 0;
        /* An unreadable peer might contain a newer version. Do not overwrite
         * it after a transient mount error. A corrupt peer can be replaced. */
        writable = state[0] != -2 && state[1] != -2;
        return 0;
    }
    if (state[0] == 1 && state[1] == 1) {
        writable = 1;
        return FS_LOAD_BLANK;
    }
    return -1;
}

static int save_snapshot(void) {
    if (!fs_touched) return 0;
    if (!writable) return -1;
    unsigned char *img = (unsigned char *)FS_IMG_BASE;
    unsigned pos = FS_SECTOR_SIZE;
    DiskHeader h = { .magic = FS_DISK_MAGIC, .version = FS_DISK_VERSION,
                     .generation = generation + 1 };
    for (int i = 0; i < FS_MAX_NODES; ++i) {
        if (!nodes[i].used) continue;
        DiskNode d = { .id = i, .parent = nodes[i].parent,
                       .is_dir = nodes[i].is_dir, .is_app = nodes[i].is_app,
                       .size = nodes[i].size, .modified = nodes[i].modified };
        kmemcpy(d.name, nodes[i].name, FS_NAME_LEN);
        if (d.size >= FS_MAX_SIZE || pos + sizeof(d) + d.size >
            FS_DISK_SECTORS * FS_SECTOR_SIZE) return -1;
        kmemcpy(img + pos, &d, sizeof(d)); pos += sizeof(d);
        kmemcpy(img + pos, nodes[i].data, d.size); pos += d.size;
        h.count++;
    }
    h.bytes = pos - FS_SECTOR_SIZE;
    if (validate_payload(&h, img) < 0) return -1;
    h.sum = crc32(img + FS_SECTOR_SIZE, h.bytes);
    h.header_sum = crc32(&h, 24);
    unsigned payload_sectors = (h.bytes + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    kmemset(img + pos, 0, (1 + payload_sectors) * FS_SECTOR_SIZE - pos);
    int target = active_slot == 0 ? 1 : 0;
    unsigned lba = slot_lba(target);
    /* Previous snapshot is untouched until a complete new payload has been
     * written and read back. A torn commit header fails its own CRC. */
    if (disk_write(lba + 1, img + FS_SECTOR_SIZE, payload_sectors) < 0 ||
        disk_read(lba + 1, img + FS_SECTOR_SIZE, payload_sectors) < 0 ||
        crc32(img + FS_SECTOR_SIZE, h.bytes) != h.sum) return -1;
    kmemset(img, 0, FS_SECTOR_SIZE);
    kmemcpy(img, &h, sizeof(h));
    if (disk_write(lba, img, 1) < 0) { writable = 0; return -1; }
    DiskHeader verified;
    if (read_slot(target, &verified) != 0 || verified.generation != h.generation ||
        verified.sum != h.sum) {
        /* Commit outcome is uncertain. Require a remount before another
         * write; otherwise a retry could overwrite the newest valid copy. */
        writable = 0;
        return -1;
    }
    active_slot = target;
    generation = h.generation;
    fs_touched = 0;
    return 0;
}

int fs_save_disk(void) {
    last_sync_attempt = timer_ticks();
    int result = save_snapshot();
    save_failed = result < 0;
    if (save_failed) {
        if (sync_failures < 3) sync_failures++;
        platform_log("FS save failed; keeping unsaved RAM data\n");
    } else {
        sync_failures = 0;
    }
    return result;
}
int fs_needs_sync(void) { return fs_touched; }
int fs_sync(void) { return fs_save_disk(); }
void fs_autosync(void) {
    if (!writable || !fs_touched || sync_failures >= 3) return;
    if (sync_failures && (unsigned)(timer_ticks() - last_sync_attempt) < 5 * TIMER_HZ)
        return;
    fs_save_disk();
}
const char *fs_storage_status(void) {
    if (!writable) return "Disk protected; changes in RAM only";
    if (save_failed) return "Save failed; changes in RAM only";
    return 0;
}

/* Resolve absolute/relative paths, including . and .., without escaping root. */
int fs_resolve(int cwd, const char *path) {
    if (!path) return -1;
    int id = *path == '/' ? 0 : cwd;
    if (!fs_is_dir(id)) id = 0;
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        char name[FS_NAME_LEN]; int n = 0;
        while (*path && *path != '/') {
            if (n >= FS_NAME_LEN - 1) return -1;
            name[n++] = *path++;
        }
        name[n] = 0;
        if (!fs_is_dir(id)) return -1;
        if (!kstrcmp(name, ".")) continue;
        if (!kstrcmp(name, "..")) { if (id) id = fs_parent(id); continue; }
        id = fs_find_child(id, name);
        if (id < 0) return -1;
    }
    return id;
}
int fs_destination(int cwd, const char *path, char *name) {
    if (!path || !*path || kstrlen(path) >= FS_PATH_LEN) return -1;
    char parent[FS_PATH_LEN]; kstrcpy(parent, path);
    int slash = -1;
    for (int i = 0; parent[i]; i++) if (parent[i] == '/') slash = i;
    if (kstrlen(path + slash + 1) >= FS_NAME_LEN) return -1;
    kstrcpy(name, path + slash + 1);
    if (!valid_name(name)) return -1;
    if (slash < 0) return fs_is_dir(cwd) ? cwd : -1;
    if (!slash) return 0;
    parent[slash] = 0;
    int id = fs_resolve(cwd, parent);
    return fs_is_dir(id) ? id : -1;
}
