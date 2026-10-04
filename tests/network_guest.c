#include "net.h"
#include "platform.h"
#ifndef HTTP_PORT
#define HTTP_PORT "18080"
#endif
static char body[32768];
static int equals(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return !*a&&!*b;}
static void check(int condition,const char *message){if(!condition){platform_log("NETWORK-FAIL ");platform_log(message);platform_log(" ");platform_log(net_last_error());platform_log("\n");panic(message);}}
static void fetch(const char *path){char url[128];const char *prefix="http://10.0.2.2:" HTTP_PORT;unsigned p=0;while(*prefix)url[p++]=*prefix++;while(*path)url[p++]=*path++;url[p]=0;check(net_http_get(url,body,sizeof body)==0,"HTTP fetch");check(net_http_result()->status==200,"HTTP status");}
void network_guest(void){
    platform_validate_memory();net_init();check(net_status()->available,"NIC discovery");check(net_status()->link_up,"NIC link");
    unsigned latency;check(net_ping("10.0.2.2",&latency)==0,"gateway ping");platform_log("NETWORK-PING-PASS\n");
    fetch("/hello");check(equals(body,"BaseOS real HTTP works.\n"),"HTTP body");platform_log("NETWORK-HTTP-PASS\n");
    fetch("/chunked");check(equals(body,"Hello chunked world!\n"),"chunked body");platform_log("NETWORK-CHUNKED-PASS\n");
    fetch("/close");check(equals(body,"Close-delimited response.\n"),"close-delimited body");platform_log("NETWORK-CLOSE-PASS\n");
    fetch("/large");check(net_http_result()->length==20000,"multi-packet length");for(unsigned i=0;i<20000;i++)check(body[i]=='a'+i%26,"multi-packet body");platform_log("NETWORK-LARGE-PASS\n");
    char small[64];check(net_http_get("http://10.0.2.2:" HTTP_PORT "/large",small,sizeof small)==0,"truncated request");check(net_http_result()->truncated&&net_http_result()->length==63,"truncated result");platform_log("NETWORK-TRUNCATE-PASS\n");
    check(net_http_get("http://10.0.2.2:" HTTP_PORT "/broken",body,sizeof body)<0,"incomplete response rejection");platform_log("NETWORK-INCOMPLETE-PASS\n");
    check(net_http_start("http://10.0.2.2:" HTTP_PORT "/hello",body,sizeof body)==0,"async start");net_cancel();check(!net_busy()&&net_http_result()->state==NET_HTTP_ERROR,"cancel state");
    fetch("/hello");check(equals(body,"BaseOS real HTTP works.\n"),"fetch after cancel");platform_log("NETWORK-RECOVERY-PASS\n");
#ifdef TEST_PUBLIC_DNS
    uint32_t address;check(net_resolve("example.com",&address)==0&&address,"DNS lookup");char ip[16];net_format_ipv4(address,ip);platform_log("NETWORK-DNS-PASS ");platform_log(ip);platform_log("\n");
#endif
    platform_log("NETWORK-QEMU-PASS\n");for(;;)__asm__ volatile("hlt");
}
