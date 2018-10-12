# RdmaAcceleratingRedis

## 1 背景

Redis提供了主从策略，在配置文件中指定slaveof配置项或者在redis-cli中执行slaveof指令，slave机器上的Redis服务器会获取master机器上Redis服务器中所有的数据。

Redis主从复制的过程如下：

1. slave执行`slaveof`指令，向master发起请求；

2. master收到slave的同步请求后，将内存中所有的key-value写入本地磁盘。随后master启动一个线程将文件发送给slave；

3. slave接收到文件，读取文件内容，写入本地的Redis服务器。

同步完成之后，master和slave就拥有了完全一致的数据。在整个流程中，第2步有两点值得注意：

1. master将内存中的数据写入磁盘，然后发送这个文件，增加了往磁盘写数据和数据复制的开销；
2. 由master将磁盘文件发送给所有的客户端。

首先，数据写入磁盘非常耗时，如果master的Redis服务器中存储了相当多的数据，这个写磁盘的过程是难以接受的。我们测试发现，master网络性能较好时，slave和master同步的过程中写磁盘的时间占据了整个时间的一半以上；其次，master将文件发送给slave，当大量的slave同时请求，master的网络负载增加，网络传输性能降低。



~~下表1-1是我们为一个具有1GB图片缓冲Redis服务器分别构建一个slave和两个slave的耗时情况。~~

~~表1-1 Redis构建slave的耗时情况~~

| ~~Slave number~~ | ~~Time consuming(s)~~ | ~~Bandwidth(MB/s)~~ |
| ---------------- | --------------------- | ------------------- |
| ~~one~~          | ~~98~~                | ~~10.4~~            |
| ~~two~~          | ~~174.3~~             | ~~5.87~~            |
| ~~three~~        | ~~259.89~~            | ~~3.94~~            |



~~表1-1的结果是master只有1GB的数据时，slave同请求同步到结束的耗时情况。我们相信，当数据量更多以及更多的slave同时请求作为slave时，master-slave模式的性能更差，主要原因如下：~~

1. ~~master将内存中所有的数据先写入磁盘，然后通过网络发送。数据被写入磁盘，发送时再从磁盘读取，数据拷贝次数很多，同时磁盘的读写性能也是非常差的；~~

2. ~~多个slave请求同步的时候，数据都通过TCP网络传输，TCP网络本身开销很大，当出现竞争的时候性能会更差。~~

针对前面我们提到的两个问题，我们使用RDMA实现了新的master-slave同步方案，

1. master在内存中为Redis中的数据建立一个mapping table，节省了master将数据写入磁盘的时间开销，同时利用mapping table，可以充分利用RDMA的性能。
2. 利用RDMA单边操作的特性，数据同步的过程交给slave利用RDMA read读取mapping table的数据，当存在多个slave同时请求同步时，多个slave利用RDMA并且读取master mapping table的数据，不会造成master性能上的压力。



~~我们认为使用RDMA实现master-slave模式可以极大的提高性能，实验的结果表明，我们使用RDMA实现的master-slave模式比Redis自带的master-slave模式性能可以提升35倍-80倍，结果表1-2所示。~~

~~表1-2 Redis自带master-slave模式与RDMA实现时间对比~~

| ~~Slave   number~~ | ~~Origin Time-consuming(s)~~ | ~~RDMA Time-consuming(s)~~ |
| ------------------ | ---------------------------- | -------------------------- |
| ~~one~~            | ~~98~~                       | ~~2.8~~                    |
| ~~two~~            | ~~174.3~~                    | ~~2.7~~                    |
| ~~three~~          | ~~259.89~~                   | ~~3.33~~                   |



## 2 实验环境和结果

由于实验室没有Gb的以太网交换机，只有一个100Mb的以太网路由器，我们测试多台slave同时请求同步的时候，机器之间使用路由器连接在一起。为了测试使用更好的以太网时的性能，我们不使用路由器，将以太网网卡直接利用网线连接。测试时环境如下：

1. 以太网连接，使用路由器；
2. 以太网连接，不使用路由器，直接连接；
3. RoCE；
4. RDMA Verbs。

我们使用一些测试工具测试了相关硬件设备的性能：

| 设备             | 测试工具 | 带宽(MB/s) |
| ---------------- | -------- | ---- |
| 磁盘             | fio      | 96.97 |
| TCP using router | iperf | 11.78 |
| TCP direct       | iperf | 117.8 |
| RoCE | iperf | 190.72 |
| RDMA | ib_read_bw | 5832.04 |

我们针对不同的网络情况，测试了主从复制从开始执行到完成的时间，其中master的Redis服务器中一共存储了937MB的数据，具体测试结果如下。

