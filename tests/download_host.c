/* Deterministic downloads with the real HTTP parser and filesystem. No remote
 * hosts or fault probes: normal completion, cancellation and resource limits. */
#define main filesystem_fixture_main
#include "fs_host.c"
#undef main
#include "net_wire.h"
#define DOWNLOAD_HOST_TEST
#include "../src/download.c"
static unsigned char terminal_arena[0x100000];
#define TERM_MEMORY ((uintptr_t)terminal_arena)
#include "../src/app_storage.c"
#include "../src/app_canvas.c"
#include "../src/app_view.c"
#include "../src/term.c"
#define V (views[selected])

static NetHttpResult response;
static NetHttpParser parser;
static int busy, serial, started, cancelled;
static unsigned char payload[FS_FILE_MAX+1];
static NetStatus network_status;
int net_busy(void){return busy;}
int net_http_busy(void){return busy;}
const NetHttpResult *net_http_result(void){return &response;}
const NetStatus *net_status(void){return &network_status;}
const char *net_last_error(void){return response.error;}
int net_ping(const char *host,unsigned *ms){(void)host;(void)ms;return -1;}
int net_resolve(const char *host,uint32_t *ip){(void)host;(void)ip;return -1;}
int net_http_get(const char *url,char *body,unsigned cap){(void)url;(void)body;(void)cap;return -1;}
int net_http_start(const char *url,char *body,unsigned cap){
    if(busy)return -1;
    char host[NET_HOST_MAX],path[NET_URL_MAX];unsigned port;
    if(!net_parse_url(url,host,&port,path))return -1;
    assert(cap<=NET_HTTP_TRANSFER_MAX);memset(&response,0,sizeof response);
    response.request_id=(unsigned)++serial;response.state=NET_HTTP_CONNECTING;
    net_http_parser_init(&parser,body,cap,&response);busy=1;started++;return 0;
}
void net_cancel(void){cancelled++;busy=0;response.state=NET_HTTP_ERROR;strcpy(response.error,"Request cancelled");}
int basic_run(const char *text,int n,const ProgramIO *io){(void)text;(void)n;(void)io;return -1;}
int program_key(void){return 0;}
void program_present(void){}
int process_run(const void *p,unsigned n,const ProgramIO *io){(void)p;(void)n;(void)io;return -1;}
int process_create(const void *p,unsigned n,const char *argument,unsigned length,ProcessHandle *out){(void)p;(void)n;(void)argument;(void)length;(void)out;return -1;}
int process_create_mode(const void *file,unsigned bytes,const char *argument,unsigned length,
                        unsigned mode,ProcessHandle *out){
    return mode==PROCESS_LAUNCH_HOSTED?process_create(file,bytes,argument,length,out):PROCESS_CREATE_UNSUPPORTED;
}

int process_bind(ProcessHandle p,const ProcessIO *io){(void)p;(void)io;return 0;}
int process_unbind(ProcessHandle p){(void)p;return 0;}
int process_start(ProcessHandle p){(void)p;return 0;}
int process_step(ProcessHandle p){(void)p;return 0;}
ProcessHandle process_schedule_one(void){return 0;}
int process_binding_live(const ProcessBinding *binding){(void)binding;return 0;}
int process_status(ProcessHandle p){(void)p;return PROCESS_TASK_EMPTY;}
int process_get_result(ProcessHandle p,ProcessResult *out){(void)p;(void)out;return 0;}
int process_key(ProcessHandle p,int key){(void)p;(void)key;return 0;}
int process_request_stop(ProcessHandle p){(void)p;return 1;}
int process_reap(ProcessHandle p){(void)p;return 1;}
void process_counts(ProcessCounts *out){if(out)memset(out,0,sizeof *out);}

