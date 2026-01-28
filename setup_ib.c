#include <arpa/inet.h>
#include <unistd.h>
#include <malloc.h>

#include "sock.h"
#include "ib.h"
#include "debug.h"
#include "config.h"
#include "setup_ib.h"

struct IBRes ib_res;  // 全局InfiniBand资源结构体

// ============= Server端QP连接过程 =============
int connect_qp_server ()
{
    int			ret	      = 0, n = 0;
    int			sockfd	      = 0;
    int			peer_sockfd   = 0;
    struct sockaddr_in	peer_addr;
    socklen_t		peer_addr_len = sizeof(struct sockaddr_in);
    char sock_buf[64]		      = {'\0'};
    struct QPInfo	local_qp_info, remote_qp_info;

    // 1. 创建server socket并绑定到指定端口
    sockfd = sock_create_bind(config_info.sock_port);
    check(sockfd > 0, "Failed to create server socket.");
    listen(sockfd, 5);

    // 2. 等待client连接
    peer_sockfd = accept(sockfd, (struct sockaddr *)&peer_addr,
			 &peer_addr_len);
    check (peer_sockfd > 0, "Failed to create peer_sockfd");

    // 3. 初始化本地QP信息（LID: Local ID，qp_num: 队列对号）
    local_qp_info.lid	 = ib_res.port_attr.lid; 
    local_qp_info.qp_num = ib_res.qp->qp_num;
    
    // 4. 从client接收其QP信息
    ret = sock_get_qp_info (peer_sockfd, &remote_qp_info);
    check (ret == 0, "Failed to get qp_info from client");
    
    // 5. 将本地QP信息发送给client    
    ret = sock_set_qp_info (peer_sockfd, &local_qp_info);
    check (ret == 0, "Failed to send qp_info to client");

    // 6. 修改本地QP状态为RTS（Ready To Send），使用remote QP信息
    ret = modify_qp_to_rts (ib_res.qp, remote_qp_info.qp_num, 
			    remote_qp_info.lid);
    check (ret == 0, "Failed to modify qp to rts");

    // 7. 输出QP连接信息
    log (LOG_SUB_HEADER, "Start of IB Config");
    log ("\tqp[%"PRIu32"] <-> qp[%"PRIu32"]", 
	 ib_res.qp->qp_num, remote_qp_info.qp_num);
    log (LOG_SUB_HEADER, "End of IB Config");

    // 8. 与client同步，等待client就绪信号
    n = sock_read (peer_sockfd, sock_buf, sizeof(SOCK_SYNC_MSG));
    check (n == sizeof(SOCK_SYNC_MSG), "Failed to receive sync from client");
    
    // 9. 发送就绪信号给client
    n = sock_write (peer_sockfd, sock_buf, sizeof(SOCK_SYNC_MSG));
    check (n == sizeof(SOCK_SYNC_MSG), "Failed to write sync to client");
	
    // 10. 关闭socket连接
    close (peer_sockfd);
    close (sockfd);
    
    return 0;

 error:
    if (peer_sockfd > 0) {
	close (peer_sockfd);
    }
    if (sockfd > 0) {
	close (sockfd);
    }
    
    return -1;
}

// ============= Client端QP连接过程 =============
int connect_qp_client ()
{
    int ret	      = 0, n = 0;
    int peer_sockfd   = 0;
    char sock_buf[64] = {'\0'};

    struct QPInfo local_qp_info, remote_qp_info;

    // 1. 创建socket并连接到server
    peer_sockfd = sock_create_connect (config_info.server_name,
				       config_info.sock_port);
    check (peer_sockfd > 0, "Failed to create peer_sockfd");

    // 2. 初始化本地QP信息
    local_qp_info.lid     = ib_res.port_attr.lid; 
    local_qp_info.qp_num  = ib_res.qp->qp_num; 
   
    // 3. 将本地QP信息发送给server    
    ret = sock_set_qp_info (peer_sockfd, &local_qp_info);
    check (ret == 0, "Failed to send qp_info to server");

    // 4. 从server接收其QP信息    
    ret = sock_get_qp_info (peer_sockfd, &remote_qp_info);
    check (ret == 0, "Failed to get qp_info from server");

    // 5. 修改本地QP状态为RTS，使用server QP信息
    ret = modify_qp_to_rts (ib_res.qp, remote_qp_info.qp_num, 
			    remote_qp_info.lid);
    check (ret == 0, "Failed to modify qp to rts");

    // 6. 输出QP连接信息
    log (LOG_SUB_HEADER, "IB Config");
    log ("\tqp[%"PRIu32"] <-> qp[%"PRIu32"]", 
	 ib_res.qp->qp_num, remote_qp_info.qp_num);
    log (LOG_SUB_HEADER, "End of IB Config");

    // 7. 发送就绪信号给server
    n = sock_write (peer_sockfd, sock_buf, sizeof(SOCK_SYNC_MSG));
    check (n == sizeof(SOCK_SYNC_MSG), "Failed to write sync to client");
    
    // 8. 接收server的就绪信号
    n = sock_read (peer_sockfd, sock_buf, sizeof(SOCK_SYNC_MSG));
    check (n == sizeof(SOCK_SYNC_MSG), "Failed to receive sync from client");

    // 9. 关闭socket连接
    close (peer_sockfd);
    return 0;

 error:
    if (peer_sockfd > 0) {
	close (peer_sockfd);
    }
    
    return -1;
}

