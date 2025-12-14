/**
 * @file network_stream.c
 * @brief 网络视频流传输应用程序（服务端）
 * 
 * 功能：
 * 1. 初始化VDMA
 * 2. 从DDR读取视频帧（默认YUV422/YUYV格式）
 * 3. 通过UDP/TCP网络发送到PC端
 * 
 * 数据流：
 * CameraLink(PL, porta[7:0]+portb[7:0]拼成16-bit) →
 * Video In to AXI4-Stream(16-bit) →
 * AXI4-Stream Data Width Converter(32-bit) →
 * VDMA(S2MM写DDR, 32-bit总线) →
 * DDR(按YUV422/YUYV字节序存储) → 网络 → PC(OpenCV解码显示)
 * 
 * 使用方法：
 *   ./eth-camera-app -H <PC_IP地址> [选项]
 * 
 * 示例：
 *   ./eth-camera-app -H 10.72.43.200 -p 5000                 # UDP模式
 *   ./eth-camera-app -H 10.72.43.200 -p 5000 -t              # TCP模式
 *   ./eth-camera-app -H 10.72.43.200 --width 640 --height 480 # 修改分辨率
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

#include "vdma_control.h"

/* ==================== 配置参数 ==================== */

/* 默认视频参数（可通过命令行覆盖） */
#define DEFAULT_VIDEO_WIDTH     640
#define DEFAULT_VIDEO_HEIGHT    480
#define DEFAULT_NUM_FRAMES      3    /* 三缓冲 */

/* 本项目只支持：YUV422 packed (YUYV), 2 bytes/pixel */
#define PIXFMT_YUYV 1
#define BYTES_PER_PIXEL 2

/* 帧缓冲物理地址 */
#define DEFAULT_FRAME_BUFFER_PHYS   0x20000000  /* 由你的系统内存规划决定 */

/* 默认网络参数 */
#define DEFAULT_PORT        5000
#define DEFAULT_HOST        "10.72.43.200"    /* PC的IP地址 */
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
    uint32_t format;        /* 像素格式: 1=YUYV(YUV422) */
    uint32_t frame_size;    /* 帧数据大小 */
    uint32_t timestamp_sec; /* 时间戳（秒） */
    uint32_t timestamp_usec;/* 时间戳（微秒） */
} frame_header_t;

#define FRAME_MAGIC 0x56494446  /* "VIDF" */

/* ==================== 全局变量 ==================== */

static vdma_control_t vdma;
static volatile int running = 1;
static int sock_fd = -1;

/* 命令行参数 */
static char target_host[256] = DEFAULT_HOST;
static int target_port = DEFAULT_PORT;
static int use_tcp = 0;  /* 0=UDP, 1=TCP */
static int debug_mode = 0;  /* 调试模式：打印更多信息 */
static int force_send = 0;  /* 强制发送模式：忽略帧变化检测 */
static int diag_only = 0;   /* 仅诊断模式：不进行网络传输 */
static char save_file[256] = "";  /* 保存帧数据到文件 */

/* 视频参数（运行时） */
static int video_width = DEFAULT_VIDEO_WIDTH;
static int video_height = DEFAULT_VIDEO_HEIGHT;
static int num_frames = DEFAULT_NUM_FRAMES;
static uint32_t frame_buffer_phys = DEFAULT_FRAME_BUFFER_PHYS;

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
 * @return 1成功，0缓冲区满需跳过，-1错误
 */
int send_frame_header(int sock, uint32_t frame_num)
{
    frame_header_t header;
    struct timespec ts;
    const uint32_t frame_size = (uint32_t)video_width * (uint32_t)video_height * (uint32_t)BYTES_PER_PIXEL;
    
    clock_gettime(CLOCK_REALTIME, &ts);
    
    header.magic = htonl(FRAME_MAGIC);
    header.frame_num = htonl(frame_num);
    header.width = htonl(video_width);
    header.height = htonl(video_height);
    header.format = htonl((uint32_t)PIXFMT_YUYV);
    header.frame_size = htonl(frame_size);
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
    
    return 1;  /* 成功 */
}

/**
 * 发送帧数据（UDP分片发送）
 */
