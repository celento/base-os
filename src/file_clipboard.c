#include "file_clipboard.h"

static int mode;
static int source = -1;
static unsigned source_identity;
static char source_name[FS_NAME_LEN];
static char message[FILE_CLIPBOARD_STATUS_LEN];

/* A failed sync does not undo fs_copy/fs_move. Keep the completed result apart
 * from the clipboard selection so retrying Paste cannot repeat the mutation. */
static int pending;
static int completed_node = -1;
static unsigned completed_identity;

static void status(const char *text) {
    int i = 0;
    while (text[i] && i < FILE_CLIPBOARD_STATUS_LEN - 1) {
        message[i] = text[i];
        ++i;
    }
    message[i] = 0;
}

static void named_status(const char *prefix, const char *name) {
    status(prefix);
    int i = kstrlen(message), n = 0;
    while (name[n] && i < FILE_CLIPBOARD_STATUS_LEN - 1)
        message[i++] = name[n++];
    message[i] = 0;
}

static void clear_selection(void) {
    mode = FILE_CLIPBOARD_NONE;
    source = -1;
    source_identity = 0;
    source_name[0] = 0;
}

void file_clipboard_clear(void) {
    clear_selection();
    pending = 0;
    completed_node = -1;
    completed_identity = 0;
    message[0] = 0;
}

static int within(int node, int ancestor) {
    for (int depth = 0; node >= 0 && depth <= FS_MAX_DEPTH; ++depth) {
        if (node == ancestor) return 1;
        node = fs_parent(node);
    }
    return 0;
}

static int contains_apps(int node) {
    for (int i = 1; i < fs_node_limit(); ++i)
        if (fs_is_app(i) && within(i, node)) return 1;
    return 0;
}

int file_clipboard_set(int node, int requested_mode) {
    if (requested_mode != FILE_CLIPBOARD_COPY && requested_mode != FILE_CLIPBOARD_CUT) {
        status("Choose Copy or Cut.");
        return -1;
    }
    if (!fs_valid(node) || node == fs_root() || fs_is_app(node) || !fs_identity(node)) {
        status("Select an ordinary file or folder.");
        return -1;
    }
    if (fs_is_dir(node) && contains_apps(node)) {
        status("Folders containing apps cannot be copied or cut.");
        return -1;
    }
    file_clipboard_clear();
    mode = requested_mode;
    source = node;
    source_identity = fs_identity(node);
    kstrcpy(source_name, fs_name(node));
    named_status(mode == FILE_CLIPBOARD_COPY ? "Ready to copy: " : "Ready to move: ", source_name);
    return 0;
}

int file_clipboard_mode(void) { return mode; }
int file_clipboard_source(void) {
    return mode != FILE_CLIPBOARD_NONE && fs_valid(source) &&
           fs_identity(source) == source_identity ? source : -1;
}
unsigned file_clipboard_identity(void) { return source_identity; }
const char *file_clipboard_name(void) {
    int node = file_clipboard_source();
    if (node >= 0) kstrcpy(source_name, fs_name(node));
    return source_name;
}
int file_clipboard_pending_sync(void) { return pending; }
const char *file_clipboard_status(void) { return message; }

static const char *paste_error(int directory) {
    if (mode == FILE_CLIPBOARD_NONE) return "File clipboard is empty.";
    if (!fs_is_dir(directory)) return "Choose a destination folder.";
    /* Retry is a disk sync only, so source deletion/rename cannot turn it into
     * another copy. The completed result is identity-checked separately. */
    if (pending) return 0;
    int node = file_clipboard_source();
    if (node < 0) return "Source no longer exists; Copy or Cut again.";
    if (node == fs_root() || fs_is_app(node)) return "Select an ordinary file or folder.";
    if (fs_is_dir(node) && contains_apps(node))
        return "Folders containing apps cannot be copied or cut.";
    if (within(directory, node)) return "Cannot paste a folder into itself or its children.";
    if (mode == FILE_CLIPBOARD_CUT && fs_parent(node) != directory &&
        fs_find_child(directory, fs_name(node)) >= 0)
        return "Name already exists; rename the source or choose another folder.";
    return 0;
}

