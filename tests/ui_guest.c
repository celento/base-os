#define FEATURE_TEST
#include "../src/kernel.c"
static void check_ui(int ok,const char *why){if(!ok){platform_log("UI-FAIL: ");panic(why);}}
static void type_ui(const char *text){while(*text){key_sc=0;key_char=*text++;handle_key();}}
static void command_ui(const char *text){while(*text)term_char(*text++);term_enter();}
void feature_test(void){
    int phase=fs_find_child(fs_root(),"test-phase");
    if(phase>=0){
        check_ui(wins[0].open&&wins[0].kind==WK_EDIT,"restore first editor");
        check_ui(wins[1].open&&wins[1].kind==WK_EDIT,"restore second editor");
        check_ui(!kstrcmp(window_state[0].doc.buf,"alpha!"),"restore unsaved alpha");
        check_ui(!kstrcmp(window_state[1].doc.buf,"beta"),"restore beta");
        check_ui(!kstrcmp(fs_data(fs_find_child(0,"alpha.txt")),"alpha"),"saved document preserved");
        check_ui(wins[0].x==2&&wins[1].x==fb_w/2,"restore snapped positions");
        check_ui(window_state[2].cwd==fs_root()&&window_state[3].cwd==fs_find_child(0,"Programs"),"restore folder paths");
        term_select(4);check_ui(term_cwd()==fs_find_child(0,"Programs"),"restore terminal cwd");
        check_ui(paint_pix[0]==17,"restore paint draft");
        platform_log("SESSION-RESTORE-PASS\n");return;
    }
    platform_log("UI documents\n");
    open_edit();check_ui(win_front()==0,"first editor");type_ui("alpha");check_ui(edit_write_named("alpha.txt"),"save first doc");type_ui("!");
    ctrl_down=1;key_sc=KEY_N;key_char=0;handle_key();ctrl_down=0;
    check_ui(win_front()==1,"new editor shortcut");type_ui("beta");
    ctrl_down=1;key_sc=0x2c;handle_key();check_ui(!kstrcmp(edit_buf,"bet"),"undo editor");key_sc=0x15;handle_key();ctrl_down=0;check_ui(!kstrcmp(edit_buf,"beta"),"redo editor");
    check_ui(!kstrcmp(window_state[0].doc.buf,"alpha!"),"independent documents");
    namedlg_open(0,"test.txt");key_sc=KEY_TAB;handle_key();check_ui(name_focus==1,"save dialog focus");key_sc=KEY_ENTER;handle_key();check_ui(!name_dlg,"save cancel keyboard");
    key_sc=0x44;handle_key();check_ui(open_menu==MENU_FILE,"F10 menu");key_sc=KEY_RIGHT;handle_key();check_ui(open_menu==MENU_EDITM,"menu navigation");key_sc=KEY_ESC;handle_key();
    start_open_dialog(0);key_sc=KEY_TAB;handle_key();check_ui(pick_focus==1,"open focus");key_sc=KEY_ENTER;handle_key();check_ui(!open_dlg,"open cancel keyboard");
    mouse_x=wins[1].x+wins[1].w-2;mouse_y=wins[1].y+wins[1].h-2;mouse_left=1;int oldw=wins[1].w;
    handle_click();check_ui(resizing_win==1,"resize mouse hit");mouse_x+=20;win_resize_tick();check_ui(wins[1].w==oldw+20,"resize edge drag");mouse_left=0;resizing_win=-1;
    platform_log("UI arranging\n");
    win_arrange(0,1);win_arrange(1,2);check_ui(wins[0].w==fb_w/2-4&&wins[1].x==fb_w/2,"snap");
    int width=wins[1].w;win_arrange(1,0);check_ui(!wins[1].maximized,"restore size");win_arrange(1,2);check_ui(wins[1].w==width,"resnap");
    platform_log("UI folders\n");
    open_files(fs_root());open_files(fs_find_child(0,"Programs"));check_ui(window_state[2].cwd==0&&window_state[3].cwd!=0,"independent folders");
    mouse_x=wins[win_front()].x+45;mouse_y=files_list_y(wins[win_front()].y)+ROW_H+8;
    handle_files_click(wins[win_front()].x,wins[win_front()].y,wins[win_front()].w,wins[win_front()].h);
    check_ui(fm_selected==1,"file rows match new header and spacing");
    mouse_x=40;mouse_y=TASKBAR_Y+TASKBAR_H/2;handle_click();
    check_ui(launcher_on,"Apps button opens launcher");launcher_close();
    platform_log("UI properties\n");
    properties_id=fs_find_child(0,"alpha.txt");int prop=win_open(WK_PROPERTIES);draw_ui();win_close(prop);
    platform_log("UI terminal\n");
    open_term();command_ui("cd /Programs");command_ui("run demo.sh");command_ui("cd /Programs");command_ui("basic demo.bas");check_ui(term_canvas()!=0,"BASIC canvas");
    platform_log("UI native\n");
    command_ui("exec hello.bex");check_ui(!kstrcmp(term_get(term_count()-1),"Program finished."),"native terminal execution");
    open_term();command_ui("echo second terminal");term_select(4);check_ui(term_cwd()==fs_find_child(0,"Programs"),"independent terminal cwd");context_set(win_front());
    platform_log("UI paint\n");
    open_paint();paint_init();history_record(&paint_history,paint_pix);paint_pix[0]=17;paint_undo(0);check_ui(paint_pix[0]!=17,"paint undo");paint_undo(1);check_ui(paint_pix[0]==17,"paint redo");
    int extra=win_open(WK_CLOCK);taskbar_layout();check_ui(tb_n==MAX_WIN,"all taskbar slots");
    for(int t=0;t<tb_n;t++){
        check_ui(tb_x[t]>=112&&tb_x[t]+tb_w[t]<=fb_w-8,"taskbar remains inside screen");
        if(t)check_ui(tb_x[t]>=tb_x[t-1]+tb_w[t-1]+6,"taskbar targets do not overlap");
    }
    win_close(extra);
    platform_log("UI session\n");
    win_focus(0);session_save();check_ui(!session_status[0],"session save");
    int id=fs_create(0,"test-phase");check_ui(id>=0,"phase marker");check_ui(fs_sync()==0,"session sync");
    draw_ui();flip_vga();platform_log("UI-FEATURES-PASS\n");
}