static void reset_download(void){
    reset();memset(data_disk,0,sizeof data_disk);unsigned *m=(unsigned *)data_disk;
    m[0]=DATA_MARKER_MAGIC;m[1]=DATA_MARKER_VERSION;m[2]=DATA_DISK_SECTORS;
    m[3]=DATA_SLOT_SECTORS;m[4]=DATA_FIRST_LBA;m[5]=DATA_SECOND_LBA;m[6]=crc32(m,24);
    data_present=1;fs_init();assert(fs_load_disk()==FS_LOAD_BLANK);fs_empty_dir(0);
    busy=serial=started=cancelled=ready=0;memset(&response,0,sizeof response);download_init();
    term_select(0);term_reset();
}
static void feed(const void *bytes,unsigned count){
    assert(busy);response.state=NET_HTTP_RECEIVING;
    int result=net_http_parser_feed(&parser,bytes,count);
    if(result){busy=0;response.state=result>0?NET_HTTP_DONE:NET_HTTP_ERROR;}
}
static void header(unsigned status,unsigned size){char h[128];int n=snprintf(h,sizeof h,"HTTP/1.1 %u Test\r\nContent-Length: %u\r\nContent-Type: application/octet-stream\r\n\r\n",status,size);assert(n<(int)sizeof h);feed(h,(unsigned)n);}
static void body(unsigned size){
    for(unsigned p=0;p<size;){unsigned count=size-p>1460?1460:size-p;feed(payload+p,count);p+=count;download_tick();}
}
static void command(const char *text){while(*text)term_char(*text++);term_enter();}
static int term_contains(const char *text){for(int i=0;i<term_count();i++)if(strstr(term_get(i),text))return 1;return 0;}
static void begin(const char *path){assert(!download_start(0,"http://10.0.2.2:8000/binary",path));assert(download_active());}
static void check_absent(const char *path){assert(fs_resolve(0,path)<0);}
static FsSyncTicket start_snapshot(void){
    FsSyncTicket ticket;assert(fs_sync_request(&ticket)==0&&fs_sync_busy());
    assert(fs_sync_result(ticket)==FS_SYNC_PENDING);return ticket;
}
static void drain_snapshot(FsSyncTicket ticket,int expected){
    unsigned turns=0;
    while(fs_sync_busy()){
        assert(fs_sync_result(ticket)==FS_SYNC_PENDING);
        assert(fs_sync_step()!=FS_SYNC_IDLE);assert(++turns<100000);
    }
    assert(fs_sync_result(ticket)==expected);
}
static void expect_waiting(unsigned request,unsigned size){
    const DownloadStatus *d=download_status();
    assert(download_active()&&d->state==DOWNLOAD_ACTIVE&&d->http_state==NET_HTTP_DONE);
    assert(d->request_id==request&&d->received==size&&d->http_status==200&&d->file_id==-1);
    assert(strstr(d->message,"waiting for disk saving"));check_absent(d->path);
}

