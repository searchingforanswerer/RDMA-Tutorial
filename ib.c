#include <arpa/inet.h>
#include <unistd.h>

#include "ib.h"
#include "debug.h"

// ============= 将QP状态从RESET转换为RTS（Ready To Send） =============
int modify_qp_to_rts (struct ibv_qp *qp, uint32_t target_qp_num, uint16_t target_lid,
						union ibv_gid *target_gid, uint8_t target_gid_index)
{
    int ret = 0;

    /* 第一步：将QP状态转换为INIT */
    {
		struct ibv_qp_attr qp_attr = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = IB_PORT,
			.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |    // 本地写访问
							IBV_ACCESS_REMOTE_READ |     // 远程读访问
							IBV_ACCESS_REMOTE_ATOMIC |   // 远程原子操作
							IBV_ACCESS_REMOTE_WRITE,     // 远程写访问
		};

		ret = ibv_modify_qp (qp, &qp_attr,
				IBV_QP_STATE | IBV_QP_PKEY_INDEX |
				IBV_QP_PORT  | IBV_QP_ACCESS_FLAGS);
		check (ret == 0, "Failed to modify qp to INIT.");
    }

    /* 第二步：将QP状态转换为RTR（Ready To Receive） */
    {
		struct ibv_qp_attr  qp_attr = {
			.qp_state           = IBV_QPS_RTR,
			.path_mtu           = IB_MTU,                  // 路径MTU大小
			.dest_qp_num        = target_qp_num,           // 目标QP号
			.rq_psn             = 0,                       // 接收队列包序号
			.max_dest_rd_atomic = 1,                       // 最大目标读原子操作
			.min_rnr_timer      = 12,                      // 最小RNR超时时间
			.ah_attr = {
				.is_global      = 1,
				.grh = {
					.dgid		= *target_gid,  			// 目标 GID
					.flow_label	= 0,
					.sgid_index	= target_gid_index,
					.hop_limit 	= 255, // TTL
					.traffic_class = 0,
				},
				.dlid 			= 0,
				.sl 			= IB_SL,
				.src_path_bits	= 0,
				.port_num		= IB_PORT,
			}
		};

		ret = ibv_modify_qp(qp, &qp_attr,
					IBV_QP_STATE | IBV_QP_AV |
					IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
					IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
					IBV_QP_MIN_RNR_TIMER);
		check (ret == 0, "Failed to change qp to rtr.");
    }

    /* 第三步：将QP状态转换为RTS（Ready To Send） */
    {
		struct ibv_qp_attr  qp_attr = {
			.qp_state      = IBV_QPS_RTS,
			.timeout       = 14,                           // 连接超时时间
			.retry_cnt     = 7,                            // 重试次数
			.rnr_retry     = 7,                            // RNR（Receiver Not Ready）重试
			.sq_psn        = 0,                            // 发送队列包序号
			.max_rd_atomic = 1,                            // 最大读原子操作
		};

		ret = ibv_modify_qp (qp, &qp_attr,
					IBV_QP_STATE | IBV_QP_TIMEOUT |
					IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
					IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
		check (ret == 0, "Failed to modify qp to RTS.");
    }

    return 0;
 error:
    return -1;
}

// ============= 发送数据（带立即数） =============
// req_size: 要发送的数据字节数
// lkey: 本地内存密钥
// wr_id: 工作请求ID，用户自定义，完成时可识别
// imm_data: 立即数(4字节元数据)
// qp: 要使用的Queue Pair
// buf: 发送的缓冲区
int post_send (uint32_t req_size, uint32_t lkey, uint64_t wr_id,
	       uint32_t imm_data, struct ibv_qp *qp, char *buf)
{
    int ret = 0;
    struct ibv_send_wr *bad_send_wr;

    // 创建Scatter-Gather元素（描述要发送的内存地址和长度）
    struct ibv_sge list = {
	.addr   = (uintptr_t) buf,          // 缓冲区地址
	.length = req_size,                 // 发送数据长度
	.lkey   = lkey                      // 内存区域的本地密钥
    };

    // 创建发送工作请求（Work Request）
    struct ibv_send_wr send_wr = {
	.wr_id      = wr_id,                // 工作请求ID，用于CQ中识别
	.sg_list    = &list,                // SG元素列表
	.num_sge    = 1,                    // SG元素个数
	.opcode     = IBV_WR_SEND_WITH_IMM, // 操作码：带立即数的发送
	.send_flags = IBV_SEND_SIGNALED,    // 标志：产生完成事件
	.imm_data   = htonl (imm_data)      // 立即数（网络字节序）
    };

    // 向QP提交发送工作请求
	// qb: 要使用的 Queue Pair
	// wr: 要提交的工作请求
	// bad_wr: 如果失败，返回导致失败的wr
    ret = ibv_post_send (qp, &send_wr, &bad_send_wr);
    return ret;
}

// ============= 接收数据 =============
int post_recv (uint32_t req_size, uint32_t lkey, uint64_t wr_id, 
	       struct ibv_qp *qp, char *buf)
{
    int ret = 0;
    struct ibv_recv_wr *bad_recv_wr;

    // 创建Scatter-Gather元素（描述接收缓冲区）
    struct ibv_sge list = {
	.addr   = (uintptr_t) buf,          // 缓冲区地址
	.length = req_size,                 // 接收数据最大长度
	.lkey   = lkey                      // 内存区域的本地密钥
    };

    // 创建接收工作请求（Work Request）
    struct ibv_recv_wr recv_wr = {
	.wr_id   = wr_id,                  // 工作请求ID，用于CQ中识别
	.sg_list = &list,                  // SG元素列表
	.num_sge = 1                       // SG元素个数
    };

    // 向QP提交接收工作请求
    ret = ibv_post_recv (qp, &recv_wr, &bad_recv_wr);
    return ret;
}
