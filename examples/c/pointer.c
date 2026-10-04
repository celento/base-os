#include "baseos.h"
#include "pointer_backend.h"
/* /Programs/pointer.bex remains the hosted example in either BEX format.
 * pointer-window.bex selects only the backend adapter at compile time. */
#define WIDTH 320u
#define HEIGHT 200u
#define INK_BYTES (WIDTH*HEIGHT/8u)
#define BUTTONS (BOS_UI_BUTTON_LEFT|BOS_UI_BUTTON_RIGHT)
#define READY (BOS_UI_STATE_FOCUSED|BOS_UI_STATE_AVAILABLE|BOS_UI_STATE_CAPTURED|BOS_UI_STATE_POSITION_VALID)
#define TOP 46

typedef struct {
    unsigned sequence_lo,sequence_hi,geometry,stream,logical_w,logical_h;
    unsigned state,buttons,modifiers,reason,resets,cancels,strokes,unknown;
    unsigned drag,armed,pen,dirty,width,height;
    int x,y,last_x,last_y;
} PointerModel;
static PointerModel model;
/* Two colors, each with committed ink and a cancellable gesture preview.
 * Fixed 320-pixel stride keeps storage bounded in the BEX1 data segment. */
static unsigned char ink[2][INK_BYTES],preview[2][INK_BYTES];
static BosHandle target;
static unsigned subscriptions,wait_ms;

static void erase(unsigned char pixels[2][INK_BYTES]) {
    for(unsigned c=0;c<2;c++)for(unsigned i=0;i<INK_BYTES;i++)pixels[c][i]=0;
}
static void abort_stroke(PointerModel *m) {
    erase(preview);m->drag=m->pen=0;m->dirty=1;
}
static void clear_ink(PointerModel *m) {
    abort_stroke(m);erase(ink);
}
static int drawable(const PointerModel *m,int x,int y) {
    return x>1&&x<(int)m->width-2&&y>=TOP&&y<(int)m->height-9;
}
static void dot(int x,int y,unsigned buttons) {
    unsigned bit=(unsigned)y*WIDTH+(unsigned)x;
    for(unsigned c=0;c<2;c++)if(buttons&(1u<<c))
        preview[c][bit/8]|=(unsigned char)(1u<<(bit%8));
}
static void stroke_point(PointerModel *m,unsigned buttons) {
    /* Never clamp the reported signed coordinates. Only drawable endpoints
     * enter line arithmetic; an outside excursion breaks the drawn segment. */
    if(!drawable(m,m->x,m->y)){m->pen=0;return;}
    int x=m->pen?m->last_x:m->x,y=m->pen?m->last_y:m->y;
    int dx=m->x-x,dy=m->y-y,sx=dx<0?-1:1,sy=dy<0?-1:1;
    if(dx<0)dx=-dx;
    if(dy>0)dy=-dy;
    int error=dx+dy;
    for(;;){
        dot(x,y,buttons);
        if(x==m->x&&y==m->y)break;
        int twice=2*error;
        if(twice>=dy){error+=dy;x+=sx;}
        if(twice<=dx){error+=dx;y+=sy;}
    }
    m->last_x=m->x;m->last_y=m->y;m->pen=1;
}
static void commit_stroke(PointerModel *m) {
    for(unsigned c=0;c<2;c++)for(unsigned i=0;i<INK_BYTES;i++)ink[c][i]|=preview[c][i];
    ++m->strokes;abort_stroke(m);
}
/* Adapter boundary: hosted and owned-window endpoints feed this model. Ignore
 * unknown kinds/bits; do not infer DOWN from a MOVE or from a state snapshot. */
static void accept_event(PointerModel *m,const BosUiEventV1 *e) {
    if(e->size<sizeof *e||e->major!=BOS_UI_MAJOR||e->target!=target)return;
    if(e->type<BOS_UI_STATE_RESET||e->type>BOS_UI_GEOMETRY){++m->unknown;return;}
    unsigned before=m->buttons;
    if(e->geometry_epoch!=m->geometry||e->stream_epoch!=m->stream)abort_stroke(m);
    m->sequence_lo=e->sequence_lo;m->sequence_hi=e->sequence_hi;
    m->geometry=e->geometry_epoch;m->stream=e->stream_epoch;
    m->logical_w=e->logical_w;m->logical_h=e->logical_h;
    m->x=e->x;m->y=e->y;m->state=e->state;m->buttons=e->buttons&BUTTONS;
    m->modifiers=e->modifiers;m->reason=e->reason;m->dirty=1;
    if(e->type==BOS_UI_STATE_RESET){
        ++m->resets;abort_stroke(m);m->armed=1;return;
    }
    if(e->type==BOS_UI_CANCEL){++m->cancels;abort_stroke(m);return;}
    if(e->type==BOS_UI_GEOMETRY){abort_stroke(m);return;}
    if((m->state&READY)!=READY||(m->state&(BOS_UI_STATE_MINIMIZED|BOS_UI_STATE_BLOCKED))){
        abort_stroke(m);return;
    }
    if(e->type==BOS_UI_POINTER_BUTTON){
        if(!m->drag&&m->armed&&!before&&(e->changed_buttons&m->buttons&BUTTONS)){
            abort_stroke(m);m->drag=1;
        }
        if(m->drag){
            stroke_point(m,m->buttons?m->buttons:before);
            if(!m->buttons)commit_stroke(m); /* Only ordinary final UP commits. */
        }
    }else if(e->type==BOS_UI_POINTER_MOVE&&m->drag&&m->buttons)stroke_point(m,m->buttons);
}

