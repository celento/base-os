/* BaseOS QEMU networking vertical. One cooperative client operation at a time.
 * Deliberately not a general socket API or a production TCP/IP stack. */
#include "net.h"
#include "net_driver.h"
#include "net_wire.h"
#include "platform.h"
#ifndef NET_BASE
#define NET_BASE 0x1600000
#define NET_CAPACITY 0x10000
#endif
#define IP_ADDRESS 0x0a00020fu
#define IP_GATEWAY 0x0a000202u
#define IP_DNS     0x0a000203u
#define IP_MASK    0xffffff00u
#define DEADLINE (15u*TIMER_HZ)
#define RETRY_TICKS TIMER_HZ
#define TCP_WINDOW 4096u
#define TCP_FIN 1u
#define TCP_SYN 2u
#define TCP_RST 4u
#define TCP_PSH 8u
#define TCP_ACK 16u

enum { JOB_NONE, JOB_HTTP, JOB_DNS, JOB_PING };
enum { PHASE_NONE, PHASE_DNS_ARP, PHASE_DNS, PHASE_TCP_ARP,
       PHASE_SYN, PHASE_TCP, PHASE_PING_ARP, PHASE_PING };
typedef struct {uint32_t address, tick;uint8_t mac[6];int valid;} Neighbor;
typedef struct {
    uint8_t rx_dma[NET_RX_MEMORY] __attribute__((aligned(16)));
    uint8_t tx_dma[NET_TX_MEMORY] __attribute__((aligned(16)));
    uint8_t received[NET_FRAME_MAX], outgoing[NET_FRAME_MAX];
    NetStatus status;
    Neighbor neighbors[4];unsigned neighbor_next;
    NetHttpResult http;
    NetHttpParser parser;
    char host[NET_HOST_MAX],path[NET_URL_MAX],request[640];
    unsigned request_length,server_port,local_port,dns_id,ping_id,http_serial;
    uint32_t server,started,last_send,ping_started,ping_elapsed;
    uint32_t seq,next_ack,request_seq,request_ack,closing_tick;
    unsigned retries,ip_id;
    int job,phase,failed,tcp_open,closing,fin_seen,request_sent;
    char error[80];
    uint8_t peer_mac[6];
} Network;
#ifndef NET_MEMORY
#define NET_MEMORY ((Network *)(uintptr_t)NET_BASE)
#endif
static Network *const state=NET_MEMORY;
#define N (*state)
static int initialized,polling;
_Static_assert(sizeof(Network)<=NET_CAPACITY,"network arena overflow");
static void bytes(void *v,const void *w,unsigned count){uint8_t *d=v;const uint8_t *s=w;while(count--)*d++=*s++;}
static void zero(void *v,unsigned count){uint8_t *d=v;while(count--)*d++=0;}
static int same(const uint8_t *a,const uint8_t *b,unsigned n){while(n--)if(*a++!=*b++)return 0;return 1;}
static void string(char *d,unsigned cap,const char *s){unsigned n=0;if(!cap)return;while(s[n]&&n+1<cap){d[n]=s[n];n++;}d[n]=0;}
static uint32_t route(uint32_t dest){return (dest&IP_MASK)==(IP_ADDRESS&IP_MASK)?dest:IP_GATEWAY;}
static int send_frame(unsigned n){
    int rc=net_driver_send(N.outgoing,n);
    if(rc>0)N.status.tx_packets++;else if(rc<0)N.status.tx_errors++;
    return rc;
}
static void ethernet(const uint8_t mac[6],unsigned type){bytes(N.outgoing,mac,6);bytes(N.outgoing+6,N.status.mac,6);net_write16(N.outgoing+12,type);}
static Neighbor *neighbor(uint32_t address){
    uint32_t now=timer_ticks();
    for(unsigned i=0;i<4;i++)if(N.neighbors[i].valid&&N.neighbors[i].address==address&&
       (uint32_t)(now-N.neighbors[i].tick)<60u*TIMER_HZ)return &N.neighbors[i];
    return 0;
}
static void arp_request(uint32_t address){
    static const uint8_t broadcast[6]={255,255,255,255,255,255};ethernet(broadcast,0x806);
    uint8_t *p=N.outgoing+14;zero(p,28);net_write16(p,1);net_write16(p+2,0x800);p[4]=6;p[5]=4;net_write16(p+6,1);
    bytes(p+8,N.status.mac,6);net_write32(p+14,IP_ADDRESS);net_write32(p+24,address);send_frame(42);
}
static void arp_receive(const uint8_t *p,unsigned size){
    if(size<28||net_read16(p)!=1||net_read16(p+2)!=0x800||p[4]!=6||p[5]!=4){N.status.dropped_packets++;return;}
    unsigned op=net_read16(p+6);uint32_t src=net_read32(p+14),dest=net_read32(p+24);
    if((op!=1&&op!=2)||!src||src==0xffffffffu||(src&IP_MASK)!=(IP_ADDRESS&IP_MASK)||
       (p[8]&1)||!same(p+8,N.received+6,6)){N.status.dropped_packets++;return;}
    if(dest!=IP_ADDRESS)return;
    if(op==2&&!same(p+18,N.status.mac,6))return;
    Neighbor *entry=neighbor(src);if(!entry)entry=&N.neighbors[N.neighbor_next++%4];
    entry->address=src;entry->tick=timer_ticks();entry->valid=1;bytes(entry->mac,p+8,6);
    if(op==1){
        ethernet(p+8,0x806);uint8_t *out=N.outgoing+14;bytes(out,p,28);net_write16(out+6,2);
        bytes(out+18,p+8,6);net_write32(out+24,src);bytes(out+8,N.status.mac,6);net_write32(out+14,IP_ADDRESS);send_frame(42);
    }
}
static uint8_t *ipv4(const uint8_t mac[6],uint32_t dest,unsigned protocol,unsigned size){
    ethernet(mac,0x800);uint8_t *p=N.outgoing+14;zero(p,20);p[0]=0x45;
    net_write16(p+2,20+size);net_write16(p+4,++N.ip_id);net_write16(p+6,0x4000);p[8]=64;p[9]=(uint8_t)protocol;
    net_write32(p+12,IP_ADDRESS);net_write32(p+16,dest);net_write16(p+10,net_checksum(p,20));return p+20;
}
static int tcp_send(unsigned flags,uint32_t seq,const uint8_t *data,unsigned size){
    unsigned header=(flags&TCP_SYN)?24:20;
    if(size>NET_MTU-20-header)return -1;
    uint8_t *p=ipv4(N.peer_mac,N.server,6,header+size);zero(p,header);
    net_write16(p,N.local_port);net_write16(p+2,N.server_port);net_write32(p+4,seq);net_write32(p+8,(flags&TCP_ACK)?N.next_ack:0);
    p[12]=(uint8_t)(header<<2);p[13]=(uint8_t)flags;net_write16(p+14,TCP_WINDOW);
    if(flags&TCP_SYN){p[20]=2;p[21]=4;net_write16(p+22,1460);}
    if(size)bytes(p+header,data,size);
    net_write16(p+16,net_transport_checksum(IP_ADDRESS,N.server,6,p,header+size));return send_frame(14+20+header+size);
}
static void reset_connection(void){
    if(N.tcp_open||N.closing)tcp_send(TCP_RST|TCP_ACK,N.seq,0,0);
    else if(N.phase==PHASE_SYN)tcp_send(TCP_RST,N.seq,0,0);
    N.tcp_open=N.closing=N.fin_seen=0;
}
static void job_error(const char *message){
    string(N.error,sizeof N.error,message?message:N.http.error);
    if(N.job==JOB_HTTP){N.http.state=NET_HTTP_ERROR;if(message)string(N.http.error,sizeof N.http.error,message);}
    N.failed=1;reset_connection();N.job=JOB_NONE;N.phase=PHASE_NONE;
}
static void http_done(void){
    N.http.state=NET_HTTP_DONE;N.job=JOB_NONE;N.phase=PHASE_NONE;
    if(N.http.truncated){reset_connection();return;}
    if(N.tcp_open){tcp_send(TCP_FIN|TCP_ACK,N.seq,0,0);N.seq++;N.closing=1;N.closing_tick=timer_ticks();N.tcp_open=0;}
}
static void dns_send(void){
    Neighbor *link=neighbor(IP_DNS);if(!link)return;
    uint8_t *p=ipv4(link->mac,IP_DNS,17,8); /* Rebuild lengths after writing query. */
    int n=net_dns_query(p+8,512,N.dns_id,N.host);if(n<0){job_error("Invalid DNS hostname");return;}
    /* ipv4() clears only the IP header, preserving the just-built payload. */
    p=ipv4(link->mac,IP_DNS,17,8+(unsigned)n);net_write16(p,N.local_port);net_write16(p+2,53);net_write16(p+4,8+(unsigned)n);net_write16(p+6,0);
    unsigned checksum=net_transport_checksum(IP_ADDRESS,IP_DNS,17,p,8+(unsigned)n);net_write16(p+6,checksum?checksum:65535);send_frame(42+(unsigned)n);
}
static void ping_send(void){
    uint8_t *p=ipv4(N.peer_mac,N.server,1,32);zero(p,32);p[0]=8;net_write16(p+4,N.ping_id);net_write16(p+6,1);
    for(unsigned i=8;i<32;i++)p[i]=(uint8_t)(i^0xa5);
    net_write16(p+2,net_checksum(p,32));send_frame(66);
}
static void after_resolve(void){
    if(!N.server||N.server==0xffffffffu||(N.server&0xf0000000u)==0xe0000000u){job_error("Invalid destination address");return;}
    if(N.job==JOB_DNS){N.job=JOB_NONE;N.phase=PHASE_NONE;return;}
    N.phase=N.job==JOB_HTTP?PHASE_TCP_ARP:PHASE_PING_ARP;N.retries=0;N.last_send=timer_ticks()-RETRY_TICKS;
    if(N.job==JOB_HTTP)N.http.state=NET_HTTP_CONNECTING;
}
static void udp_receive(uint32_t source,const uint8_t *p,unsigned length){
    if(length<8)return;
    unsigned size=net_read16(p+4);
    if(size<8||size>length||(net_read16(p+6)&&net_transport_checksum(source,IP_ADDRESS,17,p,size))){N.status.dropped_packets++;return;}
    if(N.phase!=PHASE_DNS||source!=IP_DNS||net_read16(p)!=53||net_read16(p+2)!=N.local_port)return;
    int answer=net_dns_answer(p+8,size-8,N.dns_id,N.host,&N.server);
    if(answer<0)job_error("DNS returned no usable IPv4 answer");else if(answer>0)after_resolve();
}
static void icmp_receive(uint32_t source,const uint8_t *p,unsigned size){
    if(size<8||net_checksum(p,size)){N.status.dropped_packets++;return;}
    if(p[0]==0&&p[1]==0&&N.phase==PHASE_PING&&source==N.server&&
       net_read16(p+4)==N.ping_id&&net_read16(p+6)==1){
        N.ping_elapsed=timer_ticks()-N.ping_started;N.job=JOB_NONE;N.phase=PHASE_NONE;return;
    }
    if(p[0]==8&&!p[1]){
        uint8_t *out=ipv4(N.received+6,source,1,size);bytes(out,p,size);out[0]=0;net_write16(out+2,0);net_write16(out+2,net_checksum(out,size));send_frame(34+size);
    }
}
static void tcp_receive(uint32_t source,const uint8_t *p,unsigned size){
    if(size<20)return;
    unsigned header=(p[12]>>4)*4u;
    if(header<20||header>size||net_transport_checksum(source,IP_ADDRESS,6,p,size)){N.status.dropped_packets++;return;}
    if((N.phase!=PHASE_SYN&&N.phase!=PHASE_TCP&&!N.closing)||source!=N.server||
       net_read16(p)!=N.server_port||net_read16(p+2)!=N.local_port)return;
    unsigned flags=p[13];uint32_t seq=net_read32(p+4),ack=net_read32(p+8);unsigned data_size=size-header;
    if(N.phase==PHASE_SYN){
        if((flags&TCP_RST)&&(flags&TCP_ACK)&&ack==N.seq){job_error("TCP connection refused");return;}
        if((flags&(TCP_SYN|TCP_ACK|TCP_RST))!=(TCP_SYN|TCP_ACK)||ack!=N.seq)return;
        N.next_ack=seq+1;N.phase=PHASE_TCP;N.tcp_open=1;N.retries=0;
        N.http.state=NET_HTTP_RECEIVING;tcp_send(TCP_ACK,N.seq,0,0);
        N.request_seq=N.seq;N.seq+=N.request_length;N.request_ack=N.request_seq;
        N.request_sent=tcp_send(TCP_ACK|TCP_PSH,N.request_seq,(const uint8_t *)N.request,N.request_length)>0;
        N.last_send=timer_ticks();return;
    }
    if(flags&TCP_RST){if(seq==N.next_ack){if(N.closing){N.closing=0;return;}job_error("TCP connection reset");}else tcp_send(TCP_ACK,N.seq,0,0);return;}
    if(flags&TCP_SYN){tcp_send(TCP_ACK,N.seq,0,0);return;}
    if(!(flags&TCP_ACK))return;
    /* Modulo arithmetic handles sequence wrap; never accept acknowledgments
     * of bytes we have not sent. Challenge invalid ACKs instead. */
    if((int32_t)(ack-N.seq)>0){tcp_send(TCP_ACK,N.seq,0,0);return;}
    if((int32_t)(ack-N.request_ack)>0)N.request_ack=ack;
    if(N.closing){
        if(seq==N.next_ack&&(flags&TCP_FIN)){N.next_ack+=data_size+1;N.fin_seen=1;tcp_send(TCP_ACK,N.seq,0,0);}
        else if(data_size)tcp_send(TCP_ACK,N.seq,0,0);
        if(N.fin_seen&&ack==N.seq)N.closing=0;
        return;
    }
    if(!data_size&&!(flags&TCP_FIN))return;
    int32_t delta=(int32_t)(seq-N.next_ack);
    if(delta>0){tcp_send(TCP_ACK,N.seq,0,0);return;} /* Retransmit gap via dup ACK. */
    unsigned skip=delta<0?(unsigned)(-(int64_t)delta):0;
    if(skip>data_size){tcp_send(TCP_ACK,N.seq,0,0);return;}
    unsigned new_size=data_size-skip;int rc=0;
    if(new_size){rc=net_http_parser_feed(&N.parser,p+header+skip,new_size);N.next_ack+=new_size;}
    if(flags&TCP_FIN){N.next_ack++;N.fin_seen=1;if(!rc)rc=net_http_parser_eof(&N.parser);}
    tcp_send(TCP_ACK,N.seq,0,0);
    if(rc<0)job_error(0);else if(rc>0)http_done();
}
static void receive(unsigned length){
    if(length<14){N.status.dropped_packets++;return;}
    static const uint8_t broadcast[6]={255,255,255,255,255,255};
    if(!same(N.received,N.status.mac,6)&&!same(N.received,broadcast,6))return;
    unsigned type=net_read16(N.received+12);const uint8_t *p=N.received+14;unsigned size=length-14;
    if(type==0x806){arp_receive(p,size);return;}if(type!=0x800)return;
    if(size<20||(p[0]>>4)!=4){N.status.dropped_packets++;return;}
    unsigned header=(p[0]&15)*4u,total=net_read16(p+2);
    if(header<20||header>size||total<header||total>size||net_checksum(p,header)||
       (net_read16(p+6)&0xbfff)||!p[8]||net_read32(p+16)!=IP_ADDRESS){N.status.dropped_packets++;return;}
    uint32_t source=net_read32(p+12);unsigned proto=p[9];size=total-header;p+=header;
    if(proto==1)icmp_receive(source,p,size);else if(proto==17)udp_receive(source,p,size);else if(proto==6)tcp_receive(source,p,size);
}
void net_init(void){
    if(initialized)return;
    zero(state,sizeof *state);initialized=1;
    N.status.address=IP_ADDRESS;N.status.gateway=IP_GATEWAY;N.status.dns=IP_DNS;
    N.status.available=net_driver_init(N.rx_dma,N.tx_dma,N.status.mac);N.status.link_up=net_driver_link();
    platform_log(N.status.available?"NET RTL8139 ready: 10.0.2.15/24\n":"NET no RTL8139 adapter (offline)\n");
}
const NetStatus *net_status(void){net_init();N.status.link_up=net_driver_link();return &N.status;}
const char *net_last_error(void){return initialized?N.error:"Network is not initialized";}
int net_busy(void){return initialized&&N.job!=JOB_NONE;}
int net_http_busy(void){return initialized&&N.job==JOB_HTTP;}
const NetHttpResult *net_http_result(void){net_init();return &N.http;}
void net_cancel(void){if(net_busy())job_error("Request cancelled");else if(initialized)reset_connection();}
static int begin(int job,const char *host){
    net_init();if(net_busy())return -1;reset_connection();N.job=job;N.failed=0;N.phase=PHASE_NONE;
    N.error[0]=0;N.started=timer_ticks();N.last_send=N.started-RETRY_TICKS;N.retries=0;N.server=0;
    N.local_port=49152u+((N.started+N.ip_id*37u)&16383u);N.dns_id=(N.started+N.ip_id*71u+0x4261u)&65535u;N.ping_id=N.dns_id;
    if(!N.status.available){job_error("No RTL8139 adapter; start QEMU with -nic user,model=rtl8139");return -1;}
    if(!net_driver_link()){job_error("Network link is down");return -1;}
    unsigned host_length=0;while(host[host_length]&&host_length<NET_HOST_MAX)host_length++;
    if(host_length>=NET_HOST_MAX){job_error("DNS hostname exceeds 127 characters");return -1;}
    if(host!=N.host)string(N.host,sizeof N.host,host);
    if(net_parse_ipv4(N.host,&N.server))after_resolve();
    else {uint8_t query[160];if(net_dns_query(query,sizeof query,N.dns_id,N.host)<0){job_error("Invalid DNS hostname");return -1;}
        N.phase=PHASE_DNS_ARP;if(job==JOB_HTTP)N.http.state=NET_HTTP_RESOLVING;}
    return N.failed?-1:0;
}
static void append(char *out,unsigned *position,const char *text){while(*text&&*position+1<sizeof N.request)out[(*position)++]=*text++;out[*position]=0;}
int net_http_start(const char *url,char *body,unsigned capacity){
    net_init();if(net_busy())return -1;reset_connection();zero(&N.http,sizeof N.http);N.http.request_id=++N.http_serial;
    if(!body||capacity<2||capacity>NET_HTTP_BODY_MAX){N.http.state=NET_HTTP_ERROR;string(N.http.error,sizeof N.http.error,"HTTP buffer must hold 2..32768 bytes");string(N.error,sizeof N.error,N.http.error);return -1;}
    body[0]=0;
    if(!net_parse_url(url,N.host,&N.server_port,N.path)){
        N.http.state=NET_HTTP_ERROR;string(N.http.error,sizeof N.http.error,"Use http://host[:port]/path; HTTPS/TLS is not supported");string(N.error,sizeof N.error,N.http.error);return -1;
    }
    net_http_parser_init(&N.parser,body,capacity,&N.http);unsigned p=0;N.request[0]=0;
    append(N.request,&p,"GET ");append(N.request,&p,N.path);append(N.request,&p," HTTP/1.1\r\nHost: ");append(N.request,&p,N.host);
    if(N.server_port!=80){char digits[6],reverse[5];unsigned n=N.server_port,c=0;do{reverse[c++]=(char)('0'+n%10);n/=10;}while(n);unsigned i=0;while(c)digits[i++]=reverse[--c];digits[i]=0;append(N.request,&p,":");append(N.request,&p,digits);}
    append(N.request,&p,"\r\nUser-Agent: BaseOS/0.1\r\nAccept: text/html, text/plain, */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");
    N.request_length=p;return begin(JOB_HTTP,N.host);
}
void net_poll(void){
    if(!initialized||!N.status.available||polling)return;
    polling=1;
    for(unsigned i=0;i<32;i++){
        int size=net_driver_recv(N.received,sizeof N.received);if(!size)break;
        if(size<0){N.status.rx_errors++;continue;}N.status.rx_packets++;receive((unsigned)size);
    }
    uint32_t now=timer_ticks();N.status.link_up=net_driver_link();
    if(N.closing&&(uint32_t)(now-N.closing_tick)>=2u*TIMER_HZ)reset_connection();
    if(!N.job){polling=0;return;}
    if(!N.status.link_up){job_error("Network link went down");polling=0;return;}
    if((uint32_t)(now-N.started)>=DEADLINE){job_error("Network request timed out (15 seconds)");polling=0;return;}
    if(N.phase==PHASE_DNS_ARP||N.phase==PHASE_TCP_ARP||N.phase==PHASE_PING_ARP){
        uint32_t address=N.phase==PHASE_DNS_ARP?IP_DNS:route(N.server);Neighbor *link=neighbor(address);
        if(link){
            bytes(N.peer_mac,link->mac,6);N.retries=0;N.last_send=now;
            if(N.phase==PHASE_DNS_ARP){N.phase=PHASE_DNS;dns_send();}
            else if(N.phase==PHASE_TCP_ARP){N.phase=PHASE_SYN;N.seq=0x42610000u+now*64000u+N.ip_id*257u;N.next_ack=0;tcp_send(TCP_SYN,N.seq,0,0);N.seq++;}
            else {N.phase=PHASE_PING;N.ping_started=now;ping_send();}
        }else if((uint32_t)(now-N.last_send)>=RETRY_TICKS){N.last_send=now;arp_request(address);N.retries++;}
    }else if((uint32_t)(now-N.last_send)>=RETRY_TICKS){
        N.last_send=now;
        if(++N.retries>5){job_error(N.phase==PHASE_DNS?"DNS query timed out":N.phase==PHASE_PING?"ICMP echo timed out":"TCP peer did not respond");}
        else if(N.phase==PHASE_DNS)dns_send();
        else if(N.phase==PHASE_PING)ping_send();
        else if(N.phase==PHASE_SYN)tcp_send(TCP_SYN,N.seq-1,0,0);
        else if(N.phase==PHASE_TCP){
            if(N.request_ack!=N.seq)tcp_send(TCP_ACK|TCP_PSH,N.request_seq,(const uint8_t *)N.request,N.request_length);
            else N.retries=0; /* Response waits use the overall bounded deadline. */
        }
    }
    polling=0;
}
static void wait_for_job(void){
    /* Timer IRQ remains enabled. platform_poll collects PS/2 events without
     * re-entering apps; host tests replace this bounded sleep hook. */
    while(net_busy()){net_poll();platform_poll();
#ifndef NET_HOST_TEST
        if(net_busy())__asm__ volatile("hlt");
#endif
    }
}
int net_http_get(const char *url,char *body,unsigned capacity){if(net_http_start(url,body,capacity))return -1;wait_for_job();return N.http.state==NET_HTTP_DONE?0:-1;}
int net_resolve(const char *host,uint32_t *address){if(!host||!address||begin(JOB_DNS,host))return -1;wait_for_job();if(N.failed)return -1;*address=N.server;return 0;}
int net_ping(const char *host,unsigned *milliseconds){if(!host||begin(JOB_PING,host))return -1;wait_for_job();if(N.failed)return -1;if(milliseconds)*milliseconds=N.ping_elapsed*1000u/TIMER_HZ;return 0;}
