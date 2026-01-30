#define _GNU_SOURCE
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

#include "debug.h"
#include "sock.h"

// 从socket读取数据，处理EINTR中断和不完整读取
ssize_t sock_read (int sock_fd, void *buffer, size_t len)
{
    ssize_t nr, tot_read;
    char *buf = buffer; // 避免void指针的算术运算
    tot_read = 0;

    // 循环读取直到读完所有数据或连接关闭
    while (len !=0 && (nr = read(sock_fd, buf, len)) != 0) {
        if (nr < 0) {
            // 如果被信号中断，继续读取；否则返回错误
            if (errno == EINTR) {
                continue;
            } else {
                return -1;
            }
        }
        len -= nr;
        buf += nr;
        tot_read += nr;
    }

    return tot_read;
}

// 向socket写入数据，处理EINTR中断和不完整写入
ssize_t sock_write (int sock_fd, void *buffer, size_t len)
{
    ssize_t nw, tot_written;
    const char *buf = buffer;  // 避免void指针的算术运算

    // 循环写入直到写完所有数据
    for (tot_written = 0; tot_written < len; ) {
        nw = write(sock_fd, buf, len-tot_written);

        if (nw <= 0) {
            // 如果被信号中断，继续写入；否则返回错误
            if (nw == -1 && errno == EINTR) {
                continue;
            } else {
                return -1;
            }
        }

        tot_written += nw;
        buf += nw;
    }
    return tot_written;
}

// 创建并绑定server socket到指定端口
int sock_create_bind (char *port)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int sock_fd = -1, ret = 0;

    // 设置地址信息查询参数
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;        // IPv4或IPv6都可以
    hints.ai_flags = AI_PASSIVE;        // 用于server端绑定

    // 获取地址信息列表
    ret = getaddrinfo(NULL, port, &hints, &result);
    check(ret==0, "getaddrinfo error.");

    // 尝试列表中的每个地址，直到成功创建并绑定socket
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd < 0) {
            continue;
        }

        ret = bind(sock_fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0) {
            /* 绑定成功，跳出循环 */
            break;
        }

        close(sock_fd);
        sock_fd = -1;
    }

    check(rp != NULL, "creating socket.");

    freeaddrinfo(result);
    return sock_fd;

 error:
    if (result) {
        freeaddrinfo(result);
    }
    if (sock_fd > 0) {
        close(sock_fd);
    }
    return -1;
}

// 创建并连接client socket到server
int sock_create_connect (char *server_name, char *port)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int sock_fd = -1, ret = 0;

    // 设置地址信息查询参数
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;        // IPv4或IPv6都可以

    // 获取server地址信息列表
    ret = getaddrinfo(server_name, port, &hints, &result);
    check(ret==0, "[ERROR] %s", gai_strerror(ret));

    // 尝试列表中的每个地址，直到成功连接
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd == -1) {
            continue;
        }

        ret = connect(sock_fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0) {
            /* 连接成功，跳出循环 */
            break;
        }

        close(sock_fd);
        sock_fd = -1;
    }

    check(rp!=NULL, "could not connect.");

    freeaddrinfo(result);
    return sock_fd;

 error:
    if (result) {
        freeaddrinfo(result);
    }
    if (sock_fd != -1) {
        close(sock_fd);
    }
    return -1;
}

// 通过socket发送本地QP信息给远端，使用网络字节序
int sock_set_qp_info(int sock_fd, struct QPInfo *qp_info)
{
    int n;
    struct QPInfo tmp_qp_info;

    // 转换为网络字节序（大端）
    tmp_qp_info.lid       = htons(qp_info->lid);       // 16位端口标识符
    tmp_qp_info.qp_num    = htonl(qp_info->qp_num);    // 32位队列对号
    tmp_qp_info.gid       = qp_info->gid;
    tmp_qp_info.gid_index = qp_info->gid_index;
    
    // 将QP信息写入socket
    n = sock_write(sock_fd, (char *)&tmp_qp_info, sizeof(struct QPInfo));
    check(n==sizeof(struct QPInfo), "write qp_info to socket.");

    return 0;

 error:
    return -1;
}

// 从socket接收远端QP信息，转换为主机字节序
int sock_get_qp_info(int sock_fd, struct QPInfo *qp_info)
{
    int n;
    struct QPInfo  tmp_qp_info;

    // 从socket读取QP信息
    n = sock_read(sock_fd, (char *)&tmp_qp_info, sizeof(struct QPInfo));
    check(n==sizeof(struct QPInfo), "read qp_info from socket.");

    // 转换为主机字节序
    qp_info->lid       = ntohs(tmp_qp_info.lid);       // 16位端口标识符
    qp_info->qp_num    = ntohl(tmp_qp_info.qp_num);    // 32位队列对号
    qp_info->gid       = tmp_qp_info.gid;
    qp_info->gid_index = tmp_qp_info.gid_index;
    
    return 0;

 error:
    return -1;
}
