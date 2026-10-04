#include "net.h"
#include "platform.h"
static void lookup(const char *name){
    uint32_t ip=0;
    if(net_resolve(name,&ip)||ip!=0x0a000202){platform_log("NETWORK-DNS-FAIL ");platform_log(name);platform_log(" ");platform_log(net_last_error());platform_log("\n");panic("DNS fixture lookup");}
}
void network_dns_guest(void){
    platform_validate_memory();net_init();
    lookup("fixture.test");lookup("alias.fixture.test");lookup("fixture.test");
    platform_log("NETWORK-DNS-QEMU-PASS: real RTL8139 ARP UDP DNS A and CNAME\n");
    for(;;)__asm__ volatile("hlt");
}
