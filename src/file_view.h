#ifndef FILE_VIEW_H
#define FILE_VIEW_H

#include "fs.h"

/* A name cannot exceed 23 bytes, so longer filter input cannot match anything. */
#define FILE_VIEW_FILTER_LEN FS_NAME_LEN

enum {
    FILE_VIEW_NAME,
    FILE_VIEW_TYPE,
    FILE_VIEW_SIZE,
    FILE_VIEW_MODIFIED,
    FILE_VIEW_SORT_COUNT
};

typedef struct {
    char filter[FILE_VIEW_FILTER_LEN];
    int sort;
    int descending;
    int hidden[2]; /* Optional system nodes excluded before counting/filtering. */
} FileViewOptions;

/* ASCII, case-insensitive substring matching, local to one folder. */
int file_view_matches(const char *name, const char *filter);
/* Folder/application labels or a file's final extension ("File" if absent). */
const char *file_view_type(int id);
/* Directories stay first in either direction. Equal sort keys use ascending
 * name, then node ID; refreshing or reversing never randomizes ties. */
int file_view_compare(int a, int b, const FileViewOptions *options);
/* Read-only: enumerates visible children, sorts and snapshots object identity.
 * total counts all listed children before filtering, excluding the parent row. */
int file_view_build(int directory, const FileViewOptions *options,
                    int *ids, unsigned *identities, int capacity, int *total);
/* Validate both the object incarnation and its membership in this folder. */
int file_view_valid(int directory, int id, unsigned identity);
int file_view_find(const int *ids, const unsigned *identities, int count,
                   int id, unsigned identity);

#endif
