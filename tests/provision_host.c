/* First-boot provisioning: apps in /Applications, programs and scripts in
 * /Scripts, and the one-time move from the old root-and-/Programs layout. */
#define main filesystem_fixture_main
#include "fs_host.c"
#undef main
void platform_poll(void) {}
#include "../src/lib/example_docs.c"
#include "../src/lib/example_sheet.c"
#include "../src/lib/provision.c"

static char scratch[0x20000];

static int at(const char *path) { return fs_resolve(fs_root(), path); }
static int text_file(int parent, const char *name, const char *text) {
    int id = fs_create(parent, name);
    assert(id >= 0 && fs_write(id, text, (int)strlen(text)) == (int)strlen(text));
    return id;
}
static int root_apps(void) {
    int n = 0;
    for (int id = 1; id < fs_node_slots(); id++) n += fs_is_app(id) && fs_parent(id) == fs_root();
    return n;
}
static void every_app_installed(void) {
    int apps = at("/Applications");
    assert(fs_is_dir(apps) && fs_holds_app(apps) && !root_apps());
    for (unsigned i = 0; i < sizeof system_apps / sizeof *system_apps; i++)
        assert(fs_is_app(fs_find_child(apps, system_apps[i])));
    /* DOS program launchers are plain files holding the path DOSBox starts. */
    for (unsigned i = 0; i < sizeof dos_app_launchers / sizeof *dos_app_launchers; i++) {
        int id = fs_find_child(apps, dos_app_launchers[i].name);
        assert(id >= 0 && !fs_is_app(id) && !fs_is_dir(id));
        assert(fs_size(id) == (int)strlen(dos_app_launchers[i].target));
        assert(!memcmp(fs_data(id), dos_app_launchers[i].target, strlen(dos_app_launchers[i].target)));
    }
}
/* The layout from before Applications: apps at the root, programs in /Programs. */
static void old_layout(void) {
    assert(!fs_empty_dir(fs_root()));
    fs_mkdir(fs_root(), "Documents");
    fs_create_app(fs_root(), "Writer");
    fs_create_app(fs_root(), "Calculator");
    int programs = fs_mkdir(fs_root(), "Programs");
    text_file(programs, "hello.bex", "my own build");
    text_file(programs, "tool.sh", "echo mine\n");
    fs_mkdir(programs, "Projects");
}

int main(void) {
    /* A fresh volume. */
    reset();
    provision_system_files(scratch, sizeof scratch);
    every_app_installed();
    assert(fs_find_child(fs_root(), "Programs") < 0 && fs_is_dir(at("/Scripts")));
    assert(fs_size(at("/Scripts/demo.sh")) > 0 && fs_size(at("/Scripts/docstats.bex")) > 0);
    assert(at("/Programs/demo.sh") == at("/Scripts/demo.sh"));
    int count = fs_node_count();
    provision_system_files(scratch, sizeof scratch);
    assert(fs_node_count() == count);

    /* An old volume moves over once and keeps its own files. */
    reset();
    old_layout();
    int writer = fs_find_child(fs_root(), "Writer"), hello = at("/Programs/hello.bex");
    int projects = at("/Programs/Projects");
    provision_system_files(scratch, sizeof scratch);
    every_app_installed();
    assert(fs_parent(writer) == at("/Applications"));
    assert(fs_find_child(fs_root(), "Programs") < 0);
    assert(at("/Scripts/hello.bex") == hello && !strcmp(fs_data(hello), "my own build"));
    assert(at("/Scripts/Projects") == projects && !strcmp(fs_data(at("/Scripts/tool.sh")), "echo mine\n"));
    assert(at("/Programs/tool.sh") == at("/Scripts/tool.sh") && at("/Programs") == at("/Scripts"));
    char name[FS_NAME_LEN];
    assert(fs_destination(fs_root(), "/Programs/new.sh", name) == at("/Scripts") && !strcmp(name, "new.sh"));

    /* A name already taken in Scripts is never replaced: that file stays in
     * /Programs, which then keeps its own meaning. */
    reset();
    old_layout();
    int scripts = fs_mkdir(fs_root(), "Scripts");
    int kept = text_file(scripts, "tool.sh", "echo newer\n"), clash = at("/Programs/tool.sh");
    provision_system_files(scratch, sizeof scratch);
    every_app_installed();
    assert(at("/Scripts/tool.sh") == kept && !strcmp(fs_data(kept), "echo newer\n"));
    assert(fs_is_dir(at("/Programs")) && at("/Programs") != at("/Scripts"));
    assert(at("/Programs/tool.sh") == clash && fs_child_count(at("/Programs")) == 1);

    /* A duplicate app node at the root is dropped; it holds no data. */
    reset();
    provision_system_files(scratch, sizeof scratch);
    int duplicate = fs_create_app(fs_root(), "Paint");
    count = fs_node_count();
    provision_system_files(scratch, sizeof scratch);
    assert(!fs_valid(duplicate) && fs_node_count() == count - 1);
    every_app_installed();

    /* Removed app entries come back; other folders do not hold apps. */
    assert(!fs_delete(at("/Applications/Terminal")));
    provision_system_files(scratch, sizeof scratch);
    every_app_installed();
    assert(fs_holds_app(fs_root()) && !fs_holds_app(at("/Documents")) && !fs_holds_app(at("/Applications/Writer")));
    puts("provision ok");
    return 0;
}