static void test_complete_body_waits_for_storage(void){
    reset_download();int notes=fs_create(0,"notes");assert(fs_write(notes,"Prior snapshot",14)==14);
    begin("/waiting.bin");unsigned request=download_status()->request_id;
    header(200,FS_FILE_MAX);feed(payload,20000);assert(download_tick());
    FsSyncTicket ticket=start_snapshot();int before=fs_node_count();unsigned used=fs_used_bytes();
    for(unsigned p=20000;p<FS_FILE_MAX;){unsigned count=FS_FILE_MAX-p>1460?1460:FS_FILE_MAX-p;feed(payload+p,count);p+=count;download_tick();}
    expect_waiting(request,FS_FILE_MAX);assert(!busy&&!download_tick());
    assert(fs_node_count()==before&&fs_used_bytes()==used&&fs_sync_result(ticket)==FS_SYNC_PENDING);
    /* A second download is still refused even though the network is idle. */
    DownloadStatus waiting=*download_status();int starts=started;
    assert(download_start(0,"http://10.0.2.2/other","/other.bin")<0&&started==starts);
    assert(!memcmp(download_status(),&waiting,sizeof waiting));
    /* Another app can replace the shared HTTP parser/result and finish a
     * request. Neither its payload nor final status belongs to our download. */
    char other[1024];assert(!net_http_start("http://10.0.2.2/page",other,sizeof other));
    header(404,73);body(73);assert(response.status==404&&response.request_id!=request);
    assert(!memcmp(other,payload,73)&&!download_tick());expect_waiting(request,FS_FILE_MAX);
    /* Time spent waiting for storage is not a network deadline. A different
     * app's later timeout must not replace the already complete response. */
    assert(!net_http_start("http://10.0.2.2/slow",other,sizeof other));
    now+=60*TIMER_HZ;response.state=NET_HTTP_ERROR;busy=0;
    strcpy(response.error,"Network request timed out (15 seconds)");
    unsigned turns=0;
    while(fs_sync_busy()){
        assert(!download_tick());expect_waiting(request,FS_FILE_MAX);
        assert(fs_node_count()==before&&fs_used_bytes()==used);
        assert(fs_sync_step()!=FS_SYNC_IDLE);assert(++turns<100000);
    }
    assert(fs_sync_result(ticket)==0&&!fs_needs_sync());expect_waiting(request,FS_FILE_MAX);
    /* Retained explicit completion records are not mutation leases. The old
     * snapshot is durable, but cannot make the later download durable. */
    assert(download_tick()&&!download_active());const DownloadStatus *d=download_status();
    int id=d->file_id;assert(d->state==DOWNLOAD_DONE&&d->request_id==request&&d->http_status==200&&d->received==FS_FILE_MAX);
    assert(id==fs_resolve(0,"/waiting.bin")&&fs_size(id)==FS_FILE_MAX&&!memcmp(fs_data(id),payload,FS_FILE_MAX));
    assert(fs_size(notes)==14&&!memcmp(fs_data(notes),"Prior snapshot",14));
    assert(fs_needs_sync()&&strstr(d->message,"RAM")&&strstr(d->message,"pending"));
    assert(fs_sync_release(ticket)==0);ticket=start_snapshot();
    assert(download_tick()&&strstr(download_status()->message,"Saving disk snapshot"));
    assert(!strstr(download_status()->message,"disk is synchronized"));
    data_write_error=1;drain_snapshot(ticket,-1);assert(fs_sync_release(ticket)==0);
    assert(download_tick()&&download_status()->state==DOWNLOAD_DONE&&strstr(download_status()->message,"Save failed"));
    assert(fs_needs_sync()&&!memcmp(fs_data(id),payload,FS_FILE_MAX));
    data_write_error=0;ticket=start_snapshot();drain_snapshot(ticket,0);assert(fs_sync_release(ticket)==0);
    assert(download_tick()&&strstr(download_status()->message,"disk is synchronized"));
    remount();id=fs_resolve(0,"/waiting.bin");assert(id>0&&fs_size(id)==FS_FILE_MAX&&!memcmp(fs_data(id),payload,FS_FILE_MAX));
}
static void test_waiting_cancellation_and_destination_changes(void){
    reset_download();begin("/cancel-wait.bin");FsSyncTicket ticket=start_snapshot();
    unsigned request=download_status()->request_id;header(200,543);body(543);expect_waiting(request,543);
    char other[1024];assert(!net_http_start("http://10.0.2.2/page",other,sizeof other));
    unsigned other_request=response.request_id;assert(download_cancel());
    assert(download_status()->state==DOWNLOAD_CANCELLED&&!download_active()&&busy&&!cancelled&&response.request_id==other_request);
    assert(fs_sync_busy()&&fs_sync_result(ticket)==FS_SYNC_PENDING&&!download_cancel());
    drain_snapshot(ticket,0);assert(fs_sync_release(ticket)==0);header(200,31);body(31);
    assert(!download_tick());check_absent("/cancel-wait.bin");
    /* Cancellation releases the private arena for a new, independent job. */
    begin("/retry.bin");header(200,72);body(72);assert(download_status()->state==DOWNLOAD_DONE);
    assert(fs_size(fs_resolve(0,"/retry.bin"))==72);

    for(int change=0;change<4;change++){
        reset_download();int folder=fs_mkdir(0,"destination");assert(folder>0);
        begin("/destination/file.bin");ticket=start_snapshot();request=download_status()->request_id;
        header(200,123);body(123);expect_waiting(request,123);
        drain_snapshot(ticket,0);assert(fs_sync_release(ticket)==0);
        if(change==0){int id=fs_create(folder,"file.bin");assert(fs_write(id,"New owner",9)==9);}
        else if(change==1)assert(fs_rename(folder,"renamed")==0);
        else if(change==2){unsigned identity=fs_identity(folder);assert(fs_delete(folder)==0);assert(fs_mkdir(0,"destination")==folder&&fs_identity(folder)!=identity);}
        else {int parent=fs_mkdir(0,"moved");assert(parent>0&&fs_move(folder,parent)==0);}
        int count=fs_node_count();assert(download_tick()&&!download_active()&&download_status()->state==DOWNLOAD_ERROR);
        assert(fs_node_count()==count);
        if(change==0){int id=fs_resolve(0,"/destination/file.bin");assert(strstr(download_status()->message,"now exists")&&id>0&&fs_size(id)==9&&!memcmp(fs_data(id),"New owner",9));}
        else {assert(strstr(download_status()->message,"folder changed"));assert(fs_find_child(folder,"file.bin")<0);}
    }
}
static void test_waiting_snapshot_failures(void){
    /* A failed old snapshot releases its lease. It does not lose a complete
     * download or promise that newly created RAM bytes have reached disk. */
    for(int protected=0;protected<2;protected++){
        reset_download();checkpoint("old");assert(fs_write(file(),"new",3)==3);
        begin("/failure.bin");FsSyncTicket ticket=start_snapshot();unsigned request=download_status()->request_id;
        header(200,20037);body(20037);expect_waiting(request,20037);
        data_fail_flush_at=data_flush_count+(protected?2:1);drain_snapshot(ticket,-1);
        assert(fs_sync_release(ticket)==0&&download_tick());const DownloadStatus *d=download_status();
        assert(d->state==DOWNLOAD_DONE&&d->received==20037&&d->http_status==200&&fs_needs_sync());
        int id=fs_resolve(0,"/failure.bin");assert(id>0&&fs_size(id)==20037&&!memcmp(fs_data(id),payload,20037));
        assert(strstr(d->message,"RAM only")&&!strstr(d->message,"disk is synchronized"));
        if(protected){assert(strstr(d->message,"Disk protected")&&fs_sync()<0);}
        else {
            assert(strstr(d->message,"Save failed"));data_fail_flush_at=0;
            ticket=start_snapshot();drain_snapshot(ticket,0);assert(fs_sync_release(ticket)==0);
            assert(download_tick()&&strstr(download_status()->message,"disk is synchronized"));
            remount();id=fs_resolve(0,"/failure.bin");assert(id>0&&fs_size(id)==20037&&!memcmp(fs_data(id),payload,20037));
        }
    }
    /* Unsuccessful/truncated HTTP responses never become storage waiters. */
    reset_download();begin("/bad.bin");FsSyncTicket ticket=start_snapshot();header(404,123);body(123);
    assert(download_status()->state==DOWNLOAD_ERROR&&!download_active()&&fs_sync_busy());check_absent("/bad.bin");
    begin("/oversize.bin");header(200,FS_FILE_MAX+1);body(FS_FILE_MAX+1);
    assert(download_status()->state==DOWNLOAD_ERROR&&!download_active());check_absent("/oversize.bin");
    drain_snapshot(ticket,0);assert(fs_sync_release(ticket)==0);
    /* Empty successful responses also wait: an empty destination is still
     * a filesystem mutation, and DONE is not published while it is absent. */
    int dirty=fs_create(0,"dirty");assert(dirty>0);ticket=start_snapshot();
    begin("/empty-wait.bin");header(204,0);assert(download_tick()&&download_active());
    assert(download_status()->http_status==204&&download_status()->received==0);check_absent("/empty-wait.bin");
    drain_snapshot(ticket,0);assert(fs_sync_release(ticket)==0&&download_tick());
    assert(download_status()->state==DOWNLOAD_DONE&&fs_size(fs_resolve(0,"/empty-wait.bin"))==0);
}

