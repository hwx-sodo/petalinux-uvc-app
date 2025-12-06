/**
 * @file network_stream.c
 * @brief 网络视频流传输应用程序（服务端）
 * 
 * 功能：
 * 1. 初始化VPSS和VDMA
 * 2. 从DDR读取视频帧（RGBA格式）
 * 3. 通过UDP/TCP网络发送到PC端
 * 
 * 数据流：
 * CameraLink(PL) → VPSS(YUV422→RGB) → VDMA → DDR(RGBA) → 网络 → PC
 * 
 * 使用方法：
 *   ./network-stream-app -h <PC_IP地址> [-p 端口] [-t tcp|udp]
 * 
 * 示例：
 *   ./network-stream-app -h 10.72.43.219 -p 5000 -t udp
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "vpss_control.h"
#include "vdma_control.h"

/* ==================== 配置参数 ==================== */

/* 视频参数 - 640x480@60fps */
#define VIDEO_WIDTH     640
#define VIDEO_HEIGHT    480
#define BYTES_PER_PIXEL 4    /* RGBA格式：32位 */
#define NUM_FRAMES      3    /* 三缓冲 */
#define FRAME_SIZE      (VIDEO_WIDTH * VIDEO_HEIGHT * BYTES_PER_PIXEL)

/* 帧缓冲物理地址 */
#define FRAME_BUFFER_PHYS   0x20000000

/* 默认网络参数 */
#define DEFAULT_PORT        5000
#define DEFAULT_HOST        "10.72.43.219"    /* PC的IP地址 */
#define DEFAULT_PROTOCOL    "udp"

/* 目标帧率 */
#define TARGET_FPS          60
#define FRAME_INTERVAL_US   (1000000 / TARGET_FPS)

/* UDP分片大小（避免IP分片，MTU通常1500，减去IP/UDP头） */
#define UDP_CHUNK_SIZE      1400

/* ==================== 帧头结构 ==================== */

/**
 * 帧头结构 - 每帧数据前发送
 * 用于PC端解析视频流
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 魔数: 0x56494446 ("VIDF") */
    uint32_t frame_num;     /* 帧编号 */
    uint32_t width;         /* 图像宽度 */
    uint32_t height;        /* 图像高度 */
    uint32_t format;        /* 像素格式: 0=RGBA */
    uint32_t frame_size;    /* 帧数据大小 */
    uint32_t timestamp_sec; /* 时间戳（秒） */
    uint32_t timestamp_usec;/* 时间戳（微秒） */
} frame_header_t;

#define FRAME_MAGIC 0x56494446  /* "VIDF" */

/* ==================== 全局变量 ==================== */

static vpss_control_t vpss;
static vdma_control_t vdma;
static volatile int running = 1;
static int sock_fd = -1;

/* 命令行参数 */
static char target_host[256] = DEFAULT_HOST;
static int target_port = DEFAULT_PORT;
static int use_tcp = 0;  /* 0=UDP, 1=TCP */

/* ==================== 信号处理 ==================== */

void signal_handler(int signum)
{
    printf("\n接收到信号 %d，正在退出...\n", signum);
    running = 0;
}

/* ==================== 网络初始化 ==================== */

/**
 * 初始化UDP套接字
 */
int init_udp_socket(const char *host, int port)
{
    struct sockaddr_in addr;
    int sock;
    
    printf("创建UDP套接字，目标: %s:%d\n", host, port);
    
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("创建UDP套接字失败");
        return -1;
    }
    
    /* 设置发送缓冲区大小 */
    int sndbuf = 4 * 1024 * 1024;  /* 4MB */
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    /* 连接到目标（方便后续使用send而不是sendto） */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "无效的IP地址: %s\n", host);
        close(sock);
        return -1;
    }
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("连接失败");
        close(sock);
        return -1;
    }
    
    printf("UDP套接字初始化完成\n");
    return sock;
}

/**
 * 初始化TCP套接字
 */
int init_tcp_socket(const char *host, int port)
{
    struct sockaddr_in addr;
    int sock;
    
    printf("创建TCP连接到: %s:%d\n", host, port);
    
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("创建TCP套接字失败");
        return -1;
    }
    
    /* 禁用Nagle算法以降低延迟 */
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    /* 设置发送缓冲区 */
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "无效的IP地址: %s\n", host);
        close(sock);
        return -1;
    }
    
    printf("正在连接...\n");
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("TCP连接失败");
        fprintf(stderr, "提示: 请确保PC端接收程序已启动\n");
        close(sock);
        return -1;
    }
    
    printf("TCP连接成功\n");
    return sock;
}

