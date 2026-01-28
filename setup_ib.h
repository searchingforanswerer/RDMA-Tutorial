#ifndef SETUP_IB_H_
#define SETUP_IB_H_

#include <infiniband/verbs.h>

// ============= InfiniBand资源结构体 =============
// 封装了与RDMA通信相关的所有硬件资源和配置信息
struct IBRes {
    // --------- 核心InfiniBand对象 ---------
    
    struct ibv_context		*ctx;
    // 描述：IB设备上下文
    // 作用：代表一个打开的IB设备，所有IB操作都基于此上下文
    // 类比：文件描述符，用于标识一个特定的硬件设备
    
    struct ibv_pd		*pd;
    // 描述：保护域（Protection Domain）
    // 作用：IB资源的隔离容器，同一PD中的资源可以相互访问
    //       跨PD的资源无法通信，用于安全隔离
    // 类比：进程的地址空间，不同进程的内存是隔离的
    
    struct ibv_mr		*mr;
    // 描述：内存区域（Memory Region）
    // 作用：注册用户态内存到IB硬件，使其能被RDMA访问
    //       包含内存地址、大小、访问权限、本地密钥(lkey)、远程密钥(rkey)
    // 类比：物理内存的映射表，告诉硬件哪些内存可以被RDMA操作
    
    struct ibv_cq		*cq;
    // 描述：完成队列（Completion Queue）
    // 作用：硬件向软件报告工作请求（WR）的完成情况
    //       当发送/接收操作完成时，硬件将完成信息放入CQ
    //       应用通过轮询或事件监听CQ来获知操作完成
    // 类比：邮箱，硬件通过CQ向应用发送"任务完成"的通知
    
    struct ibv_qp		*qp;
    // 描述：队列对（Queue Pair）
    // 作用：RDMA通信的核心对象，包含发送队列(SQ)和接收队列(RQ)
    //       应用通过post_send/post_recv向QP提交工作请求
    //       硬件执行这些请求并将结果报告到CQ
    // 类比：网络连接的两端，代表一个单向的通信通道
    
    // --------- 硬件属性信息 ---------
    
    struct ibv_port_attr	 port_attr;
    // 描述：IB端口属性
    // 内容：
    //   - lid: Local ID，本地标识符（IB/RoCE的地址）
    //   - state: 端口状态（激活/禁用等）
    //   - mtu: 最大传输单元
    //   - active_width: 激活的链接宽度
    //   - active_speed: 激活的链接速度
    // 作用：描述IB端口的配置和状态，用于QP连接配置
    
    struct ibv_device_attr	 dev_attr;
    // 描述：IB设备属性
    // 内容：
    //   - max_cqe: 最大CQ条目数
    //   - max_qp_wr: 最大工作请求数
    //   - max_qp: 最大QP数
    //   - max_mr: 最大内存区域数
    //   - max_mr_size: 最大MR大小
    // 作用：提供硬件的能力信息，用于资源分配规划
    
    // --------- 用户数据缓冲区 ---------
    
    char   *ib_buf;
    // 描述：RDMA数据缓冲区指针
    // 作用：存储待发送或接收的数据
    //       必须通过ibv_reg_mr注册到硬件
    //       使用memalign(4096)对齐以提高性能
    
    size_t  ib_buf_size;
    // 描述：缓冲区大小（字节）
    // 计算：msg_size × num_concurr_msgs
    //       允许并发发送/接收的消息个数 × 每个消息的大小
    // 作用：用于MR注册和缓冲区管理
};

extern struct IBRes ib_res;

int  setup_ib ();
void close_ib_connection ();

int  connect_qp_server ();
int  connect_qp_client ();

#endif /*setup_ib.h*/
