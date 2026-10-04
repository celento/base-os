/* BaseOS Browser: bounded HTML text presentation over the cooperative HTTP API.
 * This is deliberately not a web engine: no TLS, scripts, CSS, forms or images.
 * All document storage lives in one fixed arena, never in the kernel stack. */
#include "app.h"
#include "browser.h"
#include "fs.h"
#include "layout.h"
#include "net.h"

#ifndef BROWSER_BASE
#define BROWSER_BASE 0x1610000
#endif
#ifndef BROWSER_CAPACITY
#define BROWSER_CAPACITY 0x40000
#endif

#define TEXT_MAX 32768
#define LINK_MAX 96
#define LINE_MAX 2048
#define HISTORY_MAX 16
#define ANCHOR_MAX 64
#define TITLE_MAX 96
#define STATUS_MAX 160
#define ROW_H 22
#define TOOL_H 78
#define FOOT_H 28
#define PAD 14
#define STYLE_BOLD 1
#define STYLE_PRE 2
#define STYLE_HEADING 4

typedef struct { unsigned short start, length; } BrowserLine;
typedef struct { char name[64]; unsigned short position; } BrowserAnchor;
typedef struct {
    char body[NET_HTTP_BODY_MAX];
    char text[TEXT_MAX];
    unsigned char style[TEXT_MAX], link[TEXT_MAX];
    char links[LINK_MAX][NET_URL_MAX];
    BrowserLine lines[LINE_MAX];
    BrowserAnchor anchors[ANCHOR_MAX];
    char history[HISTORY_MAX][NET_URL_MAX];
    int history_scroll[HISTORY_MAX];
    char url[NET_URL_MAX], address[NET_URL_MAX], title[TITLE_MAX], status[STATUS_MAX];
    unsigned text_len, body_len, request_id, received;
    int link_count, anchor_count, line_count, history_count, history_pos;
    int scroll, rows, width, focus, cursor, selected, address_start, focused_link;
    int loading, request_state, redirects, truncated, html, body_complete;
} BrowserState;

#ifdef BROWSER_HOST_TEST
static BrowserState host_browser;
#define B host_browser
#else
#define B (*(BrowserState *)BROWSER_BASE)
_Static_assert(sizeof(BrowserState) <= BROWSER_CAPACITY, "browser arena too small");
#endif

static int browser_ready;

static const char home_html[] =
    "<title>Welcome to Browser</title><h1>Browser</h1>"
    "<p>A small HTTP browser, built into BaseOS.</p>"
    "<h2>Open a page</h2><p>Type an http:// address above and press Enter. "
    "Use Back, Forward, Reload and Stop to navigate.</p>"
    "<ul><li><a href='http://example.com/'>Example Domain</a></li>"
    "<li><a href='http://neverssl.com/'>NeverSSL</a></li>"
    "<li><a href='http://10.0.2.2:8000/'>A web server on your QEMU host</a></li></ul>"
    "<h2>Made for simple pages</h2><p>Headings, paragraphs, lists and clickable links. "
    "Use the wheel, arrows or Page Up / Page Down to scroll. Tab selects links.</p>"
    "<p>Ctrl+L focuses the address. Ctrl+R or F5 reloads. Escape stops loading.</p>"
    "<h2>Know the limits</h2><p>HTTP is unencrypted. HTTPS / TLS is not supported. "
    "Do not enter passwords or private information. CSS, JavaScript, forms, "
    "images are not supported. Pages are limited to 32 KB.</p>"
    "<p>Save or Ctrl+S keeps the complete original page in /Downloads. "
    "Terminal download can save HTTP files up to 2 MiB in the background.</p>"
    "<p>Local HTML and text files can be opened with file:///path.</p>";