static void test_binary_and_async_terminal(void){
    reset_download();command("download http://10.0.2.2:8000/binary /music.bin");
    assert(download_active()&&busy&&started==1&&term_contains("started in background"));
    assert(download_status()->received==0);check_absent("/music.bin");
    int owner_lines=term_count();term_select(1);term_reset();command("echo Desktop still works");assert(term_contains("Desktop still works"));
    int notes=fs_create(0,"notes.txt");assert(notes>0);header(200,FS_FILE_MAX);
    feed(payload,20000);assert(download_tick());assert(download_status()->received==20000&&download_active());
    assert(fs_write(notes,"Edited during download",22)==22);check_absent("/music.bin");
    for(unsigned p=20000;p<FS_FILE_MAX;){unsigned count=FS_FILE_MAX-p>1460?1460:FS_FILE_MAX-p;feed(payload+p,count);p+=count;download_tick();}
    assert(!download_active()&&download_status()->state==DOWNLOAD_DONE);int file_id=fs_resolve(0,"/music.bin");
    assert(file_id>0&&fs_size(file_id)==FS_FILE_MAX&&!memcmp(fs_data(file_id),payload,FS_FILE_MAX));
    assert(fs_size(notes)==22&&!memcmp(fs_data(notes),"Edited during download",22));
    term_select(0);assert(term_count()==owner_lines);term_reset();assert(!term_contains("complete"));
    command("downloads status");assert(term_contains("Download complete")&&term_contains("2097152"));
    assert(fs_sync()==0);command("downloads");assert(term_contains("disk is synchronized"));
    fs_init();assert(fs_load_disk()==0);file_id=fs_resolve(0,"/music.bin");
    assert(file_id>0&&fs_size(file_id)==FS_FILE_MAX&&!memcmp(fs_data(file_id),payload,FS_FILE_MAX));
    /* A remount ends the live identity binding, even when the bytes match.
     * Previously verified persistence is reported as a historical fact. */
    assert(strstr(download_status()->message,"disk save was confirmed"));
    assert(!strstr(download_status()->message,"disk is synchronized"));
}

