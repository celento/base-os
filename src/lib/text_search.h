#ifndef TEXT_SEARCH_H
#define TEXT_SEARCH_H
int text_match(const char *text,int length,int position,const char *query,int match_case);
/* Search wraps once; direction is +1 or -1. */
int text_find(const char *text,int length,const char *query,int start,int direction,int match_case);
/* Separate output preserves source on capacity failure. Returns length or -1. */
int text_replace_all(char *output,int capacity,const char *text,int length,
                     const char *query,const char *replacement,int match_case,int *count);
#endif
