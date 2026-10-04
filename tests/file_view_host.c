/* Ordinary file/folder workflows on valid in-memory disks. No malformed
 * snapshots, out-of-bounds inputs or deliberate CPU/memory fault probes. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"

static unsigned char node_arena[FS_CAPACITY];
static unsigned char image_arena[FS_IMG_CAPACITY];
static unsigned char pool_arena[FS_POOL_CAPACITY];
#undef FS_BASE
#undef FS_IMG_BASE
#undef FS_POOL_BASE
#define FS_BASE ((uintptr_t)node_arena)
#define FS_IMG_BASE ((uintptr_t)image_arena)
#define FS_POOL_BASE ((uintptr_t)pool_arena)
int platform_memory_range_available(uint32_t base, uint32_t end) { (void)base; (void)end; return 0; }
#include "../src/fs.c"
#include "ata_async_stub.h"
#include "file_clipboard.h"
#include "file_view.h"

static unsigned char floppy_disk[DISK_SECTORS * SECTOR_SIZE];
static unsigned char ide_disk[DATA_DISK_SECTORS * SECTOR_SIZE];

static int ide_present, device_unavailable, writes;

uint32_t timer_ticks(void) { return 0; }
void platform_log(const char *text) { (void)text; }
unsigned disk_sector_count(void) { return DISK_SECTORS; }
int disk_read(unsigned lba, void *buffer, int count) {
    assert(count >= 0 && lba + (unsigned)count <= DISK_SECTORS);
    memcpy(buffer, floppy_disk + lba * SECTOR_SIZE, (unsigned)count * SECTOR_SIZE);
    return 0;
}
int disk_write(unsigned lba, const void *buffer, int count) {
    assert(count >= 0 && lba >= FS_DISK_LBA && lba + (unsigned)count <= DISK_SECTORS);
    ++writes;
    if (device_unavailable) return -1;
    memcpy(floppy_disk + lba * SECTOR_SIZE, buffer, (unsigned)count * SECTOR_SIZE);
    return 0;
}
int ata_probe(void) { return ide_present; }
unsigned ata_sector_count(void) { return DATA_DISK_SECTORS; }
int ata_read(unsigned lba, void *buffer, int count) {
    assert(count >= 0 && lba + (unsigned)count <= DATA_DISK_SECTORS);
    memcpy(buffer, ide_disk + lba * SECTOR_SIZE, (unsigned)count * SECTOR_SIZE);
    return 0;
}
int ata_write(unsigned lba, const void *buffer, int count) {
    assert(count >= 0 && lba && lba + (unsigned)count <= DATA_DISK_SECTORS);
    ++writes;
    if (device_unavailable) return -1;
    memcpy(ide_disk + lba * SECTOR_SIZE, buffer, (unsigned)count * SECTOR_SIZE);
    return 0;
}
int ata_flush(void) { return 0; }

static void reset_volume(int ide) {
    memset(floppy_disk, 0, sizeof(floppy_disk));
    memset(ide_disk, 0, sizeof(ide_disk));
    ide_present = ide;
    device_unavailable = writes = 0;
    if (ide) {
        unsigned marker[] = {DATA_MARKER_MAGIC, DATA_MARKER_VERSION,
                            DATA_DISK_SECTORS, DATA_SLOT_SECTORS,
                            DATA_FIRST_LBA, DATA_SECOND_LBA, 0};
        marker[6] = crc32(marker, 24);
        memcpy(ide_disk, marker, sizeof(marker));
    }
    fs_init();
    assert(fs_load_disk() == FS_LOAD_BLANK);
    fs_empty_dir(0);
    assert(fs_node_count() == 1 && fs_sync() == 0);
    file_clipboard_clear();
    assert(file_clipboard_mode() == FILE_CLIPBOARD_NONE);
    assert(!file_clipboard_can_paste(0) && !file_clipboard_pending_sync());
}


static int make_file(int parent,const char *name,int bytes) {
    static char data[128];
    int id=fs_create(parent,name);
    assert(id>0 && bytes<=(int)sizeof(data));
    assert(fs_write(id,data,bytes)==bytes);
    return id;
}

static int ids[FS_MAX_NODES],total;
static unsigned view_identities[FS_MAX_NODES];
static FileViewOptions options;

static int build(int folder) {
    int count=file_view_build(folder,&options,ids,view_identities,FS_MAX_NODES,&total);
    for(int i=0;i<count;i++){
        if(!file_view_valid(folder,ids[i],view_identities[i]))fprintf(stderr,"folder=%d id=%d identity=%u actual=%u parent=%d sort=%d\n",folder,ids[i],view_identities[i],fs_identity(ids[i]),fs_parent(ids[i]),options.sort);
        assert(file_view_valid(folder,ids[i],view_identities[i]));
        if(i)assert(file_view_compare(ids[i-1],ids[i],&options)<=0);
    }
    return count;
}

static void matching(void) {
    assert(file_view_matches("report.TXT",""));
    assert(file_view_matches("report.TXT","PORT.t"));
    assert(file_view_matches("report.TXT","report.TXT"));
    assert(!file_view_matches("report.TXT","reports"));
    assert(!file_view_matches("report.TXT","report.TXTmore"));
    assert(!file_view_matches("","a"));
    assert(file_view_matches("",""));
    assert(file_view_matches("12345678901234567890123","12345678901234567890123"));
    assert(file_view_matches("quarter one.txt"," one."));
    assert(!file_view_matches("report.TXT","*.txt"));
}

static void sort_and_filter(void) {
    reset_volume(1);memset(&options,0,sizeof(options));
    int folder=fs_mkdir(0,"Work");
    int z=make_file(folder,"zebra.TXT",7);
    int alpha=make_file(folder,"alpha.txt",7);
    int mixed=make_file(folder,"Alpha.txt",14);
    int noext=make_file(folder,"readme",0);
    int native=make_file(folder,"tool.bex",32);
    int sheet=make_file(folder,"budget.bsh",64);
    int dot=make_file(folder,".hidden",2);
    int multi=make_file(folder,"archive.tar.gz",3);
    int trailing=make_file(folder,"trailing.",4);
    int app=fs_create_app(folder,"Application");
    int zdir=fs_mkdir(folder,"Z Folder"),adir=fs_mkdir(folder,"A Folder");
    nodes[z].modified=900;nodes[alpha].modified=900;nodes[mixed].modified=1500;
    assert(!strcmp(file_view_type(folder),"Folder"));
    assert(!strcmp(file_view_type(app),"Application"));
    assert(!strcmp(file_view_type(z),"TXT"));
    assert(!strcmp(file_view_type(noext),"File"));
    assert(!strcmp(file_view_type(dot),"File"));
    assert(!strcmp(file_view_type(trailing),"File"));
    assert(!strcmp(file_view_type(multi),"gz"));
    assert(!strcmp(file_view_type(native),"bex"));
    assert(!strcmp(file_view_type(sheet),"bsh"));
    for(int sort=0;sort<FILE_VIEW_SORT_COUNT;sort++)for(int direction=0;direction<2;direction++){
        options.sort=sort;options.descending=direction;
        assert(build(folder)==12 && total==12);
        assert(fs_is_dir(ids[0])&&fs_is_dir(ids[1]));
        for(int i=2;i<12;i++)assert(!fs_is_dir(ids[i]));
        int old[FS_MAX_NODES];unsigned old_identity[FS_MAX_NODES];
        memcpy(old,ids,sizeof(ids));memcpy(old_identity,view_identities,sizeof(view_identities));
        assert(build(folder)==12);
        assert(!memcmp(old,ids,sizeof(ids))&&!memcmp(old_identity,view_identities,sizeof(view_identities)));
        /* Equal size and modified keys keep alpha before zebra in either direction. */
        if(sort==FILE_VIEW_SIZE||sort==FILE_VIEW_MODIFIED||sort==FILE_VIEW_TYPE)
            assert(file_view_find(ids,view_identities,12,alpha,fs_identity(alpha))<
                   file_view_find(ids,view_identities,12,z,fs_identity(z)));
    }
    options.sort=FILE_VIEW_NAME;options.descending=0;assert(build(folder)==12);
    assert(ids[0]==adir&&ids[1]==zdir);
    options.descending=1;assert(build(folder)==12);assert(ids[0]==zdir&&ids[1]==adir);
    strcpy(options.filter,"TXT");assert(build(folder)==3&&total==12);
    strcpy(options.filter,"ALPHA");assert(build(folder)==2&&total==12);
    strcpy(options.filter," Folder");assert(build(folder)==2&&total==12);
    strcpy(options.filter,"absent");assert(build(folder)==0&&total==12);
    options.filter[0]=0;
    assert(fs_sync()==0);
    int dirty=fs_needs_sync(),write_count=writes;
    assert(!dirty);
    assert(build(folder)==12&&fs_needs_sync()==dirty&&writes==write_count);
}