| 网络类型         | 测试工具 | 带宽(MB/s) |
| ---------------- | -------- | ---- |
| TCP using router | iperf | 11.78 |
| TCP direct       | iperf | 117.8 |
| RoCE | iperf | 190.72 |
| RDMA | ib_read_bw | 5832.04 |




## 3 RDMA master-slave实现方案

我们利用RDMA实现Redis master-slave同步方案，性能可以得到很大的提升，主要原因如下：

1. master与slave数据的传输通过RDMA read操作。RDMA read是单边操作，所有的slave可以并行从master内存中读取数据，不会造成网络的竞争；
2. master的数据完全不需要写入到磁盘。master在内存中建立了一个mapping table，mapping table是由连续的固定大小的数据区域组成，master将其存储在内存的key-value映射到mapping table中，slave从mapping table获取数据。具体的流程如图1-2所示；
3. slave只知道master上mapping table的起始地址，slave通过起始地址加便宜了计算出数据在mapping table上的地址，直接使用RDMA read从master内存区域读取数据。

![1](./pic/communication.png)

图1-2 slave利用mapping table从master读取数据



RDMA实现的master-slave同步方案在性能上有突出的表现，并且master与slave同步的性能不会受到slave数量的影响，这受益于RDMA read单边操作。在Redis master-slave模型中，由master将磁盘文件发送给所有的slave，slave数量越多，master的网络压力越大，传输的性能也越差。但是RDMA master-slave将获取数据的任务交给了slave，利用RDMA单边操作、kernel-bypass的特性，即使slave数量不断的增加，数据同步的性能几乎不会受到任何影响。图1-3展示了Redis与我们用RDMA实现的master-slave模式带宽比较，图1-4是两种模式进行数据同步是系统的负载情况。

![1](./pic/bandwidth.png)

图1-3两种master-slave操作带宽比较



图中看到RDMA master-slave模式带宽只有345MB/s，原因是我们与Redis master-slave计算带宽的方法一致，用总的传输数据量处于同步总耗时，总耗时包括数据传输的时间以及slave收到数据后添加进本地Redis服务器的时间，实际上如果单纯计算数据传输的时间的话，我们在实验中测试只结束数据不添加进Redis服务器的带宽是上图的两倍左右。





## 4 How to run

实验的硬件环境和软件环境如下：

| Hardware                     | Configuration                          |
| ---------------------------- | -------------------------------------- |
| CPU                          | Intel(R) Core(TM) CPU i7-7700@ 3.60GHz |
| Memory                       | 16GB                                   |
| Disk                         | TOSHIBA 1T HDD                         |
| Infiniband Switch            | Mellanox MSX1012B-2BFS 40GE QSFP       |
| Infiniband Network Interface | Mellanox MCX353A-FCBT 40GbE            |
| Ethernet Router              | Tplink                                 |
| Ethernet Network Interface   | Realtek PCIe GBE                       |

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

安装完成之后在当前目录会生成一个名字为libhiredis.so的文件，将该文件拷贝到/usr/lib64，如果/usr/lib64目录不存在，则拷贝到/usr/lib目录。然后更新动态链接库缓存。

```shell
sudo cp libhiredis.so /usr/lib64
sudo /sbin/ldconfig
```

到这里，hiredis就安装成功了，我们可以在C语言代码中中使用hiredis头文件就可以操作Redis了。

下面介绍如何运行我们提供的代码得到上文中我们描述的结果。我们假设搭建一个master和两个slave的环境继续宁测试。

### 4.1 master-slave集群

进入redis-4.0.11目录；执行make指令编译redis，然后进入src目录；指定配置文件运行redis-server。

```shell
cd redis-4.0.11
make
cd src
./redis-server ../redis.conf
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

### 4.2 master数据初始化

我们设计的RDMA数据同步方案更适合于值比较大且数据比较均匀的场景，为了方便计算带宽和比较性能，我们对master的数据进行了初始化。在src目录下的redis-init目录包含了对master数据初始化的代码。

首先进入redis-init目录，然后执行make指令，最后运行redis-init就可以。在redis-init之前，需要确保master上的redis-server程序已经运行起来。

```shell
cd redis-init
make
./redis-init
```



### 4.3 RDMA 数据同步方案

RDMA数据同步方案的代码分为两部分，分别是src/rdma目录下的server目录和client目录，server目录的代码在master上运行，client目录的代码在slave上运行。

在运行RDMA代码之前，master和slave都需要先将Redis服务器启动。

```shell
cd redis-4.0.11/src
./redis-server
```

master和slave都知道执行redis-server程序即可，不需要指定配置文件。这里假设master上的redis-server数据已经初始化，如果没有，参考3-2小节。

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