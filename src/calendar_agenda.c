/* BaseOS Calendar agenda: bounded appointments and recoverable editor state.
 * No clock/time-zone conversion: dates and optional times are local wall time. */
#include "fs.h"
#include "calendar_agenda.h"

#define AGENDA_NAME "calendar.v1"
#define AGENDA_HEADER 32
#define AGENDA_RECORD 108
#define AGENDA_DRAFT 113

_Static_assert(CAL_AGENDA_FILE_MAX == 13969, "calendar format size changed");
_Static_assert(CAL_AGENDA_FILE_MAX <= 16383, "calendar must fit legacy floppy files");
_Static_assert(sizeof(unsigned) == 4, "calendar IDs require 32-bit unsigned");

enum { CA_IDLE, CA_QUEUED, CA_RAM, CA_SAVED, CA_PATH_FAILED, CA_FULL_FAILED,
       CA_WRITE_FAILED, CA_CONFLICT, CA_SYNC_FAILED, CA_RELOAD, CA_UNSUPPORTED };
static CalAppointment ca_items[CAL_AGENDA_MAX];
static CalAgendaDraft ca_draft;
static int ca_count, ca_loaded, ca_pending, ca_state;
static unsigned ca_next_id = 1;
/* Private scratch avoids a large kernel-stack allocation and borrowed FS data.
 * The cooperative app model permits no interleaved calls while a save runs. */
static char ca_source[CAL_AGENDA_FILE_MAX], ca_wire[CAL_AGENDA_FILE_MAX + 1];
static int ca_source_file = -1, ca_source_len;
static unsigned ca_source_identity;
static CalAppointment ca_decoded[CAL_AGENDA_MAX];

static void zero(void *dst, unsigned n) {
    unsigned char *p = dst;
    while (n--) *p++ = 0;
}
static int equal(const void *a, const void *b, unsigned n) {
    const unsigned char *x = a, *y = b;
    while (n--) if (*x++ != *y++) return 0;
    return 1;
}
static int text_length(const char *s, int max) {
    if (!s) return -1;
    for (int n = 0; n <= max; ++n) {
        unsigned char c = (unsigned char)s[n];
        if (!c) return n;
        if (c < 32 || c > 126) return -1;
    }
    return -1;
}
static void copy_text(char *dst, const char *src, int max) {
    int n = text_length(src, max);
    zero(dst, (unsigned)max + 1);
    if (n > 0) kmemcpy(dst, src, n);
}
int cal_agenda_days_in_month(int y, int m) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < CAL_AGENDA_YEAR_MIN || y > CAL_AGENDA_YEAR_MAX || m < 1 || m > 12) return 0;
    return days[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}
