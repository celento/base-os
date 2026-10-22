#ifndef TEXT_SEARCH_H
#define TEXT_SEARCH_H
/* Search flags. Passing 0 or 1 alone keeps the older match_case meaning. */
#define TEXT_MATCH_CASE 1
#define TEXT_WHOLE_WORD 2
int text_match(const char *text,int length,int position,const char *query,int flags);
/* Search wraps once; direction is +1 or -1. */
int text_find(const char *text,int length,const char *query,int start,int direction,int flags);
/* Separate output preserves source on capacity failure. Returns length or -1. */
int text_replace_all(char *output,int capacity,const char *text,int length,
                     const char *query,const char *replacement,int flags,int *count);
#endif
