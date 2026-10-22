#include "apps/file_view.h"

static unsigned char fold(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 'a' - 'A') : c;
}

static int compare_text(const char *a, const char *b, int folded) {
    for (int i = 0;; ++i) {
        unsigned char ac = (unsigned char)a[i], bc = (unsigned char)b[i];
        if (folded) { ac = fold(ac); bc = fold(bc); }
        if (ac != bc) return ac < bc ? -1 : 1;
        if (!ac) return 0;
    }
}

int file_view_matches(const char *name, const char *filter) {
    if (!filter[0]) return 1;
    for (int start = 0; name[start]; ++start) {
        int i = 0;
        while (filter[i] && name[start + i] &&
               fold((unsigned char)name[start + i]) == fold((unsigned char)filter[i])) ++i;
        if (!filter[i]) return 1;
    }
    return 0;
}

const char *file_view_type(int id) {
    if (!fs_valid(id)) return "File";
    if (fs_is_dir(id)) return "Folder";
    if (fs_is_app(id)) return "Application";
    const char *name = fs_name(id), *extension = 0;
    for (int i = 1; name[i]; ++i)
        if (name[i] == '.') extension = name + i + 1;
    return extension && *extension ? extension : "File";
}

static int compare_unsigned(unsigned a, unsigned b) {
    return a < b ? -1 : a > b ? 1 : 0;
}

int file_view_compare(int a, int b, const FileViewOptions *options) {
    int ad = fs_is_dir(a), bd = fs_is_dir(b), order = 0;
    if (ad != bd) return ad ? -1 : 1;
    int sort = options ? options->sort : FILE_VIEW_NAME;
    if (sort == FILE_VIEW_TYPE)
        order = compare_text(file_view_type(a), file_view_type(b), 1);
    else if (sort == FILE_VIEW_SIZE)
        order = compare_unsigned((unsigned)fs_size(a), (unsigned)fs_size(b));
    else if (sort == FILE_VIEW_MODIFIED)
        order = compare_unsigned(fs_modified(a), fs_modified(b));
    else {
        order = compare_text(fs_name(a), fs_name(b), 1);
        if (!order) order = compare_text(fs_name(a), fs_name(b), 0);
    }
    if (order) return options && options->descending ? -order : order;
    order = compare_text(fs_name(a), fs_name(b), 1);
    if (!order) order = compare_text(fs_name(a), fs_name(b), 0);
    return order ? order : compare_unsigned((unsigned)a, (unsigned)b);
}

static void swap_entry(int *ids, unsigned *identities, int a, int b) {
    int id = ids[a]; ids[a] = ids[b]; ids[b] = id;
    unsigned identity = identities[a]; identities[a] = identities[b]; identities[b] = identity;
}
static void sift_down(int *ids, unsigned *identities, int root, int count,
                      const FileViewOptions *options) {
    for (;;) {
        int child = 2 * root + 1;
        if (child >= count) return;
        if (child + 1 < count && file_view_compare(ids[child], ids[child + 1], options) < 0) ++child;
        if (file_view_compare(ids[root], ids[child], options) >= 0) return;
        swap_entry(ids, identities, root, child);
        root = child;
    }
}

int file_view_build(int directory, const FileViewOptions *options,
                    int *ids, unsigned *identities, int capacity, int *total) {
    int listed = fs_child_count(directory), count = 0, all = 0;
    int *raw = kmalloc((uint32_t)(listed ? listed : 1) * sizeof *raw);
    int size = raw ? fs_list(directory, raw, listed) : 0;
    for (int i = 0; i < size; ++i) {
        int id = raw[i];
        if(options&&(id==options->hidden[0]||id==options->hidden[1]))continue;
        ++all;
        if(count>=capacity)continue;
        if (options && !file_view_matches(fs_name(id), options->filter)) continue;
        ids[count] = id;
        identities[count++] = fs_identity(id);
    }
    kfree(raw);
    /* Heap sort: the order is total (ties end on node ID), so it needs no
     * stability, and large folders sort in n log n without an allocator. */
    for (int i = count / 2 - 1; i >= 0; --i) sift_down(ids, identities, i, count, options);
    for (int end = count - 1; end > 0; --end) {
        swap_entry(ids, identities, 0, end);
        sift_down(ids, identities, 0, end, options);
    }
    if(total)*total=all;
    return count;
}

int file_view_valid(int directory, int id, unsigned identity) {
    return identity && fs_is_dir(directory) && fs_valid(id) &&
           fs_parent(id) == directory && fs_identity(id) == identity;
}

int file_view_find(const int *ids, const unsigned *identities, int count,
                   int id, unsigned identity) {
    if (id < 0 || !identity) return -1;
    for (int i = 0; i < count; ++i)
        if (ids[i] == id && identities[i] == identity) return i;
    return -1;
}
