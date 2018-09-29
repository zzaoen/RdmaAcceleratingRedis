# Rdma Accelerating Redis

## 1 Introduction

There is a very simple to use and configure leader follower (master slave) replication because Master-Slave strategy is supported in Redis. It is easy to enable Master-Slave mode in Redis service by specifying `SLAVEOF` in configuration file or run `SLAVEOF` command in the terminal. Table 1-1 shows the time consuming situation for building a different number of slaves for  a Redis server with a 1GB image buffer.

Table 1-1 Time consuming situation of building slaves

| Slave number | Time consuming(s) | Bandwidth(MB/s) |
| ------------ | ----------------- | --------------- |
| one          | 98                | 10.4            |
| two          | 174.3             | 5.87            |
| three        | 259.89            | 3.94            |

**The mechanism of the Redis Master-Slave policy:** Master writes all the pairs of Key-Value in the memory into the local disk after receiving the synchronization request from the slave. Then master starts a thread to send the file to the slave, and the slave receives the file and then writes into the memory. In this way, the master and the slave have have exactly the same data, but in the master-slave mode, the slave is read-only, and only the master can receive the write request.

The result in Table 1-1 is the time-consuming situation in which the slave synchronizes to the end of the request with only 1 GB of data. It demonstrates that when the amount of data is more and more slaves are simultaneously requesting as slaves, the performance of the master-slave mode is even worse. The main reasons are as follows:

1. The master writes all the data in memory to the local disk first, and then sends it through the network. The data is written to the disk, and then read from the disk when sent so I/O operation induces large overhead, and the read and write performance of the disk is also very poor.
2. When multiple slave requests are synchronized, the data is transmitted over the TCP network. The TCP network induces a large overhead, and performance will be worse when there is competition.

RDMA permits high-throughout, low-latency networking, which is especially useful in massively parallel computer clusters. Therefore, implementing the master-slave mode through RDMA can greatly improve performance. The experimental results show that the master-slave mode implemented by RDMA is 35 times-80 times better than the master-slave mode implemented by TCP. Table 1-2 shows the results.

| Slave   number | Origin Time-consuming(s) | RDMA Time-consuming(s) |
| -------------- | ------------------------ | ---------------------- |
| one            | 98                       | 2.8                    |
| two            | 174.3                    | 2.7                    |
| three          | 259.89                   | 3.33                   |

## 2 Redis Cluster Plus

As mentioned above, we designed a new Redis server cluster called RCP. When the RCP cluster is initialized, all the data stored on the master is synchronized to all the slave machines in the cluster. Suppose now that there is a Redis server that stores more than 10,000 images encoded by base64. These 10,000 images are offline. Now we want to build the RCP with 10 machines getting better performance by more available servers. How to synchronize the data from the master node to another 9 slave nodes is a challenge problem.

Suppose we use the traditional TCP/IP communication, the synchronization step is as follows:

1. 9 slaves establish a TCP connection with the master respectively;
2. The master node reads a key-value pair from the local, and sends it to 9 slaves in sequence through the socket;
3. After receiving the key-value, the slave saves it to the local Redis database.
4. If the master has not been traversed, continue with step 2; otherwise stop.

The way TCP/IP looks simple, but in practice the performance is very low, we found that this synchronization method takes a long time to test. First, the TCP/IP network itself has data transmission and protocol stack overhead; Second, multi-client communicates at the same time, the master node network is highly competitive; Third, the synchronous operation cannot be completely asynchronous. In addition to the time overhead, during synchronization, the CPU load on the master machine is very high, affecting other loads running on the master itself.

We used RDMA to design a cluster synchronization scheme. Experiments show that our synchronization scheme can improve performance by 4 times compared with TCP/IP synchronization. The more slaves a cluster needs to synchronize, the more obvious the advantage of our synchronization scheme is because We use RDMA unilateral operation, and all slaves read data from the master in parallel. The process of synchronizing using RDMA is as follows:

1. The master creates a mapping table whose structure is shown in Figure 1-3. The mapping table is divided into two parts. The front part is an 8-byte address area, each area is used to record the address; the latter part is a 4 MB data area for storing data in the Redis server. The address area corresponds to the data area, and the address of the first data area is stored in the first address area;
2. 9 slaves establish an RDMA connection with the master respectively. During the process of establishing the connection, the master sends the starting address of the mapping table to each slave;
3. The slave calculates the address of the data according to the starting address of the mapping table and the offset of the data. The slave uses the calculated address to initiate an RDMA read request, reads a data area data from the master, and stores the data locally. ;
4. If the calculated address does not cross the boundary, continue with step 3; otherwise, end the process.

![1](./pic/data-table-structure.png)

## 3 Building Environment

All the testing work in this article was done under Ubuntu 16.04. The process of manually setting up three Master nodes using the Redis Cluster solution is as follows:

In order to create a cluster, we first need to configure the Redis server to run in cluster mode. So you can use the following configuration script file:

```shell
bind 192.168.1.100
port 7000
cluster-enabled yes
cluster-config-file nodes.conf
cluster-node-timeout 5000
appendonly yes
```

Then create 3 copies of the compiled Redis instance to 3 folders, and start the server according to the above configuration file:

```shell
mkdir cluster-test
cd cluster-test
./src/redis-server ./cluster.conf
```

The cluster can be easily manipulated through a basic program `redis-trib` provided by Redis. This program is written in Ruby and placed in `src/`. Therefore, you need to install the ruby development environment and the corresponding redis interface. After the pre-preparation is completed, you can use the following command to establish a cluster.

```shell
./redis-trib.rb create {ip-1}:7000 {ip-2}:7000 {ip-3}:7000
```

Such a simple command only creates a cluster of three Master Nodes, without any Slave Node for backup.