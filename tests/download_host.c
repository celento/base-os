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
#include "../src/term.c"

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
int process_task_start(int owner,const void *p,unsigned n,const ProgramIO *io){(void)owner;(void)p;(void)n;(void)io;return -1;}
int process_task_start_with_arg(int owner,const void *p,unsigned n,const ProgramIO *io,const char *argument,unsigned length){(void)argument;(void)length;return process_task_start(owner,p,n,io);}
int process_task_step(int owner){(void)owner;return 0;}
int process_task_status(int owner){(void)owner;return PROCESS_TASK_EMPTY;}
int process_task_result(int owner){(void)owner;return 0;}
int process_task_key(int owner,int key){(void)owner;(void)key;return 0;}
void process_task_stop(int owner){(void)owner;}
void process_task_clear(int owner){(void)owner;}

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
    assert(fs_sync()==0);fs_init();assert(fs_load_disk()==0);file_id=fs_resolve(0,"/music.bin");
    assert(file_id>0&&fs_size(file_id)==FS_FILE_MAX&&!memcmp(fs_data(file_id),payload,FS_FILE_MAX));
    command("downloads");assert(term_contains("disk is synchronized"));
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
    reset_download();command("download http://10.0.2.2/a \"/quoted file.bin\"");assert(download_active());command("downloads cancel");assert(!download_active());
    command("download http://10.0.2.2/a /bad extra");assert(!download_active());
    puts("downloads: asynchronous Terminal, exact 2 MiB binary/reboot, progress, cancellation, ownership, folder identity, no overwrite, size/storage limits passed");
    return 0;
}