int file_clipboard_can_paste(int directory) { return paste_error(directory) == 0; }

static int selected_result(int directory) {
    return fs_valid(completed_node) && fs_identity(completed_node) == completed_identity &&
           fs_parent(completed_node) == directory ? completed_node : -1;
}

int file_clipboard_paste(int directory, int *result_node) {
    if (result_node) *result_node = -1;
    const char *error = paste_error(directory);
    if (error) {
        status(error);
        return FILE_CLIPBOARD_ERROR;
    }
    if (pending) {
        if (result_node) *result_node = selected_result(directory);
        if (fs_sync() < 0) {
            status("Changes remain in RAM; disk save still failed. No new copy made.");
            return FILE_CLIPBOARD_RAM_ONLY;
        }
        pending = 0;
        status(fs_valid(completed_node) && fs_identity(completed_node) == completed_identity
            ? "Previous paste saved; paste again to make another copy."
            : "Pending changes saved; previous paste result no longer exists.");
        return FILE_CLIPBOARD_SYNCED;
    }

    int node = file_clipboard_source();
    int operation = mode;
    if (operation == FILE_CLIPBOARD_CUT && fs_parent(node) == directory) {
        clear_selection();
        if (result_node) *result_node = node;
        status("Already in this folder; nothing moved.");
        return FILE_CLIPBOARD_NOOP;
    }

    char original_name[FS_NAME_LEN];
    int preserve_name = operation == FILE_CLIPBOARD_COPY &&
                        fs_find_child(directory, fs_name(node)) < 0;
    if (preserve_name) kstrcpy(original_name, fs_name(node));
    int result = operation == FILE_CLIPBOARD_COPY ? fs_copy(node, directory)
                                                  : fs_move(node, directory);
    if (result < 0) {
        status(operation == FILE_CLIPBOARD_COPY
            ? "Copy did not fit; check free slots, free bytes and folder depth."
            : "Move did not fit; destination path is too deep.");
        return FILE_CLIPBOARD_ERROR;
    }
    /* fs_copy is also Duplicate, so it always chooses a " copy" name. A normal
     * cross-folder Paste can restore the unused original name before our one
     * disk sync. This rename cannot fail for the current valid-volume contract:
     * the original name is valid, fs_copy already proved subtree depth fits,
     * every legal-depth path fits FS_PATH_LEN, and fs_background_poll cannot
     * mutate the destination during the copy. Keep a defensive rollback if a
     * future filesystem contract changes; never retain an unexpected copy or
     * overwrite any pre-existing node. */
    _Static_assert(FS_MAX_DEPTH * FS_NAME_LEN < FS_PATH_LEN,
                   "legal-depth paths must fit after restoring the original name");
    if (preserve_name && fs_rename(result, original_name) < 0) {
        fs_delete(result);
        status("Copy cancelled; original name could not be used. No copy retained.");
        return FILE_CLIPBOARD_ERROR;
    }
    completed_node = operation == FILE_CLIPBOARD_COPY ? result : node;
    completed_identity = fs_identity(completed_node);
    if (result_node) *result_node = completed_node;
    if (operation == FILE_CLIPBOARD_CUT) clear_selection();
    if (fs_sync() < 0) {
        pending = 1;
        status(operation == FILE_CLIPBOARD_COPY
            ? "Copied in RAM; disk save failed. Paste again retries save only."
            : "Moved in RAM; disk save failed. Cut has been cleared.");
        return FILE_CLIPBOARD_RAM_ONLY;
    }
    pending = 0;
    named_status(operation == FILE_CLIPBOARD_COPY ? "Copied and saved: " : "Moved and saved: ",
                 fs_name(completed_node));
    return FILE_CLIPBOARD_SYNCED;
}
