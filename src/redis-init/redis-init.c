#include <hiredis/hiredis.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Initial redis server's content 
 * 
 */
#define LEN 1024 * 1024 * 4
#define KEY_COUNT 256


int main(){
    FILE *fp;
    fp = fopen("./picture-base64", "rb");
   
    char buf[LEN];
    fread(buf, 1, LEN, fp);

    redisContext* redis_conn = redisConnect("127.0.0.1", 6379); 
    if(redis_conn->err)   
        printf("connection error:%s\n", redis_conn->errstr); 

    int start = 0;
    while(start++ < KEY_COUNT){
        redisCommand(redis_conn, "set %d %s", start, buf);
    }

    fclose(fp);
    return 0;
}