int cal_agenda_valid_date(int y, int m, int d) {
    return d >= 1 && d <= cal_agenda_days_in_month(y, m);
}
static int digit(char c) { return c >= '0' && c <= '9'; }
int cal_agenda_parse_date(const char *s, int *y, int *m, int *d) {
    if (text_length(s, 10) != 10 || !y || !m || !d || s[4] != '-' || s[7] != '-') return 0;
    for (int i = 0; i < 10; ++i) if (i != 4 && i != 7 && !digit(s[i])) return 0;
    int yy=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+s[3]-'0';
    int mm=(s[5]-'0')*10+s[6]-'0', dd=(s[8]-'0')*10+s[9]-'0';
    if (!cal_agenda_valid_date(yy,mm,dd)) return 0;
    *y=yy; *m=mm; *d=dd; return 1;
}
int cal_agenda_parse_time(const char *s, int *minute) {
    int len=text_length(s,5);
    if (!minute || len < 0) return 0;
    if (!len) { *minute=-1; return 1; }
    if (len != 5 || s[2] != ':' || !digit(s[0]) || !digit(s[1]) || !digit(s[3]) || !digit(s[4])) return 0;
    int hh=(s[0]-'0')*10+s[1]-'0', mm=(s[3]-'0')*10+s[4]-'0';
    if (hh > 23 || mm > 59) return 0;
    *minute=hh*60+mm; return 1;
}
int cal_agenda_step_month(int *y, int *m, int *d, int dir) {
    if (!y || !m || !d || (dir != -1 && dir != 1) || !cal_agenda_valid_date(*y,*m,*d)) return 0;
    int yy=*y, mm=*m+dir;
    if (mm < 1) { mm=12; --yy; }
    if (mm > 12) { mm=1; ++yy; }
    int days=cal_agenda_days_in_month(yy,mm);
    if (!days) return 0;
    *y=yy; *m=mm; if (*d>days) *d=days; return 1;
}
int cal_agenda_step_day(int *y, int *m, int *d, int dir) {
    if (!y || !m || !d || (dir != -1 && dir != 1) || !cal_agenda_valid_date(*y,*m,*d)) return 0;
    int yy=*y, mm=*m, dd=*d+dir;
    if (dd < 1) {
        dd=1;
        if (!cal_agenda_step_month(&yy,&mm,&dd,-1)) return 0;
        dd=cal_agenda_days_in_month(yy,mm);
    } else if (dd > cal_agenda_days_in_month(yy,mm)) {
        dd=1;
        if (!cal_agenda_step_month(&yy,&mm,&dd,1)) return 0;
    }
    *y=yy; *m=mm; *d=dd; return 1;
}
static int valid_item(int y, int m, int d, int minute, const char *title) {
    int n=text_length(title,CAL_AGENDA_TITLE_MAX), visible=0;
    if (!cal_agenda_valid_date(y,m,d) || minute < -1 || minute > 1439 || n <= 0) return 0;
    for (int i=0; i<n; ++i) if (title[i] != ' ') visible=1;
    return visible;
}
static int before(const CalAppointment *a, const CalAppointment *b) {
    if (a->year != b->year) return a->year < b->year;
    if (a->month != b->month) return a->month < b->month;
    if (a->day != b->day) return a->day < b->day;
    if (a->minute != b->minute) return a->minute < b->minute;
    return a->id < b->id;
}
static void sort_items(void) {
    for (int i=1; i<ca_count; ++i) {
        CalAppointment value=ca_items[i]; int j=i;
        while (j && before(&value,&ca_items[j-1])) { ca_items[j]=ca_items[j-1]; --j; }
        ca_items[j]=value;
    }
}
static int index_of(unsigned id) {
    for (int i=0; i<ca_count; ++i) if (ca_items[i].id==id) return i;
    return -1;
}
static unsigned get16(const char *p) { return (unsigned char)p[0] | (unsigned)(unsigned char)p[1]<<8; }
static unsigned get32(const char *p) { return get16(p) | get16(p+2)<<16; }
static void put16(char *p,unsigned v) { p[0]=(char)v; p[1]=(char)(v>>8); }
static void put32(char *p,unsigned v) { put16(p,v); put16(p+2,v>>16); }
/* CRC includes header and all payload with its own four-byte field zeroed. */
static unsigned wire_crc(const char *p, int n) {
    unsigned crc=~0u;
    for (int i=0; i<n; ++i) {
        unsigned c=i>=24 && i<28 ? 0 : (unsigned char)p[i];
        crc ^= c;
        for (int b=0; b<8; ++b) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
static int padded_text(const char *p,int max) {
    int n=text_length(p,max);
    if (n < 0) return 0;
    for (int i=n; i<=max; ++i) if (p[i]) return 0;
    return 1;
}
static int encode(void) {
    int size=AGENDA_HEADER+ca_count*AGENDA_RECORD+AGENDA_DRAFT;
    zero(ca_wire,(unsigned)size);
    kmemcpy(ca_wire,"BCA1",4); put16(ca_wire+4,1); put16(ca_wire+6,AGENDA_HEADER);
    put16(ca_wire+8,(unsigned)ca_count); ca_wire[10]=(char)ca_draft.active; ca_wire[11]=(char)ca_draft.field;
    put32(ca_wire+12,ca_next_id); put32(ca_wire+16,ca_draft.edit_id); put32(ca_wire+20,(unsigned)size);
    for (int i=0; i<ca_count; ++i) {
        CalAppointment *a=&ca_items[i]; char *p=ca_wire+AGENDA_HEADER+i*AGENDA_RECORD;
        put32(p,a->id); put16(p+4,(unsigned)a->year); p[6]=(char)a->month; p[7]=(char)a->day;
        put16(p+8,a->minute<0?65535u:(unsigned)a->minute);
        p[10]=(char)text_length(a->title,CAL_AGENDA_TITLE_MAX);
        kmemcpy(p+12,a->title,CAL_AGENDA_TITLE_MAX+1);
    }
    char *p=ca_wire+AGENDA_HEADER+ca_count*AGENDA_RECORD;
    kmemcpy(p,ca_draft.date,11); kmemcpy(p+11,ca_draft.time,6); kmemcpy(p+17,ca_draft.title,96);
    put32(ca_wire+24,wire_crc(ca_wire,size)); return size;
}
/* Parse entirely into scratch; rejection never installs a partial agenda. */
static int decode(const char *data,int n) {
    if (n < AGENDA_HEADER+AGENDA_DRAFT || n > CAL_AGENDA_FILE_MAX || !equal(data,"BCA1",4) ||
        get16(data+4)!=1 || get16(data+6)!=AGENDA_HEADER || get32(data+20)!=(unsigned)n ||
        get32(data+24)!=wire_crc(data,n) || get32(data+28)) return 0;
    unsigned count=get16(data+8), next=get32(data+12), editing=get32(data+16);
    unsigned active=(unsigned char)data[10],field=(unsigned char)data[11];
    if (count>CAL_AGENDA_MAX || n!=AGENDA_HEADER+(int)count*AGENDA_RECORD+AGENDA_DRAFT || active>1 || field>2) return 0;
    for (unsigned i=0; i<count; ++i) {
        const char *p=data+AGENDA_HEADER+i*AGENDA_RECORD;
        CalAppointment *a=&ca_decoded[i]; zero(a,sizeof *a);
        a->id=get32(p); a->year=(int)get16(p+4); a->month=(unsigned char)p[6]; a->day=(unsigned char)p[7];
        unsigned minute=get16(p+8); a->minute=minute==65535?-1:(int)minute;
        if (!a->id || (next && a->id>=next) || p[11] || !padded_text(p+12,95) ||
            (unsigned char)p[10]!=(unsigned)text_length(p+12,95)) return 0;
        kmemcpy(a->title,p+12,96);
        if (!valid_item(a->year,a->month,a->day,a->minute,a->title)) return 0;
        if (i && !before(&ca_decoded[i-1],a)) return 0;
        for (unsigned j=0; j<i; ++j) if (a->id==ca_decoded[j].id) return 0;
    }
    const char *p=data+AGENDA_HEADER+count*AGENDA_RECORD;
    if (!padded_text(p,10) || !padded_text(p+11,5) || !padded_text(p+17,95)) return 0;
    if (!active) {
        if (editing || field) return 0;
        for (int i=0; i<AGENDA_DRAFT; ++i) if (p[i]) return 0;
    } else if (editing) {
        int found=0;
        for (unsigned i=0; i<count; ++i) if (ca_decoded[i].id==editing) found=1;
        if (!found) return 0;
    }
    ca_count=(int)count; ca_next_id=next;
    kmemcpy(ca_items,ca_decoded,(int)(count*sizeof ca_items[0]));
    zero(&ca_draft,sizeof ca_draft); ca_draft.active=(int)active; ca_draft.field=(int)field; ca_draft.edit_id=editing;
    kmemcpy(ca_draft.date,p,11); kmemcpy(ca_draft.time,p+11,6); kmemcpy(ca_draft.title,p+17,96);
    return 1;
}
static int agenda_file(void) {
    int p=fs_find_child(fs_root(),"prefs");
    return fs_is_dir(p) && !fs_is_app(p) ? fs_find_child(p,AGENDA_NAME) : -1;
}
static int source_matches(int f) {
    return f>=0 && f==ca_source_file && fs_identity(f)==ca_source_identity &&
        !fs_is_dir(f) && !fs_is_app(f) && fs_size(f)==ca_source_len && equal(fs_data(f),ca_source,(unsigned)ca_source_len);
}
static void capture_source(int f,const char *data,int n) {
    ca_source_file=f; ca_source_identity=fs_identity(f); ca_source_len=n;
    kmemcpy(ca_source,data,n);
}
static void observe(void) {
    if (ca_pending || (ca_state!=CA_RAM && ca_state!=CA_SAVED && ca_state!=CA_SYNC_FAILED)) return;
    if (!source_matches(agenda_file())) {
        /* A recovered/open draft is private work even after its old copy was
         * durable: do not let external replacement discard it on shutdown. */
        ca_pending=ca_state!=CA_SAVED || ca_draft.active;
        ca_state=ca_pending?CA_CONFLICT:CA_RELOAD;
    } else if (ca_state!=CA_SAVED) {
        if (fs_storage_status()) ca_state=CA_SYNC_FAILED;
        else ca_state=fs_needs_sync()?CA_RAM:CA_SAVED;
    }
}
static void save(void) {
    if (fs_sync_busy()) { ca_state=CA_QUEUED; return; }
    int p=fs_find_child(fs_root(),"prefs");
    if (p>=0 && (!fs_is_dir(p) || fs_is_app(p))) { ca_state=CA_PATH_FAILED; return; }
    int f=p<0?-1:fs_find_child(p,AGENDA_NAME);
    if (f>=0 && (fs_is_dir(f) || fs_is_app(f))) { ca_state=CA_PATH_FAILED; return; }
    if (f>=0 && !source_matches(f)) { ca_state=CA_CONFLICT; return; }
    int n=encode();
    unsigned count=(unsigned)fs_node_count()+(p<0)+(f<0), capacity=fs_capacity_for_nodes(count);
    unsigned used=fs_used_bytes()-(unsigned)(f<0?0:fs_size(f));
    if (count>(unsigned)fs_node_limit() || used>capacity || (unsigned)n>capacity-used || (unsigned)n>fs_file_limit()) {
        ca_state=CA_FULL_FAILED; return;
    }
    int new_dir=p<0,new_file=f<0,result;
    if (new_dir) p=fs_mkdir(fs_root(),"prefs");
    if (p<0) { ca_state=p==FS_ERR_BUSY?CA_QUEUED:CA_WRITE_FAILED; return; }
    if (new_file) f=fs_create(p,AGENDA_NAME);
    result=f<0?f:fs_write(f,ca_wire,n);
    if (result!=n) {
        if (new_file && f>=0) (void)fs_delete(f);
        if (new_dir) (void)fs_delete(p);
        ca_state=result==FS_ERR_BUSY?CA_QUEUED:CA_WRITE_FAILED; return;
    }
    capture_source(f,ca_wire,n); ca_pending=0; ca_state=CA_RAM; observe();
}
void cal_agenda_load(void) {
    if (ca_loaded && (ca_pending || ca_draft.active || ca_state==CA_RAM || ca_state==CA_SYNC_FAILED)) return;
    ca_loaded=1; ca_pending=0; ca_state=CA_IDLE; ca_count=0; ca_next_id=1;
    ca_source_file=-1; ca_source_identity=0; ca_source_len=0; zero(&ca_draft,sizeof ca_draft);
    int p=fs_find_child(fs_root(),"prefs");
    if (p<0) return;
    if (!fs_is_dir(p) || fs_is_app(p)) { ca_state=CA_PATH_FAILED; return; }
    int f=fs_find_child(p,AGENDA_NAME);
    if (f<0) return;
    if (fs_is_dir(f) || fs_is_app(f)) { ca_state=CA_PATH_FAILED; return; }
    int n=fs_read(f,ca_wire,(int)sizeof ca_wire);
    if (n<0) { ca_state=CA_WRITE_FAILED; return; }
    if (fs_size(f)!=n || !decode(ca_wire,n)) { ca_state=CA_UNSUPPORTED; return; }
    capture_source(f,ca_wire,n); ca_state=CA_RAM;
    if (!fs_sync_busy()) observe();
}
static void ensure_loaded(void) { if (!ca_loaded) cal_agenda_load(); }
static void changed(void) { ca_pending=1; ca_state=CA_QUEUED; }
int cal_agenda_tick(void) {
    if (!ca_loaded) return 0;
    int old=ca_state;
    if (!fs_sync_busy()) { observe(); if (ca_pending && ca_state==CA_QUEUED) save(); }
    return old!=ca_state;
}
const char *cal_agenda_status(void) {
    switch(ca_state) {
    case CA_QUEUED: return "Agenda/draft queued for storage.";
    case CA_RAM: return "Agenda/draft in RAM; waiting for disk.";
    case CA_SAVED: return "Agenda and draft saved.";
    case CA_PATH_FAILED: return "Calendar path occupied; fix, then Ctrl+S.";
    case CA_FULL_FAILED: return "No room; free space, then Ctrl+S.";
    case CA_WRITE_FAILED: return "Agenda not saved; Ctrl+S retries.";
    case CA_CONFLICT: return ca_draft.active ? "File changed; draft kept. Fix path, Ctrl+S." : "Calendar file changed; fix path, Ctrl+S.";
    case CA_SYNC_FAILED: return "RAM only; disk failed. Ctrl+S retries.";
    case CA_RELOAD: return ca_draft.active ? "File changed; draft kept. Fix path, Ctrl+S." : "Calendar file changed; reopen to reload.";
    case CA_UNSUPPORTED: return "Unsupported Calendar file; bytes kept.";
    default: return "Local appointments; time is optional.";
    }
}
int cal_agenda_retry_save(void) {
    if (!ca_loaded) return 0;
    if (fs_sync_busy()) { if (ca_pending) ca_state=CA_QUEUED; return FS_ERR_BUSY; }
    observe();
    if (ca_pending) save();
    if (ca_pending) return -1;
    int result=fs_sync(); cal_agenda_tick(); return result;
}
int cal_agenda_prepare_shutdown(void) {
    if (!ca_loaded) return 0;
    if (fs_sync_busy()) (void)fs_sync();
    if (fs_sync_busy()) return FS_ERR_BUSY;
    observe(); if (ca_pending) save(); return ca_pending?-1:0;
}
int cal_agenda_count(void) { ensure_loaded(); return ca_count; }
const CalAppointment *cal_agenda_get(int i) { ensure_loaded(); return i>=0 && i<ca_count?&ca_items[i]:0; }
const CalAppointment *cal_agenda_find(unsigned id) { ensure_loaded(); int i=index_of(id); return i>=0?&ca_items[i]:0; }
static void fill_item(CalAppointment *a,unsigned id,int y,int m,int d,int minute,const char *title) {
    a->id=id; a->year=y; a->month=m; a->day=d; a->minute=minute; copy_text(a->title,title,95);
}
unsigned cal_agenda_add(int y,int m,int d,int minute,const char *title) {
    ensure_loaded();
    if (ca_count>=CAL_AGENDA_MAX || !ca_next_id || !valid_item(y,m,d,minute,title)) return 0;
    unsigned id=ca_next_id++; fill_item(&ca_items[ca_count++],id,y,m,d,minute,title);
    sort_items(); changed(); return id;
}
int cal_agenda_update(unsigned id,int y,int m,int d,int minute,const char *title) {
    ensure_loaded(); int i=index_of(id);
    if (i<0 || !valid_item(y,m,d,minute,title)) return 0;
    /* A caller may pass the current item's title; copy it before sorting. */
    CalAppointment a; zero(&a,sizeof a); fill_item(&a,id,y,m,d,minute,title);
    if (equal(&a,&ca_items[i],sizeof a)) return 1;
    ca_items[i]=a; sort_items(); changed(); return 1;
}
int cal_agenda_delete(unsigned id) {
    ensure_loaded(); int i=index_of(id);
    if (i<0 || (ca_draft.active && ca_draft.edit_id==id)) return 0;
    for (;i<ca_count-1;++i) ca_items[i]=ca_items[i+1];
    --ca_count; changed(); return 1;
}
const CalAgendaDraft *cal_agenda_draft(void) { ensure_loaded(); return &ca_draft; }
int cal_agenda_set_draft(const CalAgendaDraft *draft) {
    ensure_loaded();
    if (!draft || (draft->active!=0 && draft->active!=1) || draft->field<0 || draft->field>2 ||
        text_length(draft->date,10)<0 || text_length(draft->time,5)<0 || text_length(draft->title,95)<0 ||
        (draft->active && draft->edit_id && index_of(draft->edit_id)<0)) return 0;
    CalAgendaDraft value; zero(&value,sizeof value);
    if (draft->active) {
        value.active=1; value.field=draft->field; value.edit_id=draft->edit_id;
        copy_text(value.date,draft->date,10); copy_text(value.time,draft->time,5); copy_text(value.title,draft->title,95);
    }
    if (!equal(&value,&ca_draft,sizeof value)) { ca_draft=value; changed(); }
    return 1;
}
int cal_agenda_clear_draft(void) {
    ensure_loaded();
    if (ca_draft.active) { zero(&ca_draft,sizeof ca_draft); changed(); }
    return 1;
}
unsigned cal_agenda_commit_draft(void) {
    ensure_loaded(); int y,m,d,minute;
    if (!ca_draft.active || !cal_agenda_parse_date(ca_draft.date,&y,&m,&d) ||
        !cal_agenda_parse_time(ca_draft.time,&minute) || !valid_item(y,m,d,minute,ca_draft.title)) return 0;
    unsigned id=ca_draft.edit_id;
    if (id) { if (!cal_agenda_update(id,y,m,d,minute,ca_draft.title)) return 0; }
    else { id=cal_agenda_add(y,m,d,minute,ca_draft.title); if (!id) return 0; }
    zero(&ca_draft,sizeof ca_draft); changed(); return id;
}
