#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/text_search.c"
int main(void){
    const char *text="One fish\ntwo FISH\nred fish\n";int n=(int)strlen(text),count;
    assert(text_find(text,n,"fish",0,1,0)==4);
    assert(text_find(text,n,"fish",5,1,0)==13);
    assert(text_find(text,n,"fish",5,1,1)==22);
    assert(text_find(text,n,"fish",0,-1,0)==22);
    assert(text_find(text,n,"fish",n,1,0)==4);
    assert(text_find(text,n,"absent",0,1,0)==-1);
    char output[100];
    assert(text_replace_all(output,sizeof output,text,n,"fish","bird",0,&count)==n&&count==3);
    assert(!strcmp(output,"One bird\ntwo bird\nred bird\n"));
    assert(text_replace_all(output,sizeof output,"aaa",3,"aa","b",1,&count)==2&&!strcmp(output,"ba")&&count==1);
    assert(text_replace_all(output,sizeof output,"aaaa",4,"a","",1,&count)==0&&count==4);
    assert(text_replace_all(output,4,"cat",3,"cat","tiger",1,&count)==-1);
    static char large[65536],changed[65536];memset(large,'a',50000);large[50000]=0;
    assert(text_replace_all(changed,sizeof changed,large,50000,"a","b",1,&count)==50000&&count==50000);
    puts("text search: forward/backward wrap, case, replacement, capacity and 50 KB documents passed");
}
