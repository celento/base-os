#define FEATURE_TEST
#include "../src/kernel.c"
void feature_test(void) {
    open_files(fs_root());open_calc();open_wordle();open_term();
    term_char('h');term_char('e');term_char('l');term_char('p');term_enter();
    open_edit();const char *text="A smooth desktop.\nRounded corners and readable text.";
    while(*text)edit_insert(*text++);
    int id=win_front();wins[id].x=350;wins[id].y=160;wins[id].w=720;wins[id].h=480;win_clamp(&wins[id]);
    unsigned began=timer_ticks();
    for(int i=0;i<20;i++){
        wins[id].x=350+i*3;win_clamp(&wins[id]);
#ifdef RENDER_OPTIMIZED
        dragging_win=id;drag_active=1;render_desktop_frame();
        gfx_present();
#else
        draw_desktop();draw_desktop_icons(-1);draw_ui();draw_menubar(-1,-1);draw_taskbar();flip_vga();
#endif
    }
    platform_log("RENDER_TICKS=");kprint_uint(timer_ticks()-began);platform_log(" FRAMES=20\n");
#ifdef RENDER_OPTIMIZED
    dragging_win=-1;drag_active=0;render_desktop_frame();
    const uint8_t *shown=(const uint8_t *)PRESENT_BASE;
    for(int i=0;i<fb_w*fb_h;i++)if(shown[i]!=fb[i]){platform_log("Mismatch x=");kprint_uint(i%fb_w);platform_log(" y=");kprint_uint(i/fb_w);platform_log("\n");panic("cached composition differs from full repaint");}
    gfx_present();
#endif
    platform_log("RENDER-PASS\n");
}
