/* Deterministic feature tests using normal HTML and a cooperative HTTP fixture. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "layout.h"
#define main filesystem_fixture_main
#include "fs_host.c"
#undef main
static uint8_t test_mirror[0x100000];
#undef PRESENT_BASE
#define PRESENT_BASE ((uintptr_t)test_mirror)
#define GFX_HOST_TEST
#include "../src/gfx.c"
#define BROWSER_HOST_TEST
#include "../src/browser.c"

uint8_t app_accent=COLOR_BLUE,app_accent_dk=COLOR_NAVY,app_text=COLOR_BLACK;
uint8_t app_text_dim=COLOR_GRAY,app_chrome=COLOR_LTGRAY,app_chrome_dk=COLOR_GRAY;
static NetHttpResult result;
static int busy,starts_count,cancels,serial;
static char started_url[NET_URL_MAX];
static char *http_body;
static unsigned http_capacity;
int net_busy(void){return busy;}
int net_http_busy(void){return busy;}
const NetHttpResult *net_http_result(void){return &result;}
void net_cancel(void){busy=0;cancels++;result.state=NET_HTTP_ERROR;strcpy(result.error,"Request cancelled");}
int net_http_start(const char *url,char *body,unsigned cap){
    assert(!busy);starts_count++;strcpy(started_url,url);memset(&result,0,sizeof result);
    result.request_id=(unsigned)++serial;result.state=NET_HTTP_CONNECTING;
    busy=1;http_body=body;http_capacity=cap;body[0]=0;return 0;
}
static const char sample_html[]="<title>Local sample</title><h1>Local file</h1><p>Saved on the BaseOS disk.</p>";
static uint8_t back[1024*768],linear[1024*768*4];
static void reset_browser(void){download_cancel();reset();fs_empty_dir(0);int sample=fs_create(0,"sample.html");assert(sample==1);assert(fs_write(sample,sample_html,sizeof sample_html-1)==sizeof sample_html-1);browser_ready=0;busy=0;starts_count=0;cancels=0;serial=0;memset(&result,0,sizeof result);browser_init();}
static void complete(const char *html,const char *type){
    assert(busy);unsigned n=(unsigned)strlen(html);assert(n<http_capacity);memcpy(http_body,html,n+1);
    result.state=NET_HTTP_DONE;result.status=200;result.length=n;strcpy(result.content_type,type);busy=0;
    assert(browser_tick());assert(!browser_loading());
}
static const char example_html[]=
    "<!doctype html><html><head><title>BaseOS &amp; the web</title>"
    "<style>.hidden {color: red}</style><script>window.hidden = true;</script></head><body>"
    "<h1>A real HTTP page</h1><p>Readable &amp; clickable, with <strong>bold text</strong>.</p>"
    "<ul><li>First item</li><li><a href='../next?x=1&amp;y=2'>Next page</a></li></ul>"
    "<p><a href='#details'>Jump to details</a> | <a href='https://example.com/'>HTTPS limit</a></p>"
    "<h2 id='details'>Details</h2><pre>one   two\n  three</pre>"
    "<p>&#65; &#x42; &ldquo;quotes&rdquo; &mdash; nice.</p>"
    "<img src='photo.png' alt='A sample photo'><form><input name='test'></form></body></html>";
static void test_url_resolution(void){
    char out[NET_URL_MAX];
    assert(normalize(out," example.com ")&&!strcmp(out,"http://example.com/"));
    assert(normalize(out,"10.0.2.2:8000")&&!strcmp(out,"http://10.0.2.2:8000/"));
    assert(normalize(out,"localhost:8000/index")&&!strcmp(out,"http://localhost:8000/index"));
    assert(normalize(out,"HTTP://example.com/a/../b/./page?q=1#ok")&&!strcmp(out,"http://example.com/b/page?q=1#ok"));
    assert(resolve_url(out,"http://example.com/docs/index.html","../next?x=1&y=2")&&!strcmp(out,"http://example.com/next?x=1&y=2"));
    assert(resolve_url(out,"http://example.com/docs/index.html","/root")&&!strcmp(out,"http://example.com/root"));
    assert(resolve_url(out,"http://example.com/docs/index.html?old=1","?new=2")&&!strcmp(out,"http://example.com/docs/index.html?new=2"));
    assert(resolve_url(out,"http://example.com/docs/index.html?old=1","#details")&&!strcmp(out,"http://example.com/docs/index.html?old=1#details"));
    assert(resolve_url(out,"http://example.com/docs/index.html","//other.example/path")&&!strcmp(out,"http://other.example/path"));
    assert(resolve_url(out,"file:///docs/index.html","../sample.html")&&!strcmp(out,"file:///sample.html"));
}
static void test_document_and_async(void){
    reset_browser();assert(!starts_count);assert(strstr(B.text,"HTTP is unencrypted"));assert(B.link_count==3);
    browser_open("http://example.com/docs/index.html");assert(browser_loading()&&busy&&starts_count==1);
    assert(strstr(B.text,"HTTP is unencrypted")); /* old page stays readable while loading */
    assert(browser_tick());assert(!browser_tick());result.state=NET_HTTP_RECEIVING;result.length=128;assert(browser_tick());assert(strstr(browser_status(),"128 bytes"));
    complete(example_html,"text/html; charset=utf-8");
    assert(!strcmp(browser_title(),"BaseOS & the web"));assert(strstr(B.text,"Readable & clickable"));
    assert(!strstr(B.text,"window.hidden"));assert(!strstr(B.text,"color: red"));assert(!strstr(B.text,"doctype"));
    assert(strstr(B.text,"* First item"));assert(strstr(B.text,"one   two\n  three"));
    assert(strstr(B.text,"A B \"quotes\" - nice."));assert(strstr(B.text,"[A sample photo]"));assert(strstr(B.text,"Form controls are not supported"));
    assert(B.link_count==3);assert(!strcmp(B.links[0],"http://example.com/next?x=1&y=2"));
    unsigned heading=(unsigned)(strstr(B.text,"A real HTTP page")-B.text);assert(B.style[heading]&STYLE_HEADING);
    assert(strstr(browser_status(),"HTTP 200"));assert(sizeof B<0x40000);
    browser_draw(40,30,720,520);assert(B.line_count>10);
}
static void test_navigation(void){
    reset_browser();browser_open("http://example.com/first");complete("<h1>First</h1><a href='/second'>Second</a>","text/html");
    browser_draw(40,30,720,520);browser_key(KEY_TAB,0,0);assert(B.focused_link==1);browser_key(KEY_ENTER,0,0);
    assert(!strcmp(started_url,"http://example.com/second"));complete("<title>Second</title><p>Second page</p>","text/html");
    assert(B.history_count==3);browser_key(KEY_LEFT,0,BROWSER_MOD_ALT);assert(!strcmp(started_url,"http://example.com/first"));
    complete("<title>First</title><p>First page</p>","text/html");browser_key(KEY_RIGHT,0,BROWSER_MOD_ALT);assert(!strcmp(started_url,"http://example.com/second"));
    complete("<title>Second</title><p>Second page</p>","text/html");
    browser_key(0x13,0,BROWSER_MOD_CTRL);assert(browser_loading());int before=cancels;
    browser_key(0x3f,0,0);assert(cancels==before+1&&browser_loading());browser_key(KEY_ESC,0,0);assert(!browser_loading()&&!busy);assert(strstr(browser_status(),"Stopped"));
    browser_open("https://example.com/");assert(!browser_loading()&&!busy);assert(strstr(B.text,"TLS / HTTPS is not available"));
    browser_key(KEY_LEFT,0,BROWSER_MOD_ALT);complete("<p>Again</p>","text/html");browser_open("http://example.com/third");complete("Third","text/plain");assert(B.history_pos==B.history_count-1);assert(!button_enabled(1));
    browser_open("http://example.com/plain");complete("<p>&amp; remains literal</p>","text/plain");assert(!strcmp(B.text,"<p>&amp; remains literal</p>"));
    browser_open_file(1);assert(!strcmp(browser_url(),"file:///sample.html"));assert(!strcmp(browser_title(),"Local sample"));
}
static void test_address_and_clicks(void){
    reset_browser();browser_draw(40,30,360,200);browser_key(0x26,0,BROWSER_MOD_CTRL);
    const char *url="http://example.com/a";while(*url)browser_key(0,*url++,0);assert(!strcmp(B.address,"http://example.com/a"));
    browser_key(KEY_LEFT,0,0);browser_key(0,'b',0);assert(!strcmp(B.address,"http://example.com/ba"));browser_key(0x53,0,0);assert(!strcmp(B.address,"http://example.com/b"));
    browser_key(KEY_ENTER,0,0);complete("<a href='/next'>Next link</a><p>Text below</p>","text/html");
    browser_draw(40,30,360,200);browser_click(40,30,360,200,40+PAD+3,30+TOOL_H+8+5);
    assert(!strcmp(started_url,"http://example.com/next"));complete("Next page","text/plain");
    browser_click(40,30,360,200,button_x(40,0)+10,30+15);assert(!strcmp(started_url,"http://example.com/b"));complete("Returned","text/plain");
    browser_click(40,30,360,200,address_left(40)+9,30+49);assert(B.focus);browser_key(0x1e,0,BROWSER_MOD_CTRL);browser_key(KEY_BACKSPACE,0,0);assert(!B.address[0]);
    url="about:home";while(*url)browser_key(0,*url++,0);browser_click(40,30,360,200,40+360-30,30+50);assert(!strcmp(browser_url(),"about:home"));
}
static void test_redirects_and_ownership(void){
    reset_browser();browser_open("http://example.com/start");result.state=NET_HTTP_DONE;result.status=302;strcpy(result.location,"/finish");busy=0;
    browser_tick();assert(browser_loading());assert(!strcmp(started_url,"http://example.com/finish"));complete("Redirected","text/plain");assert(!strcmp(B.text,"Redirected"));
    browser_open("http://example.com/start");result.state=NET_HTTP_DONE;result.status=301;strcpy(result.location,"https://example.com/secure");busy=0;
    browser_tick();assert(!browser_loading()&&!busy);assert(strstr(B.text,"server redirected to HTTPS"));
    browser_open("http://example.com/request");int old=cancels;result.request_id++;busy=1;browser_close();assert(cancels==old&&busy);
    int pos=B.history_pos;browser_open("http://example.com/blocked");assert(B.history_pos==pos);assert(strstr(browser_status(),"another app"));
    busy=0;browser_open("http://example.com/request");result.request_id++;browser_tick();assert(!browser_loading());assert(strstr(browser_status(),"replaced"));
    busy=0;browser_open("http://example.com/photo");complete("PNG bytes","image/png");assert(strstr(B.text,"Content type not supported"));
}
static void test_scroll_reflow_and_bounds(void){
    reset_browser();static char page[22000];strcpy(page,"<title>Long page</title><h1>Long page</h1>");
    for(int i=0;i<120;i++){char line[130];snprintf(line,sizeof line,"<p id='row%d'>Paragraph %d has readable words and <a href='/next'>a clickable link</a>.</p>",i,i);strcat(page,line);}
    browser_open("http://example.com/long");complete(page,"text/html");
    browser_draw(40,30,720,520);int wide=B.line_count;browser_scroll(12);assert(B.scroll==12);browser_key(0x51,0,0);assert(B.scroll>12);
    browser_draw(40,30,360,200);assert(B.line_count>wide);browser_key(0x4f,0,0);assert(B.scroll==B.line_count-B.rows);browser_key(0x47,0,0);assert(B.scroll==0);
    int old_starts=starts_count;browser_open("http://example.com/long#row20");assert(starts_count==old_starts);assert(B.scroll>0);
    /* All pixels, including at the supported minimum, stay in the client area. */
    int sizes[][2]={{360,200},{520,350},{720,520},{960,680}};
    for(unsigned k=0;k<sizeof sizes/sizeof sizes[0];k++){
        memset(back,0x55,sizeof back);browser_draw(20,20,sizes[k][0],sizes[k][1]);
        for(int yy=0;yy<768;yy++)for(int xx=0;xx<1024;xx++)if(xx<20||xx>=20+sizes[k][0]||yy<20||yy>=20+sizes[k][1])assert(back[yy*1024+xx]==0x55);
    }
}
static void test_save_original_pages(void){
    reset_browser();assert(!browser_can_save());assert(browser_save_page(0,"/home.html")==-1);
    browser_open("http://example.com/article");assert(!browser_can_save());assert(browser_save_page(0,"/pending.html")==-1);
    complete(example_html,"text/html");assert(browser_can_save());
    int id=browser_save_page(0,"/original.html");assert(id>0);
    assert(fs_size(id)==(int)sizeof example_html-1&&!memcmp(fs_data(id),example_html,sizeof example_html-1));
    assert(strstr(fs_data(id),"<script>")); /* Original source, never the parsed display. */
    assert(browser_save_page(0,"/original.html")==-1&&!memcmp(fs_data(id),example_html,sizeof example_html-1));
    browser_key(0x1f,0,BROWSER_MOD_CTRL);int first=fs_resolve(0,"/Downloads/page.html");assert(first>0);
    browser_draw(40,30,360,200);browser_click(40,30,360,200,button_x(40,5)+10,30+15);
    int second=fs_resolve(0,"/Downloads/page-2.html");assert(second>0&&second!=first);
    assert(fs_size(first)==fs_size(second)&&!memcmp(fs_data(first),fs_data(second),fs_size(first)));
    assert(fs_sync()==0);remount();assert(fs_resolve(0,"/Downloads/page.html")>=0);
    browser_open("http://example.com/large");result.truncated=1;complete("Partial body","text/plain");assert(!browser_can_save());
    int before=fs_node_count();assert(browser_save_page(0,"/partial.txt")==-1&&fs_node_count()==before);
    browser_open("http://example.com/broken");result.state=NET_HTTP_ERROR;strcpy(result.error,"Connection closed early");busy=0;browser_tick();assert(!browser_can_save());
    browser_open("http://example.com/next");browser_key(KEY_ESC,0,0);assert(!browser_can_save());
    browser_open("http://example.com/plain");complete("a < b & original text","text/plain");browser_key(0x1f,0,BROWSER_MOD_CTRL);
    int plain=fs_resolve(0,"/Downloads/page.txt");assert(plain>0&&!strcmp(fs_data(plain),"a < b & original text"));
    /* A complete response may be saved even if only rendering was bounded. */
    browser_open("http://example.com/short");complete("Complete body","text/plain");B.truncated=1;assert(browser_can_save());
    before=fs_node_count();while(fs_node_count()<fs_node_limit()){char name[24];snprintf(name,sizeof name,"file%d",fs_node_count());assert(fs_create(0,name)>0);}
    assert(browser_save_page(0,"/no-slots.txt")==-1&&fs_node_count()==fs_node_limit());assert(before<fs_node_limit());
}
static void edit_download_address(const char *url){browser_key(0x26,0,BROWSER_MOD_CTRL);while(*url)browser_key(0,*url++,0);}
static void start_address_download(const char *url){edit_download_address(url);assert(browser_key(0x20,0,BROWSER_MOD_CTRL));}
static void reset_browser_data_volume(void){
    reset_browser();memset(data_disk,0,sizeof data_disk);unsigned *m=(unsigned *)data_disk;
    m[0]=DATA_MARKER_MAGIC;m[1]=DATA_MARKER_VERSION;m[2]=DATA_DISK_SECTORS;
    m[3]=DATA_SLOT_SECTORS;m[4]=DATA_FIRST_LBA;m[5]=DATA_SECOND_LBA;m[6]=crc32(m,24);
    data_present=1;fs_init();assert(fs_load_disk()==FS_LOAD_BLANK);fs_empty_dir(0);
}
static void finish_binary(unsigned count){
    assert(busy&&count<http_capacity);for(unsigned i=0;i<count;i++)http_body[i]=(char)((i*37+91)&255);
    result.state=NET_HTTP_DONE;result.status=200;result.length=count;busy=0;
    assert(download_tick());assert(browser_tick());
}
static void test_download_names(void){
    reset_browser();
    const char *urls[]={"http://example.com/?file=secret.wav#track", "http://example.com/folder/", "http://example.com/a/../music.mp3?name=evil.bin#other", "http://example.com/this-is-a-very-long-filename.MPEG?q=x", "http://example.com/%2e%2e%2fphoto.png", "http://example.com/.wav"};
    const char *names[]={"download", "download", "music.mp3", "this-is-a-very-lon.mpeg", "_2e_2e_2fphoto.png", "download.wav"};
    for(unsigned i=0;i<sizeof urls/sizeof urls[0];i++){
        start_address_download(urls[i]);assert(download_active());char path[64];snprintf(path,sizeof path,"/Downloads/%s",names[i]);
        if(strcmp(download_status()->path,path))fprintf(stderr,"name %u: %s expected %s\n",i,download_status()->path,path);
        assert(!strcmp(download_status()->path,path));assert(!strcmp(B.download_path,path));assert(B.download_id==result.request_id);
        assert(cancel_download());assert(!busy&&download_status()->state==DOWNLOAD_CANCELLED);
    }
    start_address_download("http://example.com/music.mp3?other=foo.wav#fragment");finish_binary(123);
    int first=fs_resolve(0,"/Downloads/music.mp3");assert(first>0);start_address_download("http://example.com/music.mp3?other=x");
    assert(!strcmp(download_status()->path,"/Downloads/music-2.mp3"));finish_binary(234);assert(fs_size(first)==123);
    int count=starts_count;
    const char *unsupported[]={"https://example.com/secret", "ftp://example.com/file", "file:///sample.html", "javascript:alert(1)", "http://user:password@example.com/a"};
    for(unsigned i=0;i<sizeof unsupported/sizeof unsupported[0];i++){start_address_download(unsupported[i]);assert(starts_count==count&&!busy);assert(B.download_notice[0]);}
    assert(!strstr(B.download_text,"password"));
    browser_draw(40,30,360,200);assert(!B.focus);browser_key(0x4f,0,0);
    assert(B.download_scroll==B.download_line_count-B.rows&&B.download_scroll>0);
}
static void test_browser_binary_download(void){
    reset_browser_data_volume();browser_open("http://example.com/article");complete(example_html,"text/html");
    int history=B.history_count;start_address_download("http://example.com/audio.wav?source=browser#track");
    assert(B.history_count==history&&browser_can_save()&&!browser_loading());assert(B.download_visible);
    assert(http_body!=B.body&&http_capacity>sizeof B.body);assert(strstr(B.text,"Readable & clickable"));
    result.state=NET_HTTP_RECEIVING;result.length=47000;assert(download_tick());assert(browser_tick());
    assert(strstr(B.download_text,"47000 bytes"));assert(fs_resolve(0,"/Downloads/audio.wav")<0);
    browser_close();assert(download_active()&&busy);finish_binary(90001);
    int file=fs_resolve(0,"/Downloads/audio.wav");assert(file>0&&fs_size(file)==90001);
    for(unsigned i=0;i<90001;i++)assert((unsigned char)fs_data(file)[i]==((i*37+91)&255));
    assert(strstr(B.download_text,"RAM")&&strstr(B.download_text,"pending"));
    assert(fs_sync()==0);assert(browser_tick());assert(strstr(B.download_text,"disk is synchronized"));
    assert(browser_save_page(0,"/original.html")>0);assert(!memcmp(fs_data(fs_resolve(0,"/original.html")),example_html,sizeof example_html-1));
    start_address_download("http://example.com/cancel.bin");result.state=NET_HTTP_RECEIVING;result.length=5000;download_tick();browser_tick();
    assert(browser_key(KEY_ESC,0,0));assert(!busy&&download_status()->state==DOWNLOAD_CANCELLED);assert(strstr(B.download_text,"No file was saved"));
    assert(fs_resolve(0,"/Downloads/cancel.bin")<0);
    start_address_download("http://example.com/fail.bin");strcpy(result.error,"Server closed the incomplete response");result.state=NET_HTTP_ERROR;busy=0;
    download_tick();browser_tick();assert(strstr(B.download_text,"Server closed the incomplete response"));assert(fs_resolve(0,"/Downloads/fail.bin")<0);
}
static void test_browser_download_ownership(void){
    reset_browser();assert(!download_start(0,"http://example.com/terminal.bin","/terminal.bin"));unsigned terminal=result.request_id;int before=cancels;
    start_address_download("http://example.com/browser.bin");assert(busy&&result.request_id==terminal&&cancels==before);assert(!download_button_enabled(1));
    assert(strstr(B.download_notice,"another app"));browser_key(KEY_ESC,0,0);browser_close();assert(busy&&cancels==before);
    download_cancel();
    browser_open("http://example.com/loading");unsigned page=result.request_id;start_address_download("http://example.com/browser.bin");
    assert(busy&&result.request_id==page&&strstr(B.download_notice,"Use Stop"));browser_key(KEY_ESC,0,0);assert(!busy);
    start_address_download("http://example.com/owned.bin");unsigned owned=B.download_id;
    start_address_download("http://example.com/new.bin");assert(B.download_id==owned&&result.request_id==owned&&strstr(B.download_notice,"already running"));
    download_cancel();assert(!download_start(0,"http://example.com/terminal.bin","/terminal.bin"));before=cancels;
    assert(browser_tick());assert(!download_button_enabled(1));assert(strstr(B.download_text,"replaced"));assert(!strstr(B.download_text,"/terminal.bin"));
    cancel_download();browser_close();assert(busy&&cancels==before);download_cancel();
}
static void test_browser_download_mouse_and_bounds(void){
    reset_browser();browser_open("http://example.com/index");complete("<a href='/sound.wav?x=1#track'>Download sound</a><p>Read while downloading</p>","text/html");
    browser_draw(40,30,360,200);int before=starts_count;
    browser_click(40,30,360,200,download_button_x(40,2)+5,30+85);assert(B.pick_link);
    browser_click(40,30,360,200,40+PAD+3,30+TOOL_H+8+5);assert(B.focused_link==1&&!B.pick_link&&starts_count==before);
    browser_click(40,30,360,200,download_button_x(40,0)+5,30+85);assert(download_active()&&starts_count==before+1);
    assert(!strcmp(started_url,"http://example.com/sound.wav?x=1#track"));assert(!strcmp(B.download_path,"/Downloads/sound.wav"));
    for(int i=0;i<4;i++)assert(download_button_x(40,i)+download_button_widths[i]<=400);
    int sizes[][2]={{360,200},{520,350},{720,520},{960,680}};
    for(unsigned k=0;k<sizeof sizes/sizeof sizes[0];k++){
        memset(back,0x55,sizeof back);browser_draw(20,20,sizes[k][0],sizes[k][1]);
        for(int yy=0;yy<768;yy++)for(int xx=0;xx<1024;xx++)if(xx<20||xx>=20+sizes[k][0]||yy<20||yy>=20+sizes[k][1])assert(back[yy*1024+xx]==0x55);
    }
    browser_draw(40,30,360,200);assert(B.download_line_count>B.rows);browser_scroll(2);assert(B.download_scroll==2);
    browser_key(0x4f,0,0);assert(B.download_scroll==B.download_line_count-B.rows);browser_key(0x47,0,0);assert(!B.download_scroll);
    browser_click(40,30,360,200,download_button_x(40,3)+5,30+85);assert(!B.download_visible&&download_active());
    browser_click(40,30,360,200,download_button_x(40,3)+5,30+85);assert(B.download_visible);
    browser_click(40,30,360,200,download_button_x(40,1)+5,30+85);assert(!busy&&download_status()->state==DOWNLOAD_CANCELLED);
    browser_key(KEY_TAB,0,0);assert(B.focused_link==1&&!B.download_visible);browser_key(0x20,0,BROWSER_MOD_CTRL);assert(download_active());download_cancel();browser_tick();
}
static void write_preview(const char *path){
    reset_browser();browser_open("http://10.0.2.2:8000/docs/index.html");complete(example_html,"text/html");
    memset(back,0x55,sizeof back);browser_draw(32,28,760,600);
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n1024 768\n255\n");
    for(unsigned i=0;i<sizeof back;i++){uint32_t c=pal32[back[i]];fputc((c>>16)&255,f);fputc((c>>8)&255,f);fputc(c&255,f);}fclose(f);
}
int main(int argc,char **argv){
    gfx_init(back,linear,1024,768,32,4096);
    test_url_resolution();test_document_and_async();test_navigation();test_address_and_clicks();test_redirects_and_ownership();test_scroll_reflow_and_bounds();test_save_original_pages();test_download_names();test_browser_binary_download();test_browser_download_ownership();test_browser_download_mouse_and_bounds();
    if(argc>1)write_preview(argv[1]);
    puts("browser: HTTP lifecycle, HTML rendering, links, navigation, address editing, redirects, local files, scrolling and client-area bounds passed");return 0;
}
