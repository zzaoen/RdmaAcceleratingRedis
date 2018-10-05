我们的实验环境网络结构如下：

![](pic\Ethernet-archi.jpg)

图1-1 基于TCP的redis master-slave同步网络环境

![](pic\RDMA-archi.jpg)

图1-2 基于RDMA的redis master-slave同步网络环境

上面的两幅图中都是一个master与两个slave，在以太网环境下，所有的机器都与路由器相连，在RDMA环境下，所有的机器都与Mellanox交换机相连。确实，以太网的硬件参数与RDMA相差很多，原因是我们实验室没有Gb的以太网交换机，我们的实验需要测试多台机器，所以只能选择使用路由器。如果只是一台master以一台slave进行同步，机器可以使用网线直连，TCP网络性能会提升，后面我会给出数据。

所以，先回答问题1、5、6、8：

对于问题1，我们的两组对比实验的网络结构分别是：

-100Mb with TCP

-40GE with RDMA verbs（Not RoCE）



对于问题5和6，

redis master-slave是指redis本身的slaveof指令实现master与slave数据同步；

RDMA master-slave是指我们使用RDMA实现的master与slave数据同步。

问题6说的图1-2的比较的是：

method1: using TCP(100Mb) and file writing to disk in the slave and master.

method2: using RDMA(56Gb) without file writing to disk, only using memory. 



对于问题8，关于RoCE。在之前的比较中，我们只关注了redis自带的和我们用RDMA实现了这两种master-slave数据同步方案的比较，我们并没有测试RoCE。我在相关文章上看过RoCE的介绍，但是并没有对RoCE认识并不多，我对RoCE的认识是我们利用Mellanox硬件，在使用网络时指定Mellanox网卡的IP地址用来通信，比如scp指令传输文件是指定Mellanox网卡的IP地址，那么文件传输就是RoCE，不过我并不确定自己理解的对不对。

![](pic\RoCE.png)



根据您的问题，我补充了两个实验，一个master和一个slave的情况

- master与slave的以太网网卡直接相连数据同步，不经过路由器
- master与slave通过RoCE数据同步(如果我上面理解的RoCE正确的话)

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





问题7提到为什么Redis的master-slave机制需要将内存数据写入到磁盘。这个是我们参考网上关上资料确定的，在Redis运行日志中也可以确定master收到slave的请求先将内存数据写入本地磁盘。![tcp-direct-master-log](pic\answer\tcp-direct-master-log.png)

图1-3 master和slave数据同步时，master的日志输出

从日志我们可以看到，slave请求同步后，master





接下来，我将详细说明我们的实验结果以及原因。

 

