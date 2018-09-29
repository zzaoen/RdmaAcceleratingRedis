# RdmaAcceleratingRedis

## 1 背景

Redis提供了主从策略，在配置文件中指定slaveof配置项或者在redis-cli中执行slaveof指令，就可以构建master-slave模式的Redis服务。下表1-1是我们为一个具有1GB图片缓冲Redis服务器分别构建一个slave和两个slave的耗时情况。

表1-1 Redis构建slave的耗时情况

| Slave number | Time consuming(s) | Bandwidth(MB/s) |
| ------------ | ----------------- | --------------- |
| one          | 98                | 10.4            |
| two          | 174.3             | 5.87            |
| three        | 259.89            | 3.94            |

Redis主从策略的机制是：master收到slave的同步请求后，将内存中所有的key-value写入本地磁盘中，随后master启动一个线程将文件发送给slave，slave接收到文件再读取写入内存。这样，master和slave就拥有了完全一致的数据，不过在master-slave模式下，slave是只读的，只有master可以接收写。

表1-1的结果是master只有1GB的数据时，slave同请求同步到结束的耗时情况。我们相信，当数据量更多以及更多的slave同时请求作为slave时，master-slave模式的性能更差，主要原因如下：

1. master将内存中所有的数据先写入磁盘，然后通过网络发送。数据被写入磁盘，发送时再从磁盘读取，数据拷贝次数很多，同时磁盘的读写性能也是非常差的；

2. 多个slave请求同步的时候，数据都通过TCP网络传输，TCP网络本身开销很大，当出现竞争的时候性能会更差。


我们认为使用RDMA实现master-slave模式可以极大的提高性能，实验的结果表明，我们使用RDMA实现的master-slave模式比Redis自带的master-slave模式性能可以提升35倍-80倍，结果表1-2所示。

表1-2 Redis自带master-slave模式与RDMA实现时间对比

| Slave   number | Origin Time-consuming(s) | RDMA Time-consuming(s) |
| -------------- | ------------------------ | ---------------------- |
| one            | 98                       | 2.8                    |
| two            | 174.3                    | 2.7                    |
| three          | 259.89                   | 3.33                   |











~~Redis本身自带了集群实现方案，通过修改Redis中的配置文件并执行脚本可以将几台独立的Redis服务器组合成一个集群对外提供服务。我们可以集群模式运行redis-cli，或者在其他语言中使用Redis的API接口编写客户端访问Redis集群（比如Jedis），当我们执行set命令向集群中添加一对键值时，程序将这对键值存放在集群的其中一台服务上，也就是说执行set时，这对key-value在系统中只存在一份，即使集群启动的时候指定采用副本，那么一对key-value也只存放在一台Redis服务器和这台服务器对应的副本机器上；当我们执行get命令从集群获取一个key对应的值时，程序帮我们找到这个key所在Redis服务器的，并将value返回给客户端。如图1-1所示：~~



~~图1-1 Redis自带的集群方案~~

~~我们发现，Redis本身的集群方案有一些值得考虑的地方：~~

1. ~~不管集群由多少台机器组成，一对key-value在系统中只存在一份。对于一些应用来说总是存在热点数据，大量的客户端总是访问服务器中一小部分数据。Redis集群方案只是增加了集群的容量，然而并不能为热点数据带来更好的性能；~~

2. ~~Redis服务是单线程的，这意味着Redis服务器只能在响应一个客户端的请求之后才继续响应下一个客户端的请求。在Redis集群中，客户端连接到集群中某一台Redis服务器，如果客户端get指定的key在当前的Redis服务器中没有被检索到，那么这台Redis服务器负责将请求转发给其他的Redis服务器，并在得到数据后返回客户端结果。在某些应用场景，用户可能同时对一个或几个key发起get请求，所有的请求最终都等待同一台服务器返回结果，增加的延时，且单个服务器的压力过大。~~

~~针对上面提到的Redis集群问题，我们准备设计一种新的Redis集群方案，我们称为RCP（Redis Cluster Plus），在RCP集群中，每一台Redis服务器上存储的数据完全相同，客户端不管连接到哪一台Redis服务器上，执行get请求都可以立即得到结果。在RCP集群中，有一台Redis服务器是数据源，我们称为master，其他Redis服务器称为slave。在RCP初始化的时候，master上所有的数据都将同步到slave上，整个集群中每一台Redis服务器都可以独立对外提供服务，当大量的客户端访问RCP集群时，请求分布在集群中所有的Redis服务器，而不需要集中到其中一台，增加了集群并行的能力，可以提升系统的性能。~~~~利用RDMA可以使我们的集群方案~~

