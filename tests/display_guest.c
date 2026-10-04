#define FEATURE_TEST
#include "../src/kernel.c"
static void check_display(int ok, const char *why) { if (!ok) panic(why); }
void feature_test(void) {
    if (fs_find_child(fs_root(), "display-test") >= 0) {
        check_display(fb_w == 1024 && fb_h == 768, "saved resolution was not restored");
        platform_log("DISPLAY-PERSISTENCE-PASS\n");
        return;
    }
    check_display(display_available(), "QEMU standard VGA not found");
    open_edit();
    const char *text = "This document stays open across display changes.\n";
    while (*text) edit_insert(*text++);
    win_open(WK_SETTINGS);
    for (int mode = 0; mode < DISPLAY_MODE_COUNT; ++mode) {
        check_display(display_request(mode), "display request rejected");
        check_display(fb_w == display_mode(mode)->width && fb_h == display_mode(mode)->height,
                      "display geometry mismatch");
        check_display(display_pending, "missing display confirmation");
        key_sc = KEY_ENTER; key_char = 0; handle_key();
        check_display(!display_pending, "Enter did not keep resolution");
        check_display(window_state[0].doc.len > 0, "editor draft lost during mode switch");
        render_desktop_frame(); flip_vga();
        platform_log("DISPLAY-MODE-"); kprint_uint(mode); platform_log("\n");
        timer_delay(TIMER_HZ * 2);
    }
    check_display(display_request(0), "revert test request");
    key_sc = KEY_ESC; handle_key();
    check_display(fb_w == 1280 && fb_h == 800 && !display_pending, "Escape did not revert");
    check_display(display_request(1), "saved mode request"); display_keep();
    int marker = fs_create(fs_root(), "display-test"); check_display(marker >= 0, "display marker");
    session_save(); check_display(fs_sync() == 0, "display preference save");
    check_display(display_request(0), "timeout request");
    platform_log("DISPLAY-TIMEOUT-WAIT\n");
    /* Return to the actual desktop loop for its unmodified 15-second timeout. */
}
