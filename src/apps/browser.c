/* BaseOS Browser: bounded HTML text presentation over the cooperative HTTP API.
 * This is deliberately not a web engine: no TLS, scripts, CSS, forms or images.
 * All document storage lives in one fixed arena, never in the kernel stack. */
#include "ui/app.h"
#include "apps/browser.h"
#include "kernel/download.h"
#include "kernel/fs.h"
#include "kernel/layout.h"
#include "kernel/net.h"
#include "ui/icon_draw.h"
#include "ui/icons.h"
#include "ui/kit.h"
#include "ui/ui_icons.h"

#define MODERN (ui_theme.style == UI_STYLE_MODERN)
#define SB (ui_theme.scrollbar)

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
    int loading, request_state, redirects, truncated, html, body_complete, error_page;
    char error_message[STATUS_MAX];
    /* Only an accepted request ID grants control over the download service.
     * This small presentation snapshot never contains response bytes. */
    unsigned download_id, download_received, download_limit;
    int download_state, download_http, download_visible, download_scroll;
    int download_line_count, pick_link;
    char download_path[64], download_url[NET_URL_MAX];
    char download_message[160], download_notice[160], download_text[896];
    BrowserLine download_lines[48];
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
    "Download or Ctrl+D saves an HTTP address or selected link in the background. "
    "Pick link selects a link with the mouse without opening it.</p>"
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
    B.truncated=0;B.title[0]=0;B.width=0;B.scroll=0;B.focused_link=0;B.error_page=0;
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
static int tool_height(int w);
static int status_h(void){return MODERN?28:24;}
static void geometry(int w,int h) { B.rows=(h-tool_height(w)-status_h()-16)/ROW_H;if(B.rows<1)B.rows=1;reflow(w-2*PAD-SB);clamp_scroll(); }
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
    B.body_complete=0;document_reset();B.error_page=1;copy(B.error_message,sizeof B.error_message,message);copy(B.title,sizeof B.title,title);emit_string(title,STYLE_BOLD,0);newline(1);emit_string(message,0,0);
    newline(1);emit_string("Use Back to return, or enter another address.",0,0);set_status(message);
}
static int open_internal(const char *input,int add_history);
static int jump_fragment(const char *url) {
    const char *fragment=url;while(*fragment&&*fragment!='#')fragment++;
    if(!*fragment)return 0;
    fragment++;
    if(!*fragment){B.scroll=0;return 1;}
    for(int i=0;i<B.anchor_count;i++)if(equal(fragment,B.anchors[i].name)) {
        if(!B.width)reflow(BROWSER_W-2*PAD-SB);
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
    B.download_visible=0;B.pick_link=0;
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
void browser_close(void){if(!browser_ready)return;stop_request();B.pick_link=0;set_status("Page stopped | Background download continues, if active");}
const char *browser_title(void){browser_init();return B.title;}
const char *browser_url(void){browser_init();return B.url;}
const char *browser_status(void){browser_init();return B.status;}
int browser_loading(void){return browser_ready&&B.loading;}

int browser_can_save(void){return browser_ready&&!B.loading&&B.body_complete;}
int browser_save_page(int cwd,const char *path){
    browser_init();
    if(!browser_can_save()){set_status("Only a completely loaded HTML or text page can be saved. Reload first.");return -1;}
    if(fs_sync_busy()){set_status("Disk saving is in progress. The page is still available; retry Save shortly.");return FS_ERR_BUSY;}
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
    if(fs_sync_busy()){set_status("Disk saving is in progress. The page is still available; retry Save shortly.");return 1;}
    int parent=fs_find_child(fs_root(),"Downloads");
    if(parent<0)parent=fs_mkdir(fs_root(),"Downloads");
    if(!fs_is_dir(parent)){set_status("Cannot create /Downloads. Check that it is a folder and space is available.");return 1;}
    char name[FS_NAME_LEN],path[FS_PATH_LEN],digits[12];
    for(unsigned n=1;n<=(unsigned)fs_node_count()+1;n++){
        copy(name,sizeof name,"page");if(n>1){append(name,sizeof name,"-");number(digits,n);append(name,sizeof name,digits);}
        append(name,sizeof name,B.html?".html":".txt");
        if(fs_find_child(parent,name)<0){copy(path,sizeof path,"/Downloads/");append(path,sizeof path,name);browser_save_page(fs_root(),path);return 1;}
    }
    set_status("No unused page file name is available in /Downloads.");return 1;
}

/* Build a safe bounded basename from the path only. Percent escapes stay
 * literal/sanitized; query strings and fragments never become file names. */
static void download_basename(const char *url,char stem[FS_NAME_LEN],char extension[8]) {
    const char *p=url+7;while(*p&&*p!='/'&&*p!='?'&&*p!='#')p++;
    const char *base=p,*end=p;
    while(*end&&*end!='?'&&*end!='#'){if(*end=='/')base=end+1;end++;}
    const char *dot=end;while(dot>base&&dot[-1]!='.')dot--;
    extension[0]=0;
    if(dot>base&&dot<end&&end-dot<=5) {
        char ext[8];unsigned n=0;ext[n++]='.';
        for(const char *q=dot;q<end;q++)ext[n++]=(char)lower(*q);
        ext[n]=0;
        static const char *const known[]={".wav",".mp3",".mpg",".mpeg",".png",".jpg",".jpeg",".gif",".bmp",".html",".htm",".txt",".pdf",".zip",".bin",".bex",".bas",".sh"};
        for(unsigned i=0;i<sizeof known/sizeof known[0];i++)if(equal(ext,known[i])){copy(extension,8,ext);end=dot-1;break;}
    }
    unsigned n=0;
    for(const char *q=base;q<end&&n<FS_NAME_LEN-1;q++) {
        int c=(unsigned char)*q;
        if(c=='.'&&!n)continue;
        stem[n++]=(char)((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'?c:'_');
    }
    while(n&&stem[n-1]=='.')n--;
    stem[n]=0;if(!n)copy(stem,FS_NAME_LEN,"download");
}
static void download_summary(void) {
    char digits[12];B.download_text[0]=0;
    if(B.download_notice[0]){append(B.download_text,sizeof B.download_text,B.download_notice);append(B.download_text,sizeof B.download_text,"\n\n");}
    if(B.download_path[0]) {
        append(B.download_text,sizeof B.download_text,B.download_path);append(B.download_text,sizeof B.download_text,"\n");
        append(B.download_text,sizeof B.download_text,B.download_state==DOWNLOAD_ACTIVE?"Downloading: ":B.download_state==DOWNLOAD_DONE?"Complete: ":B.download_state==DOWNLOAD_CANCELLED?"Cancelled: ":"Error: ");
        number(digits,B.download_received);append(B.download_text,sizeof B.download_text,digits);append(B.download_text,sizeof B.download_text," bytes");
        if(B.download_http){append(B.download_text,sizeof B.download_text," | HTTP ");number(digits,(unsigned)B.download_http);append(B.download_text,sizeof B.download_text,digits);}
        append(B.download_text,sizeof B.download_text,"\n");
        append(B.download_text,sizeof B.download_text,B.download_message);
        if(B.download_state==DOWNLOAD_ACTIVE)append(B.download_text,sizeof B.download_text,"\nCancel stops only this Browser download. Closing Browser leaves it running.");
        append(B.download_text,sizeof B.download_text,"\nFile limit: ");number(digits,B.download_limit);append(B.download_text,sizeof B.download_text,digits);
        append(B.download_text,sizeof B.download_text," bytes\n");append(B.download_text,sizeof B.download_text,B.download_url);
    } else if(!B.download_notice[0])copy(B.download_text,sizeof B.download_text,"No Browser download yet.\nType an HTTP address, then Download or Ctrl+D.\nTab or Pick link selects a link to download.\nHTTP is unencrypted. HTTPS is unsupported.");
}
static int refresh_download(void) {
    if(!B.download_id)return 0;
    const DownloadStatus *d=download_status();
    if(d->request_id!=B.download_id) {
        B.download_id=0;
        if(B.download_state==DOWNLOAD_ACTIVE){B.download_state=DOWNLOAD_ERROR;copy(B.download_message,sizeof B.download_message,"Another app replaced the transfer status. Check /Downloads for the last result.");download_summary();return 1;}
        return 0;
    }
    if(B.download_state==d->state&&B.download_received==d->received&&B.download_http==d->http_status&&equal(B.download_message,d->message))return 0;
    B.download_state=d->state;B.download_received=d->received;B.download_http=d->http_status;
    copy(B.download_message,sizeof B.download_message,d->message);download_summary();return 1;
}
static int own_download_active(void) {
    if(!B.download_id)return 0;
    const DownloadStatus *d=download_status();return d->request_id==B.download_id&&d->state==DOWNLOAD_ACTIVE;
}
static int download_problem(const char *message) {
    copy(B.download_notice,sizeof B.download_notice,message);set_status(message);B.download_visible=1;B.download_scroll=0;B.focus=0;B.pick_link=0;download_summary();return 1;
}
static int start_download(void) {
    char url[NET_URL_MAX],stem[FS_NAME_LEN],extension[8],name[FS_NAME_LEN],path[FS_PATH_LEN],suffix[12];
    const char *target=!B.focus&&B.focused_link?B.links[B.focused_link-1]:B.address;
    if(!normalize(url,target))return download_problem("Enter an HTTP address shorter than 256 characters.");
    if(!starts(url,"http://"))return download_problem("Only HTTP downloads are supported. HTTPS is never downgraded; local files and other schemes are not downloaded.");
    /* Do not send credentials, even accidentally through an address. */
    for(int i=7;url[i]&&url[i]!='/'&&url[i]!='?'&&url[i]!='#';i++)if(url[i]=='@')return download_problem("Addresses containing credentials are not supported.");
    if(download_active())return download_problem(own_download_active()?"This Browser download is already running. Cancel it or wait before starting another.":"A download is already running in another app. Wait for it to finish; Browser cannot cancel it.");
    if(net_busy())return download_problem(B.loading?"A page is loading. Use Stop, then Download, or wait for the page to finish.":"Network is busy in another app. Wait for it to finish.");
    int parent=fs_find_child(fs_root(),"Downloads");if(parent<0)parent=fs_mkdir(fs_root(),"Downloads");
    if(!fs_is_dir(parent))return download_problem("Cannot create /Downloads. Check that it is a folder and space is available.");
    download_basename(url,stem,extension);
    for(unsigned n=1;n<=(unsigned)fs_node_count()+2;n++) {
        suffix[0]=0;if(n>1){suffix[0]='-';number(suffix+1,n);}
        unsigned room=(unsigned)fs_name_max()-(unsigned)len(extension)-(unsigned)len(suffix);copy(name,room+1,stem);
        append(name,sizeof name,suffix);append(name,sizeof name,extension);
        if(fs_find_child(parent,name)>=0)continue;
        copy(path,sizeof path,"/Downloads/");append(path,sizeof path,name);
        if(download_start(fs_root(),url,path)<0)return download_problem(download_last_error());
        const DownloadStatus *d=download_status();B.download_id=d->request_id;B.download_state=-1;
        B.download_limit=d->limit;copy(B.download_path,sizeof B.download_path,path);copy(B.download_url,sizeof B.download_url,url);
        B.download_notice[0]=0;B.download_visible=1;B.download_scroll=0;B.pick_link=0;B.focus=0;
        refresh_download();set_status("Download started | Page returns to reading | Scroll for details");return 1;
    }
    return download_problem("No unused download name is available in /Downloads.");
}
static int cancel_download(void) {
    if(!own_download_active())return download_problem("No active download owned by Browser. Other apps' transfers are left alone.");
    download_cancel();B.download_notice[0]=0;refresh_download();B.download_visible=1;B.download_scroll=0;
    set_status("Download cancelled | No file was saved");return 1;
}
static void download_reflow(int width) {
    if(width<40)width=40;
    B.download_line_count=0;unsigned p=0;
    while(B.download_text[p]&&B.download_line_count<48) {
        unsigned start=p,end=p,last_space=p;int pixels=0;
        while(B.download_text[p]&&B.download_text[p]!='\n') {
            int step=ui_advance(B.download_text[p]);if(pixels+step>width&&p>start)break;
            if(B.download_text[p]==' ')last_space=p;
            pixels+=step;p++;
        }
        end=p;
        if(B.download_text[p]=='\n')p++;
        else if(B.download_text[p]&&last_space>start){end=last_space;p=last_space+1;}
        BrowserLine *line=&B.download_lines[B.download_line_count++];line->start=(unsigned short)start;line->length=(unsigned short)(end-start);
    }
    int max=B.download_line_count-B.rows;if(max<0)max=0;
    if(B.download_scroll>max)B.download_scroll=max;
    if(B.download_scroll<0)B.download_scroll=0;
}

static void completion_status(const NetHttpResult *result) {
    char digits[12];copy(B.status,sizeof B.status,"HTTP ");number(digits,(unsigned)result->status);append(B.status,sizeof B.status,digits);
    append(B.status,sizeof B.status," | ");number(digits,result->length);append(B.status,sizeof B.status,digits);append(B.status,sizeof B.status," bytes");
    if(result->truncated||B.truncated)append(B.status,sizeof B.status," | Page truncated");
    else append(B.status,sizeof B.status," | HTTP only");
}
int browser_tick(void) {
    if(!browser_ready)return 0;
    int changed=refresh_download();
    if(!B.loading)return changed;
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
        if(r->content_type[0]&&!B.html&&!starts(r->content_type,"text/")){error_document("Content type not supported","This browser displays HTML and plain text. Use Download or Ctrl+D to save this address to /Downloads.");return 1;}
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
    return changed;
}

int browser_scroll(int lines){browser_init();if(B.download_visible){int old=B.download_scroll;B.download_scroll+=lines;download_reflow(B.width);return old!=B.download_scroll;}int old=B.scroll;B.scroll+=lines;clamp_scroll();return old!=B.scroll;}
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
    B.focus=0;B.download_visible=0;B.focused_link+=direction;
    if(B.focused_link<1)B.focused_link=B.link_count;
    if(B.focused_link>B.link_count)B.focused_link=1;
    for(int i=0;i<B.line_count;i++){BrowserLine line=B.lines[i];int found=0;
        for(unsigned p=line.start;p<(unsigned)line.start+line.length;p++)if(B.link[p]==B.focused_link){found=1;break;}
        if(found){if(i<B.scroll)B.scroll=i;else if(i>=B.scroll+B.rows)B.scroll=i-B.rows+1;clamp_scroll();break;}}
    set_status(B.links[B.focused_link-1]);
}
int browser_key(int sc,char ch,int modifiers) {
    browser_init();
    if((modifiers&BROWSER_MOD_CTRL)&&(sc==0x20||lower(ch)=='d'))return start_download();
    if((modifiers&BROWSER_MOD_CTRL)&&(sc==0x1f||lower(ch)=='s'))return save_page();
    if((modifiers&BROWSER_MOD_CTRL)&&(sc==0x26||lower(ch)=='l')){B.focus=1;B.selected=1;B.cursor=len(B.address);return 1;}
    if(((modifiers&BROWSER_MOD_CTRL)&&(sc==0x13||lower(ch)=='r'))||sc==0x3f)return reload();
    if(modifiers&BROWSER_MOD_ALT){if(sc==KEY_LEFT)return history_step(-1);if(sc==KEY_RIGHT)return history_step(1);if(sc==0x47)return open_internal("about:home",1);}
    if(sc==KEY_ESC){if(B.loading)return stop();if(B.download_visible&&own_download_active())return cancel_download();B.download_visible=0;B.pick_link=0;B.focus=0;B.selected=0;B.focused_link=0;return 1;}
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
    if(B.download_visible&&(sc==0x47||sc==0x4f))return browser_scroll(sc==0x47?-48:48);
    if(sc==0x47){int old=B.scroll;B.scroll=0;return old!=0;}
    if(sc==0x4f){int old=B.scroll;B.scroll=B.line_count;clamp_scroll();return old!=B.scroll;}
    return 0;
}

/* Navigation bar geometry, shared by drawing and hit testing. Wide windows
 * get one row: navigation | address Go | download tools | Save. Narrow
 * windows move the address field and Go to a second row. Tools are flat
 * icon buttons at the style's control size; in Modern the bar continues the
 * window header. */
enum{BT_BACK,BT_FORWARD,BT_RELOAD,BT_STOP,BT_HOME,BT_GO,BT_DOWNLOAD,BT_PICK,BT_CANCEL,BT_PROGRESS,BT_SAVE,BT_COUNT};
static const int tool_icons[BT_COUNT]={ICON_BACK,ICON_FORWARD,ICON_RELOAD,ICON_STOP,ICON_HOME,ICON_GO,ICON_DOWNLOAD,ICON_LINK,ICON_CLOSE,ICON_LIST,ICON_SAVE};
typedef struct{int x,y,w,h;}ToolRect;
typedef struct{ToolRect b[BT_COUNT],address;int divider[3],dividers;}ToolLayout;
#define TOOL_PAD (MODERN?12:8)
#define TOOL_GAP (MODERN?4:2)
#define TOOL_SPLIT (MODERN?17:13)
#define TOOL_ADDRESS_MIN 240
static int tool_size(void){return ui_theme.control_h;}
static int tool_vpad(void){return MODERN?7:4;}
/* Width of nav (5) + downloads (4) + Save with dividers and padding. */
static int tool_buttons_w(int bw){return 2*TOOL_PAD+10*bw+7*TOOL_GAP+2*TOOL_SPLIT;}
static int tool_wide(int w){return w>=tool_buttons_w(tool_size())+TOOL_SPLIT+TOOL_ADDRESS_MIN+TOOL_GAP+tool_size();}
static int tool_height(int w){
    int one=tool_size()+2*tool_vpad()+1;
    return tool_wide(w)?one:one+tool_size()+6;
}
static void tool_layout(int x,int y,int w,ToolLayout *L) {
    /* At the narrowest sizes the buttons narrow to keep one row of tools. */
    int wide=tool_wide(w),bh=tool_size(),bw=bh,row=y+tool_vpad(),px=x+TOOL_PAD,rx=x+w-TOOL_PAD,half=TOOL_SPLIT/2;
    if(w<tool_buttons_w(bw))bw=(w-tool_buttons_w(0))/10;
    L->dividers=0;
    for(int i=BT_BACK;i<=BT_HOME;i++){L->b[i]=(ToolRect){px,row,bw,bh};px+=bw+TOOL_GAP;}
    px+=half-TOOL_GAP;L->divider[L->dividers++]=px;px+=TOOL_SPLIT-half;
    rx-=bw;L->b[BT_SAVE]=(ToolRect){rx,row,bw,bh};
    rx-=TOOL_SPLIT-half;L->divider[L->dividers++]=rx;rx-=half;
    for(int i=BT_PROGRESS;i>=BT_DOWNLOAD;i--){rx-=bw;L->b[i]=(ToolRect){rx,row,bw,bh};rx-=TOOL_GAP;}
    rx+=TOOL_GAP;
    if(wide) {
        rx-=TOOL_SPLIT-half;L->divider[L->dividers++]=rx;rx-=half;
        rx-=bw;L->b[BT_GO]=(ToolRect){rx,row,bw,bh};rx-=TOOL_GAP;
        L->address=(ToolRect){px,row,rx-px,bh};
    } else {
        row+=bh+6;
        L->b[BT_GO]=(ToolRect){x+w-TOOL_PAD-bh,row,bh,bh};
        L->address=(ToolRect){x+TOOL_PAD,row,w-2*TOOL_PAD-bh-TOOL_GAP,bh};
    }
}
static int tool_enabled(int index){return index==BT_BACK?B.history_pos>0:index==BT_FORWARD?B.history_pos+1<B.history_count:index==BT_STOP?B.loading:
    index==BT_SAVE?browser_can_save():index==BT_CANCEL?own_download_active():index==BT_PICK?B.link_count>0:1;}
static int tool_toggled(int index){return (index==BT_PICK&&B.pick_link)||(index==BT_PROGRESS&&B.download_visible);}
/* The desktop passes its hover tracker while the window is active, so
 * pointer motion over a control repaints it. Without one nothing hovers. */
static int (*hover_fn)(int x,int y,int w,int h);
void browser_set_hover(int (*fn)(int x,int y,int w,int h)){hover_fn=fn;}
static int hover(ToolRect r){return hover_fn?hover_fn(r.x,r.y,r.w,r.h):0;}
/* One tool: flat until hovered, tinted while toggled, accent for Go. */
static void tool_draw(const ToolRect *r,int index) {
    int state=tool_enabled(index)?hover(*r):KIT_DISABLED,rr=MODERN?ui_theme.r_control:KIT_R_SQUARE;
    uint8_t ink=TK_ICON;
    if(state&KIT_DISABLED)ink=MODERN?TK_LINE_STRONG:TK_LINE;
    else if(index==BT_GO){
        kit_box(r->x,r->y,r->w,r->h,MODERN?rr:KIT_R_NOTCH,(state&KIT_PRESSED)?TK_ACCENT_700:(state&KIT_HOVER)?TK_ACCENT_600:TK_ACCENT,
                MODERN?0:TK_ACCENT_700);
        ink=TK_ON_ACCENT;
    }else if(tool_toggled(index)){
        if(MODERN){kit_fill(r->x,r->y,r->w,r->h,rr,TK_ACCENT_100,1);ink=TK_ACCENT_700;}
        else kit_box(r->x,r->y,r->w,r->h,KIT_R_NOTCH,TK_RAISED,TK_LINE);
    }else if(state&(KIT_HOVER|KIT_PRESSED))kit_fill(r->x,r->y,r->w,r->h,rr,(state&KIT_PRESSED)?TK_LINE_SUBTLE:TK_HOVER,1);
    icon_draw(tool_icons[index],r->x+(r->w-ICON_SIZE)/2,r->y+(r->h-ICON_SIZE)/2,ink);
}
/* Address text inset from the field edge, matching kit_field. */
static int address_inset(void){return (MODERN?10:6)+1;}
static void fit_address(int w) {
    int room=w-2*address_inset();
    if(B.address_start>B.cursor)B.address_start=B.cursor;
    int pixels=0;for(int i=B.address_start;i<B.cursor;i++)pixels+=ui_advance(B.address[i]);
    while(pixels>room&&B.address_start<B.cursor)pixels-=ui_advance(B.address[B.address_start++]);
    while(B.address_start>0&&pixels+ui_advance(B.address[B.address_start-1])<room-8)pixels+=ui_advance(B.address[--B.address_start]);
}
/* Classic: a white field. Modern: a soft fill that turns into a white
 * field with a focus ring while editing. */
static void address_draw(const ToolRect *a) {
    if(MODERN&&!B.focus)kit_fill(a->x,a->y,a->w,a->h,ui_theme.r_control,TK_HOVER,1);
    else kit_field(a->x,a->y,a->w,a->h,B.focus?KIT_FOCUS:0);
    fit_address(a->w);
    int tx=a->x+address_inset(),ty=kit_ty(a->y,a->h),limit=a->x+a->w-address_inset();
    if(B.focus&&B.selected&&B.address[0]){
        int sw=0;for(int i=B.address_start;B.address[i];i++)sw+=ui_advance(B.address[i]);
        if(sw>limit-tx)sw=limit-tx;
        draw_rect(tx-1,ty,sw+2,UI_FONT_H,TK_ACCENT);
    }
    draw_string_clip(B.address+B.address_start,tx,ty,B.focus&&B.selected?TK_ON_ACCENT:TK_TEXT,limit);
    if(B.focus&&!B.selected){int cursor=tx;for(int i=B.address_start;i<B.cursor;i++)cursor+=ui_advance(B.address[i]);if(cursor<limit)kit_caret(cursor,ty);}
}
static int content_top(int y,int w){return y+tool_height(w)+8;}
static int content_bottom(int y,int h){return y+h-status_h()-8;}
/* Length of the next wrapped line of p no wider than w, breaking at a
 * space when one fits. */
static int wrap_next(const char *p,int w,int cap) {
    int used=0,n=0,split=0;
    while(p[n]&&n+1<cap&&used+ui_advance(p[n])<=w){if(p[n]==' ')split=n;used+=ui_advance(p[n]);n++;}
    if(p[n]&&split)n=split;
    return n;
}
/* Error pages: a large icon, the title and the message, centred. */
static void error_draw(int x,int top,int w,int bottom) {
    char line[STATUS_MAX];
    int tw=w-64<460?w-64:460,lines=0;
    if(tw<80)return;
    const char *m=B.error_message;
    /* Count wrapped lines first, so the block centres vertically. */
    for(const char *p=m;*p;lines++){
        int n=wrap_next(p,tw,(int)sizeof line);
        if(!n)break;
        p+=n;while(*p==' ')p++;
    }
    int need=32+14+UI_FONT_H+10+lines*20+10+UI_FONT_H,y=top+(bottom-top-need)/3;
    if(y<top)y=top;
    if(y+32>bottom)return;
    ui_icon_draw(UI_ICON_A_ERR,x+(w-32)/2,y,32,TK_ICON,TK_RAISED);y+=46;
    if(y+UI_FONT_H>bottom)return;
    kit_text_center(B.title,x+16,y,w-32,TK_TEXT,1);y+=UI_FONT_H+10;
    int lx=x+(w-tw)/2;
    for(const char *p=m;*p&&y+UI_FONT_H<=bottom;y+=20){
        int n=wrap_next(p,tw,(int)sizeof line);
        if(!n)break;
        for(int i=0;i<n;i++)line[i]=p[i];
        line[n]=0;kit_text_center(line,lx,y,tw,TK_TEXT2,0);
        p+=n;while(*p==' ')p++;
    }
    y+=10;
    if(y+UI_FONT_H<=bottom)kit_text_center("Use Back to return, or enter another address.",x+16,y,w-32,TK_TEXT2,0);
}
static void status_draw(int x,int y,int w,int h) {
    int sy=y+h-status_h();
    if(MODERN){
        draw_hline(x,sy,w,TK_LINE_SUBTLE);
        kit_fill_bottom(x,sy+1,w,status_h()-1,ui_theme.r_window>1?ui_theme.r_window-1:0,ui_theme.statusbar);
    }else kit_statusbar(x,sy,w,status_h());
    int ty=kit_ty(sy+1,status_h()-1),pad=MODERN?12:8,right=x+w-pad;
    /* A running download stays visible at the right while reading. */
    if(own_download_active()&&!B.download_visible){
        char text[40]="Downloading ",digits[12];number(digits,B.download_received);append(text,sizeof text,digits);append(text,sizeof text," bytes");
        int tw=kit_text_w(text,0);
        if(tw+16+6<w/2){
            kit_text(text,right-tw,ty,TK_TEXT2,tw,0);right-=tw+6+16;
            icon_draw(ICON_DOWNLOAD,right,sy+1+(status_h()-1-ICON_SIZE)/2,TK_ACCENT);right-=12;
        }
    }
    int lx=x+pad;
    if(B.loading){
        icon_draw(ICON_RELOAD,lx,sy+1+(status_h()-1-ICON_SIZE)/2,TK_ACCENT);lx+=ICON_SIZE+6;
    }else if(B.error_page){
        ui_icon_draw(UI_ICON_ERR,lx,sy+1+(status_h()-1-16)/2,16,TK_ICON,ui_theme.statusbar);lx+=22;
    }
    kit_text(B.status,lx,ty,TK_TEXT2,right-lx,0);
}
void browser_draw(int x,int y,int w,int h) {
    browser_init();if(w<BROWSER_MIN_W||h<BROWSER_MIN_H)return;geometry(w,h);
    uint8_t paper=TK_RAISED,ink=TK_TEXT,link_color=TK_ACCENT_700;
    int tool_h=tool_height(w);ToolLayout L;tool_layout(x,y,w,&L);
    draw_rect(x,y,w,h,paper);
    kit_toolbar(x,y,w,tool_h);
    for(int i=0;i<BT_COUNT;i++)tool_draw(&L.b[i],i);
    for(int i=0;i<L.dividers;i++)kit_vseparator(L.divider[i],L.b[BT_BACK].y+5,tool_size()-10);
    address_draw(&L.address);
    int cy=content_top(y,w),content_right=x+w-PAD-SB;
    if(B.download_visible){
        download_summary();download_reflow(w-2*PAD-SB);
        for(int row=0;row<B.rows;row++){int index=B.download_scroll+row;if(index>=B.download_line_count)break;BrowserLine line=B.download_lines[index];int px=x+PAD;
            for(unsigned p=line.start;p<(unsigned)line.start+line.length;p++){
                int step=ui_advance(B.download_text[p]);if(px+step>content_right)break;
                draw_char(B.download_text[p],px,cy+row*ROW_H,ink);px+=step;
            }}
    } else if(B.error_page) {
        error_draw(x,cy,w-SB,content_bottom(y,h));
    } else for(int row=0;row<B.rows;row++) {
        int index=B.scroll+row;if(index>=B.line_count)break;BrowserLine line=B.lines[index];int px=x+PAD,py=cy+row*ROW_H;
        unsigned end=(unsigned)line.start+line.length;
        for(unsigned p=line.start;p<end;p++) {
            int step=advance(p);if(px+step>content_right)break;
            unsigned char style=B.style[p],link=B.link[p];uint8_t color=link?link_color:ink;
            if(link&&link==B.focused_link)draw_rect(px,py-1,step,UI_FONT_H+2,TK_ACCENT_100);
            if(style&STYLE_PRE)draw_edit_char(B.text[p],px,py,color);
            else if(style&STYLE_BOLD){char c[2]={B.text[p],0};draw_string_bold(c,px,py,color);}
            else draw_char(B.text[p],px,py,color);
            if(link)draw_hline(px,py+UI_FONT_H,step,color);
            px+=step;
        }
    }
    int line_count=B.download_visible?B.download_line_count:B.line_count,scroll=B.download_visible?B.download_scroll:B.scroll;
    int sy=y+tool_h,sh=y+h-status_h()-sy;
    if(line_count>B.rows&&!(B.error_page&&!B.download_visible))kit_vscroll(x+w-SB,sy,sh,scroll,B.rows,line_count,0);
    status_draw(x,y,w,h);
}
int browser_click(int x,int y,int w,int h,int mx,int my) {
    browser_init();if(w<BROWSER_MIN_W||h<BROWSER_MIN_H)return 0;geometry(w,h);
    ToolLayout L;tool_layout(x,y,w,&L);
    for(int i=0;i<BT_COUNT;i++)if(hit(mx,my,L.b[i].x,L.b[i].y,L.b[i].w,L.b[i].h)) {
        if(!tool_enabled(i))return 0;
        switch(i){
        case BT_BACK:return history_step(-1);
        case BT_FORWARD:return history_step(1);
        case BT_RELOAD:return reload();
        case BT_STOP:return stop();
        case BT_HOME:return open_internal("about:home",1);
        case BT_SAVE:return save_page();
        case BT_GO:return open_internal(B.address,1);
        case BT_DOWNLOAD:return start_download();
        case BT_CANCEL:return cancel_download();
        case BT_PICK:B.pick_link=!B.pick_link;B.download_visible=0;B.focus=0;set_status(B.pick_link?"Click a link to select it, then Download | Escape exits picking":"Link picking off | Click a link to open it");return 1;
        default:B.download_visible=!B.download_visible;B.pick_link=0;B.focus=0;download_summary();return 1;
        }
    }
    if(hit(mx,my,L.address.x,L.address.y,L.address.w,L.address.h)) {
        B.focus=1;B.selected=0;fit_address(L.address.w);int px=L.address.x+address_inset();B.cursor=B.address_start;
        while(B.address[B.cursor]&&px+ui_advance(B.address[B.cursor])/2<mx)px+=ui_advance(B.address[B.cursor++]);
        return 1;
    }
    /* The scrollbar runs between the toolbar and the status bar. */
    int cy=content_top(y,w),sy=y+tool_height(w),sh=y+h-status_h()-sy;
    if(B.download_visible){download_reflow(w-2*PAD-SB);if(hit(mx,my,x+w-SB,sy,SB,sh)&&B.download_line_count>B.rows){B.download_scroll=(my-sy)*B.download_line_count/sh-B.rows/2;download_reflow(w-2*PAD-SB);return 1;}return 0;}
    if(hit(mx,my,x+w-SB,sy,SB,sh)&&B.line_count>B.rows&&!B.error_page) {
        int old=B.scroll;B.scroll=(my-sy)*B.line_count/sh-B.rows/2;clamp_scroll();B.focus=0;return old!=B.scroll;
    }
    if(hit(mx,my,x+PAD,cy,w-2*PAD-SB,B.rows*ROW_H)) {
        B.focus=0;B.selected=0;int row=B.scroll+(my-cy)/ROW_H;
        if(row<B.line_count&&!B.error_page){BrowserLine line=B.lines[row];int px=x+PAD;
            for(unsigned p=line.start;p<(unsigned)line.start+line.length;p++){int step=advance(p);if(mx>=px&&mx<px+step&&B.link[p]){if(B.pick_link){B.focused_link=B.link[p];B.pick_link=0;set_status("Selected link | Download or Ctrl+D saves it");return 1;}return open_internal(B.links[B.link[p]-1],1);}px+=step;}}
        B.focused_link=0;return 1;
    }
    return 0;
}
