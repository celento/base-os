/* A bounded, cooperative HTTP-to-file service. Partial bytes live only in our
 * private arena. A destination is created only after a complete 2xx response.
 * Built-in apps are serialized: creation/write/rollback is one desktop turn. */
#include "download.h"
#include "layout.h"
#include <stdint.h>
#ifndef DOWNLOAD_BASE
#define DOWNLOAD_BASE 0xB30000
#endif
#ifndef DOWNLOAD_CAPACITY
#define DOWNLOAD_CAPACITY 0x210000
#endif

typedef struct {
    DownloadStatus status;
    int parent, body_complete;
    unsigned parent_identity;
    char name[FS_NAME_LEN], parent_path[FS_PATH_LEN], error[160];
    char body[NET_HTTP_TRANSFER_MAX];
} Download;
#ifdef DOWNLOAD_HOST_TEST
static Download host_download;
#define D host_download
#else
#define D (*(Download *)(uintptr_t)DOWNLOAD_BASE)
#endif
_Static_assert(sizeof(Download)<=DOWNLOAD_CAPACITY,"download arena overflow");
static int ready;
static int sync_status(void);
static void copy(char *out,unsigned capacity,const char *in){unsigned n=0;if(!capacity)return;while(in[n]&&n+1<capacity){out[n]=in[n];n++;}out[n]=0;}
static int length(const char *s,unsigned cap){unsigned n=0;if(!s)return -1;while(n<cap&&s[n])n++;return n<cap?(int)n:-1;}
static void append(char *out,unsigned capacity,const char *in){unsigned n=0;while(n<capacity&&out[n])n++;if(n<capacity)copy(out+n,capacity-n,in);}
static int fail_start(const char *message){copy(D.error,sizeof D.error,message);return -1;}
static int finish(int state,const char *message){D.body_complete=0;D.status.state=state;copy(D.status.message,sizeof D.status.message,message);return 1;}
void download_init(void){if(ready)return;kmemset(&D,0,__builtin_offsetof(Download,body));D.status.file_id=-1;copy(D.status.message,sizeof D.status.message,"No download yet.");ready=1;}
int download_active(void){return ready&&D.status.state==DOWNLOAD_ACTIVE;}
const DownloadStatus *download_status(void){download_init();sync_status();return &D.status;}
const char *download_last_error(void){download_init();return D.error;}
int download_start(int cwd,const char *url,const char *path){
    download_init();D.error[0]=0;
    if(download_active())return fail_start("A download is already running. Use downloads or cancel.");
    if(net_busy())return fail_start("Network is busy in another app. Wait for it to finish.");
    int url_length=length(url,NET_URL_MAX);
    if(url_length<7)return fail_start("Use an http:// URL of at most 255 characters. HTTPS is unsupported.");
    for(int i=0;i<7;i++)if(url[i]!="http://"[i])return fail_start("Only http:// is supported. HTTPS is never downgraded.");
    char name[FS_NAME_LEN];int parent=fs_destination(cwd,path,name);
    if(parent<0)return fail_start("Choose a new file name in an existing folder.");
    if(fs_find_child(parent,name)>=0)return fail_start("Destination already exists. Choose another name; nothing was replaced.");
    unsigned limit=fs_file_limit();if(limit>FS_FILE_MAX)limit=FS_FILE_MAX;
    if(!limit)return fail_start("This volume cannot store file data.");
    char parent_path[FS_PATH_LEN],requested_url[NET_URL_MAX];
    fs_path(parent,parent_path,sizeof parent_path);
    unsigned parent_length=(unsigned)length(parent_path,sizeof parent_path);
    if(parent_length+(parent!=fs_root())+(unsigned)length(name,sizeof name)>=FS_PATH_LEN)
        return fail_start("The destination path is too long.");
    /* Accept retry inputs borrowed from download_status(), too. */
    copy(requested_url,sizeof requested_url,url);
    /* Rejected requests must leave the previous result intact. After the
     * service is accepted, retain its diagnostics even if the NIC is offline. */
    kmemset(&D.status,0,sizeof D.status);D.status.file_id=-1;D.body_complete=0;
    D.parent=parent;D.parent_identity=fs_identity(parent);copy(D.name,sizeof D.name,name);
    copy(D.parent_path,sizeof D.parent_path,parent_path);copy(D.status.path,sizeof D.status.path,parent_path);
    if(parent!=fs_root())append(D.status.path,sizeof D.status.path,"/");
    append(D.status.path,sizeof D.status.path,name);copy(D.status.url,sizeof D.status.url,requested_url);D.status.limit=limit;
    if(net_http_start(D.status.url,D.body,limit+1)){
        const char *message=net_http_result()->error;
        if(!message[0])message="Could not start the HTTP request.";
        finish(DOWNLOAD_ERROR,message);return fail_start(message);
    }
    const NetHttpResult *result=net_http_result();D.status.request_id=result->request_id;D.status.http_state=result->state;
    D.status.state=DOWNLOAD_ACTIVE;copy(D.status.message,sizeof D.status.message,"Downloading in background; use downloads for progress, cancel to stop.");return 0;
}
static int sync_status(void){
    if(D.status.state!=DOWNLOAD_DONE)return 0;
    char message[sizeof D.status.message];const char *storage=fs_storage_status();
    copy(message,sizeof message,storage?"File complete in RAM; ":fs_needs_sync()?"Complete file saved in RAM; disk autosave is pending.":"Complete file saved; disk is synchronized.");
    if(storage)append(message,sizeof message,storage);
    if(!kstrcmp(message,D.status.message))return 0;
    copy(D.status.message,sizeof D.status.message,message);return 1;
}
int download_tick(void){
    if(!download_active())return ready?sync_status():0;
    int changed=0;
    if(!D.body_complete){
        const NetHttpResult *result=net_http_result();
        if(result->request_id!=D.status.request_id)return finish(DOWNLOAD_ERROR,"Another app replaced the HTTP result. No file was saved; retry.");
        changed=D.status.received!=result->length||D.status.http_state!=result->state||D.status.http_status!=result->status;
        D.status.received=result->length;D.status.http_state=result->state;D.status.http_status=result->status;
        if(result->state==NET_HTTP_ERROR)return finish(DOWNLOAD_ERROR,result->error[0]?result->error:"Network request failed. No file was saved.");
        if(result->state!=NET_HTTP_DONE)return changed;
        if(result->truncated||result->length>D.status.limit)return finish(DOWNLOAD_ERROR,"Response exceeds this volume's file limit. No file was saved.");
        if(result->status>=300&&result->status<400)return finish(DOWNLOAD_ERROR,"HTTP redirect was not followed. Use the final HTTP URL; HTTPS is unsupported.");
        if(result->status<200||result->status>=300)return finish(DOWNLOAD_ERROR,"Server returned an unsuccessful HTTP status. No file was saved.");
        /* The network is free for another app now. Latch the validated body
         * and final exposed HTTP fields; never read its shared result again.
         * Our private body remains owned until commit, cancellation or error. */
        D.body_complete=1;changed=1;
        copy(D.status.message,sizeof D.status.message,"Response complete; waiting for disk saving to finish. No file saved yet; Cancel discards the download.");
    }
    /* Check before the compound create/write/rollback operation. Device-only
     * polls cannot acquire a lease or dispatch an application in between. */
    if(fs_sync_busy())return changed;
    if(!fs_is_dir(D.parent)||fs_identity(D.parent)!=D.parent_identity||fs_resolve(fs_root(),D.parent_path)!=D.parent)
        return finish(DOWNLOAD_ERROR,"Destination folder changed during the request. No file was saved.");
    if(fs_find_child(D.parent,D.name)>=0)return finish(DOWNLOAD_ERROR,"Destination now exists. Nothing was replaced; choose another name.");
    if(D.status.received>fs_file_limit()||D.status.received>fs_capacity()-fs_used_bytes())
        return finish(DOWNLOAD_ERROR,"Not enough storage space for the complete response. No file was saved.");
    int file=fs_create(D.parent,D.name);
    if(file<0)return finish(DOWNLOAD_ERROR,"Could not create destination (folder, name, or file slots). No file was saved.");
    if(fs_write(file,D.body,(int)D.status.received)!=(int)D.status.received){
        fs_delete(file);return finish(DOWNLOAD_ERROR,"Could not save the complete response. No destination file was left.");
    }
    D.status.file_id=file;
    return finish(DOWNLOAD_DONE,"Complete file saved in RAM; normal disk autosave is pending.");
}
int download_cancel(void){
    if(!download_active())return 0;
    if(!D.body_complete){
        const NetHttpResult *result=net_http_result();
        if(result->request_id==D.status.request_id&&net_http_busy())net_cancel();
    }
    return finish(DOWNLOAD_CANCELLED,"Download cancelled. No file was saved.");
}
