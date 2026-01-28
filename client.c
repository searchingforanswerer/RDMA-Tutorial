#define _GNU_SOURCE
#include <stdlib.h>
#include <stdbool.h>
#include <sys/time.h>

#include "debug.h"
#include "config.h"
#include "setup_ib.h"
#include "ib.h"
#include "client.h"

// ============= 客户端工作线程 =============
void *client_thread_func (void *arg)
{
    int         ret             = 0, i = 0, n = 0;
    long	thread_id	= (long) arg;
    int         num_concurr_msgs= config_info.num_concurr_msgs;  // 并发消息数
    int         msg_size        = config_info.msg_size;          // 单个消息大小
    int		num_wc		= 20;                                 // 一次轮询CQ的最大WC条目数
    bool	start_sending   = false;                          // 是否开始发送
    bool        stop            = false;                         // 是否停止

    pthread_t   self;
    cpu_set_t   cpuset;

    // RDMA硬件资源
    struct ibv_qp	*qp	    = ib_res.qp;                     // 队列对
    struct ibv_cq	*cq	    = ib_res.cq;                     // 完成队列
    struct ibv_wc	*wc	    = NULL;                          // 工作完成数组
    uint32_t             lkey       = ib_res.mr->lkey;           // 内存区域本地密钥
    char		*buf_ptr    = ib_res.ib_buf;               // 数据缓冲区指针
    int			 buf_offset = 0;                         // 缓冲区偏移
    size_t               buf_size   = ib_res.ib_buf_size;        // 缓冲区总大小

    // 性能统计
    struct timeval      start, end;
    long                ops_count  = 0;
    double              duration   = 0.0;
    double              throughput = 0.0;

    /* ========== 设置线程亲和性（CPU亲和性） ========== */
    // 将线程绑定到指定的CPU核心，提高缓存命中率
    CPU_ZERO (&cpuset);
    CPU_SET  ((int)thread_id, &cpuset);
    self = pthread_self ();
    ret  = pthread_setaffinity_np (self, sizeof(cpu_set_t), &cpuset);
    check (ret == 0, "thread[%ld]: failed to set thread affinity", thread_id);

    /* ========== 预先提交接收请求（Pre-post Receives） ========== */
    // 初始化工作完成数组
    wc = (struct ibv_wc *) calloc (num_wc, sizeof(struct ibv_wc));
    check (wc != NULL, "thread[%ld]: failed to allocate wc", thread_id);

    // 预先提交num_concurr_msgs个接收请求
    // 这样硬件总是有接收缓冲区可用，避免丢包
    for (i = 0; i < num_concurr_msgs; i++) {
	ret = post_recv (msg_size, lkey, (uint64_t)buf_ptr, qp, buf_ptr);
	check (ret == 0, "thread[%ld]: failed to post recv", thread_id);
	// 循环使用缓冲区
	buf_offset = (buf_offset + msg_size) % buf_size;
	buf_ptr += buf_offset;
    }

    /* ========== 等待Server的启动信号 ========== */
    // Client等待Server发来的MSG_CTL_START信号才开始发送
    while (start_sending != true) {
        // 轮询完成队列，检查是否有完成的工作请求
        do {
            n = ibv_poll_cq (cq, num_wc, wc);
        } while (n < 1);
        check (n > 0, "thread[%ld]: failed to poll cq", thread_id);

        // 处理所有完成的工作请求
        for (i = 0; i < n; i++) {
            // 检查工作请求是否成功完成
            if (wc[i].status != IBV_WC_SUCCESS) {
                check (0, "thread[%ld]: wc failed status: %s.",
                       thread_id, ibv_wc_status_str(wc[i].status));
            }
	    
	    // 只处理接收操作
	    if (wc[i].opcode == IBV_WC_RECV) {
		/* 接收完成，提交新的接收请求 */
		post_recv (msg_size, lkey, (uint64_t)buf_ptr, qp, buf_ptr);
		buf_offset = (buf_offset + msg_size) % buf_size;
		buf_ptr += buf_offset;

                /* 检查立即数是否为启动信号 */
                if (ntohl(wc[i].imm_data) == MSG_CTL_START) {
		    start_sending = true;
		    break;
                }
            }
        }
    }
    log ("thread[%ld]: ready to send", thread_id);
    
    /* ========== 预先提交发送请求（Pre-post Sends） ========== */
    // 初始化缓冲区指针
    buf_ptr = ib_res.ib_buf;
    // 预先提交num_concurr_msgs个发送请求
    for (i = 0; i < num_concurr_msgs; i++) {
	ret = post_send (msg_size, lkey, 0, MSG_REGULAR, qp, buf_ptr);
	check (ret == 0, "thread[%ld]: failed to post send", thread_id);
	buf_offset = (buf_offset + msg_size) % buf_size;
	buf_ptr += buf_offset;
    }
    
    /* ========== 主通信循环 ========== */
    while (stop != true) {    
	/* 轮询完成队列 */
	n = ibv_poll_cq (cq, num_wc, wc);
        if (n < 0) {
            check (0, "thread[%ld]: Failed to poll cq", thread_id);
        }

        // 处理所有完成的工作请求
        for (i = 0; i < n; i++) {
            // 检查工作请求状态
            if (wc[i].status != IBV_WC_SUCCESS) {
                if (wc[i].opcode == IBV_WC_SEND) {
                    check (0, "thread[%ld]: send failed status: %s",
                           thread_id, ibv_wc_status_str(wc[i].status));
                } else {
                    check (0, "thread[%ld]: recv failed status: %s",
                           thread_id, ibv_wc_status_str(wc[i].status));
                }
            }

            /* ========== 处理接收完成事件 ========== */
            if (wc[i].opcode == IBV_WC_RECV) {
		ops_count += 1;  // 统计接收操作数
		debug ("ops_count = %ld", ops_count);

		/* 跳过预热操作，开始计时 */
		if (ops_count == NUM_WARMING_UP_OPS) {
		    gettimeofday (&start, NULL);
		}

		/* 检查是否收到停止信号 */
		if (ntohl(wc[i].imm_data) == MSG_CTL_STOP) {
		    gettimeofday (&end, NULL);  // 记录结束时间
		    stop = true;
		    break;
		}
		
		/* 将接收到的消息回显给Server */
		char *msg_ptr = (char *)wc[i].wr_id;  // 获取接收缓冲区地址
		post_send (msg_size, lkey, 0, MSG_REGULAR, qp, msg_ptr);

                /* 预先提交新的接收请求 */
                post_recv (msg_size, lkey, (uint64_t)buf_ptr, qp, buf_ptr);
		buf_offset = (buf_offset + msg_size) % buf_size;
		buf_ptr += buf_offset;
	    }
	} /* 完成所有工作完成项的处理 */
    }

    /* ========== 统计和输出吞吐量 ========== */
    // 计算耗时（微秒）
    duration   = (double)((end.tv_sec - start.tv_sec) * 1000000 + 
			  (end.tv_usec - start.tv_usec));
    // 计算吞吐量（百万操作数/秒）
    throughput = (double)(ops_count - NUM_WARMING_UP_OPS) / duration;
    log ("thread[%ld]: throughput = %f (Mops/s)",  thread_id, throughput);
    

    free (wc);
    pthread_exit ((void *)0);

 error:
    if (wc != NULL) {
        free (wc);
    }
    pthread_exit ((void *)-1);
}

