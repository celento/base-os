#include "example_docs.h"
#include "platform.h"

static char *append(char *out, const char *text) {
    while (*text) *out++ = *text++;
    return out;
}
unsigned example_stats_document(char *out, unsigned capacity) {
    static const char intro[] = "BaseOS Document Stats sample\nRead it in Editor or stream it from a C application.\n\n";
    static const char ending[] = "The last line has no newline.";
    char row[] = "Entry 0000: Quiet harbor, bright windows, and 00 little boats.\n";
    unsigned length = sizeof(intro)-1 + 900u*(sizeof(row)-1) + sizeof(ending)-1;
    if (!out || capacity < length) return 0;
    char *next = append(out, intro);
    for (unsigned i=0;i<900;i++) {
        unsigned value=i;
        for (int digit=9;digit>=6;digit--) { row[digit]=(char)('0'+value%10);value/=10; }
        row[46]=(char)('0'+(i%97)/10);
        row[47]=(char)('0'+(i%97)%10);
        next=append(next,row);
        if (!(i&31u)) platform_poll();
    }
    append(next,ending);
    return length;
}
