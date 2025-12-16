/**
 * @file network_stream.c
 * @brief 网络视频流传输主程序
 * 
 * 数据流架构：
 *   CameraLink(PL, portA[7:0]+portB[7:0] → 16-bit)
 *     → Video In to AXI4-Stream (16-bit)
 *     → AXI4-Stream Data Width Converter (16-bit → 32-bit)
 *     → VDMA S2MM (32-bit总线写入DDR)
 *     → DDR (YUV422/YUYV字节序存储)
 *     → 本程序读取并通过网络发送
 *     → PC端 (OpenCV解码显示)
 * 
 * 使用方法：
 *   ./eth-camera-app -H <PC_IP> [-p 端口] [-t] [-d] [-f]
 * 
 * 示例：
 *   ./eth-camera-app -H 10.72.43.200              # UDP模式
 *   ./eth-camera-app -H 10.72.43.200 -t           # TCP模式
 *   ./eth-camera-app -H 10.72.43.200 -d           # 调试模式
 *   ./eth-camera-app -D                           # 仅诊断
 *   ./eth-camera-app -D -s frame.bin              # 诊断并保存帧
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "vdma_control.h"

/*============================================================================
 * 配置参数
 *============================================================================*/

/** 默认网络参数 */
#define DEFAULT_HOST        "10.72.43.200"
#define DEFAULT_PORT        5000

/** 目标帧率 */
#define TARGET_FPS          30
#define FRAME_INTERVAL_US   (1000000 / TARGET_FPS)

/** UDP分片大小 (MTU 1500 - IP/UDP头) */
#define UDP_CHUNK_SIZE      1400

/*============================================================================
 * 帧头协议
 *============================================================================*/

/**
 * 帧头结构 - 每帧数据前发送
 * PC端根据此头解析视频参数
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;             /* 魔数: 0x56494446 ("VIDF") */
    uint32_t frame_num;         /* 帧编号 */
    uint32_t width;             /* 图像宽度 */
    uint32_t height;            /* 图像高度 */
    uint32_t format;            /* 像素格式: 1=YUYV, 2=UYVY */
    uint32_t frame_size;        /* 帧数据大小 */
    uint32_t timestamp_sec;     /* 时间戳（秒） */
    uint32_t timestamp_usec;    /* 时间戳（微秒） */
} frame_header_t;

#define FRAME_MAGIC     0x56494446  /* "VIDF" */
#define PIXFMT_YUYV     1
#define PIXFMT_UYVY     2

/*============================================================================
 * 全局变量
 *============================================================================*/

static vdma_context_t g_vdma;
static volatile int g_running = 1;
static int g_sock_fd = -1;

/* 命令行参数 */
static char g_target_host[256] = DEFAULT_HOST;
static int g_target_port = DEFAULT_PORT;
static int g_use_tcp = 0;
static int g_debug_mode = 0;
static int g_force_send = 0;
static int g_diag_only = 0;
static char g_save_file[256] = "";

/*============================================================================
 * 信号处理
 *============================================================================*/

static void signal_handler(int signum)
{
    printf("\n收到信号 %d，正在退出...\n", signum);
    g_running = 0;
}

/*============================================================================
 * 网络发送函数
 *============================================================================*/

/**
 * 初始化UDP套接字
 */
static int init_udp_socket(const char *host, int port)
{
    struct sockaddr_in addr;
    int sock;
    
    printf("[网络] 创建UDP连接到 %s:%d\n", host, port);
    
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("创建UDP套接字失败");
        return -1;
    }
    
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
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("连接失败");
        close(sock);
        return -1;
    }
    
    printf("[网络] UDP套接字就绪\n");
    return sock;
}

/**
 * 初始化TCP套接字
 */
static int init_tcp_socket(const char *host, int port)
{
    struct sockaddr_in addr;
    int sock;
    
    printf("[网络] 创建TCP连接到 %s:%d\n", host, port);
    
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("创建TCP套接字失败");
        return -1;
    }
    
    /* 禁用Nagle算法 */
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
    
    printf("[网络] 正在连接...\n");
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("TCP连接失败");
        fprintf(stderr, "提示: 请确保PC端接收程序已启动\n");
        close(sock);
        return -1;
    }
    
    printf("[网络] TCP连接成功\n");
    return sock;
}

/**
 * 发送帧头
 */
