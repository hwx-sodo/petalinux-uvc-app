/**
 * @file network_stream.c
 * @brief 网络视频流传输应用程序（服务端）
 * 
 * 功能：
 * 1. 初始化VPSS和VDMA
 * 2. 从DDR读取视频帧（RGBA格式）
 * 3. 通过UDP/TCP网络发送到PC端
 * 
 * 数据流（两种常见情况）：
 * 1) 走VPSS颜色转换: CameraLink(PL) → VPSS(YUV422→RGB) → VDMA → DDR(RGBA) → 网络 → PC
 * 2) 不走VPSS直写:   CameraLink(PL) → (AXIS宽度转换/打包) → VDMA → DDR(YUV422) → 网络 → PC
 * 
 * 使用方法：
 *   ./network-stream-app -H <PC_IP地址> [-p 端口] [-t] [--format rgba|yuyv|uyvy] [--no-vpss]
 * 
 * 示例：
 *   ./network-stream-app -H 10.72.43.200 -p 5000        # UDP模式
 *   ./network-stream-app -H 10.72.43.200 -p 5000 -t     # TCP模式
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */
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
#define BYTES_PER_PIXEL 4    /* 默认: RGBA格式：32位 */
#define NUM_FRAMES      3    /* 三缓冲 */

/* 帧缓冲物理地址 */
#define FRAME_BUFFER_PHYS   0x20000000  /* 与设备树reserved_memory一致 (0x20000000-0x40000000) */

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
    uint32_t format;        /* 像素格式: 0=RGBA, 1=YUYV(YUV422), 2=UYVY(YUV422) */
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
static int debug_mode = 0;  /* 调试模式：打印更多信息 */
static int force_send = 0;  /* 强制发送模式：忽略帧变化检测 */
static int diag_only = 0;   /* 仅诊断模式：不进行网络传输 */
static char save_file[256] = "";  /* 保存帧数据到文件 */
static int no_vpss = 0;     /* 不初始化/启动VPSS（当硬件链路不经过VPSS时使用） */

/* 当前视频参数（可通过参数覆盖） */
static int video_width = VIDEO_WIDTH;
static int video_height = VIDEO_HEIGHT;
static int bytes_per_pixel = BYTES_PER_PIXEL;
static size_t frame_size = 0;

typedef enum {
    PIXFMT_RGBA = 0,
    PIXFMT_YUYV = 1,
    PIXFMT_UYVY = 2,
} pixel_format_t;

static pixel_format_t pixel_format = PIXFMT_RGBA;

static const char* pixel_format_to_string(pixel_format_t fmt)
{
    switch (fmt) {
        case PIXFMT_RGBA: return "RGBA";
        case PIXFMT_YUYV: return "YUYV (YUV422)";
        case PIXFMT_UYVY: return "UYVY (YUV422)";
        default: return "UNKNOWN";
    }
}

static int pixel_format_to_bpp(pixel_format_t fmt)
{
    switch (fmt) {
        case PIXFMT_RGBA: return 4;
        case PIXFMT_YUYV: return 2;
        case PIXFMT_UYVY: return 2;
        default: return 4;
    }
}

static pixel_format_t parse_pixel_format(const char *s)
{
    if (!s) return PIXFMT_RGBA;
    if (strcasecmp(s, "rgba") == 0) return PIXFMT_RGBA;
    if (strcasecmp(s, "yuyv") == 0) return PIXFMT_YUYV;
    if (strcasecmp(s, "uyvy") == 0) return PIXFMT_UYVY;
    return PIXFMT_RGBA;
}

static void hexdump_bytes(const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        printf("%02X ", p[i]);
    }
}

/**
 * 打印一段内存的前若干个字节，同时以16bit/32bit(小端)视角输出。
 * 这样你可以直接对照硬件的“16位拼接”和“32位打包”是否发生了字节/半字交换。
 */
