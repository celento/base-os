#define FEATURE_TEST
#include "../src/kernel.c"
static void editor_check(int ok,const char *why){if(!ok)panic(why);}
static void editor_type(const char *s){while(*s){key_sc=0;key_char=*s++;handle_key();}}
void feature_test(void){
    int docs=fs_find_child(fs_root(),"Documents");
    int saved=fs_find_child(docs,"large-editor.txt");
    if(saved>=0){
        open_fs_file(saved);editor_check(edit_len==50000,"large editor reboot length");
        editor_check(edit_buf[0]=='b'&&edit_buf[49999]=='b',"large editor reboot contents");
        platform_log("EDITOR-LARGE-REBOOT-PASS\n");return;
    }
    editor_check(fs_file_limit()>65536,"editor test needs data volume");
    open_edit();editor_type("One fish\ntwo FISH\nred fish\n");
    ctrl_down=1;key_sc=0x21;key_char=0;handle_key();ctrl_down=0;
    editor_check(edit_search.open&&edit_search.focus,"Ctrl+F opens search");editor_type("fish");
    key_sc=KEY_ENTER;key_char=0;handle_key();editor_check(edit_sel_a==4&&edit_sel_b==8,"find wraps");
    key_sc=0x3d;handle_key();editor_check(edit_sel_a==13,"F3 next match");
    shift_down=1;handle_key();shift_down=0;editor_check(edit_sel_a==4,"Shift+F3 previous");
    ctrl_down=1;key_sc=0x23;handle_key();ctrl_down=0;
    key_sc=KEY_TAB;handle_key();editor_type("bird");
    ctrl_down=shift_down=1;key_sc=KEY_ENTER;key_char=0;handle_key();ctrl_down=shift_down=0;
    editor_check(!kstrcmp(edit_buf,"One bird\ntwo bird\nred bird\n"),"replace all");
    ctrl_down=1;key_sc=0x2c;handle_key();ctrl_down=0;
    editor_check(!kstrcmp(edit_buf,"One fish\ntwo FISH\nred fish\n"),"replace all single undo");
    ctrl_down=1;key_sc=0x15;handle_key();ctrl_down=0;editor_check(edit_buf[4]=='b',"replace redo");
    key_sc=KEY_ESC;handle_key();editor_check(!edit_search.open&&front_kind()==WK_EDIT,"Escape closes search only");
    ctrl_down=1;key_sc=0x47;handle_key();ctrl_down=0;shift_down=1;key_sc=KEY_RIGHT;handle_key();handle_key();shift_down=0;
    editor_check(edit_sel_a==0&&edit_sel_b==2,"keyboard selection");
    edit_clear();kmemset(edit_buf,'a',50000);edit_buf[50000]=0;edit_len=50000;edit_caret=edit_len;
    edit_search_open(1);kstrcpy(edit_search.text[0],"a");kstrcpy(edit_search.text[1],"b");edit_replace_everywhere();
    editor_check(edit_len==50000&&edit_buf[49999]=='b',"50 KB replace all");
    edit_undo(0);editor_check(edit_buf[49999]=='a',"50 KB undo");edit_undo(1);
    fm_set_cwd(docs);editor_check(edit_write_named("large-editor.txt"),"50 KB save");
    editor_check(fs_size(edit_file)==50000,"50 KB file size");
    int editor=win_front();open_edit();editor_type("Independent second document");
    editor_check(window_state[editor].doc.len==50000,"large independent document");
    win_focus(editor);session_save();editor_check(!session_status[0]&&fs_sync()==0,"50 KB session persistence");
    edit_search_open(1);kstrcpy(edit_search.text[0],"b");kstrcpy(edit_search.text[1],"sample");
    render_desktop_frame();flip_vga();platform_log("EDITOR-LARGE-PASS\n");
}