static int send_frame_header(int sock, uint32_t frame_num, 
                             int width, int height, size_t frame_size)
{
    frame_header_t header;
    struct timespec ts;
    
    clock_gettime(CLOCK_REALTIME, &ts);
    
    header.magic = htonl(FRAME_MAGIC);
    header.frame_num = htonl(frame_num);
    header.width = htonl((uint32_t)width);
    header.height = htonl((uint32_t)height);
    header.format = htonl(PIXFMT_YUYV);  /* 固定YUYV格式 */
    header.frame_size = htonl((uint32_t)frame_size);
    header.timestamp_sec = htonl((uint32_t)ts.tv_sec);
    header.timestamp_usec = htonl((uint32_t)(ts.tv_nsec / 1000));
    
    ssize_t sent = send(sock, &header, sizeof(header), 0);
    if (sent != sizeof(header)) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* 缓冲区满 */
        }
        perror("发送帧头失败");
        return -1;
    }
    
    return 1;
}

/**
 * UDP分片发送帧数据
 */
static int send_frame_udp(int sock, const uint8_t *data, size_t size,
                          uint32_t frame_num, int width, int height)
{
    /* 发送帧头 */
    int ret = send_frame_header(sock, frame_num, width, height, size);
    if (ret <= 0) return ret;
    
    /* 分片发送数据 */
    size_t offset = 0;
    while (offset < size) {
        size_t chunk = (size - offset) > UDP_CHUNK_SIZE ? 
                       UDP_CHUNK_SIZE : (size - offset);
        
        ssize_t sent = send(sock, data + offset, chunk, 0);
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
    
    return 1;
}

/**
 * TCP发送帧数据
 */
static int send_frame_tcp(int sock, const uint8_t *data, size_t size,
                          uint32_t frame_num, int width, int height)
{
    /* 发送帧头 */
    int ret = send_frame_header(sock, frame_num, width, height, size);
    if (ret <= 0) return ret;
    
    /* 发送数据 */
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
    
    return 1;
}

/*============================================================================
 * 文件保存
 *============================================================================*/

static int save_frame_to_file(const uint8_t *data, size_t size, 
                              int index, const char *base_name)
{
    char filename[512];
    
    /* 生成文件名: base.bin -> base_f0.bin */
    const char *dot = strrchr(base_name, '.');
    if (dot) {
        int prefix_len = (int)(dot - base_name);
        snprintf(filename, sizeof(filename), "%.*s_f%d%s", 
                 prefix_len, base_name, index, dot);
    } else {
        snprintf(filename, sizeof(filename), "%s_f%d.bin", base_name, index);
    }
    
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror("无法创建文件");
        return -1;
    }
    
    /* 分块写入 */
    size_t written = 0;
    size_t chunk_size = 64 * 1024;
    
    while (written < size) {
        size_t to_write = (size - written) < chunk_size ? 
                          (size - written) : chunk_size;
        size_t w = fwrite(data + written, 1, to_write, f);
        if (w != to_write) break;
        written += w;
        
        if (written % (256 * 1024) == 0) {
            fflush(f);
            fsync(fileno(f));
            usleep(1000);
        }
    }
    
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    
    if (written != size) {
        printf("写入不完整: %zu / %zu\n", written, size);
        return -1;
    }
    
    printf("帧#%d 已保存到 %s (%zu bytes)\n", index, filename, size);
    return 0;
}

/*============================================================================
 * 主循环
 *============================================================================*/

