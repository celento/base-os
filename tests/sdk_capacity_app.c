#include "baseos.h"
static unsigned char data[BOS_DOCUMENT_MAX];
int main(void){
    const char *path="/Documents/capacity.txt";
    for(unsigned i=0;i<sizeof data;i++)data[i]=(unsigned char)(i*13u+7u);
    /* On a deliberately full, otherwise valid volume, the replacement must
     * leave the original complete document available to the same application. */
    if(bos_replace_file(path,data,sizeof data)!=-1)return 1;
    if(bos_file_size(path)!=4)return 2;
    if(bos_read_file(path,data,4)!=4)return 3;
    if(data[0]!='k'||data[1]!='e'||data[2]!='e'||data[3]!='p')return 4;
    if(bos_replace_file("/Documents/no-room.txt",data,1)!=-1)return 5;
    if(bos_file_size("/Documents/no-room.txt")!=-1)return 6;
    return 0;
}
