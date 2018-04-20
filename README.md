# LinuxKernelSourceAnalyze
Here are some system call source code analyze based on Linux2.6, include<br>
#### linux process data structure
struct task_struct<br>
struct thread_info<br>
struct mm_struct<br>
struct vm_area_struct<br>
struct wait_queue..
#### file-system data structure
vfs<br>
mapping layer<br>
generic block layer<br>
I/O scheduler layer<br>
driver<br>
hard disck..
#### TCPIP data structure
struct tcp_sock<br>
struct inet_sock<br>
struct sock<br>
struct sock_common<br>
struct sk_buf<br>
struct rtable<br>
struct dst_entry..
#### read/write for file-system
read/write for direct IO/indirect IO<br>
epoll_create<br>
epoll_ctl<br>
epoll_wait..
#### send for TCPIP
user space process->socket layer->transport control layer->network layer->network interface card.<br>
send()<br>
sock_sendmsg()<br>
tcp_sendmsg()<br>
tcp_write_xmit()<br>
ip_queue_xmit()<br>
ip_output()<br>
e1000_xmit_frame()<br>
e1000_tx_queue()
#### recv for TCPIP
network interface card->interrupt context->network layer->transport control layer, socket layer->user space process.<br>
e1000_intr()<br>
netif_receive_skb()<br>
ip_rcv()<br>
tcp_v4_rcv()<br>
tcp_v4_do_rcv()<br>
skb_queue_head()<br>
tcp_recvmsg()<br>
sock_recvmsg()<br>
recv()
## AliOS-things
#### task_and_ipc.vsd
任务数据结构、状态转换、消息队列RingBuffer、IPC相关
#### kernel
内核源码中文注释
