/* Ordinary shared shell policies, using exact production helper extraction. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "app_view.h"
#define MAX_WIN 8
#define TITLE_H 32
#define NATIVE_TOOLBAR_H 30
#define NATIVE_CLIENT_PAD 8
#define EDIT_CHAR_W 8
#define EDIT_LINE_H 10
#define KEY_ENTER 28
#define KEY_BACKSPACE 14
#define KEY_ESC 1
#define WK_HELLO 0
#define WK_TERM 11
#define WK_PROPERTIES 19
#define WK_SPREADSHEET 23
#define WK_NATIVE 24
typedef struct {int kind,x,y,w,h,z,open,seq,min;} Win;
static Win wins[MAX_WIN];
static int native_output_scroll[MAX_WIN],dirty,ctrl_down,alt_down,key_sc;
static char key_char;
static struct {unsigned buttons;} device_input;
static unsigned clock_now=42;
static int live[8],visible[8],stopped,key_slot=-1,last_key,term_scrolled,closed=-1,refreshes;
static const char *output_lines[]={"01234567890123456789012345678901234567890123456789012345678901234567890123456789","","Final output row"};
static int kstrlen(const char *s){return (int)strlen(s);}
int app_view_running(int slot){return live[slot];}
int app_view_key(int slot,int key){key_slot=slot;last_key=key;return 1;}
void app_view_stop(int slot){stopped=slot+1;live[slot]=0;}
int app_view_output_visible(int slot){return visible[slot];}
void app_view_output_show(int slot,int value){visible[slot]=value;}
int app_view_output_count(int slot){(void)slot;return 3;}
const char *app_view_output_line(int slot,int row){(void)slot;return output_lines[row];}
static void term_scroll(int delta){term_scrolled=delta;}
static unsigned timer_ticks(void){return clock_now;}
void native_ui_refresh(unsigned ticks,unsigned buttons){assert(ticks==42&&buttons==3);refreshes++;}
static void win_close(int slot){closed=slot;wins[slot].open=0;}
#include "native_window_shell_kernel.inc"
int main(void){
    wins[2]=(Win){.kind=WK_NATIVE,.w=240,.h=100,.open=1};
    assert(native_output_columns(&wins[2])==27);
    assert(native_output_rows(&wins[2])==2);
    assert(native_output_total(2)==5); /* Entire bounded rows remain readable. */
    native_output_scroll_by(2,100);assert(!native_output_scroll[2]);
    device_input.buttons=3;native_output_toggle(2);assert(visible[2]&&refreshes==1&&dirty);
    native_output_scroll_by(2,100);assert(native_output_scroll[2]==3);
    native_output_scroll_by(2,-100);assert(!native_output_scroll[2]);
    native_output_toggle(2);assert(!visible[2]&&refreshes==2);
    for(int owned=0;owned<2;owned++){
        live[2]=1;ctrl_down=alt_down=0;
        key_sc=KEY_ESC;key_char=0;assert(native_task_key_dispatch(2,owned));assert(last_key==27&&closed==-1);
        key_sc=KEY_ENTER;assert(native_task_key_dispatch(2,owned));assert(last_key==13);
        key_sc=KEY_BACKSPACE;assert(native_task_key_dispatch(2,owned));assert(last_key==8);
        key_sc=30;key_char='a';assert(native_task_key_dispatch(2,owned));assert(last_key=='a'&&key_slot==2);
        alt_down=1;key_char='b';native_task_key_dispatch(2,owned);assert(last_key=='a');alt_down=0;
        ctrl_down=1;key_sc=0x2e;native_task_key_dispatch(2,owned);assert(stopped==3&&!live[2]);ctrl_down=0;
    }
    key_sc=0x49;live[2]=1;visible[2]=1;native_task_key_dispatch(2,1);assert(native_output_scroll[2]==3&&!term_scrolled);
    native_task_key_dispatch(2,0);assert(term_scrolled==8);
    live[2]=0;key_sc=KEY_ESC;last_key=0;
    assert(native_task_key_dispatch(2,1)&&!last_key&&closed==-1);assert(!native_task_key_dispatch(2,0));
    native_window_complete((AppViewUpdate){2,APP_VIEW_LIFECYCLE});assert(closed==-1);
    native_window_complete((AppViewUpdate){2,APP_VIEW_AUTO_CLOSE});assert(closed==2&&!wins[2].open);
    closed=-1;wins[2].kind=WK_TERM;wins[2].open=1;native_window_complete((AppViewUpdate){2,APP_VIEW_AUTO_CLOSE});assert(closed==-1);
    assert(session_window_kind(WK_HELLO)&&session_window_kind(WK_TERM)&&session_window_kind(WK_SPREADSHEET));
    assert(!session_window_kind(-1)&&!session_window_kind(WK_PROPERTIES)&&!session_window_kind(WK_NATIVE)&&!session_window_kind(25));
    puts("Owned shell: shared byte keys, forced Stop, deferred completion, bounded Output reflow/scroll and persistence exclusion passed.");
}
