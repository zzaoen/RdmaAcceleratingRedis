# RdmaAcceleratingRedis

## 1 背景

Redis本身自带了集群实现方案，通过修改Redis中的配置文件并执行脚本可以将几台独立的Redis服务器组合成一个集群对外提供服务。当我们以集群模式运行redis-cli，或者在其他语言中使用更加简单的方式使用客户端访问Redis集群（比如Jedis），我们执行set命令向集群中添加一对键值时，程序将这对键值存放在集群的其中一台服务上，也就是说执行set时，这对键值在系统中只存在一份；当我们执行get命令从集群获取一个key对应的值时，程序帮我们找到这个key所在Redis服务器的，并将value返回给客户端。

我们发现，在上面所描述的Redis集群方案有一些值得考虑的地方，比如：

1. 不管集群由多少台机器组成，一对key-value在系统中只存在一份。对于一些应用来说，热点数据可能是非常集中的，大量的客户端总是访问少量的热点数据。Redis集群并不能解决这样的问题，因为热点数据在集群中也只有一份。
2. 我们知道Redis服务是单线程的，这意味着Redis服务器只能在响应一个客户端的请求之后才继续请求下一个客户端的请求。Redis集群在执行客户端get指令的时候，如果当前响应客户端的Redis服务器中不存在指定的key，那么这个Redis服务器会重定向到存在key的Redis服务器上。也就是说，当客户端请求的是同一个key的时候，Redis集群将这些请求都转发给存在key的服务器来处理，造成单个服务器的压力过大。

我们的目的是使用RDMA优化Redis现有的集群方案，我们的主要工作如下：

1. 我们改变了现有的集群方案，我们希望一个Redis集群中所有的Redis服务器的数据是完全同步的，每台Redis服务器都可以完全独立、对等的对外提供服务。这样，在遇到前面我们描述的应用场景是，Redis集群可以提供更好的性能。我们设计了测试用例，比较了高负载情况下，Redis自带的集群与我们的集群方案的性能。
2. 我们利用RDMA设计了一个Redis集群中所有Redis服务器数据同步的程序，与使用Socket进行同步的程序进行了性能上的比较。
3. 我们尝试用一个上层的RDMA库rsocket替换Redis中已有的socket通信代码。



​	我们的应用场景来源于将Redis作为图片缓存的服务，比如社交软件。服务器将图片放在磁盘中，客户端访问图片时服务器需要在磁盘中查找到图片，在读取到内存中，经过网络发送给客户端。对于图片服务器来说，图片存在热点问题，一小部分图片占据了主要的访问量，将这些图片加载到基于内存的key-value系统是不错的选择。Redis作为图片缓冲的策略是将图片转换成base64的编码存储在Redis服务器中，客户端访问Redis服务器获取图片的base64编码后在本地转码得到原图。



## 2 RDMA集群备份策略

我们使用RDMA将一个Redis服务器中所有的键值对备份到集群中其他的机器上，使得这个集群中的Redis服务器数据保持一致。这样的集群对外提供服务，在高并发的情景下可以使得集群更高的性能。

作为对比，我们可以先看看基于Socket的集群备份策略是如何实现的。

现在假设我们有3台Redis服务器，分别为server1、server2和server3，其中server1已经存储了许多图片缓存，我们希望将server2和server3作为server1的热备份，使用Socket将server1中所有的键值对信息发送给server2和server3，并让server2和server3把收到的数据存储在自己的内存中。具体流程如下：

1. server2和server3分别于server1建立Socket连接；
2. server1遍历本地Redis库，使用get得到数据，将得到的键值信息依次发送给server2和server3；
3. server2和server3收到数据，分别执行set将数据插入到本地的Redis库中。

第2步是耗时操作，





## 3 环境搭建

本文所有的测试工作均在Ubuntu 16.04下完成。

使用Redis集群方案手动地建立3台Master节点的搭建过程如下：

### 3.1 在集群模式下创建Redis实例

为了创建一个cluster，我们首先要配置Redis server运行在集群模式下。因此可以用以下的配置脚本文件：

```shell
bind 192.168.1.100	#这个参数根据每个Node的IP有所不同
port 7000
cluster-enabled yes	#启用集群模式
cluster-config-file nodes.conf
cluster-node-timeout 5000
appendonly yes
```

然后建立3份编译好的Redis实例拷贝到3个文件夹隔离，并分别按照上述的配置文件启动server：

```shell
mkdir cluster-test
cd cluster-test
./src/redis-server ./cluster.conf	#按照上述配置文件启动server
```

### 3.2 用Redis实例建立集群

通过Redis提供的一个基础程序`redis-trib`可以方便地对集群进行操作，这个程序使用Ruby编写，放置在`src/`中。因此需要安装ruby的开发环境和对应的redis接口。前置准备工作完成后可以通过以下命令建立集群。

```shell
./redis-trib.rb create 127.0.0.1:7000 27.0.0.1:7001 127.0.0.1:7002 127.0.0.1:7003 127.0.0.1:7004 127.0.0.1:7005
```

这样的简单命令只创建了3台Master Node构成的集群，没有任何备份用的Slave Node。

## 4 Redis集群工作原理简述

Redis cluster是多个Redis实例组成的一个整体，Redis用户只关心他所存储的数据集合，不关心数据在这个集群中是怎么被放置的。Redis cluster具有Master节点互相连接、集群消息通过集群总线通信、节点与节点之间通过二进制协议通信、客户端和集群节点之间通过文本协议进行等特点。

### 4.1 Redis集群分区实现原理

Redis cluster中有许多不同编号的slot，这些slot是虚拟的。cluster工作的时候，每个Master节点负责一部分slot，当有某个key映射到某个Master负责的slot后，这个Master负责为这个key提供服务。

Master节点维护一个位序列，Master节点用bit来标识对于某个slot是否属于自己管理。在Redis cluster中，我们拥有16384个slot，这个数是固定的，我们存储在Redis cluster中的所有的key都被映射到这些slot中，key到slot的基本映射算法如下：

```shell
HASH_SLOT = CRC16(key) mod 16384 
```

### 4.2 重定向客户端

Redis cluster并不会代理查询，那么如果客户端访问了一个key不存在的节点，那么客户端就会接收到一条信息，告诉客户端想要的slot所在真正的Master node。



## 4 实验结果