/* ==================== 数据发送 ==================== */

/**
 * 发送帧头
 */
int send_frame_header(int sock, uint32_t frame_num)
{
    frame_header_t header;
    struct timespec ts;
    
    clock_gettime(CLOCK_REALTIME, &ts);
    
    header.magic = htonl(FRAME_MAGIC);
    header.frame_num = htonl(frame_num);
    header.width = htonl(VIDEO_WIDTH);
    header.height = htonl(VIDEO_HEIGHT);
    header.format = htonl(0);  /* RGBA */
    header.frame_size = htonl(FRAME_SIZE);
    header.timestamp_sec = htonl(ts.tv_sec);
    header.timestamp_usec = htonl(ts.tv_nsec / 1000);
    
    ssize_t sent = send(sock, &header, sizeof(header), 0);
    if (sent != sizeof(header)) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* 缓冲区满，跳过这帧 */
        }
        perror("发送帧头失败");
        return -1;
    }
    
    return 0;
}

/**
 * 发送帧数据（UDP分片发送）
 */
int send_frame_udp(int sock, const uint8_t *data, size_t size, uint32_t frame_num)
{
    /* 先发送帧头 */
    if (send_frame_header(sock, frame_num) < 0) {
        return -1;
    }
    
    /* 分片发送帧数据 */
    size_t offset = 0;
    while (offset < size) {
        size_t chunk_size = (size - offset) > UDP_CHUNK_SIZE ? 
                            UDP_CHUNK_SIZE : (size - offset);
        
        ssize_t sent = send(sock, data + offset, chunk_size, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100);  /* 短暂等待 */
                continue;
            }
            perror("发送数据失败");
            return -1;
        }
        offset += sent;
    }
    
    return 0;
}

/**
 * 发送帧数据（TCP整帧发送）
 */
int send_frame_tcp(int sock, const uint8_t *data, size_t size, uint32_t frame_num)
{
    /* 先发送帧头 */
    if (send_frame_header(sock, frame_num) < 0) {
        return -1;
    }
    
    /* 发送帧数据 */
    size_t offset = 0;
    while (offset < size) {
        ssize_t sent = send(sock, data + offset, size - offset, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100);
                continue;
            }
            perror("发送数据失败");
            return -1;
        }
        offset += sent;
    }
    
    return 0;
}

/* ==================== 主循环 ==================== */