// ============= 初始化InfiniBand资源 =============
int setup_ib ()
{
    int	ret		         = 0;
    struct ibv_device **dev_list = NULL;    
    memset (&ib_res, 0, sizeof(struct IBRes));

    // 1. 获取IB设备列表
    dev_list = ibv_get_device_list(NULL);
    check(dev_list != NULL, "Failed to get ib device list.");

    // 2. 打开第一个IB设备
    ib_res.ctx = ibv_open_device(*dev_list);
    check(ib_res.ctx != NULL, "Failed to open ib device.");

    // 3. 分配保护域（Protection Domain）
    ib_res.pd = ibv_alloc_pd(ib_res.ctx);
    check(ib_res.pd != NULL, "Failed to allocate protection domain.");

    // 4. 查询IB端口属性（包括LID）
    ret = ibv_query_port(ib_res.ctx, IB_PORT, &ib_res.port_attr);
    check(ret == 0, "Failed to query IB port information.");
    
    // 5. 分配并注册内存区域（Memory Region）
    ib_res.ib_buf_size = config_info.msg_size * config_info.num_concurr_msgs;
    ib_res.ib_buf      = (char *) memalign (4096, ib_res.ib_buf_size);
    check (ib_res.ib_buf != NULL, "Failed to allocate ib_buf");

    // 使用本地写、远程读写权限注册MR
    ib_res.mr = ibv_reg_mr (ib_res.pd, (void *)ib_res.ib_buf,
			    ib_res.ib_buf_size,
			    IBV_ACCESS_LOCAL_WRITE |
			    IBV_ACCESS_REMOTE_READ |
			    IBV_ACCESS_REMOTE_WRITE);
    check (ib_res.mr != NULL, "Failed to register mr");
    
    // 6. 查询IB设备属性（最大CQE、最大WR等）
    ret = ibv_query_device(ib_res.ctx, &ib_res.dev_attr);
    check(ret==0, "Failed to query device");
    
    // 7. 创建完成队列（Completion Queue）
    ib_res.cq = ibv_create_cq (ib_res.ctx, ib_res.dev_attr.max_cqe, 
			       NULL, NULL, 0);
    check (ib_res.cq != NULL, "Failed to create cq");
    
    // 8. 创建队列对（Queue Pair）
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = ib_res.cq,
        .recv_cq = ib_res.cq, // 发送接收共用一个cq
        .cap = {
            .max_send_wr = ib_res.dev_attr.max_qp_wr,      // 最大发送WR数
            .max_recv_wr = ib_res.dev_attr.max_qp_wr,      // 最大接收WR数
            .max_send_sge = 1,                              // 最大发送SGE数
            .max_recv_sge = 1,                              // 最大接收SGE数
        },
        .qp_type = IBV_QPT_RC,  // 可靠连接类型
    };

    ib_res.qp = ibv_create_qp (ib_res.pd, &qp_init_attr);
    check (ib_res.qp != NULL, "Failed to create qp");

    // 9. 连接QP（server或client根据config决定）
    if (config_info.is_server) {
	ret = connect_qp_server ();
    } else {
	ret = connect_qp_client ();
    }
    check (ret == 0, "Failed to connect qp");

    ibv_free_device_list (dev_list);
    return 0;

 error:
    if (dev_list != NULL) {
	ibv_free_device_list (dev_list);
    }
    return -1;
}

// ============= 关闭IB连接，释放所有资源 =============
void close_ib_connection ()
{
    if (ib_res.qp != NULL) {
	ibv_destroy_qp (ib_res.qp);
    }
 
    if (ib_res.cq != NULL) {
	ibv_destroy_cq (ib_res.cq);
    }

    if (ib_res.mr != NULL) {
	ibv_dereg_mr (ib_res.mr);
    }

    if (ib_res.pd != NULL) {
        ibv_dealloc_pd (ib_res.pd);
    }

    if (ib_res.ctx != NULL) {
        ibv_close_device (ib_res.ctx);
    }

    if (ib_res.ib_buf != NULL) {
	free (ib_res.ib_buf);
    }
}
