#define FEATURE_TEST
#include "../src/kernel.c"
static void draft_check(int ok,const char *why){if(!ok)panic(why);}
static int same_bytes(const char *a,const char *b,int length){for(int i=0;i<length;i++)if(a[i]!=b[i])return 0;return 1;}
static void fill_doc(int slot,int length,int alphabet){
    context_set(slot);edit_clear();
    for(int i=0;i<length;i++)edit_buf[i]=(char)((alphabet?'A':'0')+i%(alphabet?26:10));
    edit_buf[length]=0;edit_len=length;edit_caret=alphabet?length:12345;
}
void feature_test(void){
    int phase=fs_find_child(fs_root(),"recovery-phase");
    if(phase>=0){
        draft_check(wins[0].open&&wins[1].open&&wins[0].kind==WK_EDIT&&wins[1].kind==WK_EDIT,"draft windows restored");
        for(int slot=0;slot<2;slot++){
            context_set(slot);int length=65535-slot;
            draft_check(edit_len==length,"maximum draft length preserved");
            for(int i=0;i<length;i++)draft_check(edit_buf[i]==(char)((slot?'0':'A')+i%(slot?10:26)),"every draft byte preserved");
            draft_check(edit_buf[length]==0&&edit_caret==(slot?12345:length),"draft caret and terminator");
        }
        platform_log("SESSION-DRAFT-REBOOT-PASS\n");return;
    }
    draft_check(fs_file_limit()>65535,"draft test needs data disk");
    phase=fs_create(fs_root(),"recovery-phase");draft_check(phase>=0,"phase marker");
    open_edit();kstrcpy(edit_buf,"first draft");edit_len=11;
    open_edit();kstrcpy(edit_buf,"second draft");edit_len=12;
    session_save();draft_check(!session_status[0],"initial session save");
    int prefs=fs_find_child(fs_root(),"prefs");
    int first=fs_find_child(prefs,"draft0.txt"),second=fs_find_child(prefs,"draft1.txt");
    draft_check(first>=0&&second>=0,"draft files created");
    int fillers[8],count=0;char *bytes=(char *)DOWNLOAD_BASE;kmemset(bytes,'x',FS_FILE_MAX);
    while(fs_used_bytes()<fs_capacity()){
        unsigned remaining=fs_capacity()-fs_used_bytes();if(remaining>FS_FILE_MAX)remaining=FS_FILE_MAX;
        char name[]="filler0";name[6]+=(char)count;
        int id=fs_create(fs_root(),name);draft_check(id>=0&&count<8,"filler file slot");
        draft_check(fs_write(id,bytes,(int)remaining)==(int)remaining,"fill exact storage capacity");fillers[count++]=id;
    }
    int before_nodes=fs_node_count();
    context_set(0);kstrcpy(edit_buf,"changed now");edit_len=11;
    context_set(1);kmemset(edit_buf,'b',100);edit_len=100;edit_buf[100]=0;
    session_save();draft_check(session_status[0],"full volume reports session failure");
    draft_check(fs_node_count()==before_nodes,"failed session adds no nodes");
    draft_check(fs_size(first)==11&&same_bytes(fs_data(first),"first draft",11),"first draft unchanged after preflight failure");
    draft_check(fs_size(second)==12&&same_bytes(fs_data(second),"second draft",12),"second draft unchanged after preflight failure");
    context_set(0);kmemset(edit_buf,'a',20);edit_len=20;edit_buf[20]=0;
    context_set(1);kstrcpy(edit_buf,"bbb");edit_len=3;
    session_save();draft_check(!session_status[0],"shrinking draft funds another draft at full capacity");
    draft_check(fs_size(first)==20&&fs_size(second)==3&&fs_used_bytes()==fs_capacity(),"replacement drafts conserve full volume bytes");
    for(int i=0;i<count;i++)fs_delete(fillers[i]);
    fill_doc(0,65535,1);fill_doc(1,65534,0);win_focus(0);
    session_save();draft_check(!session_status[0],"maximum draft session save");
    draft_check(fs_size(first)==65535&&fs_size(second)==65534,"maximum draft sizes on disk");
    draft_check(fs_sync()==0,"maximum draft sync");
    platform_log("SESSION-DRAFT-PASS\n");
}