static void zero(void *p, unsigned n) { unsigned char *d=p; while(n--) *d++=0; }
static int len(const char *s) { int n=0; while(s[n]) n++; return n; }
static int lower(int c) { return c>='A'&&c<='Z'?c+32:c; }
static int space(int c) { return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f'; }
static int equal(const char *a,const char *b) { while(*a&&*a==*b){a++;b++;}return *a==*b; }
static int starts(const char *s,const char *prefix) { while(*prefix)if(lower(*s++)!=lower(*prefix++))return 0;return 1; }
static void copy(char *d,unsigned cap,const char *s) { unsigned n=0;if(!cap)return;while(s[n]&&n+1<cap){d[n]=s[n];n++;}d[n]=0; }
static void append(char *d,unsigned cap,const char *s) { unsigned n=(unsigned)len(d);if(n<cap)copy(d+n,cap-n,s); }
static void number(char *d,unsigned v) { char b[12];int n=0;do{b[n++]=(char)('0'+v%10);v/=10;}while(v);int j=0;while(n)d[j++]=b[--n];d[j]=0; }
static void set_status(const char *s) { copy(B.status,sizeof B.status,s); }
static int scheme(const char *s) {
    if(!((lower(*s)>='a'&&lower(*s)<='z')))return 0;
    for(int i=0;s[i];i++){
        if(s[i]==':'){
            /* Treat host:port as a convenient address, not an unknown scheme. */
            int j=i+1;while(s[j]>='0'&&s[j]<='9')j++;
            if(j>i+1&&(!s[j]||s[j]=='/'||s[j]=='?'||s[j]=='#'))return 0;
            return 1;
        }
        if(s[i]=='/'||s[i]=='?'||s[i]=='#')break;
    }
    return 0;
}

/* Collapse dot segments in an HTTP or local-file path, without changing the
 * origin, query or fragment. This also keeps long relative links bounded. */
static int normalize(char out[NET_URL_MAX],const char *input) {
    char temp[NET_URL_MAX];unsigned n=0;
    while(space(*input))input++;
    while(input[n]&&n+1<sizeof temp){temp[n]=input[n];n++;}
    if(input[n])return 0;
    while(n&&space(temp[n-1]))n--;
    temp[n]=0;
    if(!n)return 0;
    if(!scheme(temp)) {
        if(n+7>=sizeof temp)return 0;
        for(unsigned i=n+1;i;i--)temp[i+6]=temp[i-1];
        for(int i=0;i<7;i++)temp[i]="http://"[i];
        n+=7;
    }
    int begin=-1;
    if(starts(temp,"http://"))begin=7;
    else if(starts(temp,"https://"))begin=8;
    else if(starts(temp,"file://"))begin=7;
    if(begin<0){copy(out,NET_URL_MAX,temp);return 1;}
    for(int i=0;i<begin;i++)temp[i]=(char)lower(temp[i]);
    int p=begin;
    if(!starts(temp,"file://"))while(temp[p]&&temp[p]!='/'&&temp[p]!='?'&&temp[p]!='#')p++;
    else if(temp[p]!='/'){copy(out,NET_URL_MAX,temp);return 1;}
    int end=p;while(temp[end]&&temp[end]!='?'&&temp[end]!='#')end++;
    int pos=0;for(int i=0;i<p;i++)out[pos++]=temp[i];
    if(p==end){if(n+1>=NET_URL_MAX)return 0;out[pos++]='/';}
    else {
        int i=p;out[pos++]='/';
        while(i<end){
            while(i<end&&temp[i]=='/')i++;
            int start=i;while(i<end&&temp[i]!='/')i++;
            int count=i-start;if(!count)break;
            if(count==1&&temp[start]=='.')continue;
            if(count==2&&temp[start]=='.'&&temp[start+1]=='.') {
                if(pos>p+1){if(out[pos-1]=='/')pos--;while(pos>p+1&&out[pos-1]!='/')pos--;}
                continue;
            }
            if(pos>p+1&&out[pos-1]!='/')out[pos++]='/';
            for(int j=start;j<i;j++)out[pos++]=temp[j];
            if(i<end&&pos+1<NET_URL_MAX)out[pos++]='/';
        }
    }
    for(int i=end;temp[i];i++){if(pos+1>=NET_URL_MAX)return 0;out[pos++]=temp[i];}
    out[pos]=0;return 1;
}

static int resolve_url(char out[NET_URL_MAX],const char *base,const char *href) {
    char joined[NET_URL_MAX];unsigned p=0;
    while(space(*href))href++;
    if(!*href){copy(out,NET_URL_MAX,base);return 1;}
    if(scheme(href))return normalize(out,href);
    if(href[0]=='/'&&href[1]=='/') {
        copy(joined,sizeof joined,starts(base,"https:")?"https:":"http:");
        if(len(joined)+len(href)>=NET_URL_MAX)return 0;
        append(joined,sizeof joined,href);return normalize(out,joined);
    }
    int authority=starts(base,"http://")?7:starts(base,"https://")?8:starts(base,"file://")?7:0;
    if(!authority)return 0;
    if(href[0]=='/') {
        p=(unsigned)authority;
        if(!starts(base,"file://"))while(base[p]&&base[p]!='/'&&base[p]!='?'&&base[p]!='#')p++;
    } else {
        while(base[p]&&base[p]!='#'&&(href[0]=='#'||base[p]!='?'))p++;
        if(href[0]!='?'&&href[0]!='#')while(p>(unsigned)authority&&base[p-1]!='/')p--;
    }
    if(p+(unsigned)len(href)>=sizeof joined)return 0;
    for(unsigned i=0;i<p;i++)joined[i]=base[i];
    copy(joined+p,sizeof joined-p,href);return normalize(out,joined);
}

static char codepoint(unsigned c) {
    if(c>=32&&c<127)return (char)c;
    if(c==160)return ' ';
    if(c==0x2018||c==0x2019)return '\'';
    if(c==0x201c||c==0x201d)return '"';
    if(c==0x2013||c==0x2014)return '-';
    if(c==0x2022)return '*';
    if(c==0x2026)return '.';
    return '?';
}
/* Return one visible character and consume a known entity/UTF-8 character. */
static char decode(const char *s,unsigned n,unsigned *used) {
    *used=1;
    unsigned c=(unsigned char)s[0];
    if(c=='&') {
        unsigned end=1;while(end<n&&end<12&&s[end]!=';'&&!space(s[end])&&s[end]!='<')end++;
        if(end<n&&s[end]==';') {
            char entity[13];for(unsigned i=1;i<end;i++)entity[i-1]=s[i];entity[end-1]=0;
            char v=0;
            if(equal(entity,"amp"))v='&';else if(equal(entity,"lt"))v='<';else if(equal(entity,"gt"))v='>';
            else if(equal(entity,"quot"))v='"';else if(equal(entity,"apos"))v='\'';else if(equal(entity,"nbsp"))v=' ';
            else if(equal(entity,"ndash")||equal(entity,"mdash"))v='-';
            else if(equal(entity,"lsquo")||equal(entity,"rsquo"))v='\'';
            else if(equal(entity,"ldquo")||equal(entity,"rdquo"))v='"';
            else if(equal(entity,"bull"))v='*';else if(equal(entity,"hellip"))v='.';
            else if(entity[0]=='#') {
                unsigned value=0,i=1,base=10;int valid=1;
                if(entity[i]=='x'||entity[i]=='X'){base=16;i++;}
                if(!entity[i])valid=0;
                for(;entity[i];i++){int d=entity[i]>='0'&&entity[i]<='9'?entity[i]-'0':lower(entity[i])>='a'&&lower(entity[i])<='f'?lower(entity[i])-'a'+10:-1;
                    if(d<0||(unsigned)d>=base||value>0x10ffff/base){valid=0;break;}value=value*base+(unsigned)d;}
                if(valid)v=codepoint(value);
            }
            if(v){*used=end+1;return v;}
        }
    }
    if(c>=128) {
        unsigned count=c>=0xf0&&c<=0xf4?4:c>=0xe0&&c<=0xef?3:c>=0xc2&&c<=0xdf?2:1;
        unsigned value=c&((1u<<(7-count))-1u);
        if(count<=n&&count>1){unsigned i;for(i=1;i<count;i++){unsigned next=(unsigned char)s[i];if((next&0xc0)!=0x80)break;value=(value<<6)|(next&0x3f);}
            if(i==count){*used=count;return codepoint(value);}}
        return '?';
    }
    return (char)c;
}

static void emit(char c,int style,int link) {
    if(B.text_len+1>=TEXT_MAX){B.truncated=1;return;}
    unsigned p=B.text_len++;B.text[p]=c;B.style[p]=(unsigned char)style;B.link[p]=(unsigned char)link;B.text[B.text_len]=0;
}
static void newline(int blank) {
    while(B.text_len&&B.text[B.text_len-1]==' ')B.text_len--;
    if(B.text_len&&B.text[B.text_len-1]!='\n')emit('\n',0,0);
    if(blank&&B.text_len&&(B.text_len<2||B.text[B.text_len-2]!='\n'))emit('\n',0,0);
    B.text[B.text_len]=0;
}
static void emit_string(const char *s,int style,int link){while(*s)emit(*s++,style,link);}
static int tag_equal(const char *s,unsigned n,const char *name){unsigned i=0;while(name[i]){if(i>=n||lower(s[i])!=name[i])return 0;i++;}return i==n;}
static void attribute(const char *s,unsigned n,const char *name,char *out,unsigned cap) {
    unsigned i=0;out[0]=0;
    while(i<n&&!space(s[i]))i++;
    while(i<n){
        while(i<n&&(space(s[i])||s[i]=='/'))i++;
        unsigned begin=i;while(i<n&&!space(s[i])&&s[i]!='='&&s[i]!='/'&&s[i]!='>')i++;
        unsigned size=i-begin;while(i<n&&space(s[i]))i++;
        if(i>=n)break;
        if(s[i]!='='){if(i==begin)i++;continue;}i++;while(i<n&&space(s[i]))i++;
        char quote=0;if(i<n&&(s[i]=='\''||s[i]=='"'))quote=s[i++];
        unsigned start=i;while(i<n&&(quote?s[i]!=quote:!space(s[i])&&s[i]!='>'))i++;
        if(tag_equal(s+begin,size,name)) {
            unsigned p=0,j=start;
            while(j<i&&p+1<cap){unsigned used;char c=decode(s+j,i-j,&used);out[p++]=c;j+=used;}
            out[p]=0;
            /* Reject overlong values rather than following a different URL. */
            if(j<i)out[0]=0;
            return;
        }
        if(quote&&i<n)i++;
    }
}
static void document_reset(void) {
    B.text_len=0;B.text[0]=0;B.link_count=0;B.anchor_count=0;B.line_count=0;
    B.truncated=0;B.title[0]=0;B.width=0;B.scroll=0;B.focused_link=0;
}
static void parse_document(const char *s,unsigned n,int html) {
    document_reset();int bold=0,heading=0,pre=0,active_link=0,in_head=0,in_title=0,title_pos=0,list=0,pending_space=0;
    char hidden[12]={0};
    for(unsigned i=0;i<n;) {
        if(html&&s[i]=='<') {
            if(i+4<=n&&s[i+1]=='!'&&s[i+2]=='-'&&s[i+3]=='-') {
                i+=4;while(i+2<n&&!(s[i]=='-'&&s[i+1]=='-'&&s[i+2]=='>'))i++;i=i+2<n?i+3:n;continue;
            }
            unsigned end=i+1;char quote=0;
            while(end<n){char c=s[end];if(quote){if(c==quote)quote=0;}else if(c=='\''||c=='"')quote=c;else if(c=='>')break;end++;}
            if(end==n){i++;continue;}
            unsigned p=i+1;while(p<end&&space(s[p]))p++;int close=p<end&&s[p]=='/';if(close)p++;
            unsigned start=p;while(p<end&&((s[p]>='a'&&s[p]<='z')||(s[p]>='A'&&s[p]<='Z')||(s[p]>='0'&&s[p]<='9')))p++;
            unsigned size=p-start;char tag[12];unsigned tn=size<sizeof tag-1?size:sizeof tag-1;
            for(unsigned j=0;j<tn;j++)tag[j]=(char)lower(s[start+j]);
            tag[tn]=0;
            if(hidden[0]){if(close&&equal(tag,hidden))hidden[0]=0;i=end+1;continue;}
            if(!close&&(equal(tag,"script")||equal(tag,"style"))){copy(hidden,sizeof hidden,tag);i=end+1;continue;}
            if(equal(tag,"head")){in_head=!close;i=end+1;continue;}
            if(equal(tag,"title")){in_title=!close;i=end+1;continue;}
            if(in_head){i=end+1;continue;}
            if(!close) {
                char name[64];attribute(s+start,end-start,"id",name,sizeof name);
                if(!name[0]&&equal(tag,"a"))attribute(s+start,end-start,"name",name,sizeof name);
                if(name[0]&&B.anchor_count<ANCHOR_MAX){BrowserAnchor *a=&B.anchors[B.anchor_count++];copy(a->name,sizeof a->name,name);a->position=(unsigned short)B.text_len;}
            }
            if(equal(tag,"a")) {
                active_link=0;
                if(!close&&B.link_count<LINK_MAX) {
                    char href[NET_URL_MAX];attribute(s+start,end-start,"href",href,sizeof href);
                    if(href[0]&&resolve_url(B.links[B.link_count],B.url,href))active_link=++B.link_count;
                }
            } else if(size==2&&tag[0]=='h'&&tag[1]>='1'&&tag[1]<='6') {
                newline(1);heading=!close;pending_space=0;
            } else if(equal(tag,"b")||equal(tag,"strong")){if(close){if(bold)bold--;}else if(bold<16)bold++;}
            else if(equal(tag,"pre")){newline(1);pre=!close;pending_space=0;}
            else if(equal(tag,"br")){emit('\n',0,0);pending_space=0;}
            else if(equal(tag,"hr")){newline(1);emit_string("--------------------",0,0);newline(1);pending_space=0;}
            else if(equal(tag,"p")||equal(tag,"div")||equal(tag,"section")||equal(tag,"article")||equal(tag,"header")||equal(tag,"footer")||equal(tag,"blockquote")){newline(equal(tag,"p")||equal(tag,"blockquote"));pending_space=0;}
            else if(equal(tag,"ul")||equal(tag,"ol")){newline(0);if(close){if(list)list--;}else if(list<8)list++;pending_space=0;}
            else if(equal(tag,"li")){newline(0);if(!close){for(int k=1;k<list;k++)emit_string("  ",0,0);emit_string("* ",0,0);}pending_space=0;}
            else if(equal(tag,"tr")){newline(0);pending_space=0;}
            else if(equal(tag,"td")||equal(tag,"th")){if(close)emit_string("  ",0,0);}
            else if(!close&&equal(tag,"img")){char alt[160];attribute(s+start,end-start,"alt",alt,sizeof alt);if(alt[0]){emit_string(" [",0,active_link);emit_string(alt,0,active_link);emit_string("] ",0,active_link);}}
            else if(!close&&equal(tag,"form")){newline(1);emit_string("[Form controls are not supported]",0,0);newline(1);pending_space=0;}
            i=end+1;continue;
        }
        unsigned used=1;char c=!html&&s[i]=='&'?'&':decode(s+i,n-i,&used);i+=used;
        if(hidden[0])continue;
        if(in_title) {
            if(space(c))c=' ';
            if(c>=' '&&title_pos<TITLE_MAX-1&&!(c==' '&&(!title_pos||B.title[title_pos-1]==' ')))B.title[title_pos++]=c;
            B.title[title_pos]=0;continue;
        }
        if(in_head)continue;
        int style=(bold||heading?STYLE_BOLD:0)|(heading?STYLE_HEADING:0)|(pre||!html?STYLE_PRE:0);
        if((pre||!html)&&c=='\n'){emit('\n',style,0);pending_space=0;continue;}
        if((pre||!html)&&c=='\t'){emit_string("    ",style,active_link);continue;}
        if(space(c)) {
            if(pre||!html){if(c!='\r')emit(' ',style,active_link);}else pending_space=1;
            continue;
        }
        if((unsigned char)c<32)continue;
        if(pending_space&&B.text_len&&B.text[B.text_len-1]!='\n')emit(' ',style,active_link);
        pending_space=0;emit(c,style,active_link);
    }
    while(B.text_len&&(B.text[B.text_len-1]==' '||B.text[B.text_len-1]=='\n'))B.text_len--;
    B.text[B.text_len]=0;
    if(!B.title[0])copy(B.title,sizeof B.title,B.url);
    if(!B.text_len)emit_string("This page has no readable text.",0,0);
}

static int advance(unsigned p) {
    if(B.style[p]&STYLE_PRE)return EDIT_CHAR_W;
    if(B.style[p]&STYLE_BOLD){char c[2]={B.text[p],0};return uib_string_w(c);}
    return ui_advance(B.text[p]);
}
static void clamp_scroll(void) {int max=B.line_count-B.rows;if(max<0)max=0;if(B.scroll>max)B.scroll=max;if(B.scroll<0)B.scroll=0;}
static void reflow(int width) {
    if(width<40)width=40;
    if(width==B.width)return;
    B.width=width;B.line_count=0;
    unsigned p=0;
    while(p<B.text_len&&B.line_count<LINE_MAX) {
        unsigned start=p,last_space=p;int pixels=0,found_space=0;
        while(p<B.text_len&&B.text[p]!='\n') {
            int w=advance(p);if(pixels+w>width&&p>start)break;
            if(B.text[p]==' '&&!(B.style[p]&STYLE_PRE)){last_space=p;found_space=1;}
            pixels+=w;p++;
        }
        unsigned end=p,next=p;
        if(p<B.text_len&&B.text[p]=='\n')next=p+1;
        else if(p<B.text_len&&found_space&&last_space>start){end=last_space;next=last_space+1;while(next<B.text_len&&B.text[next]==' ')next++;}
        if(end==start&&next==start)next=++end;
        BrowserLine *line=&B.lines[B.line_count++];line->start=(unsigned short)start;line->length=(unsigned short)(end-start);p=next;
    }
    if(p<B.text_len)B.truncated=1;
    clamp_scroll();
}
static void geometry(int w,int h) { B.rows=(h-TOOL_H-FOOT_H-16)/ROW_H;if(B.rows<1)B.rows=1;reflow(w-2*PAD-14);clamp_scroll(); }
static void save_scroll(void){if(B.history_pos>=0&&B.history_pos<B.history_count)B.history_scroll[B.history_pos]=B.scroll;}
static void history_add(const char *url) {
    if(B.history_pos>=0&&equal(B.history[B.history_pos],url))return;
    save_scroll();B.history_count=B.history_pos+1;
    if(B.history_count==HISTORY_MAX){for(int i=1;i<HISTORY_MAX;i++){copy(B.history[i-1],NET_URL_MAX,B.history[i]);B.history_scroll[i-1]=B.history_scroll[i];}B.history_count--;}
    B.history_pos=B.history_count++;copy(B.history[B.history_pos],NET_URL_MAX,url);B.history_scroll[B.history_pos]=0;
}
static void address_set(const char *url){copy(B.address,sizeof B.address,url);B.cursor=len(B.address);B.address_start=0;B.selected=0;}
static void stop_request(void) {
    if(B.loading&&net_http_result()->request_id==B.request_id&&net_http_busy())net_cancel();
    B.loading=0;
}
static void error_document(const char *title,const char *message) {
    B.body_complete=0;document_reset();copy(B.title,sizeof B.title,title);emit_string(title,STYLE_BOLD,0);newline(1);emit_string(message,0,0);
    newline(1);emit_string("Use Back to return, or enter another address.",0,0);set_status(message);
}
static int open_internal(const char *input,int add_history);
static int jump_fragment(const char *url) {
    const char *fragment=url;while(*fragment&&*fragment!='#')fragment++;
    if(!*fragment)return 0;
    fragment++;
    if(!*fragment){B.scroll=0;return 1;}
    for(int i=0;i<B.anchor_count;i++)if(equal(fragment,B.anchors[i].name)) {
        if(!B.width)reflow(BROWSER_W-2*PAD-14);
        for(int j=0;j<B.line_count;j++)if(B.lines[j].start+B.lines[j].length>=B.anchors[i].position){B.scroll=j;clamp_scroll();return 1;}
    }
    return 0;
}
void browser_init(void) {
    if(browser_ready)return;
    zero(&B,sizeof B);browser_ready=1;B.rows=16;B.history_pos=-1;
    open_internal("about:home",1);B.focus=1;B.selected=1;
}
static int local_open(void) {
    if(!starts(B.url,"file:///")){error_document("Local address not supported","Use file:/// followed by an absolute BaseOS path.");return 1;}
    char path[NET_URL_MAX];copy(path,sizeof path,B.url+7);
    for(int i=0;path[i];i++)if(path[i]=='#'||path[i]=='?'){path[i]=0;break;}
    int id=fs_resolve(fs_root(),path);
    if(id<0||fs_is_dir(id)||fs_is_app(id)){error_document("File not found","Open an existing local HTML or text file.");return 1;}
    int n=fs_read(id,B.body,sizeof B.body);
    if(n<0){error_document("File could not be read","The local file could not be read.");return 1;}
    B.body[n]=0;B.body_len=(unsigned)n;B.body_complete=fs_size(id)==n;
    int plen=len(path);B.html=(plen>=5&&starts(path+plen-5,".html"))||(plen>=4&&starts(path+plen-4,".htm"));
    parse_document(B.body,B.body_len,B.html);set_status(B.body_complete?"Local file | HTTP only browser":"Local file | Page truncated; cannot save an incomplete copy");jump_fragment(B.url);return 1;
}
static int open_internal(const char *input,int add_history) {
    char url[NET_URL_MAX];
    if(!normalize(url,input)){set_status("Enter an address shorter than 256 characters.");return 1;}
    if(starts(url,"http://")&&net_busy()&&!(B.loading&&net_http_busy()&&net_http_result()->request_id==B.request_id)) {
        set_status("Network is busy in another app. Try Go again when it finishes.");return 1;
    }
    int same=1;unsigned i=0;while(url[i]&&url[i]!='#'&&B.url[i]&&B.url[i]!='#'){if(url[i]!=B.url[i])same=0;i++;}
    if((url[i]&&url[i]!='#')||(B.url[i]&&B.url[i]!='#'))same=0;
    if(same&&url[i]=='#'&&!B.loading&&B.text_len) {
        if(add_history)history_add(url);
        copy(B.url,sizeof B.url,url);address_set(url);B.focus=0;
        if(!jump_fragment(url))set_status("This page does not contain that named anchor.");
        return 1;
    }
    stop_request();B.body_complete=0;if(add_history)history_add(url);
    copy(B.url,sizeof B.url,url);address_set(url);B.focus=0;B.redirects=0;
    if(equal(url,"about:home")){B.html=1;parse_document(home_html,sizeof home_html-1,1);set_status("Ready | HTTP is unencrypted | No HTTPS / TLS");return 1;}
    if(starts(url,"https:")){error_document("HTTPS is not supported","This browser supports unencrypted HTTP only. TLS / HTTPS is not available.");return 1;}
    if(starts(url,"file:"))return local_open();
    if(!starts(url,"http://")){error_document("Address not supported","Use http://host/path, file:///path or about:home. Scripts and other URL schemes are not supported.");return 1;}
    if(net_http_start(url,B.body,sizeof B.body)!=0){error_document("Could not load page",net_http_result()->error);return 1;}
    B.loading=1;B.request_id=net_http_result()->request_id;B.request_state=-1;B.received=0;set_status("Connecting...");return 1;
}
int browser_open(const char *url){browser_init();return open_internal(url,1);}
int browser_open_file(int id) {
    browser_init();char path[FS_PATH_LEN],url[NET_URL_MAX];
    if(!fs_valid(id)||fs_is_dir(id)||fs_is_app(id)){set_status("Select an HTML or text file.");return 1;}
    fs_path(id,path,sizeof path);if(len(path)+7>=NET_URL_MAX){set_status("The local file path is too long for Browser.");return 1;}
    copy(url,sizeof url,"file://");append(url,sizeof url,path);return open_internal(url,1);
}
void browser_close(void){if(!browser_ready)return;stop_request();set_status("Stopped | HTTP only browser");}
const char *browser_title(void){browser_init();return B.title;}
const char *browser_url(void){browser_init();return B.url;}
const char *browser_status(void){browser_init();return B.status;}
int browser_loading(void){return browser_ready&&B.loading;}

int browser_can_save(void){return browser_ready&&!B.loading&&B.body_complete;}
int browser_save_page(int cwd,const char *path){
    browser_init();
    if(!browser_can_save()){set_status("Only a completely loaded HTML or text page can be saved. Reload first.");return -1;}
    char name[FS_NAME_LEN];int parent=fs_destination(cwd,path,name);
    if(parent<0){set_status("Choose a new file name in an existing folder.");return -1;}
    if(fs_find_child(parent,name)>=0){set_status("That file already exists. Choose another name; nothing was replaced.");return -1;}
    if(B.body_len>fs_file_limit()||B.body_len>fs_capacity()-fs_used_bytes()){set_status("Not enough space for the complete page. No file was saved.");return -1;}
    int id=fs_create(parent,name);if(id<0){set_status("Could not create the file. Check available file slots.");return -1;}
    if(fs_write(id,B.body,(int)B.body_len)!=(int)B.body_len){fs_delete(id);set_status("Could not save the complete page. No destination file was left.");return -1;}
    set_status("Saved original page: ");append(B.status,sizeof B.status,path);
    append(B.status,sizeof B.status,fs_storage_status()?" | RAM only; disk unavailable":" | Disk autosave pending");return id;
}
static int save_page(void){
    if(!browser_can_save()){set_status("Wait for a complete page before saving. Truncated pages cannot be saved.");return 1;}
    int parent=fs_find_child(fs_root(),"Downloads");
    if(parent<0)parent=fs_mkdir(fs_root(),"Downloads");
    if(!fs_is_dir(parent)){set_status("Cannot create /Downloads. Check that it is a folder and space is available.");return 1;}
    char name[FS_NAME_LEN],path[64],digits[12];
    for(unsigned n=1;n<=FS_MAX_NODES;n++){
        copy(name,sizeof name,"page");if(n>1){append(name,sizeof name,"-");number(digits,n);append(name,sizeof name,digits);}
        append(name,sizeof name,B.html?".html":".txt");
        if(fs_find_child(parent,name)<0){copy(path,sizeof path,"/Downloads/");append(path,sizeof path,name);browser_save_page(fs_root(),path);return 1;}
    }
    set_status("No unused page file name is available in /Downloads.");return 1;
}

static void completion_status(const NetHttpResult *result) {
    char digits[12];copy(B.status,sizeof B.status,"HTTP ");number(digits,(unsigned)result->status);append(B.status,sizeof B.status,digits);
    append(B.status,sizeof B.status," | ");number(digits,result->length);append(B.status,sizeof B.status,digits);append(B.status,sizeof B.status," bytes");
    if(result->truncated||B.truncated)append(B.status,sizeof B.status," | Page truncated");
    else append(B.status,sizeof B.status," | HTTP only");
}
int browser_tick(void) {
    if(!browser_ready||!B.loading)return 0;
    const NetHttpResult *r=net_http_result();
    if(r->request_id!=B.request_id){B.loading=0;set_status("Another app replaced this request. Reload to try again.");return 1;}
    if(r->state==NET_HTTP_DONE) {
        B.loading=0;
        if(r->status>=300&&r->status<400&&r->location[0]) {
            char redirect[NET_URL_MAX];
            if(B.redirects>=5){error_document("Too many redirects","Stopped after five redirects.");return 1;}
            if(!resolve_url(redirect,B.url,r->location)){error_document("Redirect not supported","The redirect address is unsupported or too long.");return 1;}
            copy(B.url,sizeof B.url,redirect);address_set(redirect);
            if(B.history_pos>=0)copy(B.history[B.history_pos],NET_URL_MAX,redirect);
            if(starts(redirect,"https:")){error_document("HTTPS is not supported","The server redirected to HTTPS. This browser cannot establish TLS connections.");return 1;}
            if(!starts(redirect,"http://")){error_document("Redirect not supported","HTTP pages may redirect only to another HTTP address.");return 1;}
            B.redirects++;
            if(net_http_start(redirect,B.body,sizeof B.body)!=0){error_document("Could not follow redirect",net_http_result()->error);return 1;}
            B.loading=1;B.request_id=net_http_result()->request_id;B.request_state=-1;B.received=0;set_status("Following redirect...");return 1;
        }
        B.body_len=r->length<sizeof B.body?r->length:sizeof B.body-1;B.body[B.body_len]=0;
        B.body_complete=!r->truncated&&r->length<sizeof B.body;
        B.html=!r->content_type[0]||starts(r->content_type,"text/html")||starts(r->content_type,"application/xhtml+xml");
        if(r->content_type[0]&&!B.html&&!starts(r->content_type,"text/")){error_document("Content type not supported","This browser displays HTML and plain text. Use Terminal download to save image or document bytes.");return 1;}
        parse_document(B.body,B.body_len,B.html);completion_status(r);
        if(B.history_pos>=0)B.scroll=B.history_scroll[B.history_pos];
        jump_fragment(B.url);return 1;
    }
    if(r->state==NET_HTTP_ERROR){B.loading=0;error_document("Could not load page",r->error[0]?r->error:"The network request failed.");return 1;}
    if(B.request_state!=r->state||B.received!=r->length) {
        B.request_state=r->state;B.received=r->length;
        if(r->state==NET_HTTP_RESOLVING)set_status("Looking up host...");
        else if(r->state==NET_HTTP_CONNECTING)set_status("Connecting...");
        else {char digits[12];number(digits,r->length);copy(B.status,sizeof B.status,"Loading ");append(B.status,sizeof B.status,digits);append(B.status,sizeof B.status," bytes... | Stop cancels");}
        return 1;
    }
    return 0;
}

int browser_scroll(int lines){browser_init();int old=B.scroll;B.scroll+=lines;clamp_scroll();return old!=B.scroll;}
static int history_step(int direction) {
    int next=B.history_pos+direction;if(next<0||next>=B.history_count)return 0;
    /* Do not move the history cursor if another app owns the network. */
    if(starts(B.history[next],"http://")&&net_busy()&&!(B.loading&&net_http_busy()&&net_http_result()->request_id==B.request_id)){set_status("Network is busy in another app. Try again when it finishes.");return 1;}
    save_scroll();B.history_pos=next;int saved=B.history_scroll[next];int result=open_internal(B.history[next],0);if(!B.loading)B.scroll=saved;return result;
}
static int reload(void){save_scroll();return open_internal(B.url,0);}
static int stop(void){if(!B.loading)return 0;stop_request();set_status("Stopped | Reload to try again");return 1;}
static void select_link(int direction) {
    if(!B.link_count){B.focus=1;B.selected=1;return;}
    B.focus=0;B.focused_link+=direction;
    if(B.focused_link<1)B.focused_link=B.link_count;
    if(B.focused_link>B.link_count)B.focused_link=1;
    for(int i=0;i<B.line_count;i++){BrowserLine line=B.lines[i];int found=0;
        for(unsigned p=line.start;p<(unsigned)line.start+line.length;p++)if(B.link[p]==B.focused_link){found=1;break;}
        if(found){if(i<B.scroll)B.scroll=i;else if(i>=B.scroll+B.rows)B.scroll=i-B.rows+1;clamp_scroll();break;}}
    set_status(B.links[B.focused_link-1]);
}
int browser_key(int sc,char ch,int modifiers) {
    browser_init();
    if((modifiers&BROWSER_MOD_CTRL)&&(sc==0x1f||lower(ch)=='s'))return save_page();
    if((modifiers&BROWSER_MOD_CTRL)&&(sc==0x26||lower(ch)=='l')){B.focus=1;B.selected=1;B.cursor=len(B.address);return 1;}
    if(((modifiers&BROWSER_MOD_CTRL)&&(sc==0x13||lower(ch)=='r'))||sc==0x3f)return reload();
    if(modifiers&BROWSER_MOD_ALT){if(sc==KEY_LEFT)return history_step(-1);if(sc==KEY_RIGHT)return history_step(1);if(sc==0x47)return open_internal("about:home",1);}
    if(sc==KEY_ESC){if(B.loading)return stop();B.focus=0;B.selected=0;B.focused_link=0;return 1;}
    if(sc==KEY_TAB){if(B.focus){B.focus=0;B.focused_link=0;}select_link(modifiers&BROWSER_MOD_SHIFT?-1:1);return 1;}
    if(B.focus) {
        int n=len(B.address);
        if((modifiers&BROWSER_MOD_CTRL)&&(sc==0x1e||lower(ch)=='a')){B.selected=1;return 1;}
        if(modifiers&(BROWSER_MOD_CTRL|BROWSER_MOD_ALT))return 0;
        if(sc==KEY_ENTER)return open_internal(B.address,1);
        if(sc==KEY_LEFT){if(B.selected)B.cursor=0;else if(B.cursor)B.cursor--;B.selected=0;return 1;}
        if(sc==KEY_RIGHT){if(B.selected)B.cursor=n;else if(B.cursor<n)B.cursor++;B.selected=0;return 1;}
        if(sc==0x47||sc==0x4f){B.cursor=sc==0x47?0:n;B.selected=0;return 1;}
        if(sc==KEY_BACKSPACE||sc==0x53) {
            if(B.selected){B.address[0]=0;B.cursor=0;B.selected=0;return 1;}
            int p=sc==KEY_BACKSPACE?B.cursor-1:B.cursor;if(p<0||p>=n)return 0;
            for(int i=p;i<n;i++)B.address[i]=B.address[i+1];
            if(sc==KEY_BACKSPACE)B.cursor--;
            return 1;
        }
        if(ch>=' '&&ch<='~') {
            if(B.selected){B.address[0]=0;n=0;B.cursor=0;B.selected=0;}
            if(n+1>=NET_URL_MAX){set_status("The address is limited to 255 characters.");return 1;}
            for(int i=n;i>=B.cursor;i--)B.address[i+1]=B.address[i];
            B.address[B.cursor++]=ch;return 1;
        }
        return 0;
    }
    if(sc==KEY_ENTER&&B.focused_link)return open_internal(B.links[B.focused_link-1],1);
    if(sc==KEY_BACKSPACE)return history_step(-1);
    if(sc==KEY_UP)return browser_scroll(-1);
    if(sc==KEY_DOWN)return browser_scroll(1);
    if(sc==0x49)return browser_scroll(-(B.rows>1?B.rows-1:1));
    if(sc==0x51||sc==KEY_SPACE)return browser_scroll(B.rows>1?B.rows-1:1);
    if(sc==0x47){int old=B.scroll;B.scroll=0;return old!=0;}
    if(sc==0x4f){int old=B.scroll;B.scroll=B.line_count;clamp_scroll();return old!=B.scroll;}
    return 0;
}

static const char *const button_labels[]={"Back","Forward","Reload","Stop","Home","Save"};
static const int button_widths[]={46,64,58,46,50,50};
static int button_x(int x,int index){int pos=x+8;for(int i=0;i<index;i++)pos+=button_widths[i]+4;return pos;}
static int button_enabled(int index){return index==0?B.history_pos>0:index==1?B.history_pos+1<B.history_count:index==3?B.loading:index==5?browser_can_save():1;}
static int address_left(int x){return x+10;}
static int address_width(int w){return w-64;}
static void fit_address(int w) {
    if(B.address_start>B.cursor)B.address_start=B.cursor;
    int pixels=0;for(int i=B.address_start;i<B.cursor;i++)pixels+=ui_advance(B.address[i]);
    while(pixels>w-16&&B.address_start<B.cursor)pixels-=ui_advance(B.address[B.address_start++]);
    while(B.address_start>0&&pixels+ui_advance(B.address[B.address_start-1])<w-24)pixels+=ui_advance(B.address[--B.address_start]);
}
void browser_draw(int x,int y,int w,int h) {
    browser_init();if(w<BROWSER_MIN_W||h<BROWSER_MIN_H)return;geometry(w,h);
    uint8_t paper=COLOR_WHITE,ink=gfx_gray(35),muted=gfx_gray(112),border=gfx_gray(218),link_color=gfx_rgb(35,92,172);
    draw_rect(x,y,w,h,paper);draw_rect(x,y,w,TOOL_H,app_chrome);draw_hline(x,y+TOOL_H-1,w,border);
    for(int i=0;i<6;i++){int bx=button_x(x,i),enabled=button_enabled(i);draw_round_rect(bx,y+7,button_widths[i],26,4,enabled?paper:app_chrome);
        draw_round_frame(bx,y+7,button_widths[i],26,4,border);draw_string(button_labels[i],bx+(button_widths[i]-ui_string_w(button_labels[i]))/2,y+11,enabled?app_text:muted);}
    int ax=address_left(x),aw=address_width(w);draw_round_rect(ax,y+41,aw,28,4,paper);draw_round_frame(ax,y+41,aw,28,4,B.focus?app_accent:border);
    fit_address(aw);int tx=ax+7,limit=ax+aw-8;
    if(B.focus&&B.selected)draw_rect(tx,y+46,aw-14,18,gfx_rgb(205,226,251));
    draw_string_clip(B.address+B.address_start,tx,y+46,ink,limit);
    if(B.focus&&!B.selected){int cursor=tx;for(int i=B.address_start;i<B.cursor;i++)cursor+=ui_advance(B.address[i]);if(cursor<limit)draw_vline(cursor,y+46,18,app_accent);}
    int go=x+w-46;draw_round_rect(go,y+41,36,28,4,app_accent);draw_string("Go",go+(36-ui_string_w("Go"))/2,y+46,COLOR_WHITE);
    int cy=y+TOOL_H+8,content_right=x+w-PAD-14;
    for(int row=0;row<B.rows;row++) {
        int index=B.scroll+row;if(index>=B.line_count)break;BrowserLine line=B.lines[index];int px=x+PAD,py=cy+row*ROW_H;
        unsigned end=(unsigned)line.start+line.length;
        for(unsigned p=line.start;p<end;p++) {
            int step=advance(p);if(px+step>content_right)break;
            unsigned char style=B.style[p],link=B.link[p];uint8_t color=link?link_color:ink;
            if(link&&link==B.focused_link)draw_rect(px,py,step,UI_FONT_H+1,gfx_rgb(225,237,255));
            if(style&STYLE_PRE)draw_edit_char(B.text[p],px,py,color);
            else if(style&STYLE_BOLD){char c[2]={B.text[p],0};draw_string_bold(c,px,py,color);}
            else draw_char(B.text[p],px,py,color);
            if(link)draw_hline(px,py+UI_FONT_H,step,color);
            px+=step;
        }
    }
    if(B.line_count>B.rows){int sy=cy,sh=B.rows*ROW_H;draw_round_rect(x+w-10,sy,4,sh,2,gfx_gray(237));int thumb=sh*B.rows/B.line_count;if(thumb<16)thumb=16;if(thumb>sh)thumb=sh;
        int ty=sy+(sh-thumb)*B.scroll/(B.line_count-B.rows);draw_round_rect(x+w-10,ty,4,thumb,2,gfx_gray(170));}
    int fy=y+h-FOOT_H;draw_rect(x,fy,w,FOOT_H,app_chrome);draw_hline(x,fy,w,border);draw_string_clip(B.status,x+10,fy+5,app_text_dim,x+w-10);
}
int browser_click(int x,int y,int w,int h,int mx,int my) {
    browser_init();if(w<BROWSER_MIN_W||h<BROWSER_MIN_H)return 0;geometry(w,h);
    for(int i=0;i<6;i++)if(hit(mx,my,button_x(x,i),y+7,button_widths[i],26)) {
        if(!button_enabled(i))return 0;
        return i==0?history_step(-1):i==1?history_step(1):i==2?reload():i==3?stop():i==4?open_internal("about:home",1):save_page();
    }
    if(hit(mx,my,x+w-46,y+41,36,28))return open_internal(B.address,1);
    if(hit(mx,my,address_left(x),y+41,address_width(w),28)) {
        B.focus=1;B.selected=0;fit_address(address_width(w));int px=address_left(x)+7;B.cursor=B.address_start;
        while(B.address[B.cursor]&&px+ui_advance(B.address[B.cursor])/2<mx)px+=ui_advance(B.address[B.cursor++]);
        return 1;
    }
    int cy=y+TOOL_H+8;
    if(hit(mx,my,x+w-15,cy,15,B.rows*ROW_H)&&B.line_count>B.rows) {
        int old=B.scroll;B.scroll=(my-cy)*B.line_count/(B.rows*ROW_H);clamp_scroll();B.focus=0;return old!=B.scroll;
    }
    if(hit(mx,my,x+PAD,cy,w-2*PAD-14,B.rows*ROW_H)) {
        B.focus=0;B.selected=0;int row=B.scroll+(my-cy)/ROW_H;
        if(row<B.line_count){BrowserLine line=B.lines[row];int px=x+PAD;
            for(unsigned p=line.start;p<(unsigned)line.start+line.length;p++){int step=advance(p);if(mx>=px&&mx<px+step&&B.link[p])return open_internal(B.links[B.link[p]-1],1);px+=step;}}
        B.focused_link=0;return 1;
    }
    return 0;
}
