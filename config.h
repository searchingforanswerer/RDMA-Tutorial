#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdbool.h>
#include <inttypes.h>

// ============= 应用配置信息结构体 =============
// 存储从命令行或配置文件解析得到的应用参数
struct ConfigInfo {
    // --------- 角色配置 ---------
    
    bool is_server;
    // 描述：标识当前节点的角色
    // 取值：true = server端，false = client端
    // 作用：控制程序的执行流程（是监听还是连接）
    
    // --------- 数据传输配置 ---------
    
    int  msg_size;
    // 描述：单个消息的大小（字节）
    // 典型值：1KB ~ 1MB
    // 作用：控制RDMA发送/接收操作的数据量
    //       影响带宽和延迟的权衡
    
    int  num_concurr_msgs;
    // 描述：并发消息数
    // 含义：允许同时在网络上传输的消息数
    // 作用：与msg_size相乘得到总缓冲区大小
    //       影响吞吐量（更多消息在途 = 更高吞吐）
    // 例如：msg_size=64KB, num_concurr_msgs=16
    //       则ib_buf_size = 64KB × 16 = 1MB
    
    // --------- 网络配置 ---------
    
    char *sock_port;
    // 描述：TCP Socket端口号（字符串）
    // 作用：Server在此端口监听，Client连接至此端口
    //       用于交换QP信息（LID和QP号）
    //       不是RDMA数据的传输端口
    
    char *server_name;
    // 描述：Server主机名或IP地址
    // 作用：Client通过此信息连接到Server
    //       只在Client端有意义（Server端可以忽略）
    
} __attribute__((aligned(64)));
// 内存对齐到64字节边界，提高缓存性能

extern struct ConfigInfo config_info;

void print_config_info ();

#endif /* CONFIG_H_*/
