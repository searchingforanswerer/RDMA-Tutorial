#define _GNU_SOURCE
#include <stdlib.h>
#include <stdbool.h>
#include <sys/time.h>
#include <pthread.h>
#include <sched.h>

#include "debug.h"
#include "ib.h"
#include "setup_ib.h"
#include "config.h"
#include "server.h"

// ============= 服务器工作线程 =============
void *server_thread (void *arg)
{
    int         ret             = 0, i = 0, n = 0;
    long        thread_id       = (long) arg;
    int         num_concurr_msgs= config_info.num_concurr_msgs;  // 并发消息数
    int         msg_size        = config_info.msg_size;          // 单个消息大小
    int         num_wc          = 20;                             // 一次轮询CQ的最大WC条目数
    bool        stop            = false;                         // 是否停止

    pthread_t   self;
    cpu_set_t   cpuset;

    // RDMA硬件资源
    struct ibv_qp       *qp         = ib_res.qp;                 // 队列对
    struct ibv_cq       *cq         = ib_res.cq;                 // 完成队列
    struct ibv_wc       *wc         = NULL;                      // 工作完成数组
    uint32_t             lkey       = ib_res.mr->lkey;           // 内存区域本地密钥
    char                *buf_ptr    = ib_res.ib_buf;             // 数据缓冲区指针
    int                  buf_offset = 0;                         // 缓冲区偏移
    size_t               buf_size   = ib_res.ib_buf_size;        // 缓冲区总大小

    // 性能统计
    struct timeval      start, end;
    long                ops_count  = 0;
    double              duration   = 0.0;
    double              throughput = 0.0;

    /* ========== 设置线程亲和性（CPU亲和性） ========== */
    CPU_ZERO (&cpuset);
    CPU_SET  ((int)thread_id, &cpuset);
    self = pthread_self ();
    ret  = pthread_setaffinity_np (self, sizeof(cpu_set_t), &cpuset);
    check (ret == 0, "thread[%ld]: failed to set thread affinity", thread_id);

    /* ========== 预先提交接收请求（Pre-post Receives） ========== */
    wc = (struct ibv_wc *) calloc (num_wc, sizeof(struct ibv_wc));
    check (wc != NULL, "thread[%ld]: failed to allocate wc", thread_id);

    // 预先提交num_concurr_msgs个接收请求
    for (i = 0; i < num_concurr_msgs; i++) {
        ret = post_recv (msg_size, lkey, (uint64_t)buf_ptr, qp, buf_ptr);
        check (ret == 0, "thread[%ld]: failed to post recv", thread_id);
        buf_offset = (buf_offset + msg_size) % buf_size;
        buf_ptr += buf_offset;
    }

    /* ========== 发送启动信号给Client ========== */
    // Server首先发送MSG_CTL_START信号，告知Client可以开始发送消息
    ret = post_send (0, lkey, 0, MSG_CTL_START, qp, buf_ptr);
    check (ret == 0, "thread[%ld]: failed to signal the client to start", thread_id);

    /* ========== 主通信循环 ========== */
    // Server不断接收Client的消息，并立即回显
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
		
		/* 检查是否达到测试总数 */
		if (ops_count == TOT_NUM_OPS) {
		    gettimeofday (&end, NULL);  // 记录结束时间
		    stop = true;
		    break;
		}

		/* ========== 回显消息：将接收的消息发送回Client ========== */
                char *msg_ptr = (char *)wc[i].wr_id;  // 获取接收缓冲区地址
		post_send (msg_size, lkey, 0, MSG_REGULAR, qp, msg_ptr);

		/* 预先提交新的接收请求，准备接收下一条消息 */
                post_recv (msg_size, lkey, wc[i].wr_id, qp, msg_ptr);
	    }
	}
    }

    /* ========== 第二阶段：发送停止信号给Client ========== */
    // 当Server完成测试后，发送MSG_CTL_STOP信号通知Client停止
    ret = post_send (0, lkey, IB_WR_ID_STOP, MSG_CTL_STOP, qp, ib_res.ib_buf);
    check (ret == 0, "thread[%ld]: failed to signal the client to stop", thread_id);

    /* ========== 等待停止信号发送完成 ========== */
    // 继续轮询CQ直到停止信号发送完成
    stop = false;
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

            /* 检查停止信号是否发送完成 */
            if (wc[i].opcode == IBV_WC_SEND) {
		if (wc[i].wr_id == IB_WR_ID_STOP) {
		    stop = true;
		    break;
		}
	    }
	}
    }

    /* ========== 统计和输出吞吐量 ========== */
    // 计算耗时（微秒）
    duration   = (double)((end.tv_sec - start.tv_sec) * 1000000 +
                          (end.tv_usec - start.tv_usec));
    // 计算吞吐量（百万操作数/秒）
    // 使用(ops_count - NUM_WARMING_UP_OPS)排除预热操作
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

// ============= 运行服务器 =============
int run_server ()
{
    int   ret         = 0;
    long  num_threads = 1;  // 服务器使用1个线程
    long  i           = 0;

    pthread_t           *threads = NULL;
    pthread_attr_t       attr;
    void                *status;

    // 初始化线程属性
    pthread_attr_init (&attr);
    pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_JOINABLE);

    // 分配线程数组
    threads = (pthread_t *) calloc (num_threads, sizeof(pthread_t));
    check (threads != NULL, "Failed to allocate threads.");

    // 创建工作线程
    for (i = 0; i < num_threads; i++) {
	ret = pthread_create (&threads[i], &attr, server_thread, (void *)i);
	check (ret == 0, "Failed to create server_thread[%ld]", i);
    }

    // 等待所有线程完成
    bool thread_ret_normally = true;
    for (i = 0; i < num_threads; i++) {
        ret = pthread_join (threads[i], &status);
        check (ret == 0, "Failed to join thread[%ld].", i);
        if ((long)status != 0) {
            thread_ret_normally = false;
            log ("server_thread[%ld]: failed to execute", i);
        }
    }

    if (thread_ret_normally == false) {
        goto error;
    }

    pthread_attr_destroy    (&attr);
    free (threads);

    return 0;

 error:
    if (threads != NULL) {
        free (threads);
    }
    pthread_attr_destroy    (&attr);
    
    return -1;
}
