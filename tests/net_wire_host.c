#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "net_wire.h"

static void response(const char *wire,const char *expected,unsigned stride,int eof){
    NetHttpParser parser;NetHttpResult result={0};char body[512];
    net_http_parser_init(&parser,body,sizeof body,&result);
    unsigned length=(unsigned)strlen(wire),pos=0;int rc=0;
    while(pos<length){unsigned n=length-pos;if(n>stride)n=stride;rc=net_http_parser_feed(&parser,(const uint8_t *)wire+pos,n);assert(rc>=0);pos+=n;}
    if(eof)rc=net_http_parser_eof(&parser);
    assert(rc==1&&result.length==strlen(expected)&&!strcmp(body,expected)&&!result.truncated);
}
int main(void){
    uint32_t ip;char text[16],host[NET_HOST_MAX],path[NET_URL_MAX];unsigned port;
    assert(net_parse_ipv4("10.0.2.15",&ip)&&ip==0x0a00020f);net_format_ipv4(ip,text);assert(!strcmp(text,"10.0.2.15"));
    assert(net_parse_ipv4("255.255.255.255",&ip)&&ip==UINT32_MAX);
    assert(net_parse_url("http://Example.COM:8080/a?query=one#section",host,&port,path));
    assert(!strcmp(host,"example.com")&&port==8080&&!strcmp(path,"/a?query=one"));
    assert(net_parse_url("http://example.com?x=1",host,&port,path)&&!strcmp(path,"/?x=1")&&port==80);
    assert(net_parse_url("http://10.0.2.2/",host,&port,path));
    assert(!net_parse_url("https://example.com/",host,&port,path));
    uint8_t packet[512];int n=net_dns_query(packet,sizeof packet,0x1234,"fixture.test");assert(n==30);
    assert(net_read16(packet)==0x1234&&net_read16(packet+4)==1&&packet[12]==7);
    net_write16(packet+2,0x8180);net_write16(packet+6,1);
    uint8_t answer[]={0xc0,0x0c,0,1,0,1,0,0,0,60,0,4,10,0,2,2};
    memcpy(packet+n,answer,sizeof answer);assert(net_dns_answer(packet,n+sizeof answer,0x1234,"FIXTURE.TEST.",&ip)==1&&ip==0x0a000202);
    assert(net_dns_answer(packet,n+sizeof answer,0x4321,"fixture.test",&ip)==0);
    n=net_dns_query(packet,sizeof packet,4,"alias.fixture.test");assert(n>0);net_write16(packet+2,0x8180);net_write16(packet+6,2);
    unsigned pos=n;uint8_t cname[]={0xc0,0x0c,0,5,0,1,0,0,0,60,0,14,7,'f','i','x','t','u','r','e',4,'t','e','s','t',0};
    memcpy(packet+pos,cname,sizeof cname);unsigned target=pos+12;pos+=sizeof cname;
    memcpy(packet+pos,answer,sizeof answer);net_write16(packet+pos,0xc000|target);pos+=sizeof answer;
    assert(net_dns_answer(packet,pos,4,"alias.fixture.test",&ip)==1&&ip==0x0a000202);
    uint8_t odd[]={0x12,0x34,0x56};assert(net_checksum(odd,3)==0x97cb);
    uint8_t udp[]={0xc0,0,0,53,0,8,0,0};net_write16(udp+6,net_transport_checksum(0x0a00020f,0x0a000203,17,udp,sizeof udp));assert(!net_transport_checksum(0x0a00020f,0x0a000203,17,udp,sizeof udp));
    const char *fixed="HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 12\r\n\r\nHello world!";
    const char *chunked="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n6;test=1\r\nHello \r\n6\r\nworld!\r\n0\r\nX-Fixture: done\r\n\r\n";
    const char *closed="HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nHello world!";
    for(unsigned stride=1;stride<=17;stride++){response(fixed,"Hello world!",stride,0);response(chunked,"Hello world!",stride,0);response(closed,"Hello world!",stride,1);}
    response("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n","",1,0);
    NetHttpParser parser;NetHttpResult result={0};char body[8];
    net_http_parser_init(&parser,body,sizeof body,&result);assert(net_http_parser_feed(&parser,(const uint8_t *)fixed,strlen(fixed))==1);assert(result.truncated&&result.length==7&&!strcmp(body,"Hello w"));
    memset(&result,0,sizeof result);net_http_parser_init(&parser,body,sizeof body,&result);
    const char *redirect="HTTP/1.1 302 Found\r\nLocation: http://fixture.test/new\r\nContent-Length: 0\r\n\r\n";
    assert(net_http_parser_feed(&parser,(const uint8_t *)redirect,strlen(redirect))==1);assert(result.status==302&&!strcmp(result.location,"http://fixture.test/new"));
    puts("network wire: IPv4/URL, checksums, DNS A/CNAME, split HTTP framing, truncation, redirect metadata passed");
}