static void test_completed_destination_versions(void){
    /* Skip every intermediate download tick: even if the first status query
     * follows a successful save, unrelated current data must not bless an old
     * completion. Timestamps deliberately stay zero throughout this fixture. */
    for(int confirmed=0;confirmed<2;confirmed++)for(int change=0;change<8;change++){
        reset_download();int folder=fs_mkdir(0,"destination");assert(folder>0);
        begin("/destination/file.bin");header(200,123);body(123);
        DownloadStatus completed=*download_status();int id=completed.file_id;
        unsigned identity=fs_identity(id),revision=fs_content_revision(id),stamp=fs_modified(id);
        assert(completed.state==DOWNLOAD_DONE&&revision&&strstr(completed.message,"pending"));
        if(confirmed){assert(fs_sync()==0);assert(strstr(download_status()->message,"disk is synchronized"));}
        if(change==0)assert(fs_delete(id)==0);
        else if(change==1){
            assert(fs_delete(id)==0&&fs_create(folder,"file.bin")==id);
            assert(fs_write(id,(char *)payload,123)==123&&fs_identity(id)!=identity);
        }else if(change==2)assert(fs_rename(id,"renamed.bin")==0);
        else if(change==3)assert(fs_move(id,0)==0);
        else if(change==4)assert(fs_rename(folder,"renamed")==0);
        else if(change==5){
            char edited[123];memcpy(edited,payload,sizeof edited);edited[122]^=1;
            assert(fs_write(id,edited,sizeof edited)==sizeof edited);
            assert(fs_modified(id)==stamp&&fs_content_revision(id)!=revision);
        }else if(change==6)assert(fs_write(id,"short",5)==5);
        else {
            /* Even an identical-byte rewrite is a new content version. */
            assert(fs_write(id,(char *)payload,123)==123);
            assert(fs_modified(id)==stamp&&fs_content_revision(id)!=revision);
        }
        assert(fs_sync()==0&&!fs_needs_sync()&&download_tick());
        const DownloadStatus *d=download_status();
        assert(d->state==DOWNLOAD_DONE&&!download_active());
        assert(d->request_id==completed.request_id&&d->received==123&&d->http_status==200&&d->http_state==NET_HTTP_DONE);
        assert(d->file_id==completed.file_id&&!strcmp(d->url,completed.url)&&!strcmp(d->path,completed.path));
        const char *confirmation=confirmed?"disk save was confirmed":"disk save was not confirmed";
        assert(strstr(d->message,confirmation)&&!strstr(d->message,"disk is synchronized"));
        assert(strstr(d->message,change==0?"no longer present":change==1?"no longer be identified":change<5?"moved or was renamed":"written again after completion"));
        assert(!download_tick());
        term_reset();command("downloads");
        assert(term_contains("Download complete")&&term_contains(confirmation));
        assert(!term_contains("disk is synchronized"));
        /* The full status is wrapped, rather than silently truncated at 80. */
        char displayed[sizeof d->message]={0};int lines=(strlen(d->message)+TERM_COLS-1)/TERM_COLS;
        for(int line=term_count()-lines;line<term_count();line++)strcat(displayed,term_get(line));
        assert(!strcmp(displayed,d->message));
    }
    /* Deletion while dirty must already stop the current-RAM claim. */
    reset_download();begin("/gone.bin");header(200,31);body(31);
    assert(fs_delete(download_status()->file_id)==0&&download_tick());
    assert(strstr(download_status()->message,"no longer present"));
    assert(!strstr(download_status()->message,"File complete in RAM"));
    /* A first query after remount cannot equate a new runtime identity to the
     * original result, even when current bytes and path happen to match. */
    reset_download();begin("/remounted.bin");header(200,31);body(31);
    assert(fs_sync()==0);remount();
    assert(strstr(download_status()->message,"disk save was not confirmed"));
    assert(!strstr(download_status()->message,"disk is synchronized"));

    for(int empty=0;empty<2;empty++){
        reset_download();begin("/complete.bin");header(200,empty?0:123);
        if(empty)assert(download_tick());else body(123);
        int id=download_status()->file_id;unsigned revision=fs_content_revision(id);
        FsSyncTicket ticket=start_snapshot();
        assert(download_tick()&&strstr(download_status()->message,"Saving disk snapshot"));
        assert(!strstr(download_status()->message,"disk is synchronized"));
        drain_snapshot(ticket,0);assert(fs_sync_release(ticket)==0);
        assert(download_tick()&&strstr(download_status()->message,"disk is synchronized"));
        assert(fs_content_revision(id)==revision&&!download_tick());
        /* Unrelated writes, then another save failure, do not erase the
         * historical verified result of this still-unchanged file. */
        assert(fs_create(0,"other")>0&&download_tick());
        assert(strstr(download_status()->message,"this file's disk save was confirmed"));
        assert(!strstr(download_status()->message,"disk is synchronized"));
        data_write_error=1;assert(fs_sync()<0);
        assert(strstr(download_status()->message,"this file's disk save was confirmed"));
        data_write_error=0;assert(fs_sync()==0);
        assert(download_tick()&&strstr(download_status()->message,"disk is synchronized"));
        assert(fs_write(id,"changed",7)==7&&download_tick());
        assert(strstr(download_status()->message,"disk save was confirmed"));
        assert(strstr(download_status()->message,"written again after completion"));
        assert(!strstr(download_status()->message,"disk is synchronized"));
    }
}

