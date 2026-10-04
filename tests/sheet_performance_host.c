#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "sheet_model.h"
static SheetDoc doc;
static unsigned polls;
void platform_poll(void) { ++polls; }
static void set(unsigned row, unsigned col, const char *s) {
    assert(!sheet_set(&doc,row,col,SHEET_AUTO,s,(unsigned)strlen(s)));
}
static void measure(const char *name) {
    polls=0;
    clock_t start=clock();
    assert(!sheet_recalculate(&doc));
    double ms=1000.0*(double)(clock()-start)/CLOCKS_PER_SEC;
    printf("%s: %.3f ms host CPU; %u cooperative polls\n",name,ms,polls);
}
int main(void) {
    sheet_init(&doc);
    for(unsigned i=0;i<SHEET_CELLS-1u;++i){
        char s[8]="=";
        assert(!sheet_label((i+1u)/SHEET_COLS,(i+1u)%SHEET_COLS,s+1,sizeof(s)-1u));
        set(i/SHEET_COLS,i%SHEET_COLS,s);
    }
    set(SHEET_ROWS-1,SHEET_COLS-1,"42");
    measure("3328-cell forward dependency chain");
    assert(polls >= 20u && polls <= 256u);
    assert(!doc.cells[0].error&&doc.cells[0].value==42000);
    set(SHEET_ROWS-1,SHEET_COLS-1,"=A1");
    measure("3328-cell circular dependency");
    assert(polls >= 20u && polls <= 256u);
    assert(doc.cells[0].error==SHEET_ERR_CYCLE);
    sheet_init(&doc);
    /* 120 ledger rows with five range-based per-row summary columns. */
    for(unsigned r=0;r<120u;++r){
        char s[48];
        set(r,0,"Supplies");set(r,1,"3");set(r,2,"12.345");
        snprintf(s,sizeof(s),"=B%u*C%u",r+1u,r+1u);set(r,3,s);
        set(r,4,"=SUM(D1:D120)");set(r,5,"=AVG(D1:D120)");
        set(r,6,"=MIN(D1:D120)");set(r,7,"=MAX(D1:D120)");set(r,8,"=COUNT(D1:D120)");
    }
    measure("120-row ledger, 720 formulas, 72000 range references");
    assert(polls >= 250u && polls <= 1000u);
    assert(!doc.cells[4].error&&doc.cells[4].value==4444200);
    assert(!doc.cells[5].error&&doc.cells[5].value==37035);
    assert(!doc.cells[8].error&&doc.cells[8].value==120000);
    return 0;
}
