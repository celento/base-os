#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned char arena[65536] __attribute__((aligned(16)));
#define NET_MEMORY ((Network *)arena)
#define NET_HOST_TEST
#include "../src/net.c"
static uint32_t now;
static int available=1,link_up=1,sent;
uint32_t timer_ticks(void){return now;}
void platform_poll(void){now++;}
void platform_log(const char *s){(void)s;}
int net_driver_init(uint8_t *rx,uint8_t *tx,uint8_t mac[6]){(void)rx;(void)tx;uint8_t address[]={0x52,0x54,0,0x12,0x34,0x56};memcpy(mac,address,6);return available;}
int net_driver_link(void){return available&&link_up;}
int net_driver_recv(uint8_t *frame,unsigned capacity){(void)frame;(void)capacity;return 0;}
int net_driver_send(const uint8_t *frame,unsigned size){assert(size>=14&&size<=1518);assert(frame[6]==0x52);sent++;return 1;}
static void reset(void){initialized=polling=0;now=0;sent=0;available=link_up=1;net_init();}
int main(void){
    char body[128],other[64];reset();assert(net_status()->available&&net_status()->link_up);
    assert(!net_http_start("http://10.0.2.2/hello",body,sizeof body));assert(net_busy()&&net_http_busy());
    unsigned id=net_http_result()->request_id;assert(id&&net_http_start("http://10.0.2.2/other",other,sizeof other)==-1&&net_http_result()->request_id==id);
    net_poll();assert(sent==1);net_cancel();assert(!net_busy()&&net_http_result()->state==NET_HTTP_ERROR&&!strcmp(net_last_error(),"Request cancelled"));
    assert(!net_http_start("http://10.0.2.2/again",body,sizeof body)&&net_http_result()->request_id!=id);
    link_up=0;net_poll();assert(!net_busy()&&strstr(net_last_error(),"link went down"));
    reset();assert(net_http_get("http://10.0.2.2/no-arp",body,sizeof body)==-1);assert(now>=15*TIMER_HZ&&now<=15*TIMER_HZ+1);assert(sent>=14&&sent<=16);assert(strstr(net_last_error(),"timed out"));
    reset();N.neighbors[0].valid=1;N.neighbors[0].address=IP_GATEWAY;N.neighbors[0].tick=now;memset(N.neighbors[0].mac,0x52,6);
    assert(net_http_get("http://10.0.2.2/no-tcp",body,sizeof body)==-1);assert(now>=6*TIMER_HZ&&now<7*TIMER_HZ);assert(strstr(net_last_error(),"TCP peer"));
    reset();uint32_t address=0;assert(net_resolve("10.0.2.2",&address)==0&&address==IP_GATEWAY&&!net_busy());
    reset();assert(net_ping("10.0.2.2",0)==-1&&strstr(net_last_error(),"timed out"));
    initialized=0;available=0;net_init();assert(!net_status()->available);assert(net_http_get("http://10.0.2.2/",body,sizeof body)==-1&&strstr(net_last_error(),"No RTL8139"));
    puts("network state: request ownership, cancellation, disconnect, ARP/TCP deadlines, offline handling passed");
}