static void identity_and_clipboard(void) {
    reset_volume(1);memset(&options,0,sizeof(options));
    int folder=fs_mkdir(0,"Folder"),target=fs_mkdir(0,"Target");
    int first=make_file(folder,"first.txt",4),second=make_file(folder,"second.txt",8);
    unsigned original=fs_identity(first);
    assert(build(folder)==2);
    assert(file_view_find(ids,view_identities,2,first,original)==0);
    options.sort=FILE_VIEW_SIZE;options.descending=1;assert(build(folder)==2);
    assert(file_view_find(ids,view_identities,2,first,original)==1);
    strcpy(options.filter,"second");assert(build(folder)==1);
    assert(file_view_find(ids,view_identities,1,first,original)==-1);
    assert(file_view_find(ids,view_identities,1,second,fs_identity(second))==0);
    assert(!file_view_valid(target,first,original));
    options.filter[0]=0;
    assert(fs_rename(first,"new-name.txt")==0 && build(folder)==2);
    assert(file_view_find(ids,view_identities,2,first,original)>=0);
    assert(file_clipboard_set(first,FILE_CLIPBOARD_COPY)==0);
    assert(fs_delete(first)==0);
    int replacement=make_file(folder,"replacement.txt",16);
    assert(replacement==first&&fs_identity(replacement)!=original);
    assert(!file_view_valid(folder,first,original));
    assert(build(folder)==2&&file_view_find(ids,view_identities,2,first,original)==-1);
    assert(file_clipboard_paste(target,0)==FILE_CLIPBOARD_ERROR);
    assert(fs_child_count(target)==0);
    assert(file_clipboard_set(second,FILE_CLIPBOARD_CUT)==0);
    unsigned second_identity=fs_identity(second);
    int result=-1;assert(file_clipboard_paste(target,&result)==FILE_CLIPBOARD_SYNCED);
    assert(result==second&&file_view_valid(target,second,second_identity));
    assert(!file_view_valid(folder,second,second_identity));
    assert(build(folder)==1&&ids[0]==replacement);
    assert(build(target)==1&&ids[0]==second);
    assert(fs_rename(second,"renamed.txt")==0&&build(target)==1);
    assert(file_view_find(ids,view_identities,1,second,second_identity)==0);
}

