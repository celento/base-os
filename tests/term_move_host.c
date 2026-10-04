/* Ordinary mv commands against the actual Terminal and filesystem. */
#define TERM_TASK_FIXTURE_ONLY
#include "term_task_info_host.c"

typedef struct {
    int valid, parent, size, directory, app;
    unsigned identity, revision, modified, checksum;
    char name[FS_NAME_LEN];
} NodeState;
static NodeState before_nodes[FS_MAX_NODES];
static int before_count, before_dirty;
static unsigned before_bytes;
static const char *last_line(void) { return term_get(term_count()-1); }
static int output_has(const char *text) {
    for (int i=0; i<term_count(); i++) if (strstr(term_get(i),text)) return 1;
    return 0;
}
static void node_state(int id,NodeState *out) {
    memset(out,0,sizeof *out);out->valid=fs_valid(id);
    if (!out->valid) return;
    out->parent=fs_parent(id);out->size=fs_size(id);
    out->directory=fs_is_dir(id);out->app=fs_is_app(id);
    out->identity=fs_identity(id);out->revision=fs_content_revision(id);
    out->modified=fs_modified(id);strcpy(out->name,fs_name(id));
    if (out->size) out->checksum=crc32(fs_data(id),(unsigned)out->size);
}
static void remember_tree(void) {
    for (int i=0;i<FS_MAX_NODES;i++) node_state(i,&before_nodes[i]);
    before_count=fs_node_count();before_dirty=fs_needs_sync();before_bytes=fs_used_bytes();
}
static void unchanged(void) {
    assert(fs_node_count()==before_count&&fs_needs_sync()==before_dirty&&fs_used_bytes()==before_bytes);
    for (int i=0;i<FS_MAX_NODES;i++) {
        NodeState current;node_state(i,&current);
        assert(!memcmp(&current,&before_nodes[i],sizeof current));
    }
}
static int ordinary_file(int parent,const char *name) {
    const unsigned char bytes[]={0,1,2,0xff,0x80,'\n','x',0};
    int id=fs_create(parent,name);assert(id>=0);
    assert(fs_write(id,(const char *)bytes,sizeof bytes)==sizeof bytes);return id;
}
static void fresh(void) {
    reset();assert(!fs_empty_dir(fs_root()));term_select(0);term_reset();
}
static void reject(const char *text,const char *reason) {
    term_reset();remember_tree();command(text);unchanged();
    assert(output_has(reason));assert(!output_has("Moved in RAM"));
    assert(strstr(last_line(),"Error: check command"));
}
static void finish_snapshot(FsSyncTicket ticket) {
    unsigned steps=0;
    while (fs_sync_busy()) { assert(fs_sync_step()!=FS_SYNC_IDLE);assert(++steps<100000); }
    assert(!fs_sync_result(ticket)&&!fs_sync_release(ticket));
}
int main(void) {
    fresh();int docs=fs_mkdir(0,"Documents"),source=ordinary_file(0,"note.bin");
    NodeState original;node_state(source,&original);int count=fs_node_count();
    command("mv /note.bin /Documents");
    assert(!strcmp(last_line(),"Moved in RAM; not yet saved to disk."));
    assert(fs_resolve(0,"/note.bin")<0&&fs_resolve(0,"/Documents/note.bin")==source);
    assert(fs_identity(source)==original.identity&&fs_content_revision(source)==original.revision);
    assert(fs_size(source)==original.size&&crc32(fs_data(source),fs_size(source))==original.checksum);
    assert(!strcmp(fs_name(source),original.name)&&fs_node_count()==count&&fs_needs_sync());
    remember_tree();command("mv /Documents/note.bin /Documents");unchanged();
    assert(!strcmp(last_line(),"Already in that folder; nothing moved."));
    assert(!fs_sync());remember_tree();command("mv /Documents/note.bin /Documents");unchanged();

    int native=executable("runner.bex");command("mv /runner.bex /Documents");
    assert(fs_parent(native)==docs&&!fs_is_app(native));
    command("start /Documents/runner.bex");assert(term_task_running(0));term_task_stop(0);

    /* Real Calendar collision repair: folder identity and every child byte survive. */
    int prefs=fs_mkdir(0,"prefs"),folder=fs_mkdir(prefs,"calendar.v1");
    int child=ordinary_file(folder,"original.bin"),nested=fs_mkdir(folder,"nested");
    int grandchild=ordinary_file(nested,"also.bin");
    NodeState child_before,grandchild_before;node_state(child,&child_before);node_state(grandchild,&grandchild_before);
    unsigned folder_identity=fs_identity(folder),nested_identity=fs_identity(nested);
    term_set_cwd(nested);command("mv /prefs/calendar.v1 /Documents");
    assert(fs_find_child(prefs,"calendar.v1")==-1&&fs_find_child(docs,"calendar.v1")==folder);
    assert(fs_identity(folder)==folder_identity&&fs_identity(nested)==nested_identity);
    NodeState current;node_state(child,&current);assert(!memcmp(&current,&child_before,sizeof current));
    node_state(grandchild,&current);assert(!memcmp(&current,&grandchild_before,sizeof current));
    assert(term_cwd()==nested);char path[FS_PATH_LEN];fs_path(term_cwd(),path,sizeof path);
    assert(!strcmp(path,"/Documents/calendar.v1/nested"));
    command("mv ../original.bin .");assert(fs_parent(child)==nested);

    int spaced=ordinary_file(0,"old notes.bin"),archive=fs_mkdir(docs,"archive folder");
    command("mv \"/old notes.bin\" \"/Documents/archive folder\"   ");
    assert(fs_parent(spaced)==archive&&!strcmp(fs_name(spaced),"old notes.bin"));
    term_set_cwd(archive);command("mv \"old notes.bin\" ../..");assert(fs_parent(spaced)==0);
    command("mv \"/old notes.bin\" \"/Documents/archive folder\"");assert(fs_parent(spaced)==archive);
    command("help mv");assert(output_has("mv SOURCE DESTINATION_FOLDER"));
    assert(output_has("Exactly two paths")&&output_has("No overwrite or rename"));
    command("man mv");assert(output_has("not yet saved")&&output_has("until a successful disk save"));

    fresh();docs=fs_mkdir(0,"Documents");source=ordinary_file(0,"source");
    const char *syntax[]={"mv","mv /source","mv /source /Documents extra","mv /source /Documents # comment",
        "mv \"\" /Documents","mv /source \"\"","mv \"/source /Documents","mv /source \"/Documents",
        "mv \"/source\"/suffix /Documents","mv /source \"/Documents\"suffix","mv /sou\"rce /Documents",
        "mv /source /Docu\"ments","mv /source /Documents/extra operand"};
    for (unsigned i=0;i<sizeof syntax/sizeof *syntax;i++) reject(syntax[i],"Usage: mv SOURCE");
    reject("mv /missing /Documents","source must be");
    reject("mv /source /missing","existing folder");
    reject("mv /source /source","existing folder");
    reject("mv / /Documents","not root or an app");
    int app=fs_create_app(0,"Calculator");assert(app>=0);
    reject("mv /Calculator /Documents","not root or an app");
    int app_folder=fs_mkdir(0,"apps"),subfolder=fs_mkdir(app_folder,"nested");
    assert(fs_create_app(subfolder,"Clock")>=0);
    reject("mv /apps /Documents","contains an app");
    reject("mv /Documents /Documents","itself or its children");
    int inner=fs_mkdir(docs,"inner");assert(inner>=0);
    reject("mv /Documents /Documents/inner","itself or its children");
    int clash=ordinary_file(docs,"source");reject("mv /source /Documents","already exists");
    assert(fs_find_child(docs,"source 2")<0&&fs_parent(clash)==docs&&fs_parent(source)==0);
    assert(!fs_delete(clash));clash=fs_mkdir(docs,"source");
    reject("mv /source /Documents","already exists");assert(fs_is_dir(clash));

    /* Dropped input must never turn a long command into a valid shorter move. */
    assert(!fs_delete(clash));term_reset();char long_command[128];
    strcpy(long_command,"mv /source /Documents");memset(long_command+strlen(long_command),' ',80-strlen(long_command));
    strcpy(long_command+80,"extra");remember_tree();command(long_command);unchanged();
    assert(strstr(last_line(),"exceeds 80 characters")&&T.hcount==0);
    command("echo Still working");assert(!strcmp(last_line(),"Still working"));
    term_history(-1);assert(!strcmp(term_input(),"echo Still working"));term_enter();
    long_command[80]=0;command(long_command);assert(fs_parent(source)==docs);
    command("mv /Documents/source /");assert(fs_parent(source)==0);
    for (int i=0;i<81;i++) term_char(' ');
    term_backspace();remember_tree();command("mv /source /Documents");unchanged();
    assert(strstr(last_line(),"exceeds 80 characters"));
    for (int i=0;i<81;i++) term_char(' ');
    while (T.len) term_backspace();
    command("mv /source /Documents");assert(fs_parent(source)==docs);

    /* Both parser buffers accept complete paths and reject over-limit paths. */
    char boundary[FS_PATH_LEN+64];const char *tail="Documents/source /";
    int padding=FS_PATH_LEN-1-(int)strlen("Documents/source");
    memset(boundary,'/',padding);strcpy(boundary+padding,tail);
    assert(!move_command(0,boundary)&&fs_parent(source)==0);
    memset(boundary,'/',FS_PATH_LEN);strcpy(boundary+FS_PATH_LEN,"source /Documents");
    remember_tree();assert(move_command(0,boundary)<0);unchanged();
    strcpy(boundary,"/source ");memset(boundary+8,'/',FS_PATH_LEN);boundary[8+FS_PATH_LEN]=0;
    remember_tree();assert(move_command(0,boundary)<0);unchanged();

    /* Ordinary script dispatch stops at a rejected move. */
    int script_id=fs_create(0,"move.script");
    const char *script_text="mv /source /Documents extra\ntouch /must-not-exist\n";
    assert(fs_write(script_id,script_text,strlen(script_text))==(int)strlen(script_text));
    remember_tree();command("run /move.script");unchanged();assert(fs_find_child(0,"must-not-exist")<0);
    script_text="mv /source /Documents\necho Script complete\n";
    assert(fs_write(script_id,script_text,strlen(script_text))==(int)strlen(script_text));
    command("run /move.script");assert(!strcmp(last_line(),"Script complete")&&fs_parent(source)==docs);

    /* Existing fs_move depth guard remains authoritative for all descendants. */
    fresh();memset(data_disk,0,sizeof data_disk);
    unsigned *marker=(unsigned *)data_disk;
    marker[0]=DATA_MARKER_MAGIC;marker[1]=DATA_MARKER_VERSION;
    marker[2]=DATA_DISK_SECTORS;marker[3]=DATA_SLOT_SECTORS;
    marker[4]=DATA_FIRST_LBA;marker[5]=DATA_SECOND_LBA;marker[6]=crc32(marker,24);
    data_present=1;assert(!fs_init()&&fs_load_disk()==FS_LOAD_BLANK);assert(!fs_empty_dir(0));
    int deepest=0;for (int depth=0;depth<FS_MAX_DEPTH;depth++){deepest=fs_mkdir(deepest,"d");assert(deepest>0);}
    source=ordinary_file(0,"source");term_set_cwd(deepest);remember_tree();command("mv /source .");unchanged();
    assert(output_has("too deep or invalid"));
    term_set_cwd(0);docs=fs_mkdir(0,"Documents");
    FsSyncTicket ticket;assert(!fs_sync_request(&ticket)&&fs_sync_busy());
    remember_tree();command("mv /source /Documents");unchanged();
    assert(!strcmp(last_line(),"Disk is saving; retry shortly.")&&fs_sync_busy());
    command("cat /source");assert(fs_sync_busy());
    finish_snapshot(ticket);command("mv /source /Documents");assert(fs_parent(source)==docs);
    assert(!strcmp(last_line(),"Moved in RAM; not yet saved to disk."));
    assert(!fs_sync());assert(!fs_needs_sync());
    assert(!fs_init()&&!fs_load_disk());assert(fs_resolve(0,"/source")<0);
    source=fs_resolve(0,"/Documents/source");assert(source>=0&&fs_size(source)==8);
    const unsigned char expected[]={0,1,2,0xff,0x80,'\n','x',0};assert(!memcmp(fs_data(source),expected,sizeof expected));
    puts("Terminal mv: exact paths, byte/identity-preserving trees, collisions, syntax, bounds, root/apps/cycles/depth, scripts, live lease, retry and persistence passed");
    return 0;
}