~~我们的目的是使用RDMA优化Redis现有的集群方案，我们的主要工作如下：~~

1. ~~我们改变了现有的集群方案，我们希望一个Redis集群中所有的Redis服务器的数据是完全同步的，每台Redis服务器都可以完全独立、对等的对外提供服务。这样，在遇到前面我们描述的应用场景是，Redis集群可以提供更好的性能。我们设计了测试用例，比较了高负载情况下，Redis自带的集群与我们的集群方案的性能。~~
2. ~~我们利用RDMA设计了一个Redis集群中所有Redis服务器数据同步的程序，与使用Socket进行同步的程序进行了性能上的比较。~~
3. ~~我们尝试用一个上层的RDMA库rsocket替换Redis中已有的socket通信代码。~~



~~我们的应用场景来源于将Redis作为图片缓存的服务，比如社交软件。服务器将图片放在磁盘中，客户端访问图片时服务器需要在磁盘中查找到图片，在读取到内存中，经过网络发送给客户端。对于图片服务器来说，图片存在热点问题，一小部分图片占据了主要的访问量，将这些图片加载到基于内存的key-value系统是不错的选择。Redis作为图片缓冲的策略是将图片转换成base64的编码存储在Redis服务器中，客户端访问Redis服务器获取图片的base64编码后在本地转码得到原图。~~



## 2 RDMA master-slave实现方案

~~上面说到我们设计新的Redis服务器集群RCP，在RCP集群初始化的时候，master上存储的所有数据同步到集群中所有的slave机器上。假设现在有一台Redis服务器上存储了超过10000张大小为4MB的图片base64编码，这10000张图片是离线计算得到的热门图片，现在想将这台Redis服务器作为master建立一个有10台机器的RCP集群以对外提供服务得到更好的性能。如何将master上的数据同步到另外9台Redis服务器是一个大问题。~~

~~假设我们采用传统的TCP/IP通信方式，那么同步的过程大概如下：~~

1. ~~9台slave分别于master建立TCP连接；~~
2. ~~master从本地读取一个key-value对，通过socket依次发送给9台slave；~~
3. ~~slave接收到key-value后将其存到本地的库中；~~
4. ~~如果master没有被遍历完，继续步骤2；否则停止。~~

~~TCP/IP的方式看起来简单，但是实际上性能很差，我们在测试的时候发现这样的同步方式需要相当长的时间。第一，TCP/IP网络本身数据拷贝和协议栈的开销；第二多客户端同时通信，master网络竞争激烈；第三，同步操作无法完全异步。除了时间开销之外，在同步的时候，master机器的CPU负载非常高，对master本身运行的其他负载会有影响。~~

~~我们使用RDMA设计了集群同步的方案，实验表明，我们的同步方案与TCP/IP同步相比，在性能上能提升4倍，集群需要同步的slave越多，我们的同步方案优势越明显，因为我们利用RDMA单边操作，所有的slave并行的从master读取数据。~~

我们利用RDMA实现Redis master-slave同步方案，性能可以得到很大提升的主要原因是：

1. master与slave数据的传输通过RDMA read操作。RDMA read是单边操作，所有的slave可以并行从master内存中读取数据，不会造成网络的竞争；
2. master的数据完全不需要写入到磁盘。master在内存中建立了一个mapping table，mapping table是由连续的固定大小的数据区域组成，master将其存储在内存的key-value映射到mapping table中，slave从mapping table获取数据；
3. slave只知道master上mapping table的起始地址，slave通过起始地址加便宜了计算出数据在mapping table上的地址，直接使用RDMA read从master内存区域读取数据。

![1](./pic/data-table-structure.png) 

图1-3 mapping table结构



## 3 How to run

实验的硬件环境和软件环境如下：

| Hardware          | Configuration                          |
| ----------------- | -------------------------------------- |
| CPU               | Intel(R) Core(TM) CPU i7-7700@ 3.60GHz |
| Memory            | 16GB                                   |
| Disk              | TOSHIBA 1T HDD                         |
| Switch            | Mellanox MSX1012B-2BFS 40GE QSFP       |
| Network Interface | Mellanox MCX353A-FCBT 40GbE            |

