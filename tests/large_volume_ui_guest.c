/* The existing ordinary desktop/session suite, plus a 16 MiB native stream. */
#define feature_test base_desktop_test
#include "ui_guest.c"
#undef feature_test
static int stream_result(void) { return fs_resolve(0, "/Documents/large-stream.bin"); }
void feature_test(void) {
    check_ui(fs_large_profile() && fs_file_limit() == FS_LARGE_FILE_MAX, "large desktop profile");
    int rebooted = stream_result() >= 0;
    base_desktop_test();
    if (!rebooted) {
        term_select(4); command_ui("start /Programs/large-stream.bex");
        unsigned began = timer_ticks();
        while (term_task_running(4) && timer_ticks() - began < 60 * TIMER_HZ) {
            term_task_poll(); poll_time(); platform_poll(); __asm__ volatile("hlt");
        }
        check_ui(!term_task_running(4), "large native stream did not finish");
    }
    int result = stream_result();
    check_ui(result > 0 && fs_size(result) == 16, "large native stream result absent");
    const unsigned *r = (const unsigned *)fs_data(result);
    check_ui(r[0] == 16777216 && r[1] == 2139095040u && r[2] == 4096 && r[3] == 0x4c415247,
             "large native stream result differs");
    check_ui(fs_sync() == 0, "large desktop sync");
    platform_log(rebooted ? "LARGE-DESKTOP-NATIVE-REBOOT-PASS\n" : "LARGE-DESKTOP-NATIVE-PASS\n");
}
