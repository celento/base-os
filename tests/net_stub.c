/* The existing filesystem/Terminal host fixture does not emulate hardware.
 * Network protocol and lifecycle behavior have their own dedicated fixtures. */
#include "net.h"
static NetStatus status;
static NetHttpResult result;
const NetStatus *net_status(void){return &status;}
const NetHttpResult *net_http_result(void){return &result;}
const char *net_last_error(void){return "Network unavailable in this fixture";}
int net_busy(void){return 0;}
int net_ping(const char *host,unsigned *ms){(void)host;(void)ms;return -1;}
int net_resolve(const char *host,uint32_t *address){(void)host;(void)address;return -1;}
int net_http_get(const char *url,char *body,unsigned capacity){(void)url;(void)body;(void)capacity;return -1;}
void net_format_ipv4(uint32_t address,char out[16]){(void)address;out[0]='0';out[1]=0;}