static void full_volume(int ide) {
    reset_volume(ide);memset(&options,0,sizeof(options));
    int limit=fs_node_limit();
    for(int i=1;i<limit;i++){
        char name[FS_NAME_LEN];snprintf(name,sizeof(name),"item %03d.txt",limit-i);
        assert(make_file(0,name,i%128)==i);
    }
    assert(fs_node_count()==limit);
    for(int sort=0;sort<FILE_VIEW_SORT_COUNT;sort++)for(int direction=0;direction<2;direction++){
        options.sort=sort;options.descending=direction;
        assert(build(0)==limit-1&&total==limit-1);
    }
    strcpy(options.filter,"item 0");int count=build(0);
    assert(count==(limit==64?63:99));
    options.filter[0]=0;options.sort=FILE_VIEW_NAME;options.descending=0;
    assert(build(0)==limit-1&&!strcmp(fs_name(ids[0]),"item 001.txt"));
    assert(fs_sync()==0);fs_init();assert(fs_load_disk()==0);
    assert(build(0)==limit-1&&!strcmp(fs_name(ids[0]),"item 001.txt"));
}

static void hidden_root_and_empty(void) {
    reset_volume(1);memset(&options,0,sizeof(options));
    assert(fs_mkdir(0,"trash")>0&&fs_mkdir(0,"prefs")>0);
    int folder=fs_mkdir(0,"Visible");
    assert(build(0)==1&&total==1&&ids[0]==folder);
    assert(build(folder)==0&&total==0);
}

int main(void) {
    matching();sort_and_filter();identity_and_clipboard();
    full_volume(0);full_volume(1);hidden_root_and_empty();
    puts("Files view: sorting, filtering, ties, identity, clipboard and 64/256-node volumes passed");
    return 0;
}