static int stream_loop(void)
{
    uint32_t frame_count = 0;
    int last_vdma_frame = -1;
    int skip_count = 0;
    
    struct timespec start_time, now, last_stat;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    last_stat = start_time;
    
    int width = g_vdma.width;
    int height = g_vdma.height;
    size_t frame_size = g_vdma.frame_size;
    
    printf("\n========== 开始视频流传输 ==========\n");
    printf("分辨率: %dx%d @ %d fps\n", width, height, TARGET_FPS);
    printf("格式: YUV422 (YUYV), 帧大小: %zu bytes\n", frame_size);
    printf("协议: %s, 目标: %s:%d\n", 
           g_use_tcp ? "TCP" : "UDP", g_target_host, g_target_port);
    printf("调试: %s, 强制发送: %s\n",
           g_debug_mode ? "开" : "关", g_force_send ? "开" : "关");
    printf("按 Ctrl+C 退出\n");
    printf("====================================\n\n");
    
    while (g_running) {
        /* 获取VDMA当前写入帧 */
        int write_frame = vdma_get_write_frame(&g_vdma);
        
        /* 检测帧变化 */
        if (write_frame == last_vdma_frame && frame_count > 0) {
            if (!g_force_send) {
                skip_count++;
                if (g_debug_mode && skip_count % 1000 == 0) {
                    printf("[调试] 帧号未变化，已跳过 %d 次\n", skip_count);
                }
                usleep(1000);
                continue;
            }
        }
        last_vdma_frame = write_frame;
        
        /* 获取可读帧 */
        int read_idx;
        const uint8_t *frame_data = vdma_get_read_buffer(&g_vdma, &read_idx);
        if (!frame_data) {
            usleep(1000);
            continue;
        }
        
        /* 首帧调试信息 */
        if (g_debug_mode && frame_count == 0) {
            printf("[调试] 首帧 - 读取缓冲#%d:\n", read_idx);
            printf("  前32字节: ");
            for (int i = 0; i < 32; i++) {
                printf("%02X ", frame_data[i]);
            }
            printf("\n");
            printf("  YUYV解析: ");
            for (int g = 0; g < 4; g++) {
                printf("(Y0=%3d U=%3d Y1=%3d V=%3d) ",
                       frame_data[g*4+0], frame_data[g*4+1],
                       frame_data[g*4+2], frame_data[g*4+3]);
            }
            printf("\n");
        }
        
        /* 发送帧 */
        int ret;
        if (g_use_tcp) {
            ret = send_frame_tcp(g_sock_fd, frame_data, frame_size,
                                 frame_count, width, height);
        } else {
            ret = send_frame_udp(g_sock_fd, frame_data, frame_size,
                                 frame_count, width, height);
        }
        
        if (ret < 0) {
            fprintf(stderr, "发送失败，退出\n");
            break;
        }
        
        frame_count++;
        
        /* 每秒统计 */
        clock_gettime(CLOCK_MONOTONIC, &now);
        double since_stat = (now.tv_sec - last_stat.tv_sec) + 
                           (now.tv_nsec - last_stat.tv_nsec) / 1e9;
        
        if (since_stat >= 1.0) {
            double elapsed = (now.tv_sec - start_time.tv_sec) + 
                            (now.tv_nsec - start_time.tv_nsec) / 1e9;
            double fps = frame_count / elapsed;
            double mbps = (double)frame_size * frame_count * 8 / elapsed / 1e6;
            
            printf("已发送 %u 帧 | FPS: %.1f | 码率: %.1f Mbps",
                   frame_count, fps, mbps);
            if (skip_count > 0) {
                printf(" | 跳过: %d", skip_count);
            }
            printf("\n");
            
            last_stat = now;
        }
        
        /* 帧率控制 */
        usleep(FRAME_INTERVAL_US);
    }
    
    printf("\n总计发送 %u 帧，跳过 %d 次\n", frame_count, skip_count);
    return 0;
}

/*============================================================================
 * 帮助信息
 *============================================================================*/

static void print_usage(const char *prog)
{
    printf("网络视频流传输程序\n\n");
    printf("用法: %s [选项]\n\n", prog);
    
    printf("网络选项:\n");
    printf("  -H, --host <IP>      目标IP地址 (默认: %s)\n", DEFAULT_HOST);
    printf("  -p, --port <端口>    目标端口 (默认: %d)\n", DEFAULT_PORT);
    printf("  -t, --tcp            使用TCP协议 (默认: UDP)\n");
    printf("  -f, --force          强制发送，忽略帧变化检测\n");
    printf("\n");
    
    printf("诊断选项:\n");
    printf("  -d, --debug          调试模式，打印详细信息\n");
    printf("  -D, --diag           仅诊断模式，不进行网络传输\n");
    printf("  -s, --save <文件>    保存帧数据到文件\n");
    printf("  -h, --help           显示帮助\n");
    printf("\n");
    
    printf("示例:\n");
    printf("  %s -H 10.72.43.200              # UDP发送\n", prog);
    printf("  %s -H 10.72.43.200 -t           # TCP发送\n", prog);
    printf("  %s -H 10.72.43.200 -d -f        # 调试+强制发送\n", prog);
    printf("  %s -D                           # 仅诊断\n", prog);
    printf("  %s -D -s frame.bin              # 诊断并保存帧\n", prog);
    printf("\n");
    
    printf("数据流架构:\n");
    printf("  CameraLink(16-bit) → Video In to AXI4-Stream → \n");
    printf("  Width Converter(32-bit) → VDMA S2MM → DDR(YUV422/YUYV) → 网络\n");
}

/*============================================================================
 * 主函数
 *============================================================================*/