static char *text(char *out,const char *s){while(*s)*out++=*s++;*out=0;return out;}
static char *number(char *out,unsigned n){
    char digits[10];unsigned count=0;
    do{digits[count++]=(char)('0'+n%10);n/=10;}while(n);
    while(count)*out++=digits[--count];
    *out=0;return out;
}
static char *signed_number(char *out,int n){
    unsigned magnitude=(unsigned)n;
    if(n<0){*out++='-';magnitude=0u-magnitude;}
    return number(out,magnitude);
}
static char *hex(char *out,unsigned n){
    for(int shift=28;shift>=0;shift-=4)*out++="0123456789ABCDEF"[(n>>(unsigned)shift)&15];
    *out=0;return out;
}
static char *field(char *out,const char *name,unsigned n){return number(text(out,name),n);}
/* Stable, ordinary terminal output for P and key diagnostics. No guest hooks.
 * Sequence is hexadecimal hi:lo; all other values are decimal. */
static void status(const char *tag) {
    char line[320],*p=text(line,"POINTER ");p=text(p,tag);
    p=text(p," seq=");p=hex(p,model.sequence_hi);p=text(p,":");p=hex(p,model.sequence_lo);
    p=signed_number(text(p," x="),model.x);p=signed_number(text(p," y="),model.y);
    p=field(p," buttons=",model.buttons);p=field(p," geom=",model.geometry);
    p=field(p," stream=",model.stream);p=field(p," reset=",model.resets);
    p=field(p," cancel=",model.cancels);p=field(p," drag=",model.drag);
    p=field(p," strokes=",model.strokes);p=field(p," logical=",model.logical_w);
    p=field(p,"x",model.logical_h);p=field(p," working=",model.width);
    p=field(p,"x",model.height);p=field(p," state=",model.state);
    p=field(p," reason=",model.reason);text(p,"\n");bos_print(line);
}
/* Original 3x5 pixel lettering, deliberately matching the small OS canvas. */
static const unsigned char glyphs[36][5]={
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7},
    {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},
    {7,4,6,4,4},{3,4,5,5,3},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},
    {5,5,6,5,5},{4,4,4,4,7},{5,7,7,5,5},{5,7,7,7,5},{2,5,5,5,2},
    {6,5,6,4,4},{2,5,5,3,1},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},
    {5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2},
    {7,1,2,4,7}
};
static void label(int x,int y,const char *s,unsigned color) {
    for(;*s;s++,x+=4){
        unsigned char c=(unsigned char)*s;
        for(unsigned row=0;row<5;row++){
            unsigned bits=0;
            if(c>='0'&&c<='9')bits=glyphs[c-'0'][row];
            else if(c>='A'&&c<='Z')bits=glyphs[c-'A'+10][row];
            else if(c=='-')bits=row==2?7:0;
            else if(c==':')bits=row==1||row==3?2:0;
            else if(c=='=')bits=row==1||row==3?7:0;
            else if(c=='/')bits=row<2?1:row==2?2:4;
            for(unsigned col=0;col<3;col++)if(bits&(4u>>col))bos_plot(x+(int)col,y+(int)row,color);
        }
    }
}
static unsigned pixel(unsigned x,unsigned y) {
    unsigned bit=y*WIDTH+x,mask=1u<<(bit%8),value=0;
    for(unsigned c=0;c<2;c++)if((ink[c][bit/8]|preview[c][bit/8])&mask)value|=1u<<c;
    return value;
}
static void render(const PointerModel *m) {
    char line[80],*p;
    bos_rect(0,0,m->width,m->height,0);
    bos_rect(1,TOP-2,m->width-2,m->height-TOP-5,1);
    bos_rect(2,TOP,m->width-4,m->height-TOP-9,0);
    const unsigned colors[4]={0,8,6,7};
    for(unsigned y=TOP;y<m->height-9;y++)for(unsigned x=2;x<m->width-2;){
        unsigned value=pixel(x,y),end=x+1;
        while(end<m->width-2&&pixel(end,y)==value)++end;
        if(value)bos_rect((int)x,(int)y,end-x,1,colors[value]);
        x=end;
    }
    p=text(line,"POINTER ");p=field(p,"",m->logical_w);field(p,"X",m->logical_h);label(2,2,line,8);
    p=text(line,"S=");p=hex(p,m->sequence_hi);hex(text(p,":"),m->sequence_lo);label(2,9,line,7);
    p=signed_number(text(line,"X="),m->x);signed_number(text(p," Y="),m->y);label(2,16,line,7);
    p=field(line,"B=",m->buttons);p=field(p," G=",m->geometry);field(p," T=",m->stream);label(2,23,line,7);
    p=field(line,"RESET=",m->resets);field(p," CANCEL=",m->cancels);label(2,30,line,6);
    p=field(line,"DRAG=",m->drag);field(p," DONE=",m->strokes);label(2,37,line,8);
    label(2,(int)m->height-6,"R SIZE C CLEAR O OPEN P PRINT Q EXIT",7);
}
static int open_target(void) {
    BosUiTargetInfoV1 info;
    int result=pointer_backend_open(subscriptions,&info);
    if(result!=BOS_OK)return result;
    target=info.target;
    if(info.size<sizeof info||info.major!=BOS_UI_MAJOR||!pointer_backend_target_valid(&info)){
        bos_ui_release(target);target=BOS_HANDLE_INVALID;return BOS_E_UNSUPPORTED;
    }
    abort_stroke(&model);model.armed=0;model.buttons=0;
    model.logical_w=info.logical_w;model.logical_h=info.logical_h;
    model.geometry=info.geometry_epoch;model.stream=info.stream_epoch;
    model.sequence_lo=model.sequence_hi=0;
    model.state=info.state;model.x=info.x;model.y=info.y;
    return BOS_OK; /* OPEN reset must still be read before a gesture starts. */
}
static int drain_events(void) {
    BosUiEventV1 event;int result;
    while((result=bos_ui_read(target,&event))==BOS_OK)accept_event(&model,&event);
    return result==BOS_PENDING?BOS_OK:result;
}
static void error(const char *operation,int result) {
    char line[96],*p=text(line,"POINTER ERROR ");p=text(p,operation);
    p=signed_number(text(p," result="),result);text(p,"\n");bos_print(line);
}
int main(void) {
    BosUiInfoV1 info;int result=pointer_backend_query(&info,sizeof info);
    const unsigned required=POINTER_REQUIRED_CAPABILITIES;
    if(result!=BOS_OK||info.size<sizeof info||info.major!=BOS_UI_MAJOR||
       (info.capabilities&required)!=required||!(info.subscriptions_supported&BOS_UI_SUB_POINTER)||
       info.event_bytes<sizeof(BosUiEventV1)||!info.wait_max_ms){
        bos_print(POINTER_BACKEND_UNSUPPORTED);
        return 0;
    }
    subscriptions=info.subscriptions_supported&(BOS_UI_SUB_POINTER|BOS_UI_SUB_HOVER|BOS_UI_SUB_WHEEL);
    wait_ms=info.wait_max_ms<1000?info.wait_max_ms:1000;
    model=(PointerModel){0};model.width=160;model.height=100;clear_ink(&model);
    /* Working resize is not publication. Publish the initial frame explicitly
     * before opening, and every dirty frame explicitly before a bounded wait. */
    if(bos_canvas_size(model.width,model.height)<0){bos_print("POINTER ERROR canvas\n");return 1;}
    model.logical_w=model.width;model.logical_h=model.height;
    render(&model);bos_present();model.dirty=0;
    if((result=open_target())!=BOS_OK){error("open",result);return 1;}
    bos_print("Pointer: L/R draw; R size; C clear; O reopen; P status; Q/Esc quit.\n");
    for(;;){
        if((result=drain_events())!=BOS_OK){error("read",result);break;}
        int key;
        while((key=bos_key())!=0){
            if(key=='q'||key=='Q'||key==27){status("EXIT");bos_ui_release(target);return 0;}
            if(key=='p'||key=='P')status("STATUS");
            if(key=='c'||key=='C'){clear_ink(&model);status("CLEAR");}
            if(key=='r'||key=='R'){
                unsigned w=model.width==160?320:160,h=w==160?100:200;
                if(bos_canvas_size(w,h)<0){error("resize",-1);continue;}
                clear_ink(&model);model.width=w;model.height=h;
                status("RESIZE_WORKING"); /* Old logical geometry until present. */
            }
            if(key=='o'||key=='O'){
                abort_stroke(&model);
                if((result=bos_ui_release(target))!=BOS_OK){error("release",result);goto done;}
                target=BOS_HANDLE_INVALID;
                if((result=open_target())!=BOS_OK){error("reopen",result);goto done;}
                if((result=drain_events())!=BOS_OK){error("read",result);goto done;}
                status("REOPEN");
            }
        }
        if(model.dirty){render(&model);bos_present();model.dirty=0;}
        result=bos_ui_wait(target,BOS_UI_WAIT_QUEUE|BOS_UI_WAIT_LEGACY_KEY,wait_ms);
        if(result!=BOS_OK&&result!=BOS_E_TIMEOUT){error("wait",result);break;}
    }
done:
    abort_stroke(&model);
    if(target!=BOS_HANDLE_INVALID)bos_ui_release(target);
    return 1;
}
