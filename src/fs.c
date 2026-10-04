#include "fs.h"
#include "persist.h"
#include "ata.h"

#include "platform.h"
#define FS_SECTOR_SIZE SECTOR_SIZE

#define FS_DISK_MAGIC   0x46534F42u   /* "BOSF" */
#define FS_DISK_VERSION 3
#define FS_DATA_VERSION 4
#define FS_LARGE_DATA_VERSION 5
#define FS_LARGE_DATA_PAYLOAD ((DATA_LARGE_SLOT_SECTORS - 1) * SECTOR_SIZE)
#define FS_DATA_PAYLOAD ((DATA_SLOT_SECTORS - 1) * SECTOR_SIZE)
#define FS_DATA_CAPACITY (FS_DATA_PAYLOAD - FS_LEGACY_NODES * 40)

typedef struct {
    char name[FS_NAME_LEN];
    int parent;
    int is_dir;
    int is_app;
    int size;
    int used;
    unsigned modified;
    unsigned offset; /* Nonempty data lives in one compact, bounded arena. */
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
static unsigned content_revisions[FS_MAX_NODES], next_content_revision;
unsigned fs_content_revision(int id){return fs_valid(id)?content_revisions[id]:0;}
static unsigned new_content_revision(void) {
    /* This diagnostic token must neither alias an old version nor prevent an
     * ordinary write. Once exhausted, new versions remain explicitly unknown. */
    return next_content_revision == ~0u ? 0 : ++next_content_revision;
}
static int writable;
static int active_slot = -1;
static unsigned generation;
static unsigned sync_failures;
static unsigned last_sync_attempt;
static int save_failed;
static unsigned pool_used;
/* Backend selects the wire format. Arena selection is deliberately separate:
 * blank-disk migration temporarily reads the floppy into the selected pool. */
static int data_backend; /* 0 floppy, 1 default IDE, 2 large IDE */
static uintptr_t pool_base, image_base;
static unsigned pool_capacity, image_capacity;
static int data_problem;
static int sync_new_incarnation(void);

_Static_assert(sizeof(FsNode) * FS_MAX_NODES <= FS_CAPACITY, "FS arena overflow");
_Static_assert(FS_MAX_NODES <= 32768, "node IDs exceed signed disk parents");
_Static_assert(FS_MAX_DEPTH >= FS_LEGACY_NODES - 1, "old directory trees must remain readable");
_Static_assert(FS_MAX_DEPTH * FS_NAME_LEN < FS_PATH_LEN, "canonical path buffer too small");
_Static_assert(sizeof(DiskHeader) == 28 && sizeof(DiskNode) == 40, "disk ABI changed");
_Static_assert(FS_SECTOR_SIZE + FS_LEGACY_NODES * (sizeof(DiskNode) + FS_MAX_SIZE - 1)
               <= FS_DISK_SECTORS * FS_SECTOR_SIZE, "floppy snapshot capacity too small");
_Static_assert(FS_DATA_CAPACITY + FS_MAX_NODES <= FS_POOL_CAPACITY, "file arena overflow");
_Static_assert(DATA_SLOT_SECTORS * FS_SECTOR_SIZE <= FS_IMG_CAPACITY, "data staging overflow");
_Static_assert(DATA_FIRST_LBA + DATA_SLOT_SECTORS <= DATA_SECOND_LBA &&
               DATA_SECOND_LBA + DATA_SLOT_SECTORS <= DATA_DISK_SECTORS, "data slot overlap");
_Static_assert(FS_DISK_SECTORS * FS_SECTOR_SIZE <= FS_IMG_CAPACITY, "staging overflow");
_Static_assert(FS_SECOND_LBA >= FS_DISK_LBA + FS_DISK_SECTORS &&
               FS_SECOND_LBA + FS_DISK_SECTORS <= DISK_SECTORS, "disk layout overlap");

_Static_assert(FS_LARGE_DATA_PAYLOAD - FS_LEGACY_NODES * 40 + FS_MAX_NODES <=
               FS_LARGE_POOL_CAPACITY, "large file arena overflow");
_Static_assert(DATA_LARGE_SLOT_SECTORS * SECTOR_SIZE <= FS_LARGE_IMG_CAPACITY,
               "large staging overflow");
_Static_assert(DATA_LARGE_FIRST_LBA + DATA_LARGE_SLOT_SECTORS == DATA_LARGE_SECOND_LBA &&
               DATA_LARGE_SECOND_LBA + DATA_LARGE_SLOT_SECTORS + 1 == DATA_LARGE_DISK_SECTORS,
               "large data slot overlap");

__attribute__((weak)) unsigned fs_clock(void) { return 0; }
__attribute__((weak)) void fs_background_poll(void) {}
unsigned fs_modified(int id) { return fs_valid(id) ? nodes[id].modified : 0; }
int fs_node_limit(void) { return data_backend ? FS_MAX_NODES : FS_LEGACY_NODES; }
/* Keep the original 64-node reservation even for small volumes. Additional
 * records consume 40 bytes each; no old full v4 snapshot loses capacity. */
unsigned fs_capacity_for_nodes(unsigned count) {
    if (!count || count > (unsigned)fs_node_limit()) return 0;
    if (!data_backend) return (FS_LEGACY_NODES - 1) * (FS_MAX_SIZE - 1);
    if (count < FS_LEGACY_NODES) count = FS_LEGACY_NODES;
    return (fs_large_profile() ? FS_LARGE_DATA_PAYLOAD : FS_DATA_PAYLOAD) - count * sizeof(DiskNode);
}
unsigned fs_capacity(void) { return fs_capacity_for_nodes(fs_node_count()); }
int fs_large_profile(void) { return data_backend == 2; }
int fs_large_arenas_available(void) {
    return platform_memory_range_available(FS_LARGE_POOL_BASE, RAM_LARGE_REQUIRED_END);
}
unsigned fs_file_limit(void) {
    return fs_large_profile() ? FS_LARGE_FILE_MAX : data_backend ? FS_FILE_MAX : FS_MAX_SIZE - 1;
}
const char *fs_storage_name(void) { return fs_large_profile() ? "Large IDE data disk" : data_backend ? "IDE data disk" : "Boot floppy"; }
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

/* Large byte operations keep device-only work bounded just like CRC scans.
 * This helper is for nonoverlapping byte spans, never live GUI callbacks. */
static void copy_bytes(void *destination, const void *source, unsigned length) {
    unsigned char *d = destination;
    const unsigned char *s = source;
    while (length) {
        unsigned n = length > 4096 ? 4096 : length;
        kmemcpy(d, s, n);
        d += n; s += n; length -= n;
        if (n == 4096) fs_background_poll();
    }
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

/* Measure components, including a leading slash per component. Root has no
 * components. A separate depth limit retains every valid old 64-node tree
 * while bounding recursion and paths independently of the wider node table. */
static int path_shape(int id, int *depth, int *bytes) {
    *depth = *bytes = 0;
    while (id > 0) {
        if (!fs_valid(id) || ++*depth > FS_MAX_DEPTH) return -1;
        *bytes += 1 + kstrlen(nodes[id].name);
        if (*bytes >= FS_PATH_LEN) return -1;
        id = nodes[id].parent;
    }
    return id == 0 ? 0 : -1;
}

/* Check a new node (id == -1), or every path in a moved/renamed/copied subtree,
 * before changing anything. No descendant can become an ambiguous short path. */
static int tree_fits(int id, int parent, const char *name) {
    int depth, bytes;
    if (path_shape(parent, &depth, &bytes) < 0) return 0;
    ++depth; bytes += 1 + kstrlen(name);
    if (depth > FS_MAX_DEPTH || bytes >= FS_PATH_LEN) return 0;
    if (id < 0 || !nodes[id].is_dir) return 1;
    for (int i = 1; i < fs_node_limit(); ++i) {
        if (!nodes[i].used || i == id) continue;
        int walk = i, below_depth = 0, below_bytes = 0;
        while (walk > 0 && walk != id && below_depth < FS_MAX_DEPTH) {
            ++below_depth;
            below_bytes += 1 + kstrlen(nodes[walk].name);
            walk = nodes[walk].parent;
        }
        if (walk == id && (depth + below_depth > FS_MAX_DEPTH ||
                          bytes + below_bytes >= FS_PATH_LEN)) return 0;
    }
    return 1;
}

static int alloc_node(int parent, const char *name, int is_dir, int is_app) {
    if (fs_sync_busy()) return FS_ERR_BUSY;
    if (!valid_name(name)) return -1;
    if (parent < 0 || parent >= fs_node_limit() || !nodes[parent].used)
        return -1;
    if (!nodes[parent].is_dir)
        return -1;
    if (fs_find_child(parent, name) >= 0)
        return -1;
    if (kstrlen(name) <= 0 || kstrlen(name) >= FS_NAME_LEN)
        return -1;

    if (!tree_fits(-1, parent, name) ||
        fs_used_bytes() > fs_capacity_for_nodes(fs_node_count() + 1)) return -1;
    for (int i = 0; i < fs_node_limit(); i++) {
        if (!nodes[i].used) {
            kmemset(&nodes[i], 0, (int)sizeof(FsNode));
            kstrcpy(nodes[i].name, name);
            nodes[i].parent = parent;
            nodes[i].is_dir = is_dir;
            nodes[i].is_app = is_app;
            nodes[i].size = 0;
            nodes[i].used = 1;
            identities[i]=++next_identity;
            content_revisions[i]=new_content_revision();
            nodes[i].modified = fs_clock();
            fs_touched = 1;
            return i;
        }
    }
    return -1;
}

int fs_init(void) {
    if (fs_sync_busy()) return FS_ERR_BUSY;
    if (sync_new_incarnation() < 0) return -1;
    writable = 0;
    active_slot = -1;
    generation = sync_failures = last_sync_attempt = 0;
    save_failed = 0;
    pool_used = 0;
    data_backend = data_problem = 0;
    pool_base = FS_POOL_BASE; image_base = FS_IMG_BASE;
    pool_capacity = FS_POOL_CAPACITY; image_capacity = FS_IMG_CAPACITY;
    nodes = (FsNode *)FS_BASE;
    kmemset(nodes, 0, (int)sizeof(FsNode) * FS_MAX_NODES);
    kmemset(content_revisions, 0, sizeof(content_revisions));

    nodes[0].used = 1;
    content_revisions[0]=new_content_revision();
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
    return 0;
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
const char *fs_data(int id) {
    return fs_valid(id) && nodes[id].size ? (const char *)pool_base + nodes[id].offset : "";
}

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

/* Remove a data block and compact in place. Since the destination is lower,
 * a forward byte copy is overlap-safe. No allocation or fragmentation remains. */
static void release_data(int id) {
    if (!nodes[id].size) return;
    unsigned offset = nodes[id].offset, bytes = nodes[id].size + 1;
    unsigned char *pool = (unsigned char *)pool_base;
    for (unsigned i = offset; i < pool_used - bytes; ++i) {
        pool[i] = pool[i + bytes];
        if((i & 4095u)==4095u)fs_background_poll();
    }
    for (int i = 0; i < FS_MAX_NODES; ++i)
        if (nodes[i].used && nodes[i].size && nodes[i].offset > offset) nodes[i].offset -= bytes;
    pool_used -= bytes;
    nodes[id].size = 0;
    nodes[id].offset = 0;
}
int fs_write(int id, const char *data, int len) {
    if (fs_sync_busy()) return FS_ERR_BUSY;
    if (!fs_valid(id) || nodes[id].is_dir || nodes[id].is_app || len < 0 ||
        (unsigned)len > fs_file_limit() || (len && !data)) return -1;
    if ((unsigned)len > fs_capacity() - (fs_used_bytes() - nodes[id].size)) return -1;
    unsigned previous = nodes[id].size ? nodes[id].size + 1 : 0;
    unsigned required = len ? (unsigned)len + 1 : 0;
    if (required > pool_capacity - (pool_used - previous)) return -1;
    /* fs_write(dst, fs_data(src), n), self-overwrite, and fs_copy all work even
     * when compaction moves the source. Staging is idle outside disk commits. */
    uintptr_t source = (uintptr_t)data;
    if (len && source >= pool_base && source - pool_base < pool_capacity) {
        unsigned offset = source - pool_base;
        if (offset > pool_used || (unsigned)len > pool_used - offset) return -1;
        copy_bytes((void *)image_base, data, len);
        data = (const char *)image_base;
    }
    if (len && source >= image_base && source - image_base < image_capacity &&
        (unsigned)len > image_capacity - (source - image_base)) return -1;
    if (len && len == nodes[id].size) {
        /* Common autosaves and media overwrites need no arena movement. */
        copy_bytes((char *)pool_base + nodes[id].offset, data, len);
        nodes[id].modified = fs_clock();
        content_revisions[id]=new_content_revision();
        fs_touched = 1;
        return len;
    }
    release_data(id);
    if (len) {
        nodes[id].offset = pool_used;
        char *destination = (char *)pool_base + pool_used;
        copy_bytes(destination, data, len);
        destination[len] = 0;
        pool_used += required;
    }
    nodes[id].size = len;
    nodes[id].modified = fs_clock();
    content_revisions[id]=new_content_revision();
    fs_touched = 1;
    return len;
}

int fs_read(int id, char *out, int max) {
    if (!out || max <= 0) return 0;
    if (!fs_valid(id) || nodes[id].is_dir || nodes[id].is_app)
        return -1;
    int n = nodes[id].size;
    if (n > max - 1)
        n = max - 1;
    copy_bytes(out, fs_data(id), n);
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

    int parts[FS_MAX_DEPTH];
    int n = 0;
    int cur = id;
    while (cur > 0 && n < FS_MAX_DEPTH) {
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
    for (int n = 1; n <= fs_node_limit(); n++) {
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
    /* Up to three digits at the current 256-node limit. */
    int digits = n >= 100 ? 3 : n >= 10 ? 2 : 1;
    if (i > FS_NAME_LEN - 2 - digits)
        i = FS_NAME_LEN - 2 - digits;
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
    for (int n = 1; n <= fs_node_limit(); n++) {
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
    if (fs_sync_busy()) return FS_ERR_BUSY;
    if (!fs_valid(id) || id == 0)
        return -1;
    if (nodes[id].is_dir) {
        for (int i = 0; i < FS_MAX_NODES; i++) {
            if (nodes[i].used && nodes[i].parent == id)
                fs_delete(i);
        }
    }
    release_data(id);
    nodes[id].used = 0;
    content_revisions[id]=0;
    fs_touched = 1;
    return 0;
}

int fs_move(int id, int new_parent) {
    if (fs_sync_busy()) return FS_ERR_BUSY;
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
    char name[FS_NAME_LEN];
    kstrcpy(name, nodes[id].name);
    int clash = fs_find_child(new_parent, name);
    if (clash >= 0 && clash != id) {
        int found = 0;
        for (int n = 2; n <= fs_node_limit(); n++) {
            numbered_name(name, nodes[id].name, n);
            if (fs_find_child(new_parent, name) < 0) {
                found = 1;
                break;
            }
        }
        if (!found) return -1;
    }
    if (!tree_fits(id, new_parent, name)) return -1;
    kstrcpy(nodes[id].name, name);
    nodes[id].parent = new_parent;
    nodes[id].modified = fs_clock();
    fs_touched = 1;
    return 0;
}

int fs_empty_dir(int parent) {
    if (fs_sync_busy()) return FS_ERR_BUSY;
    if (!fs_is_dir(parent))
        return -1;
    for (int i = 0; i < FS_MAX_NODES; i++) {
        if (nodes[i].used && nodes[i].parent == parent)
            fs_delete(i);
    }
    return 0;
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
    if (fs_sync_busy()) return FS_ERR_BUSY;
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
        if (fs_write(dst, fs_data(id), nodes[id].size) < 0) {
            fs_delete(dst);
            return -1;
        }
    }
    return dst;
}

int fs_unique_copy(int parent, const char *src, char *out) {
    if (!src || !out)
        return -1;
    for (int n = 1; n <= fs_node_limit(); n++) {
        make_copy_name(out, src, n);
        if (fs_find_child(parent, out) < 0)
            return 0;
    }
    return -1;
}

int fs_copy(int id, int parent) {
    if (fs_sync_busy()) return FS_ERR_BUSY;
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
    if (!tree_fits(id, parent, name)) return -1;
    int touched_before = fs_touched;
    int result = copy_into(id, parent, name);
    if (result < 0) fs_touched = touched_before;
    return result;
}

int fs_rename(int id, const char *name) {
    if (fs_sync_busy()) return FS_ERR_BUSY;
    if (!fs_valid(id) || id == 0 || !valid_name(name))
        return -1;
    int len = kstrlen(name);
    if (len <= 0 || len >= FS_NAME_LEN)
        return -1;
    int clash = fs_find_child(nodes[id].parent, name);
    if (clash >= 0 && clash != id)
        return -1;
    if (!tree_fits(id, nodes[id].parent, name)) return -1;
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

/* Reflected IEEE CRC32, polynomial 0xEDB88320. Each constant is the result
 * of eight steps of that polynomial starting with its byte index. The 1 KiB
 * read-only table replaces eight bit iterations per byte without changing
 * the initial/final complement, byte order, or on-disk checksums.
 * v1's rolling checksum above remains read-only for migration. */
static const unsigned crc32_table[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
    0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u, 0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u,
    0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u,
    0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u, 0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu,
    0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
    0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u, 0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du,
    0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u,
    0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu, 0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u,
    0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu,
    0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u, 0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
    0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u, 0xB7BD5C3Bu, 0xC0BA6CADu,
    0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au, 0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u,
    0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u,
    0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu, 0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u,
    0xD6D6A3E8u, 0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu, 0x4669BE79u,
    0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u, 0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu,
    0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u,
    0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u, 0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u,
    0x86D3D2D4u, 0xF1D4E242u, 0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
    0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u, 0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu,
    0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u, 0x54DE5729u, 0x23D967BFu,
    0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u, 0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

static unsigned crc32(const void *data, unsigned n) {
    const unsigned char *p = data;
    unsigned crc = ~0u;
    for (unsigned i = 0; i < n; ++i) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ p[i]) & 255u];
        /* Keep device service at exactly the previous 4 KiB boundaries. */
        if((i & 4095u)==4095u)fs_background_poll();
    }
    return ~crc;
}

static unsigned slot_lba(int slot) {
    return fs_large_profile() ? (slot ? DATA_LARGE_SECOND_LBA : DATA_LARGE_FIRST_LBA)
                        : data_backend ? (slot ? DATA_SECOND_LBA : DATA_FIRST_LBA)
                        : (slot ? FS_SECOND_LBA : FS_DISK_LBA);
}
static unsigned slot_sectors(void) {
    return fs_large_profile() ? DATA_LARGE_SLOT_SECTORS : data_backend ? DATA_SLOT_SECTORS : FS_DISK_SECTORS;
}
static unsigned snapshot_version(void) {
    return fs_large_profile() ? FS_LARGE_DATA_VERSION : data_backend ? FS_DATA_VERSION : FS_DISK_VERSION;
}
static unsigned volume_sectors(void) { return data_backend ? ata_sector_count() : disk_sector_count(); }
static int volume_read(unsigned lba, void *buffer, int sectors) {
    return data_backend ? ata_read(lba, buffer, sectors) : disk_read(lba, buffer, sectors);
}
static int volume_write(unsigned lba, const void *buffer, int sectors) {
    return data_backend ? ata_write(lba, buffer, sectors) : disk_write(lba, buffer, sectors);
}
static int volume_flush(void) { return data_backend ? ata_flush() : 0; }

/* Only this explicit format marker grants permission to use the optional disk.
 * A blank generic hard disk is NOT a blank BaseOS volume. */
/* Read the marker on the required kernel stack, without borrowing either
 * filesystem arena. A 64 MiB guest never touches optional high memory. */
static int valid_data_marker(void) {
    unsigned sectors = ata_sector_count();
    if (sectors != DATA_DISK_SECTORS && sectors != DATA_LARGE_DISK_SECTORS) return 0;
    unsigned marker[SECTOR_SIZE / sizeof(unsigned)];
    if (ata_read(0, marker, 1) < 0) return 0;
    int large = sectors == DATA_LARGE_DISK_SECTORS;
    if (marker[0] != DATA_MARKER_MAGIC ||
        marker[1] != (large ? DATA_LARGE_MARKER_VERSION : DATA_MARKER_VERSION) ||
        marker[2] != sectors ||
        marker[3] != (large ? DATA_LARGE_SLOT_SECTORS : DATA_SLOT_SECTORS) ||
        marker[4] != (large ? DATA_LARGE_FIRST_LBA : DATA_FIRST_LBA) ||
        marker[5] != (large ? DATA_LARGE_SECOND_LBA : DATA_SECOND_LBA) ||
        marker[6] != crc32(marker, 24)) return 0;
    for (unsigned i = 28; i < SECTOR_SIZE; ++i)
        if (((unsigned char *)marker)[i]) return 0;
    return large ? 2 : 1;
}

/* Copy seeded/live bytes before changing what their offsets refer to. Both
 * regions are independently reserved; migration never switches this back. */
static void select_large_arenas(void) {
    if (pool_base != FS_LARGE_POOL_BASE)
        copy_bytes((void *)FS_LARGE_POOL_BASE, (const void *)pool_base, pool_used);
    pool_base = FS_LARGE_POOL_BASE; pool_capacity = FS_LARGE_POOL_CAPACITY;
    image_base = FS_LARGE_IMG_BASE; image_capacity = FS_LARGE_IMG_CAPACITY;
}

static unsigned disk_node_size(const DiskHeader *h) { return h->version >= 3 ? 40 : 36; }
static void decode_node(DiskNode *d, const unsigned char *p, const DiskHeader *h) {
    kmemset(d, 0, sizeof(*d));
    kmemcpy(d, p, disk_node_size(h));
}

/* The same bounded graph validator serves mount and incremental commit. No
 * live node or file bytes are changed; offsets describe the staged image. */
typedef struct {
    unsigned offsets[FS_MAX_NODES];
    const DiskHeader *header;
    const unsigned char *image;
    unsigned phase, record, pos, total, allowance;
    unsigned id, walk, steps, path_bytes, other;
    DiskNode node;
} PayloadValidator;
enum { VALIDATE_RECORDS = 1, VALIDATE_NEXT, VALIDATE_PATH, VALIDATE_NAMES,
       VALIDATE_DONE };
#define FS_SYNC_GRAPH_BUDGET 64u
#define FS_SYNC_BYTE_BUDGET 4096u

static int validator_begin(PayloadValidator *v, const DiskHeader *h,
                           const unsigned char *image) {
    kmemset(v, 0, sizeof(*v));
    if (!h->count || h->count > (unsigned)fs_node_limit() ||
        h->bytes > (slot_sectors() - 1) * FS_SECTOR_SIZE ||
        h->bytes < h->count * disk_node_size(h)) return -1;
    v->header = h; v->image = image;
    v->allowance = fs_capacity_for_nodes(h->count);
    v->phase = VALIDATE_RECORDS; v->pos = FS_SECTOR_SIZE; v->id = 1;
    return 0;
}
/* 1 more work, 0 valid, -1 invalid. At most budget fixed-size graph checks. */
static int validator_step(PayloadValidator *v, unsigned budget) {
    const DiskHeader *h = v->header;
    const unsigned char *img = v->image;
    unsigned ds = disk_node_size(h), end = FS_SECTOR_SIZE + h->bytes;
    while (budget--) {
        if (v->phase == VALIDATE_RECORDS) {
            if (v->record == h->count) {
                if (v->pos != end || !v->offsets[0]) return -1;
                v->phase = VALIDATE_NEXT;
                continue;
            }
            DiskNode d;
            if (v->pos > end || end - v->pos < ds) return -1;
            decode_node(&d, img + v->pos, h);
            if (d.id >= (unsigned)fs_node_limit() || v->offsets[d.id] ||
                d.parent < -1 || d.parent >= fs_node_limit() ||
                d.is_dir > 1 || d.is_app > 1 || (d.is_dir && d.is_app) ||
                d.size > fs_file_limit() || d.size > end - v->pos - ds ||
                ((d.is_dir || d.is_app) && d.size)) return -1;
            int length = 0;
            while (length < FS_NAME_LEN && d.name[length]) {
                if (d.name[length] == '/') return -1;
                ++length;
            }
            if (length == FS_NAME_LEN || (d.id && !length) ||
                (d.id && (!kstrcmp(d.name, ".") || !kstrcmp(d.name, ".."))) ||
                (!d.id && (d.parent != -1 || !d.is_dir || length)) ||
                d.size > v->allowance - v->total) return -1;
            v->total += d.size; v->offsets[d.id] = v->pos;
            v->pos += ds + d.size; ++v->record;
        } else if (v->phase == VALIDATE_NEXT) {
            if (v->id >= FS_MAX_NODES) { v->phase = VALIDATE_DONE; return 0; }
            if (!v->offsets[v->id]) { ++v->id; continue; }
            decode_node(&v->node, img + v->offsets[v->id], h);
            v->walk = v->id; v->steps = v->path_bytes = 0;
            v->other = 1; v->phase = VALIDATE_PATH;
        } else if (v->phase == VALIDATE_PATH) {
            if (!v->walk) { v->phase = VALIDATE_NAMES; continue; }
            if (v->steps++ >= FS_MAX_DEPTH) return -1;
            DiskNode d, parent;
            decode_node(&d, img + v->offsets[v->walk], h);
            v->path_bytes += 1 + kstrlen(d.name);
            if (v->path_bytes >= FS_PATH_LEN || d.parent < 0 ||
                !v->offsets[d.parent]) return -1;
            v->walk = (unsigned)d.parent;
            decode_node(&parent, img + v->offsets[v->walk], h);
            if (!parent.is_dir) return -1;
        } else if (v->phase == VALIDATE_NAMES) {
            if (v->other >= v->id) { ++v->id; v->phase = VALIDATE_NEXT; continue; }
            unsigned offset = v->offsets[v->other++];
            if (!offset) continue;
            DiskNode d;
            decode_node(&d, img + offset, h);
            if (d.parent == v->node.parent && !kstrcmp(d.name, v->node.name)) return -1;
        } else return v->phase == VALIDATE_DONE ? 0 : -1;
    }
    return 1;
}

static int validate_payload(const DiskHeader *h, const unsigned char *img) {
    PayloadValidator v;
    if (validator_begin(&v, h, img) < 0) return -1;
    int result;
    do { result = validator_step(&v, FS_SYNC_GRAPH_BUDGET); }
    while (result > 0);
    return result;
}

static int all_zero(const unsigned char *p, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        if (p[i]) return 0;
        if ((i & 4095u) == 4095u) fs_background_poll();
    }
    return 1;
}

/* 0 valid, 1 wholly blank, -1 corrupt, -2 I/O error. Never modifies nodes. */
static int read_slot(int slot, DiskHeader *h) {
    unsigned char *img = (unsigned char *)image_base;
    unsigned lba = slot_lba(slot);
    unsigned span = slot_sectors();
    if (lba + span > volume_sectors()) return -2;
    if (volume_read(lba, img, 1) < 0) return -2;
    if (all_zero(img, FS_SECTOR_SIZE)) {
        if (volume_read(lba + 1, img + FS_SECTOR_SIZE, span - 1) < 0)
            return -2;
        return all_zero(img + FS_SECTOR_SIZE, (span - 1) * FS_SECTOR_SIZE)
               ? 1 : -1;
    }
    kmemcpy(h, img, sizeof(*h));
    if (h->magic != FS_DISK_MAGIC || !h->count || h->count > (unsigned)fs_node_limit() ||
        h->bytes > (span - 1) * FS_SECTOR_SIZE ||
        h->bytes < h->count * disk_node_size(h)) return -1;
    if (data_backend) {
        if (h->version != snapshot_version() || h->header_sum != crc32(h, 24)) return -1;
    } else if (h->version == 1 && slot == 0) {
        h->generation = 0;
    } else if ((h->version != 2 && h->version != FS_DISK_VERSION) ||
               h->header_sum != crc32(h, 24)) return -1;
    unsigned sectors = (h->bytes + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
    if (volume_read(lba + 1, img + FS_SECTOR_SIZE, sectors) < 0) return -2;
    unsigned sum = h->version == 1 ? checksum(img + FS_SECTOR_SIZE, h->bytes)
                                  : crc32(img + FS_SECTOR_SIZE, h->bytes);
    if (sum != h->sum || validate_payload(h, img) < 0) return -1;
    return 0;
}

static void import_payload(const DiskHeader *h) {
    const unsigned char *img = (const unsigned char *)image_base;
    unsigned pos = FS_SECTOR_SIZE;
    kmemset(nodes, 0, sizeof(FsNode) * FS_MAX_NODES);
    kmemset(content_revisions, 0, sizeof(content_revisions));
    pool_used = 0;
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
        content_revisions[d.id]=new_content_revision();
        nd->modified = d.modified;
        if (d.size) {
            nd->offset = pool_used;
            char *destination = (char *)pool_base + pool_used;
            copy_bytes(destination, img + pos, d.size);
            destination[d.size] = 0;
            pool_used += d.size + 1;
        }
        pos += d.size;
    }
}

static int load_volume(void) {
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

int fs_load_disk(void) {
    if (fs_sync_busy()) return FS_ERR_BUSY;
    if (sync_new_incarnation() < 0) return -1;
    data_backend = data_problem = 0;
    int probe = ata_probe();
    if (probe == 0) return load_volume();
    int profile = probe > 0 ? valid_data_marker() : 0;
    if (profile == 2 && !fs_large_arenas_available()) {
        data_problem = 2;
        platform_log("FS large data disk requires usable RAM through 127 MiB; disk untouched\n");
    } else if (profile) {
        if (profile == 2) select_large_arenas();
        data_backend = profile;
        int result = load_volume();
        if (result == 0) {
            platform_log(profile == 2 ? "FS mounted large IDE data disk\n" : "FS mounted IDE data disk\n");
            return 0;
        }
        if (result == FS_LOAD_BLANK) {
            /* The new disk is positively marked and wholly blank. Keep its
             * selected arenas while reading old floppy offsets and bytes. */
            data_backend = 0;
            int source = load_volume();
            if (source >= 0 && (writable || disk_sector_count() == 2880)) {
                data_backend = profile;
                writable = 1;
                active_slot = -1;
                generation = sync_failures = save_failed = 0;
                fs_touched = 1;
                platform_log(source ? "FS new IDE data disk; seed pending\n"
                                    : "FS boot files migrated to RAM; IDE save pending\n");
                return source;
            }
            data_problem = 1;
            writable = 0;
            platform_log("FS migration blocked; boot volume could not be read completely\n");
            return source < 0 ? source : -1;
        }
    }
    /* Never initialize unknown media or overwrite a corrupt optional disk.
     * The boot files remain accessible as a read-only recovery view. */
    data_backend = 0;
    int fallback = load_volume();
    if (!data_problem) data_problem = 1;
    writable = 0;
    platform_log("FS data disk unavailable; boot files are read-only\n");
    return fallback < 0 ? fallback : -1;
}

static int save_snapshot(void) {
    if (!fs_touched) return 0;
    if (!writable) return -1;
    unsigned char *img = (unsigned char *)image_base;
    unsigned pos = FS_SECTOR_SIZE;
    DiskHeader h = { .magic = FS_DISK_MAGIC,
                     .version = snapshot_version(),
                     .generation = generation + 1 };
    for (int i = 0; i < FS_MAX_NODES; ++i) {
        if (!nodes[i].used) continue;
        DiskNode d = { .id = i, .parent = nodes[i].parent,
                       .is_dir = nodes[i].is_dir, .is_app = nodes[i].is_app,
                       .size = nodes[i].size, .modified = nodes[i].modified };
        kmemcpy(d.name, nodes[i].name, FS_NAME_LEN);
        if (d.size > fs_file_limit() || pos + sizeof(d) + d.size >
            slot_sectors() * FS_SECTOR_SIZE) return -1;
        kmemcpy(img + pos, &d, sizeof(d)); pos += sizeof(d);
        copy_bytes(img + pos, fs_data(i), d.size); pos += d.size;
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
    if (volume_write(lba + 1, img + FS_SECTOR_SIZE, payload_sectors) < 0 ||
        volume_flush() < 0 ||
        volume_read(lba + 1, img + FS_SECTOR_SIZE, payload_sectors) < 0 ||
        crc32(img + FS_SECTOR_SIZE, h.bytes) != h.sum) return -1;
    kmemset(img, 0, FS_SECTOR_SIZE);
    kmemcpy(img, &h, sizeof(h));
    if (volume_write(lba, img, 1) < 0 || volume_flush() < 0) { writable = 0; return -1; }
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

/* An active snapshot owns the live FS and its existing image arena until the
 * final verified readback (or failure). Three tiny result records outlive jobs:
 * record 0 belongs to the sole async/autosave client, record 1 to fs_sync,
 * and record 2 to the kernel native completion coordinator.
 * A joining fs_sync subscribes independently, so autosave can reap its own
 * record without losing the joiner's result. Explicit owners release theirs. */
typedef struct {
    FsSyncTicket ticket;
    int occupied, result;
} SyncResult;
typedef struct {
    unsigned phase, next_phase, subscribers;
    unsigned node, pos, copied, length, cursor, crc, sectors, lba;
    int target, header_attempted;
    DiskHeader header, verified;
    PayloadValidator validator;
} SyncJob;
enum {
    SYNC_IDLE, SYNC_SERIALIZE_NODE, SYNC_SERIALIZE_DATA, SYNC_VALIDATE_STAGE,
    SYNC_CRC_STAGE, SYNC_WRITE_PAYLOAD, SYNC_FLUSH_PAYLOAD,
    SYNC_READ_PAYLOAD, SYNC_CRC_READBACK, SYNC_WRITE_HEADER, SYNC_FLUSH_HEADER,
    SYNC_READ_HEADER, SYNC_CHECK_HEADER, SYNC_READ_COMMITTED,
    SYNC_CRC_COMMITTED, SYNC_VALIDATE_COMMITTED, SYNC_IO, SYNC_FLOPPY
};
static SyncJob sync_job;
static SyncResult sync_results[3];
static unsigned sync_incarnation, sync_serial;
static int sync_autosave;
_Static_assert(sizeof(SyncJob) + sizeof(sync_results) + 3 * sizeof(unsigned) <= 1536,
               "incremental FS control state exceeds bounded budget");

int fs_sync_busy(void) { return sync_job.phase != SYNC_IDLE; }
static int sync_new_incarnation(void) {
    if (sync_incarnation == ~0u) return -1; /* Never make an old ticket current. */
    ++sync_incarnation; sync_serial = 0; sync_autosave = 0;
    kmemset(sync_results, 0, sizeof(sync_results));
    kmemset(&sync_job, 0, sizeof(sync_job));
    return 0;
}
static int sync_record(unsigned slot, FsSyncTicket *ticket) {
    if (sync_results[slot].occupied) return FS_ERR_BUSY;
    if (!sync_incarnation || sync_serial == ~0u) return -1;
    SyncResult *r = &sync_results[slot];
    r->ticket.incarnation = sync_incarnation; r->ticket.serial = ++sync_serial;
    r->occupied = 1; r->result = FS_SYNC_PENDING;
    if (ticket) *ticket = r->ticket;
    return 0;
}
static int sync_find(FsSyncTicket ticket) {
    for (unsigned i = 0; i < 3; ++i)
        if (sync_results[i].occupied && ticket.incarnation == sync_results[i].ticket.incarnation &&
            ticket.serial == sync_results[i].ticket.serial) return (int)i;
    return -1;
}
int fs_sync_result(FsSyncTicket ticket) {
    int slot = sync_find(ticket);
    return slot < 0 ? FS_SYNC_STALE : sync_results[slot].result;
}
int fs_sync_release(FsSyncTicket ticket) {
    int slot = sync_find(ticket);
    if (slot < 0) return FS_SYNC_STALE;
    if (sync_results[slot].result == FS_SYNC_PENDING) return FS_ERR_BUSY;
    sync_results[slot].occupied = 0;
    return 0;
}
static void sync_account(int result) {
    save_failed = result < 0;
    if (save_failed) {
        if (sync_failures < 3) ++sync_failures;
        platform_log("FS save failed; keeping unsaved RAM data\n");
    } else sync_failures = 0;
}
static void sync_log_number(unsigned value) {
    char text[9];
    for (unsigned i = 0; i < 8; ++i)
        text[i] = "0123456789ABCDEF"[(value >> (28 - 4 * i)) & 15];
    text[8] = 0; platform_log(text);
}
/* Only generation/timing diagnostics; never names or file contents. */
static void sync_log(const char *event, const char *result) {
    unsigned ticks = timer_ticks();
    platform_log("FS snapshot "); platform_log(event);
    platform_log(" tick="); sync_log_number(ticks);
    platform_log(" generation="); sync_log_number(sync_job.header.generation);
    if (result) { platform_log(" result="); platform_log(result); }
    platform_log("\n");
}
static enum FsSyncProgress sync_finish(int result) {
    if (result < 0 && sync_job.header_attempted) writable = 0;
    if (!result) {
        active_slot = sync_job.target;
        generation = sync_job.header.generation;
        fs_touched = 0;
    }
    for (unsigned i = 0; i < 3; ++i)
        if (sync_job.subscribers & (1u << i)) sync_results[i].result = result;
    sync_job.phase = SYNC_IDLE;
    sync_account(result);
    sync_log("end", result < 0 ? "error" : "durable");
    if (sync_autosave) { sync_results[0].occupied = 0; sync_autosave = 0; }
    return FS_SYNC_FINISHED;
}
/* Start only after reserving a completion record. No IDE payload work here. */
static void sync_start(unsigned slot) {
    kmemset(&sync_job, 0, sizeof(sync_job));
    sync_job.subscribers = 1u << slot;
    sync_job.target = active_slot == 0 ? 1 : 0;
    sync_job.lba = slot_lba(sync_job.target);
    sync_job.pos = FS_SECTOR_SIZE;
    sync_job.header.magic = FS_DISK_MAGIC;
    sync_job.header.version = snapshot_version();
    sync_job.header.generation = generation + 1;
    sync_job.phase = data_backend ? SYNC_SERIALIZE_NODE : SYNC_FLOPPY;
    last_sync_attempt = timer_ticks();
    sync_log("begin", 0);
}
int fs_sync_request(FsSyncTicket *ticket) {
    if (!ticket) return -1;
    if (fs_sync_busy() || sync_results[0].occupied) return FS_ERR_BUSY;
    if (fs_touched && !writable) return -1;
    int result = sync_record(0, ticket);
    if (result < 0) return result;
    if (!fs_touched) { sync_results[0].result = 0; return 0; }
    sync_start(0);
    /* The floppy BIOS/PIO path remains deliberately synchronous. */
    if (!data_backend) (void)fs_sync_step();
    return 0;
}
/* This entry is reserved for the native completion coordinator. Keeping a
 * separate subscriber lets it join autosave or a built-in explicit save without
 * borrowing that caller's result or changing its release policy. */
unsigned fs_incarnation(void) { return sync_incarnation; }
int fs_sync_async_supported(void) { return data_backend != 0; }
int fs_writes_allowed(void) { return writable; }
int fs_sync_request_owned(FsSyncTicket *ticket) {
    if (!ticket) return -1;
    /* Reject before sync_record/start: the floppy request path can block. */
    if (!fs_sync_async_supported()) return FS_SYNC_UNSUPPORTED;
    if (sync_results[2].occupied) return FS_ERR_BUSY;
    if (!sync_incarnation || sync_serial == ~0u) return FS_SYNC_EXHAUSTED;
    if (!fs_sync_busy() && fs_touched && !writable) return FS_SYNC_PROTECTED;
    int result = sync_record(2, ticket);
    if (result < 0) return result;
    if (fs_sync_busy()) sync_job.subscribers |= 4u;
    else if (!fs_touched) sync_results[2].result = 0;
    else sync_start(2);
    return 0;
}
static unsigned sync_crc_bytes(unsigned crc, const unsigned char *data, unsigned length) {
    for (unsigned i = 0; i < length; ++i)
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 255u];
    return crc;
}
static void sync_crc_begin(void) { sync_job.cursor = 0; sync_job.crc = ~0u; }
static int sync_crc_step(void) {
    unsigned n = sync_job.header.bytes - sync_job.cursor;
    if (n > FS_SYNC_BYTE_BUDGET) n = FS_SYNC_BYTE_BUDGET;
    sync_job.crc = sync_crc_bytes(sync_job.crc,
        (const unsigned char *)image_base + FS_SECTOR_SIZE + sync_job.cursor, n);
    sync_job.cursor += n;
    return sync_job.cursor == sync_job.header.bytes;
}
/* The transport retains this stable image buffer until a later poll completes. */
static enum FsSyncProgress sync_io(int operation, unsigned lba, void *buffer,
                                   int sectors, unsigned next) {
    int result = operation == 0 ? ata_request_read(lba, buffer, sectors) :
                 operation == 1 ? ata_request_write(lba, buffer, sectors) :
                                  ata_request_flush();
    if (result < 0) return sync_finish(-1);
    if (sync_job.phase == SYNC_WRITE_HEADER) sync_job.header_attempted = 1;
    sync_job.phase = SYNC_IO; sync_job.next_phase = next;
    return FS_SYNC_MORE;
}
enum FsSyncProgress fs_sync_step(void) {
    unsigned char *img = (unsigned char *)image_base;
    SyncJob *j = &sync_job;
    switch (j->phase) {
    case SYNC_IDLE: return FS_SYNC_IDLE;
    case SYNC_SERIALIZE_NODE: {
        while (j->node < FS_MAX_NODES && !nodes[j->node].used) ++j->node;
        if (j->node == FS_MAX_NODES) {
            j->header.bytes = j->pos - FS_SECTOR_SIZE;
            if (validator_begin(&j->validator, &j->header, img) < 0) return sync_finish(-1);
            j->phase = SYNC_VALIDATE_STAGE; return FS_SYNC_MORE;
        }
        FsNode *n = &nodes[j->node];
        DiskNode d = { .id = j->node, .parent = n->parent,
            .is_dir = n->is_dir, .is_app = n->is_app, .size = n->size, .modified = n->modified };
        if (d.size > fs_file_limit() || j->pos > slot_sectors() * FS_SECTOR_SIZE ||
            sizeof(d) + d.size > slot_sectors() * FS_SECTOR_SIZE - j->pos) return sync_finish(-1);
        kmemcpy(d.name, n->name, FS_NAME_LEN);
        kmemcpy(img + j->pos, &d, sizeof(d)); j->pos += sizeof(d);
        j->length = d.size; j->copied = 0; ++j->header.count;
        j->phase = SYNC_SERIALIZE_DATA; return FS_SYNC_MORE;
    }
    case SYNC_SERIALIZE_DATA: {
        unsigned n = j->length - j->copied;
        if (n > FS_SYNC_BYTE_BUDGET) n = FS_SYNC_BYTE_BUDGET;
        kmemcpy(img + j->pos, fs_data(j->node) + j->copied, n);
        j->pos += n; j->copied += n;
        if (j->copied == j->length) { ++j->node; j->phase = SYNC_SERIALIZE_NODE; }
        return FS_SYNC_MORE;
    }
    case SYNC_VALIDATE_STAGE:
    case SYNC_VALIDATE_COMMITTED: {
        int result = validator_step(&j->validator, FS_SYNC_GRAPH_BUDGET);
        if (result < 0) return sync_finish(-1);
        if (result) return FS_SYNC_MORE;
        if (j->phase == SYNC_VALIDATE_COMMITTED) return sync_finish(0);
        sync_crc_begin(); j->phase = SYNC_CRC_STAGE; return FS_SYNC_MORE;
    }
    case SYNC_CRC_STAGE:
        if (!sync_crc_step()) return FS_SYNC_MORE;
        j->header.sum = ~j->crc; j->header.header_sum = crc32(&j->header, 24);
        j->sectors = (j->header.bytes + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE;
        kmemset(img + j->pos, 0, (1 + j->sectors) * FS_SECTOR_SIZE - j->pos);
        j->phase = SYNC_WRITE_PAYLOAD; return FS_SYNC_MORE;
    case SYNC_WRITE_PAYLOAD:
        return sync_io(1, j->lba + 1, img + FS_SECTOR_SIZE, j->sectors, SYNC_FLUSH_PAYLOAD);
    case SYNC_FLUSH_PAYLOAD:
        return sync_io(2, 0, 0, 0, SYNC_READ_PAYLOAD);
    case SYNC_READ_PAYLOAD:
        sync_crc_begin();
        return sync_io(0, j->lba + 1, img + FS_SECTOR_SIZE, j->sectors, SYNC_CRC_READBACK);
    case SYNC_CRC_READBACK:
        if (!sync_crc_step()) return FS_SYNC_MORE;
        if (~j->crc != j->header.sum) return sync_finish(-1);
        kmemset(img, 0, FS_SECTOR_SIZE); kmemcpy(img, &j->header, sizeof(j->header));
        j->phase = SYNC_WRITE_HEADER; return FS_SYNC_MORE;
    case SYNC_WRITE_HEADER:
        return sync_io(1, j->lba, img, 1, SYNC_FLUSH_HEADER);
    case SYNC_FLUSH_HEADER:
        return sync_io(2, 0, 0, 0, SYNC_READ_HEADER);
    case SYNC_READ_HEADER:
        return sync_io(0, j->lba, img, 1, SYNC_CHECK_HEADER);
    case SYNC_CHECK_HEADER:
        kmemcpy(&j->verified, img, sizeof(j->verified));
        /* Compare every semantic header field, not just the generation/CRC.
         * This also validates magic, version, count, length and header CRC. */
        if (j->verified.magic != j->header.magic || j->verified.version != j->header.version ||
            j->verified.count != j->header.count || j->verified.bytes != j->header.bytes ||
            j->verified.sum != j->header.sum || j->verified.generation != j->header.generation ||
            j->verified.header_sum != j->header.header_sum ||
            j->verified.header_sum != crc32(&j->verified, 24)) return sync_finish(-1);
        j->phase = SYNC_READ_COMMITTED; return FS_SYNC_MORE;
    case SYNC_READ_COMMITTED:
        sync_crc_begin();
        return sync_io(0, j->lba + 1, img + FS_SECTOR_SIZE, j->sectors, SYNC_CRC_COMMITTED);
    case SYNC_CRC_COMMITTED:
        if (!sync_crc_step()) return FS_SYNC_MORE;
        if (~j->crc != j->header.sum || validator_begin(&j->validator, &j->verified, img) < 0)
            return sync_finish(-1);
        j->phase = SYNC_VALIDATE_COMMITTED; return FS_SYNC_MORE;
    case SYNC_IO: {
        enum AtaProgress progress = ata_poll(FS_SYNC_BYTE_BUDGET / FS_SECTOR_SIZE);
        if (progress == ATA_PROGRESS_WAIT) return FS_SYNC_WAIT;
        if (progress == ATA_PROGRESS_MORE) return FS_SYNC_MORE;
        if (progress != ATA_PROGRESS_DONE) return sync_finish(-1);
        j->phase = j->next_phase; return FS_SYNC_MORE;
    }
    case SYNC_FLOPPY: {
        int result = save_snapshot();
        /* Legacy save has already selected its target and generation. */
        j->target = active_slot; j->header.generation = generation;
        return sync_finish(result);
    }
    default: return sync_finish(-1);
    }
}

int fs_save_disk(void) { return fs_sync(); }
int fs_needs_sync(void) { return fs_touched; }
int fs_sync(void) {
    /* A clean FS needs no new job, but never report success over a pending one. */
    if (!fs_sync_busy() && !fs_touched) return 0;
    if (!fs_sync_busy() && !writable) {
        last_sync_attempt = timer_ticks(); sync_account(-1); return -1;
    }
    FsSyncTicket ticket;
    int result = sync_record(1, &ticket);
    if (result < 0) return result;
    if (fs_sync_busy()) sync_job.subscribers |= 2u;
    else sync_start(1);
    do {
        (void)fs_sync_step();
        result = fs_sync_result(ticket);
        if (result == FS_SYNC_PENDING) fs_background_poll();
    } while (result == FS_SYNC_PENDING);
    (void)fs_sync_release(ticket);
    return result;
}
void fs_autosync(void) {
    if (!writable || !fs_touched || fs_sync_busy() || sync_results[0].occupied || sync_failures >= 3) return;
    if (sync_failures && (unsigned)(timer_ticks() - last_sync_attempt) < 5 * TIMER_HZ) return;
    if (!data_backend) { (void)fs_sync(); return; }
    FsSyncTicket ticket;
    if (!fs_sync_request(&ticket)) sync_autosave = 1;
}

const char *fs_storage_status(void) {
    if (data_problem == 2) return "Large disk needs 128 MiB RAM; boot files read-only";
    if (data_problem) return "Data disk unavailable; boot files read-only";
    if (!writable) return "Disk protected; changes in RAM only";
    if (fs_sync_busy()) return "Saving disk snapshot; file changes paused";
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
