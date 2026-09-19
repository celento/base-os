/* Sample content for documentation screenshots. Only linked by gallery.py;
 * the normal kernel and the user's disk are never modified. */
#define FEATURE_TEST
#include "../src/kernel.c"

static void gallery_clear(void) {
    for(int i=0;i<MAX_WIN;i++)win_close(i);
    launcher_on=0;open_menu=-1;open_dlg=0;name_dlg=0;icon_sel=-1;
    context_set(-1);cursor_on=0;
}
static void gallery_place(int x,int y,int w,int h) {
    Win *v=&wins[win_front()];v->x=x;v->y=y;
    if(w)v->w=w;
    if(h)v->h=h;
    win_clamp(v);
}
static void gallery_command(const char *s) { while(*s)term_char(*s++);term_enter(); }
static void gallery_show(int n) {
    render_scene();flip_vga();
    /* Refresh the same completed frame after QEMU has consumed dirty VRAM.
     * A capture during its first display update can otherwise be incomplete. */
    unsigned start=timer_ticks();
    while(timer_ticks()-start<14)__asm__ volatile("hlt");
    flip_vga();
    platform_log("GALLERY ");kprint_uint(n);platform_log("\n");
    kqn=0;
    for(;;){
        drain_8042();int next=0;
        for(int i=0;i<kqn;i++)if(kq[i]==KEY_SPACE)next=1;
        kqn=0;if(next)break;
        __asm__ volatile("hlt");
    }
}
void feature_test(void) {
    gallery_clear();
    open_files(fs_root());gallery_place(285,85,530,475);
    open_edit();gallery_place(690,185,550,435);
    const char *note="Welcome to BaseOS\n\nA small operating system, built from scratch.\n\nMake yourself at home:\n\n  Open apps with Ctrl + Space\n  Snap windows with Alt + Left or Right\n  Save a document with Ctrl + S\n  Explore commands with help and man\n\nFiles and editor drafts survive a reboot.\n";
    while(*note)edit_insert(*note++);
    edit_write_named("Welcome.txt");
    gallery_show(0);

    gallery_clear();open_paint();gallery_place(350,105,760,500);
    for(int y=0;y<PAINT_H;y++)for(int x=0;x<PAINT_W;x++){
        uint8_t c=idx24(0x385878);
        if((x-115)*(x-115)+(y-25)*(y-25)<150)c=idx24(0xFFC878);
        int ridge=48+(x<65?(65-x)/2:(x-65)/3);
        if(y>ridge)c=idx24(0x667E91);
        if(y>72)c=idx24(0x183848);
        if(y>76&&y<88&&x>96-(y-76)*2&&x<128+(y-76)*2&&y%3==0)c=idx24(0xC0AC78);
        int hill=84+(x-25)*(x-25)/200;
        if(y>hill)c=idx24(0x102830);
        paint_pix[y*PAINT_W+x]=c;
    }
    paint_write_named("Mountain.bos");gallery_show(1);

    gallery_clear();
    const char *program="10 REM Colored columns\n20 LET A=0\n30 RECT A,0,10,100,A+112\n40 LET A=A+10\n50 IF A < 160 THEN 30\n60 PRINT \"Hello from Tiny BASIC\"\n70 PRINT \"Canvas: 160 x 100\"\n80 END\n";
    int dir=fs_find_child(fs_root(),"Programs"),file=fs_create(dir,"colors.bas");
    fs_write(file,program,kstrlen(program));
    open_edit();gallery_place(285,100,555,450);
    const char *q=program;while(*q)edit_insert(*q++);edit_file=file;edit_saved_ok=1;
    open_term();gallery_place(700,210,550,420);
    gallery_command("basic /Programs/colors.bas");gallery_show(2);

    gallery_clear();
    win_open(WK_2048);g2048_new(37);gallery_place(830,190,0,0);
    int moves[]={KEY_LEFT,KEY_DOWN,KEY_RIGHT,KEY_UP};
    for(int i=0;i<32;i++)g2048_key(moves[i%4]);
    open_wordle();wordle_new_game(1);gallery_place(335,85,0,0);
    const char *guesses[]={"slate","proud","crane"};
    for(int j=0;j<3;j++){for(int i=0;i<5;i++)wordle_key(WORDLE_ACT_NONE,guesses[j][i]);wordle_key(WORDLE_ACT_ENTER,0);}
    gallery_show(3);

    gallery_clear();theme_id=5;theme_apply();
    win_open(WK_CLOCK);gallery_place(890,150,0,0);
    win_open(WK_SETTINGS);gallery_place(300,110,0,0);gallery_show(4);

    gallery_clear();theme_id=0;theme_apply();launcher_open();gallery_show(5);
    platform_log("GALLERY-DONE\n");
}
