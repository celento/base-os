/* Ordinary Calendar agenda workflows and returned storage errors, with the real FS model.
 * No memory faults, fuzzing, debugger or QEMU work. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
static unsigned char node_arena[FS_CAPACITY], image_arena[FS_IMG_CAPACITY], pool_arena[FS_POOL_CAPACITY];
#undef FS_BASE
#undef FS_IMG_BASE
#undef FS_POOL_BASE
#define FS_BASE ((uintptr_t)node_arena)
#define FS_IMG_BASE ((uintptr_t)image_arena)
#define FS_POOL_BASE ((uintptr_t)pool_arena)
#include "../src/fs.c"
#include "ata_async_stub.h"
static unsigned char floppy[DISK_SECTORS * SECTOR_SIZE], ide[DATA_DISK_SECTORS * SECTOR_SIZE];
static int ide_present, fail_write, transport_writes;
static unsigned now;
int platform_memory_range_available(uint32_t base, uint32_t end) { (void)base; (void)end; return 0; }
void platform_log(const char *text) { (void)text; }
uint32_t timer_ticks(void) { return now; }
unsigned disk_sector_count(void) { return DISK_SECTORS; }
int disk_read(unsigned lba, void *buf, int count) {
    assert(count >= 0 && lba + count <= DISK_SECTORS);
    memcpy(buf, floppy + lba * SECTOR_SIZE, count * SECTOR_SIZE); return 0;
}
int disk_write(unsigned lba, const void *buf, int count) {
    assert(count >= 0 && lba + count <= DISK_SECTORS); ++transport_writes;
    if (fail_write) return -1;
    memcpy(floppy + lba * SECTOR_SIZE, buf, count * SECTOR_SIZE); return 0;
}
int ata_probe(void) { return ide_present; }
unsigned ata_sector_count(void) { return DATA_DISK_SECTORS; }
int ata_read(unsigned lba, void *buf, int count) {
    assert(count >= 0 && lba + count <= DATA_DISK_SECTORS);
    memcpy(buf, ide + lba * SECTOR_SIZE, count * SECTOR_SIZE); return 0;
}
int ata_write(unsigned lba, const void *buf, int count) {
    assert(count >= 0 && lba + count <= DATA_DISK_SECTORS); ++transport_writes;
    if (fail_write) return -1;
    memcpy(ide + lba * SECTOR_SIZE, buf, count * SECTOR_SIZE); return 0;
}
int ata_flush(void) { return 0; }
static unsigned agenda_writes, agenda_mutations;
static int reject_write, reject_create, reject_mkdir;
static int agenda_test_write(int id, const char *data, int n) {
    ++agenda_writes; ++agenda_mutations;
    return reject_write ? -1 : fs_write(id, data, n);
}
static int agenda_test_mkdir(int parent, const char *name) {
    ++agenda_mutations;
    return reject_mkdir ? -1 : fs_mkdir(parent, name);
}
static int agenda_test_create(int parent, const char *name) {
    ++agenda_mutations;
    return reject_create ? -1 : fs_create(parent, name);
}
static int agenda_test_delete(int id) { ++agenda_mutations; return fs_delete(id); }
#define fs_write agenda_test_write
#define fs_mkdir agenda_test_mkdir
#define fs_create agenda_test_create
#define fs_delete agenda_test_delete
#include "../src/calendar_agenda.c"
#undef fs_write
#undef fs_mkdir
#undef fs_create
#undef fs_delete
static void reset_agenda(void) {
    ca_loaded=ca_pending=ca_state=ca_count=0; ca_next_id=1;
    memset(&ca_draft,0,sizeof ca_draft);
    agenda_writes=agenda_mutations=0;
    reject_write=reject_create=reject_mkdir=0;
    cal_agenda_load();
}
static void reset_volume(int marked) {
    assert(!fs_sync_busy());
    memset(floppy,0,sizeof floppy); memset(ide,0,sizeof ide);
    ide_present=marked; fail_write=transport_writes=0; now=0;
    if (marked) {
        unsigned marker[]={DATA_MARKER_MAGIC,DATA_MARKER_VERSION,DATA_DISK_SECTORS,
            DATA_SLOT_SECTORS,DATA_FIRST_LBA,DATA_SECOND_LBA,0};
        marker[6]=crc32(marker,24); memcpy(ide,marker,sizeof marker);
    }
    assert(!fs_init() && fs_load_disk()==FS_LOAD_BLANK);
    assert(!fs_empty_dir(fs_root()) && !fs_sync()); reset_agenda();
}
static void drain(void) {
    unsigned steps=0;
    while (fs_sync_busy()) { assert(fs_sync_step()!=FS_SYNC_IDLE); assert(++steps<100000); }
}
static FsSyncTicket begin_snapshot(void) {
    FsSyncTicket ticket; assert(fs_needs_sync());
    assert(!fs_sync_request(&ticket) && fs_sync_busy()); return ticket;
}
static void finish_snapshot(FsSyncTicket ticket) {
    drain(); assert(!fs_sync_result(ticket)); assert(!fs_sync_release(ticket));
}
static void reboot(void) { assert(!fs_init() && !fs_load_disk()); reset_agenda(); }
static unsigned add(const char *title) {
    unsigned id=cal_agenda_add(2026,10,4,-1,title); assert(id); return id;
}
static void status(int state,const char *fragment) {
    assert(ca_state==state && strstr(cal_agenda_status(),fragment));
}
static void set_draft(unsigned id,const char *date,const char *time,const char *title,int field) {
    CalAgendaDraft d={0}; d.active=1; d.edit_id=id; d.field=field;
    strcpy(d.date,date); strcpy(d.time,time); strcpy(d.title,title);
    assert(cal_agenda_set_draft(&d));
}
static void dates_and_crud(void) {
    reset_volume(1);
    assert(cal_agenda_days_in_month(1900,2)==28 && cal_agenda_days_in_month(2000,2)==29);
    assert(!cal_agenda_valid_date(1900,2,29) && cal_agenda_valid_date(2000,2,29));
    assert(!cal_agenda_valid_date(999,1,1) && !cal_agenda_valid_date(10000,1,1));
    int y=2024,m=1,d=31,minute;
    assert(cal_agenda_step_month(&y,&m,&d,1) && y==2024 && m==2 && d==29);
    assert(cal_agenda_step_day(&y,&m,&d,1) && m==3 && d==1);
    assert(cal_agenda_step_day(&y,&m,&d,-1) && m==2 && d==29);
    y=1900;m=d=1; assert(!cal_agenda_step_day(&y,&m,&d,-1) && y==1900 && m==1 && d==1);
    assert(!cal_agenda_step_month(&y,&m,&d,-1));
    y=9999;m=12;d=31; assert(!cal_agenda_step_day(&y,&m,&d,1) && y==9999 && d==31);
    assert(!cal_agenda_step_month(&y,&m,&d,1) && !cal_agenda_step_day(&y,&m,&d,2));
    assert(cal_agenda_parse_date("2024-02-29",&y,&m,&d) && y==2024 && m==2 && d==29);
    assert(!cal_agenda_parse_date("2023-02-29",&y,&m,&d) && !cal_agenda_parse_date("2024-2-29",&y,&m,&d));
    assert(cal_agenda_parse_time("",&minute) && minute==-1);
    assert(cal_agenda_parse_time("00:00",&minute) && minute==0);
    assert(cal_agenda_parse_time("23:59",&minute) && minute==1439);
    assert(!cal_agenda_parse_time("24:00",&minute) && !cal_agenda_parse_time("11:60",&minute));
    assert(!cal_agenda_parse_time("9:00",&minute) && !cal_agenda_parse_time("  ",&minute));
    assert(!cal_agenda_add(2023,2,29,0,"bad") && !cal_agenda_add(2024,2,29,1440,"bad"));
    assert(!cal_agenda_add(2024,2,29,0,"") && !cal_agenda_add(2024,2,29,0,"  "));
    unsigned late=cal_agenda_add(2026,10,4,900,"late"), all=add("all day"), early=cal_agenda_add(2026,10,4,0,"midnight");
    assert(late && all && early && cal_agenda_get(0)->id==all && cal_agenda_get(1)->id==early && cal_agenda_get(2)->id==late);
    assert(cal_agenda_update(late,2026,9,30,1439,"moved") && cal_agenda_get(0)->id==late);
    assert(cal_agenda_update(early,2026,10,4,5,cal_agenda_find(early)->title));
    assert(!cal_agenda_get(-1) && !cal_agenda_get(3) && !cal_agenda_find(0));
    set_draft(all,"2026-10-04","","editing",2);
    assert(!cal_agenda_delete(all)); assert(cal_agenda_clear_draft()); assert(cal_agenda_delete(all));
    unsigned newer=add("newer"); assert(newer>early && !cal_agenda_find(all));
    assert(!agenda_mutations); /* No storage work on the key/input path. */
    assert(!cal_agenda_retry_save()); reboot();
    assert(cal_agenda_count()==3 && !cal_agenda_find(all) && cal_agenda_find(newer));
    puts("Calendar: leap dates, endpoint navigation, strict time parsing and stable sorted CRUD passed");
}
static void busy_draft_recovery(void) {
    reset_volume(1); unsigned first=add("original"); cal_agenda_tick(); status(CA_RAM,"RAM");
    FsSyncTicket ticket=begin_snapshot(); unsigned mutations=agenda_mutations;
    assert(cal_agenda_update(first,2026,10,4,600,"changed")); unsigned second=add("second");
    set_draft(first,"2026-02-3","2:","unfinished title",1);
    for (int i=0;i<1000;++i) assert(!cal_agenda_tick());
    assert(agenda_mutations==mutations && agenda_writes==1);
    assert(cal_agenda_retry_save()==FS_ERR_BUSY);
    cal_agenda_load(); assert(cal_agenda_count()==2 && !strcmp(cal_agenda_draft()->time,"2:"));
    assert(!cal_agenda_commit_draft() && cal_agenda_draft()->active);
    finish_snapshot(ticket); assert(cal_agenda_tick()); assert(agenda_writes==2);
    assert(!cal_agenda_retry_save()); status(CA_SAVED,"saved");
    reboot(); assert(cal_agenda_count()==2 && cal_agenda_find(first)->minute==600 && cal_agenda_find(second));
    const CalAgendaDraft *d=cal_agenda_draft();
    assert(d->active && d->edit_id==first && d->field==1 && !strcmp(d->date,"2026-02-3") && !strcmp(d->time,"2:") && !strcmp(d->title,"unfinished title"));
    unsigned unchanged=agenda_writes;
    assert(cal_agenda_set_draft(cal_agenda_draft()) && !ca_pending);
    for (int i=0;i<1000;++i) assert(!cal_agenda_tick());
    assert(agenda_writes==unchanged && ca_state==CA_SAVED);
    set_draft(first,"2028-02-29","23:59","edited",2);
    unsigned writes=agenda_writes; assert(cal_agenda_commit_draft()==first && !cal_agenda_draft()->active && agenda_writes==writes);
    assert(!cal_agenda_retry_save()); reboot();
    assert(!cal_agenda_draft()->active && cal_agenda_find(first)->year==2028 && cal_agenda_find(first)->minute==1439);
    unchanged=agenda_writes; assert(cal_agenda_clear_draft() && !ca_pending);
    assert(!cal_agenda_tick() && agenda_writes==unchanged);
    set_draft(0,"1900-01-01","","new from draft",0);
    unsigned third=cal_agenda_commit_draft(); assert(third>second && !cal_agenda_draft()->active);
    puts("Calendar: busy coalescing, no lease mutation, invalid draft recovery and atomic draft commit passed");
}
static void missing_and_colliding_paths(void) {
    reset_volume(1); assert(fs_create(0,"trigger")>0); FsSyncTicket ticket=begin_snapshot();
    int count=fs_node_count(); add("retained"); set_draft(0,"2026-","0","draft",2);
    assert(!cal_agenda_tick() && !agenda_mutations && fs_node_count()==count && fs_find_child(0,"prefs")<0);
    finish_snapshot(ticket); assert(cal_agenda_tick()); assert(fs_node_count()==count+2);
    assert(!cal_agenda_retry_save());
    int p=fs_find_child(0,"prefs"),f=agenda_file(); unsigned identity=fs_identity(f);
    assert(!fs_delete(f)); int unrelated=fs_create(p,"unrelated"); assert(unrelated==f && fs_identity(f)!=identity);
    assert(fs_write(unrelated,"keep",4)==4); add("second"); cal_agenda_tick();
    assert(!memcmp(fs_data(unrelated),"keep",4) && agenda_file()!=unrelated);
    f=agenda_file(); assert(!fs_rename(p,"old prefs")); add("third"); cal_agenda_tick();
    assert(fs_parent(agenda_file())!=p && fs_valid(f));
    for (int kind=0;kind<4;++kind) {
        reset_volume(1); p=kind<2?0:fs_mkdir(0,"prefs"); const char *name=kind<2?"prefs":AGENDA_NAME;
        f=kind==0?fs_create(p,name):kind==1||kind==3?fs_create_app(p,name):fs_mkdir(p,name);
        assert(f>0); if (!kind) assert(fs_write(f,"personal",8)==8);
        count=fs_node_count(); unsigned used=fs_used_bytes();
        add("retained"); set_draft(0,"partial","","draft",2); cal_agenda_tick(); status(CA_PATH_FAILED,"occupied");
        for (int i=0;i<1000;++i) assert(!cal_agenda_tick());
        cal_agenda_load(); assert(cal_agenda_count()==1 && cal_agenda_draft()->active);
        assert(!agenda_mutations && fs_node_count()==count && fs_used_bytes()==used);
        assert(cal_agenda_prepare_shutdown()<0); assert(!fs_rename(f,"unrelated")); assert(!cal_agenda_retry_save());
    }
    puts("Calendar: fresh fixed paths, missing-path busy deferral, node reuse and app/directory collisions passed");
}
static void source_conflicts(void) {
    for (int replacement=0;replacement<2;++replacement) {
        reset_volume(1); unsigned id=add("original"); assert(!cal_agenda_retry_save());
        int f=agenda_file(); unsigned identity=fs_identity(f);
        if (replacement) { assert(!fs_delete(f)); assert(fs_create(fs_find_child(0,"prefs"),AGENDA_NAME)==f); assert(fs_identity(f)!=identity); }
        assert(fs_write(f,"external",8)==8); assert(cal_agenda_tick()); status(CA_RELOAD,"changed");
        assert(!ca_pending && !cal_agenda_prepare_shutdown());
        assert(cal_agenda_update(id,2026,10,4,100,"private")); cal_agenda_tick(); status(CA_CONFLICT,"changed");
        unsigned writes=agenda_writes; for (int i=0;i<1000;++i) assert(!cal_agenda_tick());
        assert(cal_agenda_retry_save()<0 && agenda_writes==writes && !memcmp(fs_data(f),"external",8));
        assert(!fs_rename(f,"old calendar")); assert(!cal_agenda_retry_save()); assert(!memcmp(fs_data(f),"external",8));
    }
    reset_volume(1); unsigned id=add("RAM appointment"); cal_agenda_tick(); int f=agenda_file();
    assert(fs_write(f,"external",8)==8); assert(cal_agenda_tick()); status(CA_CONFLICT,"changed");
    assert(ca_pending && cal_agenda_prepare_shutdown()<0 && cal_agenda_find(id));
    reset_volume(1); add("one"); cal_agenda_tick(); FsSyncTicket ticket=begin_snapshot(); add("two");
    finish_snapshot(ticket); f=agenda_file(); assert(fs_write(f,"external",8)==8);
    cal_agenda_tick(); assert(cal_agenda_count()==2 && ca_pending && !memcmp(fs_data(f),"external",8));
    /* A saved active draft is still recovery work if its source is replaced.
     * Reopen must preserve it and must not misleadingly promise a reload. */
    reset_volume(1); id=add("with draft");
    set_draft(id,"2026-10-","1:","precious draft",1); assert(!cal_agenda_retry_save());
    f=agenda_file(); assert(fs_write(f,"external",8)==8);
    assert(cal_agenda_tick()); status(CA_CONFLICT,"draft kept");
    cal_agenda_load(); assert(ca_pending && ca_draft.active && ca_draft.edit_id==id);
    assert(!strcmp(ca_draft.title,"precious draft") && cal_agenda_prepare_shutdown()<0);
    assert(cal_agenda_retry_save()<0 && !memcmp(fs_data(f),"external",8));
    assert(!fs_rename(f,"external calendar")); assert(!cal_agenda_retry_save()); reboot();
    assert(ca_draft.active && !strcmp(ca_draft.title,"precious draft") && cal_agenda_find(id));
    /* A saved agenda without a draft may intentionally reopen a new supported
     * source, with no invented dirty work or shutdown blocker. */
    reset_volume(1); id=add("before external edit"); assert(!cal_agenda_retry_save());
    f=agenda_file(); int n=fs_size(f); static char replacement[CAL_AGENDA_FILE_MAX];
    memcpy(replacement,fs_data(f),n); replacement[AGENDA_HEADER+12]='B';
    put32(replacement+24,wire_crc(replacement,n)); assert(fs_write(f,replacement,n)==n);
    assert(cal_agenda_tick()); status(CA_RELOAD,"reopen");
    assert(!ca_pending && !cal_agenda_prepare_shutdown()); cal_agenda_load();
    assert(cal_agenda_find(id)->title[0]=='B' && !ca_pending);
    puts("Calendar: exact-source/identity conflicts, saved-draft recovery and deliberate clean reload passed");
}
static void bounded_format_and_validation(void) {
    reset_volume(0); char title[96]; memset(title,'z',95); title[95]=0;
    for (int i=0;i<CAL_AGENDA_MAX;++i) assert(cal_agenda_add(2026,10,4,i,title));
    set_draft(0,"9999-12-31","23:59",title,2);
    assert(!cal_agenda_commit_draft() && ca_draft.active && !cal_agenda_add(2026,10,4,0,title));
    assert(!cal_agenda_retry_save()); int f=agenda_file(); assert(fs_size(f)==CAL_AGENDA_FILE_MAX && fs_size(f)<16383);
    reboot(); assert(cal_agenda_count()==128 && strlen(cal_agenda_get(127)->title)==95 && cal_agenda_draft()->active);
    unsigned last=cal_agenda_get(127)->id;
    assert(cal_agenda_delete(last)); assert(cal_agenda_commit_draft()>last && cal_agenda_count()==128);
    assert(!cal_agenda_retry_save()); reboot(); assert(!ca_draft.active && cal_agenda_count()==128);
    /* Explicit malformed documents, not fuzz or memory-error injection. */
    for (int which=0;which<13;++which) {
        reset_volume(1); add("valid"); add("second"); assert(!cal_agenda_retry_save()); f=agenda_file();
        static char bad[CAL_AGENDA_FILE_MAX+1]; int n=fs_size(f); memcpy(bad,fs_data(f),n);
        int first=AGENDA_HEADER, second=first+AGENDA_RECORD;
        switch(which) {
        case 0: memcpy(bad,"OTHER",5); break;
        case 1: put16(bad+4,2); break;
        case 2: put16(bad+8,129); break;
        case 3: put32(bad+second,get32(bad+first)); break;
        case 4: bad[first+7]=32; break;
        case 5: put16(bad+first+8,1440); break;
        case 6: bad[first+12]='\n'; break;
        case 7: bad[first+10]=94; break;
        case 8: put32(bad+12,1); break;
        case 9: bad[10]=1; put32(bad+16,999); break;
        case 10: bad[n-1]=1; break;
        case 11: --n; break;
        case 12: bad[28]=1; break;
        }
        put32(bad+24,wire_crc(bad,n)); assert(fs_write(f,bad,n)==n); assert(!fs_sync()); reset_agenda();
        assert(!ca_count && !ca_pending); status(CA_UNSUPPORTED,"bytes kept");
        assert(!cal_agenda_prepare_shutdown() && fs_size(f)==n && !memcmp(fs_data(f),bad,n));
        add("retained"); cal_agenda_tick(); status(CA_CONFLICT,"changed");
        assert(cal_agenda_retry_save()<0 && !memcmp(fs_data(f),bad,n));
    }
    reset_volume(1); add("crc"); assert(!cal_agenda_retry_save()); f=agenda_file();
    char bad[512]; int n=fs_size(f); assert(n<(int)sizeof bad); memcpy(bad,fs_data(f),n); bad[24]^=1;
    assert(fs_write(f,bad,n)==n); reset_agenda(); status(CA_UNSUPPORTED,"bytes kept");
    reset_volume(1); add("last id"); ca_next_id=~0u;
    assert(add("final id")==~0u && !ca_next_id && !cal_agenda_add(2026,10,4,0,"exhausted"));
    assert(!cal_agenda_retry_save()); reboot(); assert(!ca_next_id && !cal_agenda_add(2026,10,4,0,"exhausted"));
    puts("Calendar: full128/title95 legacy roundtrip, malformed version/date/id/title/draft/CRC rejection and ID exhaustion passed");
}
static void capacity_and_returned_errors(void) {
    reset_volume(1); char name[24];
    while (fs_node_count()<fs_node_limit()-1) { snprintf(name,sizeof name,"entry-%d",fs_node_count()); assert(fs_create(0,name)>0); }
    int count=fs_node_count(); add("retained"); cal_agenda_tick(); status(CA_FULL_FAILED,"No room");
    assert(!agenda_mutations && fs_node_count()==count && fs_find_child(0,"prefs")<0);
    assert(!fs_delete(fs_find_child(0,"entry-1"))); assert(!cal_agenda_retry_save());
    reset_volume(1);
    while (fs_node_count()<64) { snprintf(name,sizeof name,"entry-%d",fs_node_count()); assert(fs_create(0,name)>0); }
    static char payload[FS_FILE_MAX]; memset(payload,'d',sizeof payload);
    unsigned remaining=fs_capacity()-64; int index=1;
    while (remaining) {
        snprintf(name,sizeof name,"entry-%d",index++); int f=fs_find_child(0,name); assert(f>0);
        unsigned n=remaining>sizeof payload?sizeof payload:remaining;
        assert(fs_write(f,payload,(int)n)==(int)n); remaining-=n;
    }
    count=fs_node_count(); unsigned used=fs_used_bytes(); add("retained"); cal_agenda_tick(); status(CA_FULL_FAILED,"No room");
    assert(!agenda_mutations && fs_node_count()==count && fs_used_bytes()==used);
    assert(cal_agenda_prepare_shutdown()<0); assert(!fs_delete(fs_find_child(0,"entry-1"))); assert(!cal_agenda_retry_save());
    for (int stage=0;stage<3;++stage) {
        reset_volume(1); reject_mkdir=stage==0; reject_create=stage==1; reject_write=stage==2;
        add("retained"); set_draft(0,"2026-","","draft",2); cal_agenda_tick(); status(CA_WRITE_FAILED,"not saved");
        assert(fs_node_count()==1 && fs_find_child(0,"prefs")<0); unsigned mutations=agenda_mutations;
        for (int i=0;i<1000;++i) assert(!cal_agenda_tick());
        assert(agenda_mutations==mutations && cal_agenda_count()==1 && ca_draft.active);
        reject_mkdir=reject_create=reject_write=0; assert(!cal_agenda_retry_save());
    }
    reset_volume(1); unsigned id=add("old"); assert(!cal_agenda_retry_save());
    int f=agenda_file(); static char old[CAL_AGENDA_FILE_MAX]; int n=fs_size(f); memcpy(old,fs_data(f),n);
    reject_write=1; assert(cal_agenda_update(id,2026,10,4,0,"new")); cal_agenda_tick();
    assert(!memcmp(old,fs_data(f),n) && !strcmp(cal_agenda_find(id)->title,"new"));
    reject_write=0; assert(!cal_agenda_retry_save());
    puts("Calendar: byte/node/metadata preflight, ordinary write errors, atomic rollback and retained drafts passed");
}
static void shutdown_and_disk_errors(void) {
    reset_volume(1); add("old"); cal_agenda_tick(); FsSyncTicket ticket=begin_snapshot();
    add("latest"); set_draft(0,"2026-0","9:","unfinished",1);
    assert(!cal_agenda_prepare_shutdown() && !fs_sync_busy()); assert(!fs_sync_result(ticket)); assert(!fs_sync_release(ticket));
    assert(fs_needs_sync()); status(CA_RAM,"RAM"); assert(!fs_sync()); cal_agenda_tick(); reboot();
    assert(cal_agenda_count()==2 && ca_draft.active && !strcmp(ca_draft.time,"9:"));
    reset_volume(1); add("old"); cal_agenda_tick(); ticket=begin_snapshot(); add("latest");
    set_draft(0,"partial","","new draft",0); fail_write=1;
    assert(!cal_agenda_prepare_shutdown()); assert(fs_sync_result(ticket)<0); assert(!fs_sync_release(ticket));
    status(CA_SYNC_FAILED,"RAM only"); assert(fs_sync()<0); cal_agenda_tick();
    unsigned writes=agenda_writes; int disk_writes=transport_writes;
    for (int i=0;i<1000;++i) assert(!cal_agenda_tick());
    assert(agenda_writes==writes && transport_writes==disk_writes);
    fail_write=0; assert(!cal_agenda_retry_save() && agenda_writes==writes); reboot();
    assert(cal_agenda_count()==2 && ca_draft.active && !strcmp(ca_draft.title,"new draft"));
    reset_volume(1); add("backoff"); cal_agenda_tick(); fail_write=1;
    for (int i=0;i<3;++i) { fs_autosync(); assert(fs_sync_busy()); drain(); cal_agenda_tick(); now+=5*TIMER_HZ; }
    status(CA_SYNC_FAILED,"RAM only"); disk_writes=transport_writes;
    for (int i=0;i<1000;++i) { fs_autosync(); cal_agenda_tick(); }
    assert(transport_writes==disk_writes); fail_write=0; assert(!cal_agenda_retry_save());
    reset_volume(0); ide_present=1; memcpy(ide,"unknown",7); assert(!fs_init() && fs_load_disk()<0); reset_agenda();
    add("recovery"); set_draft(0,"2026-","","retained",2); cal_agenda_tick(); status(CA_SYNC_FAILED,"RAM only");
    disk_writes=transport_writes;
    assert(!cal_agenda_prepare_shutdown() && fs_sync()<0 && cal_agenda_retry_save()<0);
    assert(transport_writes==disk_writes && !memcmp(ide,"unknown",7) && cal_agenda_count()==1 && ca_draft.active);
    reset_volume(1); ca_loaded=0;
    int p=fs_mkdir(0,"prefs"),f=fs_create(p,AGENDA_NAME); assert(fs_write(f,"untouched",9)==9);
    assert(!cal_agenda_tick() && !cal_agenda_prepare_shutdown() && !agenda_mutations && !memcmp(fs_data(f),"untouched",9));
    puts("Calendar: shutdown saves newest agenda+draft, older failed snapshots, retry/backoff and protected volumes passed");
}
int main(void) {
    dates_and_crud(); busy_draft_recovery(); missing_and_colliding_paths(); source_conflicts();
    bounded_format_and_validation(); capacity_and_returned_errors(); shutdown_and_disk_errors(); return 0;
}