static void dump_first_words(const uint8_t *p, size_t len)
{
    size_t n = len < 32 ? len : 32;
    printf("  原始前%zu字节: ", n);
    hexdump_bytes(p, n);
    printf("\n");

    size_t w16 = (n / 2);
    printf("  16bit(LE) 前%zu个: ", w16);
    for (size_t i = 0; i < w16; i++) {
        uint16_t v = (uint16_t)p[i * 2] | ((uint16_t)p[i * 2 + 1] << 8);
        printf("%04X ", v);
    }
    printf("\n");

    size_t w32 = (n / 4);
    printf("  32bit(LE) 前%zu个: ", w32);
    for (size_t i = 0; i < w32; i++) {
        uint32_t v = (uint32_t)p[i * 4] |
                     ((uint32_t)p[i * 4 + 1] << 8) |
                     ((uint32_t)p[i * 4 + 2] << 16) |
                     ((uint32_t)p[i * 4 + 3] << 24);
        printf("%08X ", v);
    }
    printf("\n");
}

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

/**
 * 打印VPSS完整寄存器状态
 */
void dump_vpss_registers(vpss_control_t *vpss)
{
    if (!vpss || !vpss->base_addr) {
        printf("VPSS 未初始化\n");
        return;
    }
    
    volatile uint32_t *base = (volatile uint32_t*)vpss->base_addr;
    
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                    VPSS 完整寄存器转储                        ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    /* 基本控制寄存器 */
    uint32_t ctrl = base[0x00/4];
    uint32_t gie = base[0x04/4];
    uint32_t ier = base[0x08/4];
    uint32_t isr = base[0x0C/4];
    uint32_t version = base[0x10/4];
    
    printf("║ 基本控制寄存器:                                              ║\n");
    printf("║   [0x00] Control:     0x%08X                             ║\n", ctrl);
    printf("║   [0x04] GIE:         0x%08X                             ║\n", gie);
    printf("║   [0x08] IER:         0x%08X                             ║\n", ier);
    printf("║   [0x0C] ISR:         0x%08X                             ║\n", isr);
    printf("║   [0x10] Version:     0x%08X                             ║\n", version);
    
    /* Control 位分析 */
    printf("║                                                              ║\n");
    printf("║ Control 位分析:                                              ║\n");
    printf("║   - ap_start:         %d                                     ║\n", (ctrl >> 0) & 1);
    printf("║   - ap_done:          %d                                     ║\n", (ctrl >> 1) & 1);
    printf("║   - ap_idle:          %d                                     ║\n", (ctrl >> 2) & 1);
    printf("║   - ap_ready:         %d                                     ║\n", (ctrl >> 3) & 1);
    printf("║   - auto_restart:     %d                                     ║\n", (ctrl >> 7) & 1);
    
    /* 扩展寄存器 - 可能包含配置参数 */
    printf("║                                                              ║\n");
    printf("║ 扩展寄存器 (0x20-0x7C):                                      ║\n");
    for (int i = 0x20; i < 0x80; i += 0x10) {
        printf("║   [0x%02X]: 0x%08X  [0x%02X]: 0x%08X  [0x%02X]: 0x%08X  [0x%02X]: 0x%08X ║\n",
               i, base[i/4], i+4, base[(i+4)/4], i+8, base[(i+8)/4], i+12, base[(i+12)/4]);
    }
    
    /* 诊断 */
    printf("║                                                              ║\n");
    printf("║ 诊断结果:                                                    ║\n");
    if (isr != 0) {
        printf("║   ❌ ISR有错误标志: 0x%08X                               ║\n", isr);
    }
    if (version == 0) {
        printf("║   ⚠ 版本号为0，可能不是标准VPSS IP                         ║\n");
    }
    if ((ctrl & 0x01) && (ctrl & 0x04)) {
        printf("║   ✓ VPSS已启动且处于Idle状态                                ║\n");
    } else if (ctrl & 0x01) {
        printf("║   ✓ VPSS已启动，正在处理                                    ║\n");
    } else {
        printf("║   ❌ VPSS未启动                                              ║\n");
    }
    
    printf("╚══════════════════════════════════════════════════════════════╝\n");
}

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
        int offsets[] = {0, vdma->width * 4, vdma->width * 4 * 100, 
                         frame_size / 2, vdma->width * 4 * 400, frame_size - vdma->width * 4};
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
            
            /* 按ARGB {A,R,G,B} 解析 */
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
        printf("│   通道0均值: %6.1f (如果ARGB格式，这是Alpha)              │\n", 
               (float)byte_sum[0] / pixels);
        printf("│   通道1均值: %6.1f (如果ARGB格式，这是Red)                │\n", 
               (float)byte_sum[1] / pixels);
        printf("│   通道2均值: %6.1f (如果ARGB格式，这是Green)              │\n", 
               (float)byte_sum[2] / pixels);
        printf("│   通道3均值: %6.1f (如果ARGB格式，这是Blue)               │\n", 
               (float)byte_sum[3] / pixels);
        
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
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    last_status_time = start_time;
    
    printf("\n开始网络视频流传输...\n");
    printf("分辨率: %dx%d@%dfps (RGBA格式)\n", VIDEO_WIDTH, VIDEO_HEIGHT, TARGET_FPS);
    printf("协议: %s, 目标: %s:%d\n", use_tcp ? "TCP" : "UDP", target_host, target_port);
    printf("帧大小: %d bytes (%.2f MB/s)\n", FRAME_SIZE, 
           (float)FRAME_SIZE * TARGET_FPS / 1024 / 1024);
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
        int read_frame = (current_vdma_frame + 1) % NUM_FRAMES;
        
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
        const uint8_t *rgba_frame = (uint8_t*)vdma.frame_buffer + (read_frame * FRAME_SIZE);
        
        /* 调试：第一帧时打印帧数据信息 */
        if (debug_mode && frame_count == 0) {
            printf("[DEBUG] 发送第一帧，读取帧缓冲 #%d (地址偏移: 0x%X)\n", 
                   read_frame, read_frame * FRAME_SIZE);
            printf("[DEBUG] 帧数据 开头16字节: ");
            for (int i = 0; i < 16; i++) {
                printf("%02X ", rgba_frame[i]);
            }
            printf("\n");
            
            /* 检查中间部分 */
            int mid_offset = FRAME_SIZE / 2;
            printf("[DEBUG] 帧数据 中间16字节: ");
            for (int i = 0; i < 16; i++) {
                printf("%02X ", rgba_frame[mid_offset + i]);
            }
            printf("\n");
            
            /* 检查末尾部分 */
            int end_offset = FRAME_SIZE - 16;
            printf("[DEBUG] 帧数据 末尾16字节: ");
            for (int i = 0; i < 16; i++) {
                printf("%02X ", rgba_frame[end_offset + i]);
            }
            printf("\n");
            
            /* 统计非FF字节比例 */
            int non_ff_count = 0;
            for (int i = 0; i < FRAME_SIZE; i += 256) {
                if (rgba_frame[i] != 0xFF) non_ff_count++;
            }
            int samples = FRAME_SIZE / 256;
            printf("[DEBUG] 非0xFF数据比例: %d/%d (%.1f%%)\n", 
                   non_ff_count, samples, 100.0 * non_ff_count / samples);
        }
        
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
        
        /* 每秒打印统计（或每60帧） */
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        double since_last = (current_time.tv_sec - last_status_time.tv_sec) + 
                           (current_time.tv_nsec - last_status_time.tv_nsec) / 1e9;
        
        if (since_last >= 1.0 || frame_count % 60 == 0) {
            double elapsed = (current_time.tv_sec - start_time.tv_sec) + 
                           (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
            double fps = frame_count / elapsed;
            double bitrate = (double)FRAME_SIZE * frame_count * 8 / elapsed / 1e6;
            
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
    printf("  -d  打印VPSS/VDMA寄存器状态和帧缓冲内容\n");
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
    
    /* 诊断模式：打印详细寄存器信息 */
    if (debug_mode) {
        dump_vpss_registers(&vpss);
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
