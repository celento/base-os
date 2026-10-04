/* Bounded, allocation-free protocol helpers; all integers on the wire are
 * accessed bytewise, including on unaligned Ethernet/IP payloads. */
#include "net_wire.h"
static void zero(void *v,unsigned n){uint8_t *p=v;while(n--)*p++=0;}
static unsigned length(const char *p){unsigned n=0;while(p[n])n++;return n;}
static void copy(char *d,unsigned cap,const char *s){if(!cap)return;unsigned i=0;while(s[i]&&i+1<cap){d[i]=s[i];i++;}d[i]=0;}
static char lower(char c){return c>='A'&&c<='Z'?c+32:c;}
static int equal(const char *a,const char *b){while(*a&&lower(*a)==lower(*b)){a++;b++;}return !*a&&!*b;}
static int begins(const char *a,const char *b){while(*b)if(*a++!=*b++)return 0;return 1;}
uint16_t net_read16(const uint8_t *p){return (uint16_t)((unsigned)p[0]<<8|p[1]);}
uint32_t net_read32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
void net_write16(uint8_t *p,unsigned v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
void net_write32(uint8_t *p,uint32_t v){p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v;}
static uint32_t sum(const uint8_t *p,unsigned n,uint32_t s){while(n>1){s+=net_read16(p);p+=2;n-=2;}if(n)s+=(unsigned)*p<<8;return s;}
static uint16_t finish(uint32_t s){while(s>>16)s=(s&0xffff)+(s>>16);return (uint16_t)~s;}
uint16_t net_checksum(const uint8_t *p,unsigned n){return finish(sum(p,n,0));}
uint16_t net_transport_checksum(uint32_t src,uint32_t dst,unsigned proto,const uint8_t *p,unsigned n){
    uint32_t s=(src>>16)+(src&65535)+(dst>>16)+(dst&65535)+proto+n;return finish(sum(p,n,s));
}
int net_parse_ipv4(const char *s,uint32_t *address){
    uint32_t v=0;for(unsigned i=0;i<4;i++){
        unsigned n=0,digits=0;if(*s<'0'||*s>'9')return 0;
        while(*s>='0'&&*s<='9'){n=n*10+(unsigned)(*s++-'0');if(++digits>3||n>255)return 0;}
        v=v<<8|n;if(i<3){if(*s++!='.')return 0;}else if(*s)return 0;
    }if(address)*address=v;return 1;
}
void net_format_ipv4(uint32_t address,char out[16]){
    unsigned pos=0;for(unsigned i=0;i<4;i++){
        unsigned n=(address>>(24-i*8))&255;char digits[3];unsigned count=0;
        do{digits[count++]=(char)('0'+n%10);n/=10;}while(n);
        while(count)out[pos++]=digits[--count];
        if(i!=3)out[pos++]='.';
    }out[pos]=0;
}
static int hostname(const char *host){
    unsigned n=0,label=0;char prev=0;
    for(const char *p=host;*p;p++){
        char c=lower(*p);if(++n>=NET_HOST_MAX)return 0;
        if(c=='.'){if(!label||prev=='-')return 0;label=0;}
        else {if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-')||(!label&&c=='-')||++label>63)return 0;}
        prev=c;
    }return n&&prev!='-'&&(label||prev=='.');
}
int net_parse_url(const char *url,char host[NET_HOST_MAX],unsigned *port,char path[NET_URL_MAX]){
    if(!url||!begins(url,"http://"))return 0;
    url+=7;
    unsigned n=0;while(*url&&*url!='/'&&*url!=':'&&*url!='?'&&*url!='#'){
        if(n+1>=NET_HOST_MAX)return 0;
        host[n++]=lower(*url++);
    }host[n]=0;if(!hostname(host))return 0;*port=80;
    if(*url==':'){
        url++;unsigned p=0,digits=0;while(*url>='0'&&*url<='9'){
            if(++digits>5)return 0;
            p=p*10+(unsigned)(*url++-'0');
        }if(!digits||!p||p>65535)return 0;*port=p;
    }
    if(*url&&*url!='/'&&*url!='?'&&*url!='#')return 0;
    n=0;if(*url!='/')path[n++]='/';
    while(*url&&*url!='#'){
        unsigned char c=(unsigned char)*url++;if(c<33||c>126||n+1>=NET_URL_MAX)return 0;path[n++]=(char)c;
    }path[n]=0;return 1;
}
int net_dns_query(uint8_t *out,unsigned cap,unsigned id,const char *host){
    if(!hostname(host)||cap<18)return -1;
    unsigned n=length(host);if(n&&host[n-1]=='.')n--;
    if(n+18>cap)return -1;
    zero(out,12);net_write16(out,id);net_write16(out+2,0x0100);net_write16(out+4,1);
    unsigned p=12,start=0;
    while(start<n){unsigned end=start;while(end<n&&host[end]!='.')end++;out[p++]=(uint8_t)(end-start);while(start<end)out[p++]=(uint8_t)lower(host[start++]);start++;}
    out[p++]=0;net_write16(out+p,1);net_write16(out+p+2,1);return (int)p+4;
}
static int dns_name(const uint8_t *data,unsigned len,unsigned *offset,char out[NET_HOST_MAX]){
    unsigned p=*offset,end=0,n=0,steps=0;int jumped=0;
    while(p<len&&++steps<128){
        unsigned at=p,c=data[p++];if(!c){if(!jumped)end=p;out[n]=0;*offset=end;return 1;}
        if((c&0xc0)==0xc0){
            if(p>=len)return 0;
            unsigned next=((c&63)<<8)|data[p++];
            /* Compression references an earlier occurrence; backward-only
             * pointers plus a step budget reject cycles and forward loops. */
            if(next>=at)return 0;
            if(!jumped){end=p;jumped=1;}p=next;continue;
        }
        if(c>63||c>len-p||n+c+(n?1:0)>=NET_HOST_MAX)return 0;
        if(n)out[n++]='.';
        while(c--){unsigned char ch=data[p++];if(ch<33||ch>126||ch=='.')return 0;out[n++]=lower((char)ch);}
    }return 0;
}
int net_dns_answer(const uint8_t *data,unsigned len,unsigned id,const char *host,uint32_t *address){
    if(len<12||net_read16(data)!=(id&65535))return 0;
    unsigned flags=net_read16(data+2),answers=net_read16(data+6);
    if(!(flags&0x8000)||(flags&0x7800))return 0;
    if((flags&0x020f)||net_read16(data+4)!=1||answers>64)return -1;
    char question[NET_HOST_MAX],wanted[NET_HOST_MAX];unsigned p=12;
    if(!dns_name(data,len,&p,question)||len-p<4)return -1;
    copy(wanted,sizeof wanted,host);unsigned hn=length(wanted);if(hn&&wanted[hn-1]=='.')wanted[hn-1]=0;
    if(!equal(question,wanted)||net_read16(data+p)!=1||net_read16(data+p+2)!=1)return 0;
    p+=4;unsigned start=p;
    for(unsigned pass=0;pass<8;pass++){
        int alias=0;p=start;
        for(unsigned i=0;i<answers;i++){
            char owner[NET_HOST_MAX];if(!dns_name(data,len,&p,owner)||len-p<10)return -1;
            unsigned type=net_read16(data+p),cls=net_read16(data+p+2),size=net_read16(data+p+8);p+=10;
            if(size>len-p)return -1;
            if(cls==1&&equal(owner,wanted)){
                if(type==1&&size==4){uint32_t a=net_read32(data+p);if(!a||a==0xffffffffu)return -1;*address=a;return 1;}
                if(type==5){unsigned cp=p;char target[NET_HOST_MAX];if(!dns_name(data,len,&cp,target)||cp!=p+size||!target[0])return -1;copy(wanted,sizeof wanted,target);alias=1;}
            }p+=size;
        }if(!alias)return -1;
    }return -1;
}
static int fail(NetHttpParser *p,const char *message){p->failed=1;copy(p->result->error,sizeof p->result->error,message);return -1;}
void net_http_parser_init(NetHttpParser *p,char *body,unsigned capacity,NetHttpResult *result){
    zero(p,sizeof *p);p->body=body;p->capacity=capacity;p->result=result;if(body&&capacity)body[0]=0;
}
static char *trim(char *p){while(*p==' '||*p=='\t')p++;unsigned n=length(p);while(n&&(p[n-1]==' '||p[n-1]=='\t'))p[--n]=0;return p;}
static int parse_headers(NetHttpParser *p){
    char *s=p->headers,*end=s;while(*end&&*end!='\r')end++;
    if(end-s<12||end[1]!='\n'||!(begins(s,"HTTP/1.0 ")||begins(s,"HTTP/1.1 "))||
       s[9]<'1'||s[9]>'5'||s[10]<'0'||s[10]>'9'||s[11]<'0'||s[11]>'9'||
       (s[12]&&s[12]!=' '&&s[12]!='\r'))return fail(p,"Malformed HTTP status line");
    p->result->status=(s[9]-'0')*100+(s[10]-'0')*10+s[11]-'0';
    if(p->result->status<200)return fail(p,"Interim HTTP responses are unsupported");
    s=end+2;int encoding_seen=0;
    while(*s&&*s!='\r'){
        end=s;while(*end&&*end!='\r')end++;if(!*end||end[1]!='\n')return fail(p,"Malformed HTTP header");
        *end=0;char *colon=s;while(*colon&&*colon!=':')colon++;
        if(colon==s||!*colon||*s==' '||*s=='\t')return fail(p,"Malformed HTTP header");
        *colon++=0;for(char *name=s;*name;name++)if(!((*name>='a'&&*name<='z')||(*name>='A'&&*name<='Z')||(*name>='0'&&*name<='9')||*name=='-'))return fail(p,"Malformed HTTP header name");
        char *value=trim(colon);
        if(equal(s,"Content-Length")){
            uint32_t size=0;if(!*value)return fail(p,"Invalid Content-Length");
            for(char *v=value;*v;v++){if(*v<'0'||*v>'9'||size>(0xffffffffu-(unsigned)(*v-'0'))/10)return fail(p,"Invalid Content-Length");size=size*10+(unsigned)(*v-'0');}
            if(p->has_length&&p->remaining!=size)return fail(p,"Conflicting Content-Length");
            p->has_length=1;p->remaining=size;
        }else if(equal(s,"Transfer-Encoding")){
            if(encoding_seen++||!equal(value,"chunked"))return fail(p,"Unsupported transfer encoding");
            p->chunked=1;p->chunk_state=1;
        }else if(equal(s,"Content-Encoding")){
            if(!equal(value,"identity"))return fail(p,"Compressed HTTP content is unsupported");
        }else if(equal(s,"Content-Type"))copy(p->result->content_type,sizeof p->result->content_type,value);
        else if(equal(s,"Location"))copy(p->result->location,sizeof p->result->location,value);
        s=end+2;
    }
    if(p->chunked&&p->has_length)return fail(p,"Ambiguous HTTP body framing");
    p->headers_done=1;
    if(p->result->status==204||p->result->status==304||(p->has_length&&!p->remaining))p->done=1;
    return p->done;
}
static int body_byte(NetHttpParser *p,uint8_t c){
    if(!p->body||p->capacity<1)return fail(p,"Missing HTTP response buffer");
    if(p->body_length+1>=p->capacity){p->result->truncated=1;p->done=1;return 1;}
    p->body[p->body_length++]=(char)c;p->body[p->body_length]=0;p->result->length=p->body_length;return 0;
}
static int chunk_byte(NetHttpParser *p,uint8_t c){
    if(p->chunk_state==2){if(body_byte(p,c))return p->failed?-1:1;if(!--p->chunk_remaining)p->chunk_state=3;return 0;}
    if(p->chunk_state==3){if(c!='\r')return fail(p,"Malformed HTTP chunk boundary");p->chunk_state=4;return 0;}
    if(p->chunk_state==4){if(c!='\n')return fail(p,"Malformed HTTP chunk boundary");p->chunk_state=1;return 0;}
    if(p->chunk_state==5&&++p->trailer_bytes>2048)return fail(p,"HTTP trailers exceed limit");
    if(c!='\n'){
        if(!c||p->line_length+1>=sizeof p->line)return fail(p,"HTTP chunk line exceeds limit");
        p->line[p->line_length++]=(char)c;return 0;
    }
    if(!p->line_length||p->line[p->line_length-1]!='\r')return fail(p,"Malformed HTTP chunk line");
    p->line[--p->line_length]=0;
    if(p->chunk_state==5){if(!p->line_length)p->done=1;p->line_length=0;return p->done;}
    unsigned i=0;uint32_t n=0;
    while(p->line[i]&&p->line[i]!=';'){
        unsigned char v=(unsigned char)lower(p->line[i++]);unsigned d;
        if(v>='0'&&v<='9')d=v-'0';else if(v>='a'&&v<='f')d=v-'a'+10;else return fail(p,"Invalid HTTP chunk size");
        if(n>(0xffffffffu-d)/16)return fail(p,"HTTP chunk size overflow");
        n=n*16+d;
    }
    if(!i)return fail(p,"Missing HTTP chunk size");
    p->chunk_remaining=n;p->chunk_state=n?2:5;p->line_length=0;return 0;
}
int net_http_parser_feed(NetHttpParser *p,const uint8_t *data,unsigned size){
    if(p->failed)return -1;
    if(p->done)return 1;
    for(unsigned i=0;i<size;i++){
        uint8_t c=data[i];
        if(!p->headers_done){
            if(!c||(c<32&&c!='\r'&&c!='\n'&&c!='\t'))return fail(p,"Invalid HTTP header byte");
            if(p->header_length+1>=sizeof p->headers)return fail(p,"HTTP headers exceed 4095 bytes");
            p->headers[p->header_length++]=(char)c;p->headers[p->header_length]=0;
            unsigned n=p->header_length;
            if(n>=4&&p->headers[n-4]=='\r'&&p->headers[n-3]=='\n'&&p->headers[n-2]=='\r'&&c=='\n'){
                int rc=parse_headers(p);if(rc)return rc;
            }
        }else if(p->chunked){int rc=chunk_byte(p,c);if(rc)return rc;}
        else {int rc=body_byte(p,c);if(rc)return p->failed?-1:1;if(p->has_length&&!--p->remaining){p->done=1;return 1;}}
    }return 0;
}
int net_http_parser_eof(NetHttpParser *p){
    if(p->failed)return -1;
    if(p->done)return 1;
    if(!p->headers_done)return fail(p,"Connection closed before HTTP headers");
    if(p->chunked||(p->has_length&&p->remaining))return fail(p,"Connection closed before HTTP body completed");
    p->done=1;return 1;
}
