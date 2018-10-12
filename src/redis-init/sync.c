#include <hiredis/hiredis.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Initial redis server's content 
 * 
 */
#define LEN 1024 * 1024 * 4
#define KEY_COUNT 256
clock_t time_start, time_end;

int main(){
    redisContext* redis_conn_remote = redisConnect("192.168.0.130", 6379); 
    //redisContext* redis_conn = redisConnect("192.168.1.101", 6379); 
    redisContext* redis_conn_local = redisConnect("127.0.0.1", 6379); 
    if(redis_conn_remote->err)   
        printf("connection error:%s\n", redis_conn_remote->errstr); 
    int start = 0;
    // char arr[256][4 * 1024];
    time_start = clock();
    redisReply* reply = NULL;
    while(start++ < KEY_COUNT){
        reply = redisCommand(redis_conn_remote, "get %d", start);
        redisCommand(redis_conn_local, "set %d %s", start, reply->str);
        //memcpy(arr[start-1], reply->str, strlen(reply->str));
    }
    time_end = clock();
    double duration = (double)(time_end - time_start) / CLOCKS_PER_SEC;
    printf("\ntime:%fs\n", duration);
    return 0;
}
