#include "text_search.h"
static int length_of(const char *text){int n=0;while(text[n])n++;return n;}
static char folded(char c,int exact){return !exact&&c>='A'&&c<='Z'?(char)(c+32):c;}
int text_match(const char *text,int length,int position,const char *query,int match_case){
    int size=length_of(query);
    if(!size||position<0||position>length||size>length-position)return 0;
    for(int i=0;i<size;i++)if(folded(text[position+i],match_case)!=folded(query[i],match_case))return 0;
    return 1;
}
int text_find(const char *text,int length,const char *query,int start,int direction,int match_case){
    int size=length_of(query),last=length-size;
    if(!size||last<0)return -1;
    if(start<0)start=last;
    if(start>last)start=0;
    int position=start;
    do {
        if(text_match(text,length,position,query,match_case))return position;
        position+=direction<0?-1:1;
        if(position<0)position=last;
        if(position>last)position=0;
    }while(position!=start);
    return -1;
}
int text_replace_all(char *output,int capacity,const char *text,int length,
                     const char *query,const char *replacement,int match_case,int *count){
    int query_length=length_of(query),replacement_length=length_of(replacement);
    int position=0,written=0,replaced=0;
    if(!query_length||capacity<1)return -1;
    while(position<length){
        if(text_match(text,length,position,query,match_case)){
            if(replacement_length>capacity-1-written)return -1;
            for(int i=0;i<replacement_length;i++)output[written++]=replacement[i];
            position+=query_length;replaced++;
        }else{
            if(written>=capacity-1)return -1;
            output[written++]=text[position++];
        }
    }
    output[written]=0;if(count)*count=replaced;return written;
}