static void test_content_revision_lifecycle(void){
    reset_download();assert(fs_content_revision(0));
    assert(!fs_content_revision(-1)&&!fs_content_revision(FS_MAX_NODES));
    int id=fs_create(0,"version.bin");unsigned revision=fs_content_revision(id);
    assert(id>0&&revision&&fs_content_revision(id)==revision);
    assert(fs_write(id,"123",3)==3&&fs_content_revision(id)!=revision);
    revision=fs_content_revision(id);unsigned stamp=fs_modified(id);
    assert(fs_write(id,"456",3)==3&&fs_modified(id)==stamp&&fs_content_revision(id)!=revision);
    revision=fs_content_revision(id);
    assert(fs_write(id,"456",3)==3&&fs_content_revision(id)!=revision);
    revision=fs_content_revision(id);
    assert(fs_write(id,0,1)<0&&fs_content_revision(id)==revision);
    char bytes[8];assert(fs_read(id,bytes,sizeof bytes)==3&&fs_content_revision(id)==revision);
    int folder=fs_mkdir(0,"folder");assert(fs_rename(id,"renamed.bin")==0&&fs_move(id,folder)==0);
    assert(fs_content_revision(id)==revision);
    int copy_id=fs_copy(id,0);assert(copy_id>0&&fs_content_revision(copy_id)&&fs_content_revision(copy_id)!=revision);
    assert(fs_content_revision(id)==revision&&!memcmp(fs_data(copy_id),"456",3));
    int empty=fs_create(0,"empty"),empty_copy=fs_copy(empty,0);
    assert(empty_copy>0&&fs_content_revision(empty)&&fs_content_revision(empty_copy)!=fs_content_revision(empty));
    FsSyncTicket ticket=start_snapshot();
    assert(fs_write(id,"new",3)==FS_ERR_BUSY&&fs_content_revision(id)==revision);
    assert(fs_init()==FS_ERR_BUSY&&fs_content_revision(id)==revision);
    drain_snapshot(ticket,0);assert(fs_sync_release(ticket)==0&&fs_content_revision(id)==revision);
    assert(fs_write(id,"",0)==0&&fs_content_revision(id)!=revision);
    revision=fs_content_revision(id);assert(fs_sync()==0&&fs_content_revision(id)==revision);
    assert(fs_load_disk()==0&&fs_content_revision(id)&&fs_content_revision(id)!=revision);
    revision=fs_content_revision(id);assert(fs_delete(id)==0&&!fs_content_revision(id));
    assert(fs_create(folder,"replacement")==id&&fs_content_revision(id)&&fs_content_revision(id)!=revision);
    revision=fs_content_revision(0);assert(fs_init()==0&&fs_content_revision(0)&&fs_content_revision(0)!=revision);
}

