1. Can you send a network diagram with ports/IPs and connectivity of your setup. My understanding is that you used two tests:

​                - 40GE with TCP

​                - 40GE with RDMA (RoCE)

I assume you didn’t use the 1G router mentioned for data transfer but only for management access, right? Just to make sure.



2. The BW you mentioned in table 1-1 is very low. With one slave you reach 10.4 MB/s.

How did you calculate this BW? From your understanding where are the bottlenecks?

 

3. What was the HDD disk writing and reading speed that you used?

Can you run fio to your local disk and send me your output for read and write commands?

 

4. What are the reasons for the massive difference on Table 1-2 (98 seconds when using current TCP file writing method comparing to 2.8 seconds)?

 

5. Figure 1-3 is not well understood.

What is Redis maser-slave and RDMA master-slave?

How did you calculate the BW ?

Why Redis master-slave on one server shows BW of 345 MB/s while the RDMA master-slave shows 10.4 MB/s?

It doesn’t align with your previous finding.

 

6. Table 1-2 comparison declares TCP and RDMA comparison. I wouldn’t call it that way as it doesn’t compare only TCP to RDMA. In the RDMA method you also eliminate the file writing on the master and slave side. It seems a bit misleading to me. I would change the tittle saying compare two methods

Method 1: using TCP and 40GE (make sure that we use this link and not the 1GE) and file writing to disk in the slave and master.

Method 2: using RDMA, 40GE without file writing to disk, only using memory.

 

Those are two differences.

 

7. Why do you think the original implementation of Redis for memory synchronization included writing the file to local disk on the master and slave?

 

8. What version of RoCE did you use? Do you know the differences between RoCEv1 and RoCEv2? What are they?

 

9. What was the switch configuration in your case? Can you send the switch running-config file.

 

10. What do you think would be the difference between TCP and RDMA if you would change the TCP run to transfer the file directly from memory, without using the disk. Can you make this test?

 

11. Can you send me TCP BW rate in your setup (using for example iperf tool)?

 

12. Can you send me RDMA BW rate in your setup, using ib_send_bw or similar tool?

 

13. Why do you think you didn’t reach line-rate ~40Gb/s (~4.5GB/s) on your RDMA test?