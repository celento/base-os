/* Normal browser flows over an actual QEMU RTL8139 connection. */
#include "app.h"
#include "browser.h"
#include "fs.h"
#include "net.h"
#include "platform.h"
#ifndef HTTP_PORT
#define HTTP_PORT "18080"
#endif
#define ORIGIN "http://10.0.2.2:" HTTP_PORT
static void check(int ok,const char *message){if(!ok){platform_log("BROWSER-FAIL ");platform_log(message);platform_log("\n");panic(message);}}
static int equals(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return !*a&&!*b;}
static int contains(const char *a,const char *b){for(;*a;a++){const char *x=a,*y=b;while(*x&&*x==*y){x++;y++;}if(!*y)return 1;}return 0;}
static void present(void){draw_rect(0,0,fb_w,fb_h,gfx_rgb(27,48,70));browser_draw(40,40,760,580);gfx_present();}
static void wait_page(const char *title){
    unsigned start=timer_ticks();
    while(browser_loading()&&timer_ticks()-start<TIMER_HZ*18){net_poll();if(browser_tick())present();platform_poll();if(browser_loading())__asm__ volatile("hlt");}
    check(!browser_loading(),"browser request completed");
    if(!equals(browser_title(),title)){platform_log("Expected title: ");platform_log(title);platform_log("; actual: ");platform_log(browser_title());platform_log("; status: ");platform_log(browser_status());platform_log("\n");}
    check(equals(browser_title(),title),"loaded title");present();
}
void browser_guest(void){
    platform_validate_memory();const BootInfo *bi=(const BootInfo *)BOOTINFO_ADDR;check(video_info_valid(bi),"graphics boot info");
    gfx_init((uint8_t *)FB_BASE,(uint8_t *)(uintptr_t)bi->lfb,bi->width,bi->height,bi->bpp,bi->pitch);
    app_accent=gfx_rgb(43,103,190);app_text=gfx_gray(30);app_text_dim=gfx_gray(90);app_chrome=gfx_gray(239);
    fs_init();net_init();check(net_status()->available,"RTL8139 available");browser_init();present();
    check(equals(browser_url(),"about:home"),"local home");check(!net_busy(),"opening browser does not start a request");
    browser_key(0x26,0,BROWSER_MOD_CTRL);const char *typed=ORIGIN "/index";while(*typed)browser_key(0,*typed++,0);browser_key(KEY_ENTER,0,0);
    check(browser_loading(),"asynchronous Go");wait_page("BaseOS live HTTP");check(contains(browser_status(),"HTTP 200"),"HTTP status visible");platform_log("BROWSER-HTTP-PASS\n");
    /* The first rendered row is an actual link from the network response. */
    browser_click(40,40,760,580,60,132);wait_page("Next live page");check(equals(browser_url(),ORIGIN "/next"),"click follows relative link");platform_log("BROWSER-LINK-PASS\n");
    browser_key(KEY_LEFT,0,BROWSER_MOD_ALT);wait_page("BaseOS live HTTP");browser_key(KEY_RIGHT,0,BROWSER_MOD_ALT);wait_page("Next live page");
    browser_key(0x3f,0,0);wait_page("Next live page");platform_log("BROWSER-HISTORY-PASS\n");
    browser_open(ORIGIN "/slow");unsigned wait_start=timer_ticks();
    while(net_http_result()->state!=NET_HTTP_RECEIVING&&timer_ticks()-wait_start<TIMER_HZ*2){net_poll();browser_tick();__asm__ volatile("hlt");}
    check(browser_loading()&&net_http_result()->state==NET_HTTP_RECEIVING,"request in progress before Stop");
    unsigned before=timer_ticks();browser_key(0x26,0,BROWSER_MOD_CTRL);browser_key(0,'x',0);present();
    check(timer_ticks()-before<TIMER_HZ,"input and draw do not wait for a response");browser_key(KEY_ESC,0,0);check(!browser_loading()&&!net_busy(),"Stop cancels request");
    browser_open(ORIGIN "/index");wait_page("BaseOS live HTTP");platform_log("BROWSER-STOP-RESPONSIVE-PASS\n");
    browser_open(ORIGIN "/redirect");wait_page("BaseOS live HTTP");check(equals(browser_url(),ORIGIN "/index"),"relative redirect address");
    browser_open(ORIGIN "/secure");wait_page("HTTPS is not supported");check(!net_busy(),"HTTPS redirect makes no insecure fallback");platform_log("BROWSER-REDIRECT-PASS\n");
    int file=fs_create(fs_root(),"browser-demo.html");check(file>=0,"local HTML file");const char *local="<title>Local browser file</title><h1>Local HTML</h1><p>Saved locally.</p>";
    check(fs_write(file,local,kstrlen(local))==kstrlen(local),"write local HTML");browser_open_file(file);check(equals(browser_title(),"Local browser file"),"local HTML displayed");platform_log("BROWSER-LOCAL-PASS\n");
    browser_open(ORIGIN "/index");wait_page("BaseOS live HTTP");browser_scroll(3);browser_draw(40,40,360,200);browser_scroll(-3);present();
    platform_log("BROWSER-QEMU-PASS\n");for(;;)__asm__ volatile("hlt");
}