static void test_content_revision_exhaustion(void){
    reset_download();begin("/version.bin");header(200,31);body(31);
    int id=download_status()->file_id;unsigned revision=fs_content_revision(id);
    /* Ordinary boundary fixture, as used for sync ticket exhaustion: the last
     * token remains usable; later writes and creates succeed with unknown 0. */
    next_content_revision=~0u-1;
    assert(fs_write(id,"last",4)==4&&fs_content_revision(id)==~0u);
    assert(fs_write(id,"unknown",7)==7&&!fs_content_revision(id)&&revision);
    assert(fs_sync()==0&&download_tick());
    assert(strstr(download_status()->message,"version can no longer be verified"));
    assert(strstr(download_status()->message,"disk save was not confirmed"));
    int other=fs_create(0,"unknown.bin");assert(other>0&&!fs_content_revision(other));
    begin("/unknown-download.bin");header(204,0);assert(download_tick());
    assert(fs_sync()==0&&download_status()->state==DOWNLOAD_DONE);
    assert(strstr(download_status()->message,"disk save was not confirmed"));
    assert(!strstr(download_status()->message,"disk is synchronized"));
    assert(fs_init()==0&&!fs_content_revision(0));
    assert(fs_load_disk()==0&&!fs_content_revision(fs_resolve(0,"/unknown-download.bin")));
}
static void test_no_overwrites_and_folder_identity(void){
    reset_download();int existing=fs_create(0,"keep.bin");assert(fs_write(existing,"untouched",9)==9);
    assert(download_start(0,"http://10.0.2.2/a","/keep.bin")<0&&!started);assert(!memcmp(fs_data(existing),"untouched",9));
    begin("/new.bin");int count=started;unsigned request=download_status()->request_id;
    assert(download_start(0,"http://10.0.2.2/other","/other")<0&&started==count&&download_status()->request_id==request);
    int late=fs_create(0,"new.bin");assert(fs_write(late,"Later file",10)==10);header(200,20001);body(20001);
    assert(download_status()->state==DOWNLOAD_ERROR&&strstr(download_status()->message,"now exists"));assert(fs_size(late)==10&&!memcmp(fs_data(late),"Later file",10));
    int folder=fs_mkdir(0,"destination");assert(folder>0);begin("/destination/file.bin");unsigned identity=fs_identity(folder);
    assert(fs_delete(folder)==0);int replacement=fs_mkdir(0,"destination");assert(replacement==folder&&fs_identity(replacement)!=identity);
    header(200,123);body(123);assert(download_status()->state==DOWNLOAD_ERROR);check_absent("/destination/file.bin");
    begin("/destination/file.bin");assert(fs_rename(replacement,"renamed")==0);header(200,123);body(123);
    assert(download_status()->state==DOWNLOAD_ERROR);check_absent("/renamed/file.bin");
}
static void test_cancel_limits_and_ownership(void){
    reset_download();begin("/cancel.bin");header(200,50000);feed(payload,12345);download_tick();assert(download_cancel());
    assert(cancelled==1&&!busy&&!download_active()&&download_status()->state==DOWNLOAD_CANCELLED);check_absent("/cancel.bin");
    assert(!download_cancel()&&cancelled==1);
    const DownloadStatus *previous=download_status();assert(!download_start(0,previous->url,previous->path));assert(download_active());download_cancel();
    begin("/large.bin");header(200,FS_FILE_MAX+1);body(FS_FILE_MAX+1);
    assert(download_status()->state==DOWNLOAD_ERROR&&strstr(download_status()->message,"exceeds"));check_absent("/large.bin");
    begin("/incomplete.bin");header(200,100);feed(payload,40);assert(net_http_parser_eof(&parser)<0);busy=0;response.state=NET_HTTP_ERROR;download_tick();
    assert(download_status()->state==DOWNLOAD_ERROR);check_absent("/incomplete.bin");
    begin("/timeout.bin");response.state=NET_HTTP_ERROR;strcpy(response.error,"Network request timed out (15 seconds)");busy=0;download_tick();
    assert(download_status()->state==DOWNLOAD_ERROR);check_absent("/timeout.bin");
    begin("/replaced.bin");response.request_id++;int old=cancelled;assert(download_cancel());assert(busy&&cancelled==old);check_absent("/replaced.bin");
    assert(download_start(0,"http://10.0.2.2/a","/busy.bin")<0&&busy);busy=0;
    begin("/lost.bin");response.request_id++;download_tick();assert(download_status()->state==DOWNLOAD_ERROR&&busy);check_absent("/lost.bin");busy=0;
    assert(download_start(0,"https://example.com/a","/secure.bin")<0&&!busy);check_absent("/secure.bin");
    begin("/redirect.bin");header(302,0);download_tick();assert(download_status()->state==DOWNLOAD_ERROR&&!busy);check_absent("/redirect.bin");
    begin("/error.bin");header(404,15);body(15);assert(download_status()->state==DOWNLOAD_ERROR);check_absent("/error.bin");
    begin("/empty.bin");header(204,0);download_tick();assert(download_status()->state==DOWNLOAD_DONE&&fs_size(fs_resolve(0,"/empty.bin"))==0);
}
static void test_storage_limits(void){
    reset_download();begin("/no-slots.bin");while(fs_node_count()<FS_MAX_NODES){char n[24];snprintf(n,sizeof n,"file%d",fs_node_count());assert(fs_create(0,n)>0);}
    header(200,123);body(123);assert(download_status()->state==DOWNLOAD_ERROR&&fs_node_count()==FS_MAX_NODES);check_absent("/no-slots.bin");
    reset_download();begin("/no-space.bin");
    for(int i=0;i<4;i++){char n[24];snprintf(n,sizeof n,"large%d",i);int id=fs_create(0,n);unsigned count=i<3?FS_FILE_MAX:fs_capacity()-3*FS_FILE_MAX;assert(fs_write(id,(char *)payload,count)==(int)count);}
    int before=fs_node_count();header(200,123);body(123);assert(download_status()->state==DOWNLOAD_ERROR&&fs_node_count()==before);check_absent("/no-space.bin");
    /* Legacy floppy operation stays bounded to its smaller per-file limit. */
    reset();busy=ready=0;download_init();begin("/floppy.bin");assert(download_status()->limit==FS_MAX_SIZE-1);header(200,FS_MAX_SIZE);body(FS_MAX_SIZE);
    assert(download_status()->state==DOWNLOAD_ERROR);check_absent("/floppy.bin");
}
int main(void){
    for(unsigned i=0;i<sizeof payload;i++)payload[i]=(unsigned char)(i*37+(i>>16)+91);
    test_binary_and_async_terminal();test_no_overwrites_and_folder_identity();test_cancel_limits_and_ownership();test_storage_limits();
    test_complete_body_waits_for_storage();test_waiting_cancellation_and_destination_changes();test_waiting_snapshot_failures();
    test_completed_destination_versions();test_content_revision_lifecycle();
    reset_download();command("download http://10.0.2.2/a \"/quoted file.bin\"");assert(download_active());command("downloads cancel");assert(!download_active());
    command("download http://10.0.2.2/a /bad extra");assert(!download_active());
    test_content_revision_exhaustion();
    puts("downloads: asynchronous Terminal, exact 2 MiB binary/reboot, progress, cancellation, ownership, folder identity, no overwrite, size/storage limits, deferred snapshot-lease completion and version-bound durability passed");
    return 0;
}
