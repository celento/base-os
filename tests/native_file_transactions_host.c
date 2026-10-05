/* Ordinary API traffic only: real FS/native files/physmem metadata, private host
 * frame adapter. No guest, corruption, injected transport failure or fuzz lane. */
#define main legacy_fs_main
#ifdef TRANSACTION_LARGE_PROFILE
#include "large_volume_host.c"
unsigned large_test_polls;
#else
#include "fs_host.c"
#endif
#undef main
#include "native_file_stage_host.h"
#include "../src/native_files.c"
#undef kmemcpy
#define OWNER_A (BOS_HANDLE_TYPE_PROCESS | 1u)
#define OWNER_B (BOS_HANDLE_TYPE_PROCESS | 2u)
#define RW (BOS_FILE_OPEN_READ | BOS_FILE_OPEN_WRITE)
static unsigned char large_file[FS_FILE_MAX];
static void setup_files(void) {
#ifdef TRANSACTION_LARGE_PROFILE
    high_available=1;mark();
#else
    reset();memset(data_disk,0,sizeof data_disk);
    unsigned *m=(unsigned *)data_disk;
    m[0]=DATA_MARKER_MAGIC;m[1]=DATA_MARKER_VERSION;m[2]=DATA_DISK_SECTORS;
    m[3]=DATA_SLOT_SECTORS;m[4]=DATA_FIRST_LBA;m[5]=DATA_SECOND_LBA;m[6]=crc32(m,24);
    data_present=1;
#endif
    assert(fs_init()==0&&fs_load_disk()==FS_LOAD_BLANK);
    assert(fs_empty_dir(0)==0&&fs_mkdir(0,"Documents")>0);
    native_files_init();
}
static int add_file(const char *name,const char *bytes,unsigned length) {
    int id=fs_create(fs_resolve(0,"/Documents"),name);
    assert(id>0&&fs_write(id,bytes,length)==(int)length);return id;
}
static BosFileInfo open_file(uint32_t owner,const char *path,unsigned flags) {
    BosFileInfo info;assert(native_file_open(owner,path,flags,&info)==BOS_OK);return info;
}
static void unchanged_error(int result,int expected,const BosFileInfo *info,const BosFileInfo *before) {
    assert(result==expected&&!memcmp(info,before,sizeof *info));
}
#include <time.h>
static unsigned char upload[4096];
static double elapsed_ms(struct timespec a,struct timespec b) {
    return (b.tv_sec-a.tv_sec)*1000.0+(b.tv_nsec-a.tv_nsec)/1000000.0;
}
static unsigned char pattern(unsigned offset) { return (unsigned char)(offset*29u+offset/4093u); }
static BosFileTransactionBeginV1 request(unsigned total) {
    BosFileTransactionBeginV1 input={0};
    input.struct_size=sizeof input;input.version=BOS_FILE_TRANSACTION_VERSION;input.total_bytes=total;
    return input;
}
static BosFileTransactionStatusV1 begin_replace(unsigned owner,BosFileInfo file,unsigned total) {
    BosFileTransactionBeginV1 input=request(total);
    input.source_handle=file.handle;input.expected_revision=file.revision;
    BosFileTransactionStatusV1 out;
    assert(native_file_transaction_begin(owner,BOS_FILE_TRANSACTION_REPLACE,&input,0,&out)==BOS_OK);
    assert(out.struct_size==64&&out.version==1&&out.total_bytes==total&&!out.received_bytes);
    assert(out.state==(total?BOS_FILE_TRANSACTION_UPLOADING:BOS_FILE_TRANSACTION_COMPLETE));
    return out;
}
static int create_result(unsigned owner,const char *path,unsigned total,BosFileTransactionStatusV1 *out) {
    BosFileTransactionBeginV1 input=request(total);input.path_bytes=strlen(path);
    return native_file_transaction_begin(owner,BOS_FILE_TRANSACTION_CREATE,&input,path,out);
}
static BosFileTransactionStatusV1 begin_create(unsigned owner,const char *path,unsigned total) {
    BosFileTransactionStatusV1 out;
    assert(create_result(owner,path,total,&out)==BOS_OK);
    assert(fs_resolve(0,path)<0);
    return out;
}
static void fill(unsigned owner,BosFileTransactionStatusV1 stage,unsigned stride) {
    for(unsigned offset=0;offset<stage.total_bytes;) {
        unsigned count=stage.total_bytes-offset;if(count>stride)count=stride;
        for(unsigned i=0;i<count;++i)upload[i]=pattern(offset+i);
        assert(native_file_transaction_append(owner,stage.handle,upload,count,offset)==(int)count);
        memset(upload,0xdd,sizeof upload); /* Caller immediately reuses the buffer. */
        offset+=count;
    }
    BosFileTransactionStatusV1 out;
    assert(native_file_transaction_info(owner,stage.handle,&out)==BOS_OK);
    assert(out.received_bytes==stage.total_bytes&&out.state==BOS_FILE_TRANSACTION_COMPLETE);
}
static void exact(unsigned owner,BosFileInfo file) {
    for(unsigned offset=0;offset<file.size;) {
        unsigned count=file.size-offset;if(count>sizeof upload)count=sizeof upload;
        assert(native_file_read_at(owner,file.handle,offset,upload,count)==(int)count);
        for(unsigned i=0;i<count;++i)assert(upload[i]==pattern(offset+i));
        offset+=count;
    }
}
static void completed_uploads(void) {
    const unsigned totals[]={0,1,32769,262144};
    for(unsigned t=0;t<4;++t) {
        setup_files();int id=add_file("old","original",8);
        BosFileInfo old=open_file(OWNER_A,"/Documents/old",RW), peer=open_file(OWNER_B,"/Documents/old",RW), out;
        unsigned baseline=stage_stats().free;
        struct timespec a,b,c;clock_gettime(CLOCK_MONOTONIC,&a);
        BosFileTransactionStatusV1 stage=begin_replace(OWNER_A,old,totals[t]);
        clock_gettime(CLOCK_MONOTONIC,&b);
        assert(stage_stats().free==baseline-(totals[t]+4095)/4096);
        fill(OWNER_A,stage,t==1?1:t==2?997:4096);
        assert(fs_size(id)==8&&!memcmp(fs_data(id),"original",8));
        clock_gettime(CLOCK_MONOTONIC,&c);
        assert(native_file_transaction_accept(OWNER_A,stage.handle,&out)==BOS_OK);
        struct timespec d;clock_gettime(CLOCK_MONOTONIC,&d);
        assert(out.handle==old.handle&&out.size==totals[t]&&out.revision!=old.revision);
        assert(stage_stats().free==baseline);exact(OWNER_A,out);
        memset(upload,0xaa,sizeof upload);
        assert(native_file_read_at(OWNER_B,peer.handle,0,upload,sizeof upload)==BOS_E_CHANGED);
        for(unsigned i=0;i<sizeof upload;++i)assert(upload[i]==0xaa);
        assert(native_file_transaction_abort(OWNER_A,stage.handle)==BOS_E_STALE);
        printf("replace %u bytes: host begin %.3f ms, accept %.3f ms\n",totals[t],elapsed_ms(a,b),elapsed_ms(c,d));
        native_files_init();
        stage=begin_create(OWNER_A,"/Documents/new",totals[t]);fill(OWNER_A,stage,4096);
        assert(fs_resolve(0,"/Documents/new")<0);
        assert(native_file_transaction_accept(OWNER_A,stage.handle,&out)==BOS_OK);
        assert(out.size==totals[t]&&out.flags==RW);exact(OWNER_A,out);
        assert(stage_stats().free==baseline);
    }
    puts("ordinary exact empty/one-byte/32769/262144 replacement and unpublished create passed");
}
static void conflict_and_binding(void) {
    setup_files();int id=add_file("target","old",3),docs=fs_resolve(0,"/Documents");
    BosFileInfo a=open_file(OWNER_A,"/Documents/target",RW),b=open_file(OWNER_B,"/Documents/target",RW);
    BosFileTransactionStatusV1 stage=begin_replace(OWNER_A,a,32769),status;
    fill(OWNER_A,stage,4096);
    assert(native_file_replace(OWNER_B,b.handle,"peer",4,&b)==BOS_OK);
    BosFileInfo before=a;
    unchanged_error(native_file_transaction_accept(OWNER_A,stage.handle,&a),BOS_E_CHANGED,&a,&before);
    assert(fs_size(id)==4&&!memcmp(fs_data(id),"peer",4));
    assert(native_file_transaction_info(OWNER_A,stage.handle,&status)==BOS_OK&&status.received_bytes==32769);
    assert(native_file_transaction_abort(OWNER_B,stage.handle)==BOS_E_STALE);
    assert(native_file_transaction_abort(OWNER_A,stage.handle)==BOS_OK);
    native_files_release_owner(OWNER_A);a=open_file(OWNER_A,"/Documents/target",RW);
    stage=begin_replace(OWNER_A,a,1);fill(OWNER_A,stage,1);
    assert(native_file_replace(OWNER_A,a.handle,"self",4,&a)==BOS_OK);before=a;
    unchanged_error(native_file_transaction_accept(OWNER_A,stage.handle,&a),BOS_E_CHANGED,&a,&before);
    assert(native_file_transaction_abort(OWNER_A,stage.handle)==BOS_OK);
    stage=begin_replace(OWNER_A,a,1);fill(OWNER_A,stage,1);
    assert(fs_rename(id,"renamed")==0);int other=add_file("target","different",9);
    int folder=fs_mkdir(docs,"folder");assert(folder>0&&fs_move(id,folder)==0);
    assert(fs_move(id,0)==0);before=a;
    unchanged_error(native_file_transaction_accept(OWNER_A,stage.handle,&a),BOS_E_PROTECTED,&a,&before);
    assert(fs_move(id,folder)==0&&native_file_transaction_accept(OWNER_A,stage.handle,&a)==BOS_OK);
    exact(OWNER_A,a);assert(fs_size(other)==9&&!memcmp(fs_data(other),"different",9));
    stage=begin_replace(OWNER_A,a,1);fill(OWNER_A,stage,1);
    assert(fs_delete(id)==0);int recreated=fs_create(folder,"renamed");assert(recreated>=0);
    before=a;unchanged_error(native_file_transaction_accept(OWNER_A,stage.handle,&a),BOS_E_CHANGED,&a,&before);
    assert(native_file_close(OWNER_A,a.handle)==BOS_OK);
    assert(native_file_transaction_info(OWNER_A,stage.handle,&status)==BOS_E_STALE);
    assert(stage_stats().allocated==0);
    puts("peer/self conflicts, immutable captured revision, object moves, recreated path and close cleanup passed");
}
static void create_binding_and_errors(void) {
    setup_files();int docs=fs_resolve(0,"/Documents"),parent=fs_mkdir(docs,"parent");
    BosFileTransactionStatusV1 out,before;memset(&out,0xa5,sizeof out);before=out;
    assert(create_result(OWNER_A,"/Documents/missing/new",1,&out)==BOS_E_NOT_FOUND&&!memcmp(&out,&before,sizeof out));
    add_file("plain","a",1);
    assert(create_result(OWNER_A,"/Documents/plain/new",1,&out)==BOS_E_NOT_FOUND);
    assert(create_result(OWNER_A,"/Documents/.",1,&out)==BOS_E_INVALID);
    assert(create_result(OWNER_A,"/Documents/",1,&out)==BOS_E_INVALID);
    BosFileTransactionStatusV1 stage=begin_create(OWNER_A,"/Documents/parent/new",1);fill(OWNER_A,stage,1);
    int competing=fs_create(parent,"new");assert(competing>0);
    BosFileInfo result,saved;memset(&result,0xaa,sizeof result);saved=result;
    unchanged_error(native_file_transaction_accept(OWNER_A,stage.handle,&result),BOS_E_CHANGED,&result,&saved);
    assert(fs_delete(competing)==0&&fs_rename(parent,"moved")==0);
    unchanged_error(native_file_transaction_accept(OWNER_A,stage.handle,&result),BOS_E_CHANGED,&result,&saved);
    assert(fs_rename(parent,"parent")==0);
    assert(native_file_transaction_accept(OWNER_A,stage.handle,&result)==BOS_OK);exact(OWNER_A,result);
    stage=begin_create(OWNER_A,"/Documents/parent/next",3);
    assert(native_file_transaction_append(OWNER_A,stage.handle,"x",1,1)==BOS_E_INVALID);
    assert(native_file_transaction_append(OWNER_A,stage.handle,"abcd",4,0)==BOS_E_INVALID);
    assert(native_file_transaction_append(OWNER_A,stage.handle,"x",1,0)==1);
    assert(native_file_transaction_append(OWNER_A,stage.handle,"x",1,0)==BOS_E_INVALID);
    saved=result;unchanged_error(native_file_transaction_accept(OWNER_A,stage.handle,&result),BOS_E_INVALID,&result,&saved);
    assert(native_file_transaction_info(OWNER_A,stage.handle,&out)==BOS_OK&&out.received_bytes==1);
    assert(native_file_transaction_abort(OWNER_A,stage.handle)==BOS_OK&&fs_resolve(0,"/Documents/parent/next")<0);
    puts("create parent/absence binding, missing/non-directory NOT_FOUND and sequential decision table passed");
}
static void capacity_and_cleanup(void) {
    setup_files();unsigned baseline=stage_stats().free;
    BosFileTransactionStatusV1 a=begin_create(OWNER_A,"/Documents/a",262144),b=begin_create(OWNER_B,"/Documents/b",262144),out,before;
    assert(stage_stats().free==baseline-128);
    BosFileTransactionInfoV1 query;native_file_transactions_query(&query);
    assert(query.pages_used==128&&query.transactions_used==2&&query.file_bytes==fs_file_limit()&&query.total_bytes==262144);
    memset(&out,0xa5,sizeof out);before=out;
    assert(create_result(BOS_HANDLE_TYPE_PROCESS|3,"/Documents/c",1,&out)==BOS_E_CAPACITY&&!memcmp(&out,&before,sizeof out));
    assert(create_result(OWNER_A,"/Documents/second",0,&out)==BOS_E_CAPACITY);
    assert(native_file_transaction_abort(OWNER_A,a.handle)==BOS_OK&&stage_stats().free==baseline-64);
    native_files_release_owner(OWNER_A);assert(stage_stats().free==baseline-64);
    a=begin_create(OWNER_A,"/Documents/fresh",1);assert(a.handle!=b.handle);
    native_files_release_owner(OWNER_B);assert(stage_stats().free==baseline-1);
    native_files_init();assert(stage_stats().free==baseline);
    for(unsigned i=0;i<8;++i)out=begin_create(BOS_HANDLE_TYPE_PROCESS|(i+1),"/Documents/zero",0);
    assert(create_result(BOS_HANDLE_TYPE_PROCESS|9,"/Documents/ninth",0,&out)==BOS_E_CAPACITY);
    native_files_init();
    /* Consume real available pages through ordinary allocations, then release
     * them normally. There is no fabricated allocator accounting. */
    unsigned count=stage_stats().free-63,claimed=0;uint32_t *held=malloc(count*sizeof *held);assert(held);
    unsigned keeper=BOS_HANDLE_TYPE_PROCESS|100;
    while(claimed<count){unsigned n=count-claimed;if(n>PHYS_BATCH_MAX)n=PHYS_BATCH_MAX;
        assert(physmem_core_alloc(&stage_core,keeper,PHYS_USER_IMAGE,n,held+claimed)==PHYS_OK);claimed+=n;}
    assert(create_result(OWNER_A,"/Documents/pressure",262144,&out)==BOS_E_CAPACITY&&stage_stats().free==63);
    claimed=0;while(claimed<count){unsigned n=count-claimed;if(n>PHYS_BATCH_MAX)n=PHYS_BATCH_MAX;
        assert(physmem_core_release(&stage_core,keeper,PHYS_USER_IMAGE,held+claimed,n)==PHYS_OK);claimed+=n;}
    free(held);assert(stage_stats().free==baseline);
    puts("two independent maximum stages, aggregate/owner/record caps, real page pressure and exact cleanup passed");
}
static void acceptance_capacity(void) {
    setup_files();add_file("existing","old",3);
    BosFileInfo handles[8],out,before;
    for(unsigned i=0;i<8;++i)handles[i]=open_file(OWNER_A,"/Documents/existing",RW);
    BosFileTransactionStatusV1 stage=begin_create(OWNER_A,"/Documents/new",32769);fill(OWNER_A,stage,4096);
    memset(&out,0xa5,sizeof out);before=out;
    unchanged_error(native_file_transaction_accept(OWNER_A,stage.handle,&out),BOS_E_CAPACITY,&out,&before);
    assert(native_file_close(OWNER_A,handles[0].handle)==BOS_OK);
    assert(native_file_transaction_accept(OWNER_A,stage.handle,&out)==BOS_OK);exact(OWNER_A,out);
    setup_files();int docs=fs_resolve(0,"/Documents"),last=-1;
    while(fs_node_count()<64){char name[24];snprintf(name,sizeof name,"node%d",fs_node_count());last=fs_create(docs,name);assert(last>0);}
    unsigned capacity64=fs_capacity();
    stage=begin_create(OWNER_A,"/Documents/sixtyfive",1);fill(OWNER_A,stage,1);
    assert(native_file_transaction_accept(OWNER_A,stage.handle,&out)==BOS_OK);
    assert(fs_node_count()==65&&fs_capacity()==capacity64-40);exact(OWNER_A,out);
    while(fs_node_count()<fs_node_limit()){char name[24];snprintf(name,sizeof name,"node%d",fs_node_count());last=fs_create(docs,name);assert(last>0);}
    stage=begin_create(OWNER_A,"/Documents/overflow",1);fill(OWNER_A,stage,1);before=out;
    unsigned revision=next_content_revision,identity=next_identity;
    unchanged_error(native_file_transaction_accept(OWNER_A,stage.handle,&out),BOS_E_CAPACITY,&out,&before);
    assert(next_content_revision==revision&&next_identity==identity);
    assert(fs_delete(last)==0&&native_file_transaction_accept(OWNER_A,stage.handle,&out)==BOS_OK);exact(OWNER_A,out);
    setup_files();int target=add_file("target","old",3);out=open_file(OWNER_A,"/Documents/target",RW);
    memset(large_file,'z',sizeof large_file);last=-1;
    while(fs_used_bytes()<fs_capacity()){char name[24];snprintf(name,sizeof name,"fill%d",fs_node_count());last=fs_create(fs_resolve(0,"/Documents"),name);assert(last>0);
        unsigned n=fs_capacity()-fs_used_bytes();if(n>sizeof large_file)n=sizeof large_file;assert(fs_write(last,(const char *)large_file,n)==(int)n);}
    stage=begin_replace(OWNER_A,out,262144);fill(OWNER_A,stage,4096);before=out;revision=next_content_revision;
    unchanged_error(native_file_transaction_accept(OWNER_A,stage.handle,&out),BOS_E_CAPACITY,&out,&before);
    assert(next_content_revision==revision&&fs_size(target)==3&&!memcmp(fs_data(target),"old",3));
    assert(fs_delete(last)==0&&native_file_transaction_accept(OWNER_A,stage.handle,&out)==BOS_OK);exact(OWNER_A,out);
    assert(pool_used==fs_used_bytes()+(unsigned)fs_node_count()-2); /* all remaining files nonempty */
    puts("create file-handle retry, 64/65-node metadata allowance, full-node/full-byte retry and version preservation passed");
}
static void snapshot_and_mount(void) {
    setup_files();int id=add_file("target","old",3);unsigned baseline=stage_stats().free;
    BosFileInfo file=open_file(OWNER_A,"/Documents/target",RW),before=file;
    BosFileTransactionStatusV1 stage=begin_replace(OWNER_A,file,32769),status;fill(OWNER_A,stage,4096);
    FsSyncTicket ticket;assert(fs_sync_request(&ticket)==0&&fs_sync_busy());
    unsigned incarnation=fs_incarnation();assert(fs_init()==FS_ERR_BUSY&&fs_incarnation()==incarnation);
    unchanged_error(native_file_transaction_accept(OWNER_A,stage.handle,&file),BOS_E_BUSY,&file,&before);
    BosFileTransactionStatusV1 peer=begin_create(OWNER_B,"/Documents/peer",1);fill(OWNER_B,peer,1);
    assert(native_file_transaction_abort(OWNER_B,peer.handle)==BOS_OK);
    while(fs_sync_busy())fs_sync_step();
    assert(fs_sync_result(ticket)==0&&fs_sync_release(ticket)==0);
    assert(fs_size(id)==3&&!memcmp(fs_data(id),"old",3));
    assert(native_file_transaction_accept(OWNER_A,stage.handle,&file)==BOS_OK);exact(OWNER_A,file);
    assert(fs_sync()==0);
    stage=begin_replace(OWNER_A,file,262144);fill(OWNER_A,stage,4096);
    assert(fs_init()==0&&fs_load_disk()==0);
    native_file_transactions_tick();
    assert(stage_stats().free==baseline&&native_file_transaction_info(OWNER_A,stage.handle,&status)==BOS_OK);
    assert(status.state==BOS_FILE_TRANSACTION_INVALIDATED&&status.total_bytes==262144&&!status.received_bytes);
    assert(native_file_transaction_append(OWNER_A,stage.handle,"x",1,0)==BOS_E_CHANGED);
    before=file;unchanged_error(native_file_transaction_accept(OWNER_A,stage.handle,&file),BOS_E_CHANGED,&file,&before);
    assert(native_file_transaction_abort(OWNER_A,stage.handle)==BOS_OK&&native_file_transaction_abort(OWNER_A,stage.handle)==BOS_E_STALE);
    native_files_init();file=open_file(OWNER_A,"/Documents/target",RW);assert(file.size==32769);exact(OWNER_A,file);
    stage=begin_replace(OWNER_A,file,1);
    assert(fs_init()==0&&!native_file_transactions_available());
    assert(native_file_transaction_info(OWNER_A,stage.handle,&status)==BOS_OK&&status.state==BOS_FILE_TRANSACTION_INVALIDATED);
    assert(create_result(OWNER_B,"/Documents/floppy",1,&status)==BOS_E_UNSUPPORTED);
    assert(native_file_transaction_abort(OWNER_A,stage.handle)==BOS_OK&&stage_stats().free==baseline);
    puts("private upload during snapshot, BUSY retry, normal durable remount and invalidated-record cleanup passed");
}
static void ordinary_pool_latency(void) {
    static const unsigned originals[]={262144,262144,1}, replacements[]={262144,131072,262144};
    for(unsigned placement=0;placement<3;++placement)for(unsigned shape=0;shape<3;++shape){
        setup_files();memset(large_file,'z',sizeof large_file);
        unsigned target_total=fs_capacity()-262144, half=target_total/2;
        int target=-1;
        if(!placement)target=add_file("timed","x",1);
        while(fs_used_bytes()+originals[shape]<target_total){
            if(placement==1&&target<0&&fs_used_bytes()>=half)target=add_file("timed","x",1);
            char name[24];snprintf(name,sizeof name,"pool%d",fs_node_count());
            unsigned count=target_total-fs_used_bytes()-originals[shape];
            if(count>sizeof large_file)count=sizeof large_file;
            assert(count&&add_file(name,(const char *)large_file,count)>0);
        }
        if(target<0)target=add_file("timed","x",1);
        /* Prepare the intended physical placement with an ordinary same-size
         * temporary file, then leave the target where requested. */
        if(originals[shape]!=1){
            assert(fs_write(target,(const char *)large_file,originals[shape])==(int)originals[shape]);
            /* A changed-size write moves to the end; rebuild its exact desired
             * placement through normal removal/recreation of ordinary peers. */
            if(placement<2){
                int peers[FS_MAX_NODES],counts[FS_MAX_NODES],n=0;
                for(int i=0;i<fs_node_limit();++i)if(fs_valid(i)&&!fs_is_dir(i)&&i!=target){peers[n]=i;counts[n++]=fs_size(i);}
                unsigned chosen=placement? (unsigned)n/2:0;
                for(unsigned i=chosen;i<(unsigned)n;++i){
                    char name[FS_NAME_LEN];kstrcpy(name,fs_name(peers[i]));
                    assert(fs_delete(peers[i])==0&&add_file(name,(const char *)large_file,counts[i])>0);
                }
            }
        }
        BosFileInfo file=open_file(OWNER_A,"/Documents/timed",RW);
        BosFileTransactionStatusV1 stage=begin_replace(OWNER_A,file,replacements[shape]);fill(OWNER_A,stage,4096);
        unsigned offset=nodes[target].offset,occupied=pool_used;
        struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
        assert(native_file_transaction_accept(OWNER_A,stage.handle,&file)==BOS_OK);
        clock_gettime(CLOCK_MONOTONIC,&b);exact(OWNER_A,file);
        for(int i=0;i<fs_node_limit();++i)if(fs_valid(i)&&!fs_is_dir(i)&&i!=target){
            assert(fs_size(i)<=(int)sizeof large_file);
            assert(!memcmp(fs_data(i),large_file,fs_size(i)));
        }
        printf("host pool acceptance placement=%u shape=%u pool=%u target_offset=%u old=%u new=%u elapsed_ms=%.3f\n",
            placement,shape,occupied,offset,originals[shape],replacements[shape],elapsed_ms(a,b));
    }
}
int main(int argc,char **argv) {
    unsigned ram=argc==2?(unsigned)strtoul(argv[1],0,10):64;
    stage_allocator_init(ram);
    completed_uploads();conflict_and_binding();create_binding_and_errors();capacity_and_cleanup();acceptance_capacity();snapshot_and_mount();ordinary_pool_latency();
    native_files_init();stage_allocator_destroy();
    printf("file transactions ordinary host gates passed, %u MiB allocator map\n",ram);return 0;
}