// ============= 运行客户端 =============
int run_client ()
{
    int		ret	    = 0;
    long	num_threads = 1;  // 客户端使用1个线程
    long	i	    = 0;
    
    pthread_t	   *client_threads = NULL;
    pthread_attr_t  attr;
    void	   *status;

    log (LOG_SUB_HEADER, "Run Client");
    
    /* 初始化线程属性 */
    pthread_attr_init (&attr);
    pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_JOINABLE);

    // 分配线程数组
    client_threads = (pthread_t *) calloc (num_threads, sizeof(pthread_t));
    check (client_threads != NULL, "Failed to allocate client_threads.");

    // 创建工作线程
    for (i = 0; i < num_threads; i++) {
	ret = pthread_create (&client_threads[i], &attr, 
			      client_thread_func, (void *)i);
	check (ret == 0, "Failed to create client_thread[%ld]", i);
    }

    // 等待所有线程完成
    bool thread_ret_normally = true;
    for (i = 0; i < num_threads; i++) {
	ret = pthread_join (client_threads[i], &status);
	check (ret == 0, "Failed to join client_thread[%ld].", i);
	if ((long)status != 0) {
            thread_ret_normally = false;
            log ("thread[%ld]: failed to execute", i);
        }
    }

    if (thread_ret_normally == false) {
        goto error;
    }

    pthread_attr_destroy (&attr);
    free (client_threads);
    return 0;

 error:
    if (client_threads != NULL) {
        free (client_threads);
    }
    
    pthread_attr_destroy (&attr);
    return -1;
}
