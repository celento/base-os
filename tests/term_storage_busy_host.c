/* Actual Terminal plus filesystem lease, ordinary commands and valid native images. */
#define TERM_TASK_FIXTURE_ONLY
#include "term_task_info_host.c"
static void finish_snapshot(FsSyncTicket ticket) {
    unsigned steps = 0;
    while (fs_sync_busy()) { assert(fs_sync_step() != FS_SYNC_IDLE); assert(++steps < 100000); }
    assert(fs_sync_result(ticket) == 0 && fs_sync_release(ticket) == 0);
}
static const char *last_line(void) { return term_get(term_count()-1); }
int main(void) {
    reset(); memset(data_disk, 0, sizeof data_disk);
    unsigned *marker = (unsigned *)data_disk;
    marker[0] = DATA_MARKER_MAGIC; marker[1] = DATA_MARKER_VERSION;
    marker[2] = DATA_DISK_SECTORS; marker[3] = DATA_SLOT_SECTORS;
    marker[4] = DATA_FIRST_LBA; marker[5] = DATA_SECOND_LBA; marker[6] = crc32(marker, 24);
    data_present = 1; assert(!fs_init() && fs_load_disk() == FS_LOAD_BLANK);
    assert(!fs_empty_dir(0));
    int app = executable("counter.bex"), text = fs_create(0, "note.txt");
    assert(fs_write(text, "A readable note", 15) == 15);
    int commands = fs_create(0, "write.script");
    const char *script_text = "echo Before mutation\ntouch /from-script.txt\necho After mutation\n";
    assert(fs_write(commands, script_text, strlen(script_text)) == (int)strlen(script_text));
    int read_script = fs_create(0, "read.script");
    assert(fs_write(read_script, "cat /note.txt\n", 14) == 14);
    unsigned identity = fs_identity(text); int count = fs_node_count();
    term_select(0); term_reset();
    FsSyncTicket ticket; assert(!fs_sync_request(&ticket) && fs_sync_busy());
    const char *writes[] = {"touch /new.txt", "mkdir /folder", "rm /note.txt", "run /write.script"};
    for (unsigned i = 0; i < sizeof writes / sizeof *writes; ++i) {
        command(writes[i]); assert(!strcmp(last_line(), "Disk is saving; retry shortly."));
        assert(fs_node_count() == count && fs_identity(text) == identity && fs_size(text) == 15);
        assert(!memcmp(fs_data(text), "A readable note", 15) && fs_sync_busy());
    }
    command("cat /note.txt"); assert(!strcmp(last_line(), "A readable note"));
    command("run /read.script"); assert(!strcmp(last_line(), "A readable note"));
    command("pwd"); assert(!strcmp(last_line(), "/"));
    command("ls"); assert(strstr(last_line(), ".script"));
    command("stat /note.txt"); assert(strstr(last_line(), "Modified"));
    command("df"); assert(fs_sync_busy() && !strstr(last_line(), "synchronized"));
    command("help"); assert(strstr(last_line(), "history"));
    command("start /counter.bex");
    assert(term_task_running(0) && !strcmp(last_line(), "Native task started. Ctrl+C stops; close ends it."));
    assert(fs_valid(app) && fs_sync_busy()); term_task_stop(0);
    command("exec /counter.bex"); assert(!strcmp(last_line(), "Program finished."));
    assert(fs_sync_busy()); finish_snapshot(ticket);
    command("touch /new.txt"); assert(fs_find_child(0, "new.txt") >= 0);
    command("mkdir /folder"); assert(fs_is_dir(fs_find_child(0, "folder")));
    command("run /write.script"); assert(fs_find_child(0, "from-script.txt") >= 0);
    assert(!strcmp(last_line(), "After mutation"));
    command("rm /note.txt"); assert(!fs_valid(text));
    command("rm /missing.txt"); assert(strstr(last_line(), "Error: check command"));
    assert(!fs_sync());
    puts("Terminal storage lease: only writes report busy, script errors propagate, reads/native launch and retries work");
    return 0;
}
