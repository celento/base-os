#include "program.h"
#include "platform.h"
#include "fs.h"

typedef struct { const char *p; int32_t *v; int error, depth; } Expr;
static void spaces(Expr *e) { while (*e->p==' ' || *e->p=='\t') e->p++; }
static int32_t expression(Expr *e);
static int32_t atom(Expr *e) {
    spaces(e);
    if (++e->depth>16) { e->error=1; e->depth--; return 0; }
    int32_t n=0;
    if (*e->p=='(') { e->p++;n=expression(e);spaces(e);if(*e->p!=')')e->error=1;else e->p++; }
    else if (*e->p=='-') { e->p++;n=(int32_t)(0u-(uint32_t)atom(e)); }
    else if (*e->p>='A'&&*e->p<='Z') n=e->v[*e->p++-'A'];
    else if (*e->p>='0'&&*e->p<='9') { unsigned value=0;while(*e->p>='0'&&*e->p<='9')value=value*10+(*e->p++-'0');n=(int32_t)value; }
    else e->error=1;
    e->depth--;return n;
}
static int32_t product(Expr *e) {
    int32_t a=atom(e);spaces(e);
    while (*e->p=='*'||*e->p=='/') {
        char op=*e->p++;int32_t b=atom(e);
        if(op=='*')a=(int32_t)((uint32_t)a*(uint32_t)b);
        else if(!b || (a==INT32_MIN&&b==-1)){e->error=1;a=0;}
        else a/=b;
        spaces(e);
    }return a;
}
static int32_t expression(Expr *e) {
    int32_t a=product(e);spaces(e);
    while(*e->p=='+'||*e->p=='-'){
        char op=*e->p++;int32_t b=product(e);
        a=op=='+'?(int32_t)((uint32_t)a+(uint32_t)b):(int32_t)((uint32_t)a-(uint32_t)b);spaces(e);
    }return a;
}
static int word(Expr *e,const char *s) {
    spaces(e);const char *p=e->p;
    while(*s&&*p==*s){p++;s++;}
    if(*s||(*p>='A'&&*p<='Z'))return 0;
    e->p=p;return 1;
}
static void number(char *out,int32_t n){
    char temp[12];unsigned v=n<0?0u-(unsigned)n:(unsigned)n;int count=0;
    if(n<0)*out++='-';
    do{temp[count++]='0'+v%10;v/=10;}while(v);
    while(count)*out++=temp[--count];
    *out=0;
}
int basic_run(const char *source,int length,const ProgramIO *io) {
    unsigned offsets[256],labels[256],count=0;int32_t variables[26]={0};
    if(length<0||length>=FS_MAX_SIZE)return -1;
    for(int pos=0;pos<length;){
        int start=pos;while(pos<length&&source[pos]!='\n')pos++;int end=pos++;
        while(start<end&&(source[start]==' '||source[start]=='\r'))start++;
        if(start==end)continue;
        unsigned label=0;int digits=0;
        while(start<end&&source[start]>='0'&&source[start]<='9'){
            if(label>6553)return -1;
            label=label*10+source[start++]-'0';digits++;
        }
        if(!digits||!label||label>65535||count==256)return -1;
        unsigned i=count++;while(i&&labels[i-1]>label){labels[i]=labels[i-1];offsets[i]=offsets[i-1];i--;}
        if((i&&labels[i-1]==label)||(i+1<count&&labels[i+1]==label))return -1;
        labels[i]=label;offsets[i]=start;
    }
    uint32_t start=timer_ticks();
    unsigned pc=0,budget=10000;
    while(pc<count && budget-- && timer_ticks()-start<10*TIMER_HZ){
        char line[192];unsigned n=0,pos=offsets[pc++];
        while(pos<(unsigned)length&&source[pos]!='\n'&&source[pos]!='\r'){
            if(n==sizeof(line)-1)return -1;
            line[n++]=source[pos++];
        }line[n]=0;Expr e={line,variables,0,0};int target=-1;
        if(word(&e,"REM"))continue;
        if(word(&e,"END")){spaces(&e);if(*e.p)return -1;if(io->present)io->present();return 0;}
        if(word(&e,"PRINT")){
            spaces(&e);char out[192];
            if(*e.p=='"'){e.p++;unsigned j=0;while(*e.p&&*e.p!='"')out[j++]=*e.p++;out[j]=0;if(*e.p!='"')return -1;e.p++;}
            else number(out,expression(&e));
            io->print(out);
        }else if(word(&e,"LET")){
            spaces(&e);char v=*e.p;if(v<'A'||v>'Z')return -1;e.p++;spaces(&e);if(*e.p!='=')return -1;e.p++;
            variables[v-'A']=expression(&e);
        }else if(word(&e,"GOTO")){target=expression(&e);if(target<1||target>65535)return -1;}
        else if(word(&e,"IF")){
            int32_t a=expression(&e);spaces(&e);char op=*e.p++;
            if(op!='='&&op!='<'&&op!='>')return -1;
            int32_t b=expression(&e);if(!word(&e,"THEN"))return -1;
            int dest=expression(&e);if(dest<1||dest>65535)return -1;if(op=='='?a==b:op=='<'?a<b:a>b)target=dest;
        }else if(word(&e,"INKEY")){
            spaces(&e);char v=*e.p++;if(v<'A'||v>'Z')return -1;
            variables[v-'A']=io->key?io->key():0;
        }else if(word(&e,"WAIT")){
            int t=expression(&e);if(t<0||t>1000)return -1;
            if(io->present)io->present();
            timer_delay((t*TIMER_HZ+999)/1000);
        }else{
            int rect=word(&e,"RECT");if(!rect&&!word(&e,"PLOT"))return -1;
            int args[5],num=rect?5:3;
            for(int i=0;i<num;i++){args[i]=expression(&e);spaces(&e);if(i+1<num&&*e.p++!=',')return -1;}
            if(rect){if(args[2]<0||args[2]>160||args[3]<0||args[3]>100)return -1;
                for(int y=0;y<args[3];y++)for(int x=0;x<args[2];x++)io->plot((int)((unsigned)args[0]+x),(int)((unsigned)args[1]+y),args[4]);
            }else io->plot(args[0],args[1],args[2]);
        }
        spaces(&e);if(e.error||*e.p)return -1;
        if(target>=0){unsigned i=0;while(i<count&&labels[i]!=(unsigned)target)i++;if(i==count)return -1;pc=i;}
    }
    if(io->present)io->present();
    return pc==count?0:-2;
}
