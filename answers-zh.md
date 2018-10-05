我们的实验环境网络结构如下：

![](pic\Ethernet-archi.jpg)

图1-1 基于TCP的redis master-slave同步网络环境

![](pic\RDMA-archi.jpg)

图1-2 基于RDMA的redis master-slave同步网络环境

上面的两幅图中都是一个master与两个slave，在以太网环境下，所有的机器都与路由器相连，在RDMA环境下，所有的机器都与Mellanox交换机相连。确实，以太网的硬件参数与RDMA相差很多，原因是我们实验室没有Gb的以太网交换机。我们的实验需要测试多台机器，所以只能选择使用路由器。如果只是一台master以一台slave进行同步，机器可以使用网线直连，TCP网络性能会提升，后面我会给出新的测试数据。



先回答问题1、2、5、6、8：

对于问题1，我们的两组对比实验的网络结构分别是：

-100Mb with TCP

-40GE with RDMA verbs（Not RoCE）



对于问题2和5

redis master-slave是指redis本身的slaveof指令实现master与slave数据同步；

RDMA master-slave是指我们使用RDMA实现的master与slave数据同步。

在readme的图1-3我们确实标错了数据，非常抱歉。我们在报告中使用了带宽来进行比较，实际上我们不应该使用带宽的。在报告中，我们的带宽是通过总传输的数据量除以同步完成时间计算得到的，所以直接使用时间来进行比较更合适。非常抱歉。



问题6说的图1-2的比较的是：

method1: using TCP(100Mb) and file writing to disk in the slave and master.

method2: using RDMA(56Gb) without file writing to disk, only using memory. 



对于问题8，关于RoCE。在之前的比较中，我们只关注了redis自带的和我们用RDMA实现了这两种master-slave数据同步方案的比较，我们并没有测试RoCE。我在相关文章上看过RoCE的介绍，但是并没有对RoCE认识并不多，我对RoCE的认识是我们利用Mellanox硬件，在使用网络时指定Mellanox网卡的IP地址用来通信，比如scp指令传输文件是指定Mellanox网卡的IP地址，那么文件传输就是RoCE，不过我并不确定自己理解的对不对。

![](pic\RoCE.png)



根据您的问题，我重新做了几个实验，以下所有的测试都是一个master和一个slave的情况

- master与slave的以太网网卡经过路由器相连
- master与slave的以太网网卡直接相连，不经过路由器
- master与slave通过RoCE数据同步(如果我上面理解的RoCE正确的话)
- master与slave通过RDMA read数据同步

实验结过如下：

| Connection | TCP Using Router | TCP Direct | RoCE  | RDMA |
| ---------- | ---------------- | ---------- | ----- | ---- |
| Time(s)    | 98               | 24         | 14.15 | 2.8  |

 





现在，我将给出我们实验环境的一些数据，回答问题2，11和12。

fio测试的本地磁盘的数据如下：

![](pic\answer\fio.png)



iperf测试的TCP网络数据如下：

![](pic\answer\iperf-tcp-router.png)

上面这个是使用路由器之后iperf的测试结果。

![rdma-master-log](pic\answer\iperf-direct-tcp.png)

上面这个是没有使用路由器，网卡直接连接的iperf测试结果。



ib_send_bw和ib_read_bw数据如下：

![ib_send_bw](pic\answer\ib_send_bw.png)

![ib_read_bw](pic\answer\ib_read_bw.png)







问题7提到为什么Redis的master-slave机制需要将内存数据写入到磁盘。这个是我们参考网上关上资料确定的，在Redis运行日志中也可以确定master收到slave的请求先将内存数据写入本地磁盘。![tcp-direct-master-log](pic\answer\tcp-router-master-log.png)

图1-3 TCP using router，数据同步时master的日志输出

![tcp-router-slave-log](pic\answer\tcp-router-slave-log.png)

图1-4 TCP using router，数据同步时slave的日志输出

从日志我们可以看到，slave请求同步后，master查看是否可以进行resynchronization（如果slave之前与master同步过可以resynchronization），如果不可以进行resynchronization，master将数据写入到磁盘文件，随后文件发送给slave。slave收到文件后加载到内存中。

问题4和10的答案也可以从这里得到答案。

![rdma-master-log](pic\answer\tcp-direct-master-log.png)

图1-5 TCP Direct，数据同步时master的日志输出

