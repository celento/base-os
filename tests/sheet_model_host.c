#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sheet_model.h"

static SheetDoc doc, before;
static unsigned polls;
void platform_poll(void) { ++polls; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static void set(unsigned row, unsigned col, const char *text) {
    CHECK(sheet_set(&doc, row, col, SHEET_AUTO, text, (unsigned)strlen(text)) == 0);
}
static const SheetCell *at(const char *reference) {
    unsigned row, col;
    CHECK(!sheet_reference(reference, (unsigned)strlen(reference), &row, &col));
    return sheet_cell(&doc, row, col);
}
static void value(const char *reference, int32_t expected) {
    const SheetCell *c = at(reference);
    if (c->error || c->value != expected) {
        fprintf(stderr,"%s: error=%s actual=%d expected=%d\n",reference,sheet_error_name(c->error),c->value,expected); exit(1);
    }
}
static void error(const char *reference, unsigned expected) {
    CHECK(at(reference)->error == expected);
    CHECK(at(reference)->value == 0);
}
static void formatted(const char *reference, const char *expected) {
    unsigned n = 999;
    char text[SHEET_TEXT_MAX + 1u];
    CHECK(!sheet_format(at(reference), 0, 0, &n));
    CHECK(n == strlen(expected));
    CHECK(!sheet_format(at(reference), text, sizeof(text), &n));
    CHECK(n == strlen(expected) && !strcmp(text,expected));
}
static void budget(void) {
    sheet_init(&doc);
    set(0,0,"Monthly budget"); set(1,0,"Item"); set(1,1,"Planned"); set(1,2,"Actual"); set(1,3,"Difference");
    set(2,0,"Rent"); set(2,1,"1250"); set(2,2,"1250");
    set(3,0,"Food"); set(3,1,"400.5"); set(3,2,"387.25");
    set(4,0,"Travel"); set(4,1,"110"); set(4,2,"132.75");
    set(2,3,"=B3-C3"); set(3,3,"=B4-C4"); set(4,3,"=B5-C5");
    set(6,0,"Total"); set(6,1,"=SUM(B3:B5)"); set(6,2,"=sum(C3:C5)"); set(6,3,"=B7-C7");
    set(8,1,"=AVG(B3:B5)"); set(9,1,"=MIN(B3:B5)"); set(10,1,"=MAX(B3:B5)");
    set(11,1,"=COUNT(A1:C5)"); set(12,1,"=SUM(B3:C5, 10, (2+3)*4)");
    CHECK(!sheet_recalculate(&doc));
    value("B7",1760500); value("C7",1770000); value("D7",-9500);
    value("B9",586833); value("B10",110000); value("B11",1250000); value("B12",6000); value("B13",3560500);
    formatted("B9","586.833"); formatted("D7","-9.5");
    set(3,2,"377.25"); CHECK(!sheet_recalculate(&doc)); value("D7",500);
    /* Entry order cannot change dependency resolution. */
    sheet_init(&doc); set(0,0,"=B1+1"); set(0,1,"=C1*2"); set(0,2,"12.5");
    CHECK(!sheet_recalculate(&doc)); value("A1",26000); value("B1",25000);
    set(0,2,"2"); CHECK(!sheet_recalculate(&doc)); value("A1",5000);
}
static void references_and_text(void) {
    unsigned row = 77, col = 77;
    char label[5];
    for (unsigned r=0;r<SHEET_ROWS;++r) for(unsigned c=0;c<SHEET_COLS;++c) {
        CHECK(!sheet_label(r,c,label,sizeof(label)));
        CHECK(!sheet_reference(label,(unsigned)strlen(label),&row,&col)); CHECK(row==r && col==c);
    }
    CHECK(!sheet_reference("z128",4,&row,&col)); CHECK(row==127 && col==25);
    const char *bad[]={"A0","A129","AA1","A01","1A","A","$A$1"," A1","A1 ","A-1",""};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        row=71;col=72; CHECK(sheet_reference(bad[i],(unsigned)strlen(bad[i]),&row,&col)==-1); CHECK(row==71&&col==72);
    }
    sheet_init(&doc); set(0,0,"'000125"); set(0,1,"'=SUM(A1:A5)");
    CHECK(at("A1")->kind==SHEET_TEXT && !strcmp(at("A1")->text,"000125"));
    CHECK(at("B1")->kind==SHEET_TEXT && !strcmp(at("B1")->text,"=SUM(A1:A5)"));
    set(0,2,"=A2+5"); set(0,3,"=A1+5"); set(0,4,"=SUM(A1:B2)"); set(0,5,"=COUNT(A1:B2)");
    set(0,6,"=AVG(A2:B2)"); set(0,7,"=MIN(A2:B2)"); set(0,8,"=MAX(A2:B2)");
    CHECK(!sheet_recalculate(&doc)); value("C1",5000); error("D1",SHEET_ERR_VALUE); value("E1",0); value("F1",0);
    error("G1",SHEET_ERR_DIV0); value("H1",0); value("I1",0);
    formatted("A1","000125");formatted("B1","=SUM(A1:A5)");formatted("D1","#VALUE!"); formatted("Z128","");
    CHECK(!sheet_set(&doc,3,3,SHEET_TEXT,"",0)); CHECK(at("D4")->kind==SHEET_TEXT);
    CHECK(!sheet_set(&doc,3,4,SHEET_TEXT,"'abc\t\r\n",7)); CHECK(!sheet_recalculate(&doc));formatted("E4","'abc\t\r\n");
}
static void arithmetic(void) {
    const struct {const char *source; int32_t result;} good[]={
        {"=1+2*3",7000},{"=(1+2)*3",9000},{"=20/2/5",2000},{"=20-2-5",13000},
        {"=--5 + -+2",3000},{"=1/3",333},{"=-1/3",-333},{"=.1*.2",20},
        {"=-(2+3)",-5000},{"= + .5 + 1.",1500},{"=SUM()",0},{"=COUNT()",0},
        {"=AVG(1,2,3)",2000},{"=MIN(4,2,-3)",-3000},{"=MAX(-9,-2)",-2000},
        {"=SUM(1,AVG(2,4),MAX(5,6))*2",20000},{"=SUM(2147483, -2147483, 1)",1000},
        {"-2147483.648",INT32_MIN},{"2147483.647",INT32_MAX},
        {"=-2147483.648",INT32_MIN},{"=AVG(-2147483.648)",INT32_MIN},
        {"=AVG(2147483.647,2147483.647)",INT32_MAX},
        {"=2147483.647*1",INT32_MAX},{"=-2147483.648*1",INT32_MIN},
        {"=1.234000",1234},{"=-0.000",0},{"00001.000",1000},
        {"= 1\t+\n2\r\n",3000}
    };
    sheet_init(&doc);
    for(unsigned i=0;i<sizeof(good)/sizeof(*good);++i){set(0,0,good[i].source);CHECK(!sheet_recalculate(&doc));value("A1",good[i].result);}
    formatted("A1","3");
    set(0,0,"-2147483.648");CHECK(!sheet_recalculate(&doc));formatted("A1","-2147483.648");
    const struct {const char *source; unsigned error;} bad[]={
        {"=1/0",SHEET_ERR_DIV0},{"=AVG()",SHEET_ERR_DIV0},{"=Z129",SHEET_ERR_REF},{"=AA1",SHEET_ERR_REF},
        {"=SUM(B2:A1)",SHEET_ERR_REF},{"=SUM(A1:A129)",SHEET_ERR_REF},{"=SUM(A1:)",SHEET_ERR_REF},
        {"=1+",SHEET_ERR_SYNTAX},{"=",SHEET_ERR_SYNTAX},{"=(1+2",SHEET_ERR_SYNTAX},{"=1 2",SHEET_ERR_SYNTAX},
        {"=SUM(1,)",SHEET_ERR_SYNTAX},{"=FOO(1)",SHEET_ERR_SYNTAX},{"=2^3",SHEET_ERR_SYNTAX},
        {"=1.2345",SHEET_ERR_PRECISION},{"1.000001",SHEET_ERR_PRECISION},
        {"2147483.648",SHEET_ERR_OVERFLOW},{"-2147483.649",SHEET_ERR_OVERFLOW},
        {"=2147483+1",SHEET_ERR_OVERFLOW},{"=2147483*2",SHEET_ERR_OVERFLOW},
        {"=2147483/.001",SHEET_ERR_OVERFLOW},{"=-(-2147483.648)",SHEET_ERR_OVERFLOW},
        {"=SUM(2147483,1)",SHEET_ERR_OVERFLOW},{"=.",SHEET_ERR_SYNTAX}
    };
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i){set(0,0,bad[i].source);CHECK(!sheet_recalculate(&doc));error("A1",bad[i].error);}
    set(0,0,"1e3");CHECK(at("A1")->kind==SHEET_TEXT);
}
static void cycles_and_depth(void) {
    sheet_init(&doc);set(0,0,"=B1");set(0,1,"=C1");set(0,2,"=A1");set(0,3,"=A1+1");set(0,4,"=5");
    CHECK(!sheet_recalculate(&doc));error("A1",SHEET_ERR_CYCLE);error("B1",SHEET_ERR_CYCLE);error("C1",SHEET_ERR_CYCLE);error("D1",SHEET_ERR_CYCLE);value("E1",5000);
    set(0,2,"7");CHECK(!sheet_recalculate(&doc));value("A1",7000);value("D1",8000);
    set(0,0,"=SUM(A1:A2)");CHECK(!sheet_recalculate(&doc));error("A1",SHEET_ERR_CYCLE);
    sheet_init(&doc);
    /* Every possible cell in one forward dependency chain: no C recursion. */
    for(unsigned i=0;i<SHEET_CELLS-1u;++i){char s[8]="=";CHECK(!sheet_label((i+1)/SHEET_COLS,(i+1)%SHEET_COLS,s+1,sizeof(s)-1));set(i/SHEET_COLS,i%SHEET_COLS,s);}
    set(SHEET_ROWS-1,SHEET_COLS-1,"42");CHECK(!sheet_recalculate(&doc));value("A1",42000);value("Z128",42000);
    set(SHEET_ROWS-1,SHEET_COLS-1,"=A1");CHECK(!sheet_recalculate(&doc));
    for(unsigned i=0;i<SHEET_CELLS;++i) CHECK(doc.cells[i].error==SHEET_ERR_CYCLE);
    sheet_init(&doc);set(0,1,"1");
    for(unsigned depth=0;depth<=SHEET_PARSE_DEPTH+1u;++depth){char s[96];unsigned n=0;s[n++]='=';for(unsigned i=0;i<depth;++i)s[n++]='(';s[n++]='1';for(unsigned i=0;i<depth;++i)s[n++]=')';s[n]=0;set(0,0,s);CHECK(!sheet_recalculate(&doc));if(depth<=SHEET_PARSE_DEPTH)value("A1",1000);else error("A1",SHEET_ERR_DEPTH);}
    /* References and functions obey exactly the same structural depth bound. */
    for(unsigned mode=0;mode<2;++mode) for(unsigned depth=SHEET_PARSE_DEPTH;depth<=SHEET_PARSE_DEPTH+1u;++depth){
        char s[96];unsigned n=0;s[n++]='=';
        for(unsigned i=0;i<depth;++i){if(mode){memcpy(s+n,"SUM",3);n+=3;}s[n++]='(';}
        s[n++]='B';s[n++]='1';for(unsigned i=0;i<depth;++i)s[n++]=')';s[n]=0;
        set(0,0,s);CHECK(!sheet_recalculate(&doc));if(depth<=SHEET_PARSE_DEPTH)value("A1",1000);else error("A1",SHEET_ERR_DEPTH);
    }
}
static void atomic_edits_and_output(void) {
    sheet_init(&doc);set(0,0,"123.45");CHECK(!sheet_recalculate(&doc));before=doc;
    char long_text[SHEET_TEXT_MAX+2u];memset(long_text,'x',sizeof(long_text));
    CHECK(sheet_set(&doc,0,0,SHEET_AUTO,long_text,sizeof(long_text))==-1);
    CHECK(sheet_set(&doc,SHEET_ROWS,0,SHEET_AUTO,"x",1)==-1);
    CHECK(sheet_set(&doc,0,SHEET_COLS,SHEET_AUTO,"x",1)==-1);
    CHECK(sheet_set(&doc,0,0,SHEET_AUTO,"a\0b",3)==-1);
    CHECK(sheet_set(&doc,0,0,SHEET_NUMBER,"abc",3)==-1);
    CHECK(sheet_set(&doc,0,0,SHEET_FORMULA,"123",3)==-1);
    CHECK(sheet_set(&doc,0,0,SHEET_EMPTY,"x",1)==-1);
    CHECK(!memcmp(&doc,&before,sizeof(doc)));
    /* In-document source copy uses a complete temporary cell before commit. */
    CHECK(!sheet_set(&doc,0,0,SHEET_TEXT,doc.cells[0].text+1,4));CHECK(!strcmp(doc.cells[0].text,"23.4"));
    CHECK(!sheet_set(&doc,0,1,SHEET_TEXT,doc.cells[0].text,4));CHECK(!strcmp(doc.cells[1].text,"23.4"));
    char out[16]="untouched"; unsigned written=712;
    CHECK(sheet_format(at("A1"),out,4,&written)==-1);CHECK(!strcmp(out,"untouched")&&written==712);
    CHECK(sheet_format(at("A1"),out,sizeof(out),(unsigned *)out)==-1);CHECK(!strcmp(out,"untouched"));
    CHECK(sheet_format(at("A1"),doc.cells[0].text,sizeof(doc.cells[0].text),&written)==-1);
    CHECK(sheet_label(0,0,out,2)==-1);CHECK(!strcmp(out,"untouched"));
    /* Invalid model rejected before any cache is changed. */
    doc.cells[3].length=96;before=doc;CHECK(sheet_recalculate(&doc)==-1);CHECK(!memcmp(&doc,&before,sizeof(doc)));
}
int main(void) {
    CHECK(sizeof(SheetCell)==104u);CHECK(sizeof(SheetDoc)==356148u);
    budget();references_and_text();arithmetic();cycles_and_depth();atomic_edits_and_output();
    CHECK(polls>0);puts("Sheet model tests passed: budget, arithmetic, complete dependency chains, cycles, atomicity");
    return 0;
}