int main(int argc, char **argv)
{
    int ret = 0;
    
    /* 禁用stdout缓冲 */
    setvbuf(stdout, NULL, _IONBF, 0);
    
    /* 命令行解析 */
    static struct option long_options[] = {
        {"host",  required_argument, 0, 'H'},
        {"port",  required_argument, 0, 'p'},
        {"tcp",   no_argument,       0, 't'},
        {"force", no_argument,       0, 'f'},
        {"debug", no_argument,       0, 'd'},
        {"diag",  no_argument,       0, 'D'},
        {"save",  required_argument, 0, 's'},
        {"help",  no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "H:p:tfdDs:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'H':
                strncpy(g_target_host, optarg, sizeof(g_target_host) - 1);
                break;
            case 'p':
                g_target_port = atoi(optarg);
                break;
            case 't':
                g_use_tcp = 1;
                break;
            case 'f':
                g_force_send = 1;
                break;
            case 'd':
                g_debug_mode = 1;
                break;
            case 'D':
                g_diag_only = 1;
                break;
            case 's':
                strncpy(g_save_file, optarg, sizeof(g_save_file) - 1);
                g_diag_only = 1;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return 0;
        }
    }
    
    printf("================================================\n");
    printf("    CameraLink 网络视频流传输\n");
    printf("    Xilinx Zynq UltraScale+ MPSoC\n");
    printf("================================================\n\n");
    
    /* 信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /*----------------------------------------------------------------------
     * 步骤1: 初始化VDMA
     *----------------------------------------------------------------------*/
    printf("[1/3] 初始化VDMA...\n");
    
    if (vdma_init(&g_vdma, 
                  VIDEO_WIDTH, VIDEO_HEIGHT, BYTES_PER_PIXEL,
                  NUM_FRAME_BUFFERS, FRAME_BUFFER_BASE) < 0) {
        fprintf(stderr, "VDMA初始化失败\n");
        return 1;
    }
    
    /*----------------------------------------------------------------------
     * 步骤2: 启动VDMA
     *----------------------------------------------------------------------*/
    printf("\n[2/3] 启动VDMA...\n");
    
    if (vdma_start(&g_vdma) < 0) {
        fprintf(stderr, "VDMA启动失败\n");
        ret = 1;
        goto cleanup;
    }
    
    /* 等待视频流稳定 */
    printf("\n等待视频流稳定...\n");
    sleep(1);
    
    /*----------------------------------------------------------------------
     * 诊断模式
     *----------------------------------------------------------------------*/
    if (g_diag_only) {
        printf("\n========== 诊断模式 ==========\n");
        
        /* 打印寄存器状态 */
        vdma_dump_registers(&g_vdma);
        
        /* 打印所有帧缓冲信息 */
        for (int i = 0; i < g_vdma.num_buffers; i++) {
            vdma_dump_frame_info(&g_vdma, i);
        }
        
        /* 保存帧数据 */
        if (g_save_file[0] != '\0') {
            printf("\n保存帧数据...\n");
            
            /* 停止VDMA以避免写冲突 */
            vdma_stop(&g_vdma);
            usleep(100000);
            
            for (int i = 0; i < g_vdma.num_buffers; i++) {
                const uint8_t *data = vdma_get_frame_buffer(&g_vdma, i);
                if (data) {
                    save_frame_to_file(data, g_vdma.frame_size, i, g_save_file);
                }
            }
        }
        
        printf("\n诊断完成\n");
        printf("后续操作:\n");
        printf("  网络传输: %s -H %s -p %d\n", argv[0], g_target_host, g_target_port);
        printf("  调试发送: %s -H %s -d -f\n", argv[0], g_target_host);
        
        goto cleanup;
    }
    
    /* 调试模式下打印详细信息 */
    if (g_debug_mode) {
        printf("[调试] 打印首帧信息 (不停止VDMA)...\n");
        fflush(stdout);
        
        /* 只打印帧缓冲 #0 的摘要信息，不停止VDMA */
        const uint8_t *frame = vdma_get_frame_buffer(&g_vdma, 0);
        if (frame) {
            printf("[调试] 帧缓冲 #0 前32字节:\n  ");
            for (int i = 0; i < 32; i++) {
                printf("%02X ", frame[i]);
                if (i == 15) printf("\n  ");
            }
            printf("\n");
            fflush(stdout);
        }
        
        printf("[调试] 调试信息已打印，继续执行...\n");
        fflush(stdout);
    }
    
    /*----------------------------------------------------------------------
     * 步骤3: 初始化网络
     *----------------------------------------------------------------------*/
    printf("\n[3/3] 初始化网络...\n");
    
    if (g_use_tcp) {
        g_sock_fd = init_tcp_socket(g_target_host, g_target_port);
    } else {
        g_sock_fd = init_udp_socket(g_target_host, g_target_port);
    }
    
    if (g_sock_fd < 0) {
        fprintf(stderr, "网络初始化失败\n");
        ret = 1;
        goto cleanup;
    }
    
    /*----------------------------------------------------------------------
     * 主循环
     *----------------------------------------------------------------------*/
    ret = stream_loop();
    
cleanup:
    printf("\n清理资源...\n");
    
    if (g_sock_fd >= 0) {
        close(g_sock_fd);
        g_sock_fd = -1;
    }
    
    vdma_cleanup(&g_vdma);
    
    printf("程序退出\n");
    return ret;
}