![rdma-master-log](pic\answer\roce-master-log.png) 图1-6 RoCE，数据同步时master的日志输出

图1-3、图1-5和图1-6都是master与slave数据同步是master的日志

- 图1-3使用的是带路由器的TCP
- 图1-5是不带路由器，直连的TCP
- 图1-6使用的是RoCE。

我们整理一下数据，可以看到：

| Connection         | TCP Using Router | TCP Direct | RoCE  |
| ------------------ | ---------------- | ---------- | ----- |
| total time(s)      | 97.21            | 24.06      | 24.14 |
| file write time(s) | 13.52            | 13.36      | 13.48 |
| transfer time(s)   | 83.69            | 10.7       | 10.66 |

我们编写了一个C程序，它的功能就是从一个Redis服务器（master）get值，并立即set到本地的Redis服务器（slave）中，读取的数据量与之前测试使用的数据量同样。这个程序的功能类似于master需要将数据写入本地磁盘，而之间将内存中数据同步给slave。代码如下，

```c
#define KEY_COUNT 256
clock_t time_start, time_end;
int main(){
    redisContext* redis_conn = redisConnect("192.168.1.102", 6379); 
    redisContext* redis_conn_local = redisConnect("127.0.0.1", 6379); 
    if(redis_conn->err)   
        printf("connection error:%s\n", redis_conn->errstr); 
    int start = 0;
    time_start = clock();
    redisReply* reply = NULL;
    while(start++ < KEY_COUNT){
        reply = redisCommand(redis_conn, "get %d", start);
        redisCommand(redis_conn_local, "set %d %s", start, reply->str);
    }
    time_end = clock();
    double duration = (double)(time_end - time_start) / CLOCKS_PER_SEC;
    printf("time: %fs\n", duration);
    return 0;
}
```

我们调整了网络环境，得到了使用TCP Using Router、TCP Direct和RoCE：

| connection | TCP Using Router | TCP Direct | RoCE | RDMA |
| ---------- | ---------------- | ---------- | ---- | ---- |
| time(s)    | 6.81             | 2.92       | 2.58 | 2.8  |

所以，根据上面的测试，我们结论是：

- slaveof指令将数据写入磁盘是一个耗时的操作，当网络性能好的时候，这种开销几乎要占据一半的时间；
- 在去掉master写文件和传输文件这一过程之后，TCP using router、TCP Direct和RoCE的master和slave的同步性能得到很大的提升，并且RoCE甚至好于RDMA；

不过以上的实验都是在一台master和一台slave上测试的，我们相信在使用更多机器进行测试的时候，TCP的测试结果会更差一些。

虽然目前来看，我们的RDMA程序在一个master和一个slave的数据同步结果并不满意，我们认为在更多机器进行数据同步的时候，我们的使用RDMA read设计的程序会体现出优势。因为我们的设计理念是master为数据建立mapping table，其他所有的slave利用RDMA read直接从master获取数据，slave负责计算如何获取数据，而master几乎不需要任何干预。利用RDMA read单边操作的优势，不管有多少个slave同时向master请求数据，我们的性能都能保持稳定。

在上面的结果中，使用RoCE比我们写的RDMA read要快，主要原因是我们的RDMA代码中出现了耗时操作。当slave机器使用RDMA read从master读取到一个value的数据后，在slave需要执行Redis的set命令将数据添加到本地Redis服务器中。但是在实验中我们发现，RDMA read的数据必须被拷贝到一个buf中，而且当读取的数据操作1MB大小的时候，memcpy函数拷贝的数据总是出现错误数据。我们只能在代码中一个字符一个字符的拷贝。

```c
#define ARR_LEN 1024 * 1024 * 4
while(j < ARR_LEN)
{
	s[j]=(unsigned char)*((dis + j));
	j++;
}
```

这个耗费的非常多的时间，如果我们的RDMA代码不执行这个循环，只是从master读取数据，整个流程的耗时是1.02s，性能可以提高63.6%。但是我们目前还没有找到解决的办法。



问题13是问我们的RDMA read为什么没有达到硬件环境应有的带宽，我们的硬件环境最大带宽是3180MB/s，如果在我们的程序中去掉上面我们说的数据拷贝的代码，我们的程序完成数据传输的时间是0.0082s，带宽是2754.92MB/s。我们目前依然也在做实验，希望可以解决造成性能不是特别高的问题。







 

