#include <stdio.h>

#include "debug.h"
#include "config.h"
#include "ib.h"
#include "setup_ib.h"
#include "client.h"
#include "server.h"

FILE	*log_fp	     = NULL;  // 全局日志文件指针

int	init_env    ();
void	destroy_env ();

// ============= 程序主入口 =============
int main (int argc, char *argv[])
{
    int	ret = 0;

    /* ========== 参数解析：区分Server和Client ========== */
    // 根据命令行参数数量判断运行角色
    if (argc == 3) {
        // 3个参数：./prog server_name sock_port -> Client模式
        config_info.is_server   = false;
        config_info.server_name = argv[1];  // Server的主机名或IP
        config_info.sock_port   = argv[2];  // 连接端口
    } else if (argc == 2) {
        // 2个参数：./prog sock_port -> Server模式
        config_info.is_server = true;
        config_info.sock_port = argv[1];    // 监听端口
    } else {
        // 参数错误，打印用法
        printf ("Server: %s sock_port\n", argv[0]);
        printf ("Client: %s server_name sock_port\n", argv[0]);
        return 0;
    }    

    /* ========== 设置RDMA通信参数 ========== */
    config_info.msg_size         = 64;   // 每条消息64字节
    config_info.num_concurr_msgs = 1;    // 允许1条并发消息

    /* ========== 初始化环境（日志等） ========== */
    ret = init_env ();
    check (ret == 0, "Failed to init env");

    /* ========== 初始化InfiniBand硬件资源 ========== */
    // 分配IB设备、PD、MR、CQ、QP等资源
    // 并根据角色（Server/Client）连接QP
    ret = setup_ib ();
    check (ret == 0, "Failed to setup IB");

    /* ========== 运行工作负载（Echo协议） ========== */
    // Server：监听、接收消息、回显
    // Client：发送消息、接收回显、统计性能
    if (config_info.is_server) {
        ret = run_server ();
    } else {
        ret = run_client ();
    }
    check (ret == 0, "Failed to run workload");

 error:
    /* ========== 错误恢复：释放所有资源 ========== */
    close_ib_connection ();  // 释放IB资源（QP、CQ、MR、PD、CTX等）
    destroy_env         ();  // 关闭日志文件
    return ret;
}    

// ============= 环境初始化 =============
int init_env ()
{
    /* ========== 创建日志文件 ========== */
    // Server和Client分别创建各自的日志文件
    if (config_info.is_server) {
	    log_fp = fopen ("server.log", "w");
    } else {
	    log_fp = fopen ("client.log", "w");
    }
    check (log_fp != NULL, "Failed to open log file");

    /* ========== 打印启动信息和配置 ========== */
    log (LOG_HEADER, "IB Echo Server");
    print_config_info ();  // 输出所有配置参数

    return 0;
 error:
    return -1;
}

// ============= 环境清理 =============
void destroy_env ()
{
    /* ========== 打印完成信息并关闭日志文件 ========== */
    log (LOG_HEADER, "Run Finished");
    if (log_fp != NULL) {
        fclose (log_fp);
    }
}