int main_loop()
{
    int frame_count = 0;
    int last_vdma_frame = -1;
    struct timespec start_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    printf("\n开始网络视频流传输...\n");
    printf("分辨率: %dx%d@%dfps (RGBA格式)\n", VIDEO_WIDTH, VIDEO_HEIGHT, TARGET_FPS);
    printf("协议: %s, 目标: %s:%d\n", use_tcp ? "TCP" : "UDP", target_host, target_port);
    printf("帧大小: %d bytes (%.2f MB/s)\n", FRAME_SIZE, 
           (float)FRAME_SIZE * TARGET_FPS / 1024 / 1024);
    printf("按Ctrl+C退出\n\n");
    
    while (running) {
        /* 获取VDMA当前写入的帧 */
        int current_vdma_frame = vdma_get_current_frame(&vdma);
        
        /* 选择一个不同的帧读取 */
        int read_frame = (current_vdma_frame + 1) % NUM_FRAMES;
        
        /* 如果帧没有变化，跳过 */
        if (current_vdma_frame == last_vdma_frame && frame_count > 0) {
            usleep(1000);
            continue;
        }
        last_vdma_frame = current_vdma_frame;
        
        /* 获取帧数据 */
        const uint8_t *rgba_frame = (uint8_t*)vdma.frame_buffer + (read_frame * FRAME_SIZE);
        
        /* 发送帧 */
        int ret;
        if (use_tcp) {
            ret = send_frame_tcp(sock_fd, rgba_frame, FRAME_SIZE, frame_count);
        } else {
            ret = send_frame_udp(sock_fd, rgba_frame, FRAME_SIZE, frame_count);
        }
        
        if (ret < 0) {
            fprintf(stderr, "发送失败，退出\n");
            break;
        }
        
        frame_count++;
        
        /* 每60帧打印统计 */
        if (frame_count % 60 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            double elapsed = (current_time.tv_sec - start_time.tv_sec) + 
                           (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
            double fps = frame_count / elapsed;
            double bitrate = (double)FRAME_SIZE * frame_count * 8 / elapsed / 1e6;
            
            printf("已发送 %d 帧 (FPS: %.1f, 码率: %.1f Mbps)\n", 
                   frame_count, fps, bitrate);
        }
        
        /* 控制帧率 */
        usleep(FRAME_INTERVAL_US);
    }
    
    printf("\n总共发送 %d 帧\n", frame_count);
    return 0;
}

/* ==================== 帮助信息 ==================== */

void print_usage(const char *prog)
{
    printf("用法: %s [选项]\n", prog);
    printf("\n选项:\n");
    printf("  -h, --host <IP>      目标IP地址 (默认: %s)\n", DEFAULT_HOST);
    printf("  -p, --port <端口>    目标端口 (默认: %d)\n", DEFAULT_PORT);
    printf("  -t, --tcp            使用TCP协议 (默认: UDP)\n");
    printf("  -?, --help           显示帮助信息\n");
    printf("\n示例:\n");
    printf("  %s -h 10.72.43.219 -p 5000        # UDP模式\n", prog);
    printf("  %s -h 10.72.43.219 -p 5000 -t     # TCP模式\n", prog);
    printf("\n数据格式:\n");
    printf("  每帧数据 = 帧头(32字节) + RGBA像素数据(%d字节)\n", FRAME_SIZE);
}

/* ==================== 主函数 ==================== */

int main(int argc, char **argv)
{
    int ret = 0;
    
    /* 解析命令行参数 */
    static struct option long_options[] = {
        {"host", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"tcp",  no_argument,       0, 't'},
        {"help", no_argument,       0, '?'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:t?", long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                strncpy(target_host, optarg, sizeof(target_host) - 1);
                break;
            case 'p':
                target_port = atoi(optarg);
                break;
            case 't':
                use_tcp = 1;
                break;
            case '?':
            default:
                print_usage(argv[0]);
                return 0;
        }
    }
    
    printf("========================================\n");
    printf("网络视频流传输应用\n");
    printf("Xilinx Zynq UltraScale+ MPSoC\n");
    printf("IR Camera over Ethernet\n");
    printf("========================================\n\n");
    
    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* 初始化VPSS */
    printf("[1/5] 初始化VPSS...\n");
    if (vpss_init(&vpss, VIDEO_WIDTH, VIDEO_HEIGHT) < 0) {
        fprintf(stderr, "VPSS初始化失败\n");
        ret = 1;
        goto cleanup;
    }
    
    /* 初始化VDMA */
    printf("\n[2/5] 初始化VDMA...\n");
    if (vdma_init(&vdma, VIDEO_WIDTH, VIDEO_HEIGHT, 
                  BYTES_PER_PIXEL, NUM_FRAMES,
                  FRAME_BUFFER_PHYS) < 0) {
        fprintf(stderr, "VDMA初始化失败\n");
        ret = 1;
        goto cleanup;
    }
    
    /* 启动VDMA */
    printf("\n[3/5] 启动VDMA...\n");
    if (vdma_start(&vdma) < 0) {
        fprintf(stderr, "VDMA启动失败\n");
        ret = 1;
        goto cleanup;
    }
    
    /* 启动VPSS */
    printf("\n[4/5] 启动VPSS...\n");
    usleep(10000);
    if (vpss_start(&vpss) < 0) {
        fprintf(stderr, "VPSS启动失败\n");
        ret = 1;
        goto cleanup;
    }
    
    /* 等待数据流稳定 */
    printf("\n等待视频流稳定...\n");
    sleep(1);
    
    /* 初始化网络 */
    printf("\n[5/5] 初始化网络连接...\n");
    if (use_tcp) {
        sock_fd = init_tcp_socket(target_host, target_port);
    } else {
        sock_fd = init_udp_socket(target_host, target_port);
    }
    
    if (sock_fd < 0) {
        fprintf(stderr, "网络初始化失败\n");
        ret = 1;
        goto cleanup;
    }
    
    /* 主循环 */
    ret = main_loop();
    
cleanup:
    printf("\n清理资源...\n");
    
    if (sock_fd >= 0) {
        close(sock_fd);
    }
    
    vpss_cleanup(&vpss);
    vdma_cleanup(&vdma);
    
    printf("程序退出\n");
    return ret;
}