int send_frame_udp(int sock, const uint8_t *data, size_t size, uint32_t frame_num)
{
    /* 先发送帧头 */
    int header_ret = send_frame_header(sock, frame_num);
    if (header_ret < 0) {
        return -1;  /* 发送错误 */
    }
    if (header_ret == 0) {
        return 0;   /* 缓冲区满，跳过这帧 */
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
    int header_ret = send_frame_header(sock, frame_num);
    if (header_ret < 0) {
        return -1;  /* 发送错误 */
    }
    if (header_ret == 0) {
        return 0;   /* 缓冲区满，跳过这帧 */
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

/* ==================== 诊断函数 ==================== */

/**
 * 打印VDMA完整寄存器状态
 */
void dump_vdma_registers(vdma_control_t *vdma)
{
    if (!vdma || !vdma->base_addr) {
        printf("VDMA 未初始化\n");
        return;
    }
    
    volatile uint32_t *base = (volatile uint32_t*)vdma->base_addr;
    
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                    VDMA 完整寄存器转储                        ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    /* MM2S 通道 (Memory to Stream - 读取) */
    printf("║ MM2S 通道 (内存->流):                                        ║\n");
    printf("║   [0x00] Control:     0x%08X                             ║\n", base[0x00/4]);
    printf("║   [0x04] Status:      0x%08X                             ║\n", base[0x04/4]);
    printf("║   [0x50] VSize:       %-6d                                 ║\n", base[0x50/4]);
    printf("║   [0x54] HSize:       %-6d                                 ║\n", base[0x54/4]);
    printf("║   [0x58] Stride:      %-6d                                 ║\n", base[0x58/4]);
    printf("║   [0x5C] Addr1:       0x%08X                             ║\n", base[0x5C/4]);
    printf("║   [0x60] Addr2:       0x%08X                             ║\n", base[0x60/4]);
    printf("║   [0x64] Addr3:       0x%08X                             ║\n", base[0x64/4]);
    
    /* S2MM 通道 (Stream to Memory - 写入) */
    printf("║                                                              ║\n");
    printf("║ S2MM 通道 (流->内存) - 视频写入:                             ║\n");
    uint32_t s2mm_ctrl = base[0x30/4];
    uint32_t s2mm_status = base[0x34/4];
    printf("║   [0x30] Control:     0x%08X                             ║\n", s2mm_ctrl);
    printf("║   [0x34] Status:      0x%08X                             ║\n", s2mm_status);
    printf("║   [0xA0] VSize:       %-6d (期望: %d)                     ║\n", base[0xA0/4], vdma->height);
    printf("║   [0xA4] HSize:       %-6d (期望: %d)                   ║\n", base[0xA4/4], vdma->width * vdma->bytes_per_pixel);
    printf("║   [0xA8] Stride:      %-6d                                 ║\n", base[0xA8/4]);
    printf("║   [0xAC] Addr1:       0x%08X                             ║\n", base[0xAC/4]);
    printf("║   [0xB0] Addr2:       0x%08X                             ║\n", base[0xB0/4]);
    printf("║   [0xB4] Addr3:       0x%08X                             ║\n", base[0xB4/4]);
    
    /* 状态分析 */
    printf("║                                                              ║\n");
    printf("║ S2MM Control 位分析:                                         ║\n");
    printf("║   - Run:              %d                                     ║\n", (s2mm_ctrl >> 0) & 1);
    printf("║   - Circular:         %d                                     ║\n", (s2mm_ctrl >> 1) & 1);
    printf("║   - Reset:            %d                                     ║\n", (s2mm_ctrl >> 2) & 1);
    printf("║   - GenlockEn:        %d                                     ║\n", (s2mm_ctrl >> 3) & 1);
    printf("║   - FrameCntEn:       %d                                     ║\n", (s2mm_ctrl >> 4) & 1);
    
    printf("║                                                              ║\n");
    printf("║ S2MM Status 位分析:                                          ║\n");
    printf("║   - Halted:           %d                                     ║\n", (s2mm_status >> 0) & 1);
    printf("║   - VDMAIntErr:       %d                                     ║\n", (s2mm_status >> 4) & 1);
    printf("║   - VDMASlvErr:       %d                                     ║\n", (s2mm_status >> 5) & 1);
    printf("║   - VDMADecErr:       %d                                     ║\n", (s2mm_status >> 6) & 1);
    printf("║   - SOFEarlyErr:      %d                                     ║\n", (s2mm_status >> 7) & 1);
    printf("║   - EOLEarlyErr:      %d                                     ║\n", (s2mm_status >> 8) & 1);
    printf("║   - SOFLateErr:       %d                                     ║\n", (s2mm_status >> 11) & 1);
    printf("║   - EOLLateErr:       %d                                     ║\n", (s2mm_status >> 12) & 1);
    printf("║   - FrameCount:       %-3d (当前写入帧)                      ║\n", (s2mm_status >> 16) & 0xFF);
    printf("║   - DelayCount:       %-3d                                   ║\n", (s2mm_status >> 24) & 0xFF);
    
    /* 诊断结果 */
    printf("║                                                              ║\n");
    printf("║ 诊断结果:                                                    ║\n");
    
    if (s2mm_status & 0x01) {
        printf("║   ❌ VDMA处于HALTED状态！                                    ║\n");
    }
    if (s2mm_status & 0x10) {
        printf("║   ❌ DMA内部错误                                             ║\n");
    }
    if (s2mm_status & 0x20) {
        printf("║   ❌ DMA从设备错误                                           ║\n");
    }
    if (s2mm_status & 0x40) {
        printf("║   ❌ DMA解码错误                                             ║\n");
    }
    if (!(s2mm_status & 0x01) && (s2mm_ctrl & 0x01)) {
        printf("║   ✓ VDMA正在运行                                             ║\n");
    }
    
    printf("╚══════════════════════════════════════════════════════════════╝\n");
}

/* 新链路默认不再依赖VPSS，所以这里移除了VPSS寄存器转储。 */

/**
 * 详细检查帧缓冲区内容
 */
void check_frame_buffer(vdma_control_t *vdma)
{
    if (!vdma || !vdma->frame_buffer) {
        printf("帧缓冲未初始化\n");
        return;
    }
    
    uint8_t *fb = (uint8_t*)vdma->frame_buffer;
    int frame_size = vdma->width * vdma->height * vdma->bytes_per_pixel;
    int pixels = vdma->width * vdma->height;
    
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                    帧缓冲区详细分析                           ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    
    /* 检查每个帧缓冲 */
    for (int frame = 0; frame < vdma->num_frames; frame++) {
        uint8_t *frame_start = fb + frame * frame_size;
        uint32_t phys_addr = vdma->frame_buffer_phys + frame * frame_size;
        
        printf("\n┌──────────────────────────────────────────────────────────────┐\n");
        printf("│ 帧缓冲 #%d  物理地址: 0x%08X  大小: %d bytes          │\n", 
               frame, phys_addr, frame_size);
        printf("├──────────────────────────────────────────────────────────────┤\n");
        
        /* 多个位置的数据 */
        int line_bytes = vdma->width * vdma->bytes_per_pixel;
        int offsets[] = {0, line_bytes, line_bytes * 100,
                         frame_size / 2, line_bytes * 400, frame_size - line_bytes};
        const char *names[] = {"行0 (开头)", "行1      ", "行100    ", 
                               "行240(中间)", "行400    ", "最后一行 "};
        
        for (int p = 0; p < 6; p++) {
            int offset = offsets[p];
            if (offset >= frame_size) continue;
            
            printf("│ %s [0x%06X]:                                      │\n", names[p], offset);
            printf("│   原始: ");
            for (int i = 0; i < 16 && (offset + i) < frame_size; i++) {
                printf("%02X ", frame_start[offset + i]);
            }
            printf("│\n");

            if (vdma->bytes_per_pixel == 2) {
                /*
                 * 认为是YUV422 packed (常见为YUYV):
                 * 两个像素4字节: Y0 U0 Y1 V0
                 */
                printf("│   YUYV(2px=4B): ");
                for (int i = 0; i < 4; i++) {
                    int idx = offset + i * 4;
                    if (idx + 3 < frame_size) {
                        uint8_t y0 = frame_start[idx + 0];
                        uint8_t u0 = frame_start[idx + 1];
                        uint8_t y1 = frame_start[idx + 2];
                        uint8_t v0 = frame_start[idx + 3];
                        printf("[Y0=%3u U=%3u Y1=%3u V=%3u] ", y0, u0, y1, v0);
                    }
                }
                printf("│\n");
            } else if (vdma->bytes_per_pixel == 4) {
                /* 旧路径：按ARGB {A,R,G,B} 解析 */
                printf("│   ARGB: ");
                for (int i = 0; i < 4; i++) {
                    int idx = offset + i * 4;
                    if (idx + 3 < frame_size) {
                        printf("A%d,R%d,G%d,B%d ",
                               frame_start[idx], frame_start[idx+1],
                               frame_start[idx+2], frame_start[idx+3]);
                    }
                }
                printf("│\n");
            }
        }
        
        /* 统计分析 */
        printf("├──────────────────────────────────────────────────────────────┤\n");
        printf("│ 统计分析:                                                    │\n");
        
        int count_ff = 0, count_00 = 0;
        long byte_sum[4] = {0, 0, 0, 0};
        
        for (int i = 0; i < frame_size; i++) {
            if (frame_start[i] == 0xFF) count_ff++;
            else if (frame_start[i] == 0x00) count_00++;
            byte_sum[i % 4] += frame_start[i];
        }
        
        printf("│   0xFF 字节: %7d / %d (%.1f%%)                        │\n", 
               count_ff, frame_size, 100.0 * count_ff / frame_size);
        printf("│   0x00 字节: %7d / %d (%.1f%%)                        │\n", 
               count_00, frame_size, 100.0 * count_00 / frame_size);
        if (vdma->bytes_per_pixel == 2) {
            /* 粗略统计：按字节位置的均值（便于判断是否有变化） */
            int pairs = pixels / 2;
            if (pairs <= 0) pairs = 1;
            printf("│   Byte0均值: %6.1f (常见为Y0)                             │\n",
                   (float)byte_sum[0] / pairs);
            printf("│   Byte1均值: %6.1f (常见为U0)                             │\n",
                   (float)byte_sum[1] / pairs);
            printf("│   Byte2均值: %6.1f (常见为Y1)                             │\n",
                   (float)byte_sum[2] / pairs);
            printf("│   Byte3均值: %6.1f (常见为V0)                             │\n",
                   (float)byte_sum[3] / pairs);
        } else {
            printf("│   通道0均值: %6.1f (如果ARGB格式，这是Alpha)              │\n",
                   (float)byte_sum[0] / pixels);
            printf("│   通道1均值: %6.1f (如果ARGB格式，这是Red)                │\n",
                   (float)byte_sum[1] / pixels);
            printf("│   通道2均值: %6.1f (如果ARGB格式，这是Green)              │\n",
                   (float)byte_sum[2] / pixels);
            printf("│   通道3均值: %6.1f (如果ARGB格式，这是Blue)               │\n",
                   (float)byte_sum[3] / pixels);
        }
        
        /* 判断数据状态 */
        printf("├──────────────────────────────────────────────────────────────┤\n");
        if (count_ff > frame_size * 0.95) {
            printf("│   ❌ 几乎全是0xFF - VDMA可能未写入数据                       │\n");
        } else if (count_00 > frame_size * 0.95) {
            printf("│   ⚠ 几乎全是0x00 - 可能是黑屏或无信号                       │\n");
        } else {
            printf("│   ✓ 有数据变化 - 可能有有效视频数据                         │\n");
        }
        
        printf("└──────────────────────────────────────────────────────────────┘\n");
    }
}

/**
 * 保存帧数据到文件
 */
int save_frame_to_file(vdma_control_t *vdma, int frame_index, const char *filename)
{
    if (!vdma || !vdma->frame_buffer || frame_index < 0 || frame_index >= vdma->num_frames) {
        printf("参数错误\n");
        return -1;
    }
    
    int frame_size = vdma->width * vdma->height * vdma->bytes_per_pixel;
    uint8_t *frame = (uint8_t*)vdma->frame_buffer + frame_index * frame_size;
    
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror("无法创建文件");
        return -1;
    }
    
    size_t written = fwrite(frame, 1, frame_size, f);
    fclose(f);
    
    if (written != (size_t)frame_size) {
        printf("写入不完整: %zu / %d\n", written, frame_size);
        return -1;
    }
    
    printf("\n✓ 帧 #%d 已保存到 %s (%d 字节)\n", frame_index, filename, frame_size);
    printf("  查看命令: hexdump -C %s | head -100\n", filename);
    printf("  复制到PC: scp root@<board_ip>:%s .\n", filename);
    
    return 0;
}

/* ==================== 主循环 ==================== */

int main_loop()
{
    int frame_count = 0;
    int last_vdma_frame = -1;
    int skipped_frames = 0;  /* 跳过的帧数（帧号未变化） */
    struct timespec start_time, current_time, last_status_time;
    const int frame_size = video_width * video_height * BYTES_PER_PIXEL;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    last_status_time = start_time;
    
    printf("\n开始网络视频流传输...\n");
    printf("分辨率: %dx%d@%dfps (YUV422/YUYV)\n", video_width, video_height, TARGET_FPS);
    printf("协议: %s, 目标: %s:%d\n", use_tcp ? "TCP" : "UDP", target_host, target_port);
    printf("帧缓冲物理地址: 0x%08X\n", frame_buffer_phys);
    printf("帧大小: %d bytes (%.2f MB/s)\n", frame_size, 
           (float)frame_size * TARGET_FPS / 1024 / 1024);
    printf("调试模式: %s\n", debug_mode ? "开启" : "关闭");
    printf("强制发送: %s\n", force_send ? "开启（忽略帧变化检测）" : "关闭");
    printf("按Ctrl+C退出\n\n");
    
    /* 调试：打印初始VDMA状态 */
    if (debug_mode) {
        uint32_t vdma_status = *(volatile uint32_t*)(vdma.base_addr + 0x34);
        printf("[DEBUG] 初始VDMA状态: 0x%08X, 帧号: %d\n", 
               vdma_status, vdma_get_current_frame(&vdma));
        
        /* 检查帧缓冲前16字节 */
        const uint8_t *fb = (uint8_t*)vdma.frame_buffer;
        printf("[DEBUG] 帧缓冲前16字节: ");
        for (int i = 0; i < 16; i++) {
            printf("%02X ", fb[i]);
        }
        printf("\n");
    }
    
    while (running) {
        /* 获取VDMA当前写入的帧 */
        int current_vdma_frame = vdma_get_current_frame(&vdma);
        
        /* 选择一个不同的帧读取 */
        int read_frame = (current_vdma_frame + 1) % num_frames;
        
        /* 如果帧没有变化，根据模式决定是否跳过 */
        if (current_vdma_frame == last_vdma_frame && frame_count > 0) {
            if (!force_send) {
                /* 非强制模式：跳过未变化的帧 */
                skipped_frames++;
                
                /* 调试：每1000次跳过打印一次 */
                if (debug_mode && skipped_frames % 1000 == 0) {
                    printf("[DEBUG] 帧号未变化，已跳过 %d 次，当前帧号: %d\n", 
                           skipped_frames, current_vdma_frame);
                }
                
                usleep(1000);
                continue;
            }
            /* 强制模式：继续发送，但使用当前帧号 */
        }
        last_vdma_frame = current_vdma_frame;
        
        /* 获取帧数据 */
        const uint8_t *frame = (uint8_t*)vdma.frame_buffer + (read_frame * frame_size);
        
        /* 调试：第一帧时打印帧数据信息 */
        if (debug_mode && frame_count == 0) {
            printf("[DEBUG] 发送第一帧，读取帧缓冲 #%d (地址偏移: 0x%X)\n", 
                   read_frame, read_frame * frame_size);
            printf("[DEBUG] 帧数据 开头16字节: ");
            for (int i = 0; i < 16; i++) {
                printf("%02X ", frame[i]);
            }
            printf("\n");
            
            /* 检查中间部分 */
            int mid_offset = frame_size / 2;
            printf("[DEBUG] 帧数据 中间16字节: ");
            for (int i = 0; i < 16; i++) {
                printf("%02X ", frame[mid_offset + i]);
            }
            printf("\n");
            
            /* 检查末尾部分 */
            int end_offset = frame_size - 16;
            printf("[DEBUG] 帧数据 末尾16字节: ");
            for (int i = 0; i < 16; i++) {
                printf("%02X ", frame[end_offset + i]);
            }
            printf("\n");
            
            /* 统计非FF字节比例 */
            int non_ff_count = 0;
            for (int i = 0; i < frame_size; i += 256) {
                if (frame[i] != 0xFF) non_ff_count++;
            }
            int samples = frame_size / 256;
            printf("[DEBUG] 非0xFF数据比例: %d/%d (%.1f%%)\n", 
                   non_ff_count, samples, 100.0 * non_ff_count / samples);
        }
        
        /* 发送帧 */
        int ret;
        if (use_tcp) {
            ret = send_frame_tcp(sock_fd, frame, frame_size, frame_count);
        } else {
            ret = send_frame_udp(sock_fd, frame, frame_size, frame_count);
        }
        
        if (ret < 0) {
            fprintf(stderr, "发送失败，退出\n");
            break;
        }
        
        frame_count++;
        
        /* 每秒打印统计（或每60帧） */
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        double since_last = (current_time.tv_sec - last_status_time.tv_sec) + 
                           (current_time.tv_nsec - last_status_time.tv_nsec) / 1e9;
        
        if (since_last >= 1.0 || frame_count % 60 == 0) {
            double elapsed = (current_time.tv_sec - start_time.tv_sec) + 
                           (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
            double fps = frame_count / elapsed;
            double bitrate = (double)frame_size * frame_count * 8 / elapsed / 1e6;
            
            printf("已发送 %d 帧 (FPS: %.1f, 码率: %.1f Mbps", 
                   frame_count, fps, bitrate);
            if (skipped_frames > 0) {
                printf(", 跳过: %d", skipped_frames);
            }
            printf(")\n");
            
            last_status_time = current_time;
        }
        
        /* 控制帧率 */
        usleep(FRAME_INTERVAL_US);
    }
    
    printf("\n总共发送 %d 帧，跳过 %d 次\n", frame_count, skipped_frames);
    return 0;
}

/* ==================== 帮助信息 ==================== */

void print_usage(const char *prog)
{
    printf("用法: %s [选项]\n", prog);
    printf("\n网络传输选项:\n");
    printf("  -H, --host <IP>      目标IP地址 (默认: %s)\n", DEFAULT_HOST);
    printf("  -p, --port <端口>    目标端口 (默认: %d)\n", DEFAULT_PORT);
    printf("  -t, --tcp            使用TCP协议 (默认: UDP)\n");
    printf("  -f, --force          强制发送模式，忽略帧变化检测\n");
    printf("\n视频参数选项:\n");
    printf("      --width <像素>   图像宽度 (默认: %d)\n", DEFAULT_VIDEO_WIDTH);
    printf("      --height <像素>  图像高度 (默认: %d)\n", DEFAULT_VIDEO_HEIGHT);
    printf("      --fb-phys <hex>  帧缓冲物理地址 (默认: 0x%08X)\n", DEFAULT_FRAME_BUFFER_PHYS);
    printf("\n诊断选项:\n");
    printf("  -d, --debug          调试模式，打印详细诊断信息\n");
    printf("  -D, --diag           仅诊断模式，不进行网络传输\n");
    printf("  -s, --save <文件>    保存帧0数据到文件\n");
    printf("  -h, --help           显示帮助信息\n");
    printf("\n示例:\n");
    printf("  %s -H 10.72.43.200 -p 5000        # UDP模式发送\n", prog);
    printf("  %s -H 10.72.43.200 -d -f          # 调试+强制发送\n", prog);
    printf("  %s -D                             # 仅诊断硬件\n", prog);
    printf("  %s -D -s frame.bin                # 诊断并保存帧数据\n", prog);
    printf("\n诊断选项说明:\n");
    printf("  -d  打印VDMA寄存器状态和帧缓冲内容\n");
    printf("  -D  只运行诊断，不进行网络传输\n");
    printf("  -s  保存帧缓冲#0到二进制文件，可用hexdump或PC端分析\n");
}

/* ==================== 主函数 ==================== */

int main(int argc, char **argv)
{
    int ret = 0;
    
    /* 解析命令行参数 */
    static struct option long_options[] = {
        {"host",  required_argument, 0, 'H'},
        {"port",  required_argument, 0, 'p'},
        {"tcp",   no_argument,       0, 't'},
        {"debug", no_argument,       0, 'd'},
        {"force", no_argument,       0, 'f'},
        {"diag",  no_argument,       0, 'D'},
        {"save",  required_argument, 0, 's'},
        {"width", required_argument, 0,  1 },
        {"height",required_argument, 0,  2 },
        {"fb-phys",required_argument,0,  3 },
        {"help",  no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "H:p:tdfDs:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'H':
                strncpy(target_host, optarg, sizeof(target_host) - 1);
                break;
            case 'p':
                target_port = atoi(optarg);
                break;
            case 't':
                use_tcp = 1;
                break;
            case 'd':
                debug_mode = 1;
                break;
            case 'f':
                force_send = 1;
                break;
            case 'D':
                diag_only = 1;
                debug_mode = 1;  /* 诊断模式自动开启调试 */
                break;
            case 's':
                strncpy(save_file, optarg, sizeof(save_file) - 1);
                diag_only = 1;   /* 保存文件也进入诊断模式 */
                debug_mode = 1;
                break;
            case 1:
                video_width = atoi(optarg);
                break;
            case 2:
                video_height = atoi(optarg);
                break;
            case 3:
                frame_buffer_phys = (uint32_t)strtoul(optarg, NULL, 0);
                break;
            case 'h':
            case '?':
            default:
                print_usage(argv[0]);
                return 0;
        }
    }
    
    printf("========================================\n");
    printf("网络视频流传输应用\n");
    printf("Xilinx Zynq UltraScale+ MPSoC\n");
    printf("CameraLink YUV422 over Ethernet\n");
    printf("========================================\n\n");
    
    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* 初始化VDMA */
    printf("[1/4] 初始化VDMA...\n");
    if (video_width <= 0 || video_height <= 0) {
        fprintf(stderr, "分辨率参数非法: %dx%d\n", video_width, video_height);
        ret = 1;
        goto cleanup;
    }
    num_frames = DEFAULT_NUM_FRAMES;
    const int bytes_per_pixel = BYTES_PER_PIXEL;
    if (vdma_init(&vdma, video_width, video_height, 
                  bytes_per_pixel, num_frames,
                  frame_buffer_phys) < 0) {
        fprintf(stderr, "VDMA初始化失败\n");
        ret = 1;
        goto cleanup;
    }
    
    /* 启动VDMA */
    printf("\n[2/4] 启动VDMA...\n");
    if (vdma_start(&vdma) < 0) {
        fprintf(stderr, "VDMA启动失败\n");
        ret = 1;
        goto cleanup;
    }
    
    /* 等待数据流稳定 */
    printf("\n等待视频流稳定...\n");
    sleep(1);
    
    /* 诊断模式：打印详细寄存器信息 */
    if (debug_mode) {
        dump_vdma_registers(&vdma);
        check_frame_buffer(&vdma);
    }
    
    /* 仅诊断模式：输出诊断后退出 */
    if (diag_only) {
        /* 如果指定了保存文件 */
        if (save_file[0] != '\0') {
            save_frame_to_file(&vdma, 0, save_file);
        }
        
        printf("\n====== 诊断完成 ======\n");
        printf("后续操作:\n");
        printf("  - 网络传输测试: %s -H %s -p %d -d -f\n", argv[0], target_host, target_port);
        printf("  - 保存帧数据:   %s -D -s frame.bin\n", argv[0]);
        goto cleanup;
    }
    
    /* 初始化网络 */
    printf("\n[3/4] 初始化网络连接...\n");
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
    printf("\n[4/4] 开始发送...\n");
    ret = main_loop();
    
cleanup:
    printf("\n清理资源...\n");
    
    if (sock_fd >= 0) {
        close(sock_fd);
    }
    
    vdma_cleanup(&vdma);
    
    printf("程序退出\n");
    return ret;
}