| Software          | Configuration                                  |
| ----------------- | ---------------------------------------------- |
| OS                | Ubuntu 16.04.3 LTS                             |
| Infiniband Driver | MLNX_OFED_LINUX-4.4-2.0.7.0-ubuntu16.04-x86_64 |
| Redis             | 4.0.11                                         |
| Gcc               | 5.4.0                                          |
| hiredis           | Included in redis 4.0.11                       |

 在本仓库中，src目录下包括三个目录，其中redis目录中包含修改好配置文件的redis源码；redis-init目录先包含的代码用来初始化redis数据库中的数据；rdma目录中包括client和server两个目录，分别用来在slave和master上运行。

Infiniband驱动程序的安装这里不在叙述。用户需要唯一安装的是插件是hiredis，它是一个C语言的redis客户端库，被包含在redis源码中。下面介绍如何安装hiredis。用户需要解压redis目录下的redis-4.0.11.tar.gz，然后进入deps/hiredis目录下编译，安装。

```shell
tar -zxvf redis-4.0.11.tar.gz
cd redis-4.0.11/deps/hiredis/
make 
sudo make install
```

安装完成之后在当前目录会生成一个名字为libhiredis.so的文件，将该文件拷贝到/usr/lib64，然后更新动态链接库缓存。

```shell
sudo cp libhiredis.so /usr/lib64
sudo /sbin/ldconfig
```

到这里，hiredis就安装成功了，我们可以在C语言代码中中使用hiredis头文件就可以操作Redis了。

下面介绍如何运行我们提供的代码得到上文中我们描述的结果。我们假设搭建一个master和两个slave的环境继续宁测试。

### 3.1 master-slave集群

进入redis-4.0.11目录；执行make指令编译redis，然后进入src目录；指定配置文件运行redis-server。

```shell
cd redis-4.0.11
make
cd src
./redis-server ../redis.cfg
```

另外两台机器也按照同样的方式启动redis-server。

现在假设三台机器按照下面的方式配置：

| Name   | IP            | Port |
| ------ | ------------- | ---- |
| master | 192.168.1.100 | 6379 |
| slave1 | 192.168.1.101 | 6379 |
| slave2 | 192.168.1.102 | 6379 |

现在设置slave1同步master数据，在数据同步之前应当先对master数据进行初始化，这部分参考3.2小结。首先在master机器打开新的终端进入redis源码src目录，执行redis-cli连接到slave1上。

```shell
cd redis-4.0.11/src
./redis-cli -h 192.168.1.101 -p 6379
```

如果没有发生异常，此时master已经连接到slave1的Redis服务，接下来执行slaveof指令

```shell
slaveof 192.168.1.100 6379
```

在master和slave1的redis-server程序输出中可以看到master和slave1数据同步相关的日志，如下所示：

![1](./pic/slave.png)

从日志中可以看到slave从开始执行同步到接收到master所有数据并存储到内存中的时间，根据这些信息我们可以计算出同步的性能。

以上是master与一个slave同步数据数据的过程，多个slave与master同步的过程也类似。

### 3.2 master数据初始化

我们设计的RDMA数据同步方案更适合于值比较大且数据比较均匀的场景，为了方便计算带宽和比较性能，我们对master的数据进行了初始化。在src目录下的redis-init目录包含了对master数据初始化的代码。

首先进入redis-init目录，然后执行make指令，最后运行redis-init就可以。在redis-init之前，需要确保master上的redis-server程序已经运行起来。

```shell
cd redis-init
make
./redis-init
```



### 3.3 RDMA 数据同步方案

RDMA数据同步方案的代码分为两部分，分别是src/rdma目录下的server目录和client目录，server目录的代码在master上运行，client目录的代码在slave上运行。

首先在master上编译安装server目录下的代码。进入src目录下的server目录，编译代码并在master运行rdma-server程序。

```shell
cd rdma/server
make
./rdma-server
```

在rdma-server的代码中，我们固定了程序绑定的端口是12345。

rdma-server程序启动后会利用hiredis访问master上Redis服务器中的存放的key-value数据并在内存中建立mapping table。



在slave上编译安装client目录下的代码。进入src目录下的client目录，编译代码并在slave上运行rdma-client程序。

```
cd rdma/client
make
./rdma-client 192.168.0.100 12345
```

rdma-client在运行的时候指定了rdma-server的IP地址和端口，程序运行起来之后，客户端根据master返回mapping table的首地址计算读取数据的地址，发起RDMA read单边请求，直接从master内存读取数据。读取的数据加入到本地的Redist数据库。

所有的slave都按照上面的指令运行就可以与master同步获取所有的数据。