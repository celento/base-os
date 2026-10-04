#include "file_view.h"

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

int file_view_build(int directory, const FileViewOptions *options,
                    int *ids, unsigned *identities, int capacity, int *total) {
    int raw[FS_MAX_NODES], count = 0;
    int size = fs_list(directory, raw, FS_MAX_NODES);
    int all=0;
    if (capacity > FS_MAX_NODES) capacity = FS_MAX_NODES;
    for (int i = 0; i < size; ++i) {
        int id = raw[i];
        if(options&&(id==options->hidden[0]||id==options->hidden[1]))continue;
        ++all;
        if(count>=capacity)continue;
        if (options && !file_view_matches(fs_name(id), options->filter)) continue;
        /* Stable insertion sort: no allocator, recursion or borrowed FS data. */
        int at = count;
        while (at && file_view_compare(id, ids[at - 1], options) < 0) {
            ids[at] = ids[at - 1];
            identities[at] = identities[at - 1];
            --at;
        }
        ids[at] = id;
        identities[at] = fs_identity(id);
        ++count;
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
