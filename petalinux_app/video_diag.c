/**
 * @file video_diag.c
 * @brief 视频处理链路诊断工具
 * 
 * 功能：
 * 1. 详细检查 VPSS 寄存器状态
 * 2. 详细检查 VDMA 寄存器状态
 * 3. 分析帧缓冲区数据
 * 4. 保存帧数据到文件以便分析
 * 
 * 使用方法：
 *   ./video-diag              # 基本诊断
 *   ./video-diag -s frame.bin # 保存帧数据到文件
 *   ./video-diag -w           # 持续监控模式
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <signal.h>

/* ==================== 硬件地址定义 ==================== */

/* VPSS 地址 */
#define VPSS_BASE_ADDR    0x80000000
#define VPSS_ADDR_SIZE    0x10000

/* VDMA 地址 */
#define VDMA_BASE_ADDR    0x80020000
#define VDMA_ADDR_SIZE    0x10000

/* 帧缓冲区 */
#define FRAME_BUFFER_PHYS 0x20000000
#define FRAME_BUFFER_SIZE 0x20000000  /* 512MB */

/* 视频参数 */
#define VIDEO_WIDTH       640
#define VIDEO_HEIGHT      480
#define BYTES_PER_PIXEL   4
#define FRAME_SIZE        (VIDEO_WIDTH * VIDEO_HEIGHT * BYTES_PER_PIXEL)
#define NUM_FRAMES        3

/* ==================== 全局变量 ==================== */

static volatile int running = 1;
static void *vpss_base = NULL;
static void *vdma_base = NULL;
static void *frame_buffer = NULL;
static int vpss_fd = -1;
static int vdma_fd = -1;

/* ==================== 信号处理 ==================== */

void signal_handler(int signum) {
    running = 0;
}

/* ==================== UIO 设备查找 ==================== */

int find_uio_by_addr(uint32_t target_addr, char *dev_path, size_t path_size) {
    char addr_path[128];
    char addr_str[64];
    unsigned long addr;
    
    for (int i = 0; i < 10; i++) {
        snprintf(addr_path, sizeof(addr_path), 
                 "/sys/class/uio/uio%d/maps/map0/addr", i);
        
        FILE *f = fopen(addr_path, "r");
        if (!f) continue;
        
        if (fgets(addr_str, sizeof(addr_str), f)) {
            addr = strtoul(addr_str, NULL, 0);
            if (addr == target_addr) {
                snprintf(dev_path, path_size, "/dev/uio%d", i);
                fclose(f);
                return 0;
            }
        }
        fclose(f);
    }
    return -1;
}

/* ==================== 初始化 ==================== */

int init_hardware(void) {
    char dev_path[64];
    
    /* 查找并打开 VPSS UIO */
    if (find_uio_by_addr(VPSS_BASE_ADDR, dev_path, sizeof(dev_path)) == 0) {
        printf("找到 VPSS: %s (0x%08X)\n", dev_path, VPSS_BASE_ADDR);
        vpss_fd = open(dev_path, O_RDWR);
        if (vpss_fd >= 0) {
            vpss_base = mmap(NULL, VPSS_ADDR_SIZE, PROT_READ | PROT_WRITE,
                            MAP_SHARED, vpss_fd, 0);
            if (vpss_base == MAP_FAILED) {
                vpss_base = NULL;
                printf("  ❌ 映射失败\n");
            } else {
                printf("  ✓ 映射成功: %p\n", vpss_base);
            }
        }
    } else {
        printf("❌ 未找到 VPSS UIO 设备\n");
    }
    
    /* 查找并打开 VDMA UIO */
    if (find_uio_by_addr(VDMA_BASE_ADDR, dev_path, sizeof(dev_path)) == 0) {
        printf("找到 VDMA: %s (0x%08X)\n", dev_path, VDMA_BASE_ADDR);
        vdma_fd = open(dev_path, O_RDWR);
        if (vdma_fd >= 0) {
            vdma_base = mmap(NULL, VDMA_ADDR_SIZE, PROT_READ | PROT_WRITE,
                            MAP_SHARED, vdma_fd, 0);
            if (vdma_base == MAP_FAILED) {
                vdma_base = NULL;
                printf("  ❌ 映射失败\n");
            } else {
                printf("  ✓ 映射成功: %p\n", vdma_base);
            }
        }
    } else {
        printf("❌ 未找到 VDMA UIO 设备\n");
    }
    
    /* 映射帧缓冲区 */
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd >= 0) {
        frame_buffer = mmap(NULL, NUM_FRAMES * FRAME_SIZE,
                           PROT_READ | PROT_WRITE, MAP_SHARED,
                           mem_fd, FRAME_BUFFER_PHYS);
        close(mem_fd);
        
        if (frame_buffer == MAP_FAILED) {
            frame_buffer = NULL;
            printf("❌ 帧缓冲映射失败\n");
        } else {
            printf("✓ 帧缓冲映射成功: %p (物理地址 0x%08X)\n", 
                   frame_buffer, FRAME_BUFFER_PHYS);
        }
    }
    
    return (vpss_base || vdma_base || frame_buffer) ? 0 : -1;
}

/* ==================== 清理 ==================== */

void cleanup(void) {
    if (vpss_base) munmap(vpss_base, VPSS_ADDR_SIZE);
    if (vdma_base) munmap(vdma_base, VDMA_ADDR_SIZE);
    if (frame_buffer) munmap(frame_buffer, NUM_FRAMES * FRAME_SIZE);
    if (vpss_fd >= 0) close(vpss_fd);
    if (vdma_fd >= 0) close(vdma_fd);
}

/* ==================== VPSS 诊断 ==================== */

void dump_vpss_full(void) {
    if (!vpss_base) {
        printf("VPSS 未初始化\n");
        return;
    }
    
    volatile uint32_t *reg = (volatile uint32_t*)vpss_base;
    
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                    VPSS 完整寄存器转储                        ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    /* 基本控制寄存器 */
    printf("║ 基本控制寄存器:                                              ║\n");
    printf("║   [0x00] Control:     0x%08X                             ║\n", reg[0x00/4]);
    printf("║   [0x04] GIE:         0x%08X                             ║\n", reg[0x04/4]);
    printf("║   [0x08] IER:         0x%08X                             ║\n", reg[0x08/4]);
    printf("║   [0x0C] ISR:         0x%08X                             ║\n", reg[0x0C/4]);
    
    /* 分析 Control 寄存器 */
    uint32_t ctrl = reg[0x00/4];
    printf("║                                                              ║\n");
    printf("║   Control 位分析:                                            ║\n");
    printf("║     - ap_start:       %d                                     ║\n", (ctrl >> 0) & 1);
    printf("║     - ap_done:        %d                                     ║\n", (ctrl >> 1) & 1);
    printf("║     - ap_idle:        %d                                     ║\n", (ctrl >> 2) & 1);
    printf("║     - ap_ready:       %d                                     ║\n", (ctrl >> 3) & 1);
    printf("║     - auto_restart:   %d                                     ║\n", (ctrl >> 7) & 1);
    
    /* 读取更多寄存器 */
    printf("║                                                              ║\n");
    printf("║ 扩展寄存器 (前64个):                                         ║\n");
    for (int i = 0; i < 64; i += 4) {
        printf("║   [0x%02X]: 0x%08X  [0x%02X]: 0x%08X  [0x%02X]: 0x%08X  [0x%02X]: 0x%08X ║\n",
               i*4, reg[i], (i+1)*4, reg[i+1], (i+2)*4, reg[i+2], (i+3)*4, reg[i+3]);
    }
    
    printf("╚══════════════════════════════════════════════════════════════╝\n");
}

/* ==================== VDMA 诊断 ==================== */

void dump_vdma_full(void) {
    if (!vdma_base) {
        printf("VDMA 未初始化\n");
        return;
    }
    
    volatile uint32_t *reg = (volatile uint32_t*)vdma_base;
    
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                    VDMA 完整寄存器转储                        ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    /* MM2S 通道 (Memory to Stream - 读取) */
    printf("║ MM2S 通道 (内存->流):                                        ║\n");
    printf("║   [0x00] Control:     0x%08X                             ║\n", reg[0x00/4]);
    printf("║   [0x04] Status:      0x%08X                             ║\n", reg[0x04/4]);
    printf("║   [0x50] VSize:       %d                                   ║\n", reg[0x50/4]);
    printf("║   [0x54] HSize:       %d                                 ║\n", reg[0x54/4]);
    printf("║   [0x58] Stride:      %d                                 ║\n", reg[0x58/4]);
    printf("║   [0x5C] Addr1:       0x%08X                             ║\n", reg[0x5C/4]);
    printf("║   [0x60] Addr2:       0x%08X                             ║\n", reg[0x60/4]);
    printf("║   [0x64] Addr3:       0x%08X                             ║\n", reg[0x64/4]);
    
    /* S2MM 通道 (Stream to Memory - 写入) */
    printf("║                                                              ║\n");
    printf("║ S2MM 通道 (流->内存):                                        ║\n");
    uint32_t s2mm_ctrl = reg[0x30/4];
    uint32_t s2mm_status = reg[0x34/4];
    printf("║   [0x30] Control:     0x%08X                             ║\n", s2mm_ctrl);
    printf("║   [0x34] Status:      0x%08X                             ║\n", s2mm_status);
    printf("║   [0xA0] VSize:       %d                                   ║\n", reg[0xA0/4]);
    printf("║   [0xA4] HSize:       %d                                 ║\n", reg[0xA4/4]);
    printf("║   [0xA8] Stride:      %d                                 ║\n", reg[0xA8/4]);
    printf("║   [0xAC] Addr1:       0x%08X                             ║\n", reg[0xAC/4]);
    printf("║   [0xB0] Addr2:       0x%08X                             ║\n", reg[0xB0/4]);
    printf("║   [0xB4] Addr3:       0x%08X                             ║\n", reg[0xB4/4]);
    
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
    printf("║   - FrameCount:       %d                                     ║\n", (s2mm_status >> 16) & 0xFF);
    printf("║   - DelayCount:       %d                                     ║\n", (s2mm_status >> 24) & 0xFF);
    
    printf("╚══════════════════════════════════════════════════════════════╝\n");
}

/* ==================== 帧缓冲区诊断 ==================== */

void analyze_frame_buffer(int frame_index) {
    if (!frame_buffer) {
        printf("帧缓冲未初始化\n");
        return;
    }
    
    uint8_t *frame = (uint8_t*)frame_buffer + frame_index * FRAME_SIZE;
    uint32_t phys_addr = FRAME_BUFFER_PHYS + frame_index * FRAME_SIZE;
    
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║              帧缓冲 #%d 详细分析 (物理地址: 0x%08X)        ║\n", 
           frame_index, phys_addr);
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    /* 显示多个位置的数据 */
    int positions[] = {0, 640*4, 640*4*100, 640*4*240, 640*4*400, FRAME_SIZE - 640*4};
    const char *pos_names[] = {"行0开头", "行1开头", "行100开头", "行240(中间)", "行400", "最后一行"};
    
    for (int p = 0; p < 6; p++) {
        int offset = positions[p];
        printf("║ %s (偏移 0x%06X):                                    ║\n", 
               pos_names[p], offset);
        printf("║   原始字节: ");
        for (int i = 0; i < 16; i++) {
            printf("%02X ", frame[offset + i]);
        }
        printf("║\n");
        
        /* 按不同格式解析前4个像素 */
        printf("║   按RGBA解析: ");
        for (int i = 0; i < 4; i++) {
            int idx = offset + i * 4;
            printf("(%d,%d,%d,%d) ", frame[idx], frame[idx+1], frame[idx+2], frame[idx+3]);
        }
        printf("║\n");
        
        printf("║   按ARGB解析: ");
        for (int i = 0; i < 4; i++) {
            int idx = offset + i * 4;
            printf("A=%d,R=%d,G=%d,B=%d ", frame[idx], frame[idx+1], frame[idx+2], frame[idx+3]);
        }
        printf("║\n");
        printf("║                                                              ║\n");
    }
    
    /* 统计分析 */
    int count_ff = 0, count_00 = 0, count_other = 0;
    int byte_sum[4] = {0, 0, 0, 0};  /* 每个通道的和 */
    
    for (int i = 0; i < FRAME_SIZE; i++) {
        if (frame[i] == 0xFF) count_ff++;
        else if (frame[i] == 0x00) count_00++;
        else count_other++;
        
        byte_sum[i % 4] += frame[i];
    }
    
    int pixels = VIDEO_WIDTH * VIDEO_HEIGHT;
    printf("║ 统计分析:                                                    ║\n");
    printf("║   0xFF 字节数: %d (%.1f%%)                                  ║\n", 
           count_ff, 100.0 * count_ff / FRAME_SIZE);
    printf("║   0x00 字节数: %d (%.1f%%)                                  ║\n", 
           count_00, 100.0 * count_00 / FRAME_SIZE);
    printf("║   其他字节数:  %d (%.1f%%)                                  ║\n", 
           count_other, 100.0 * count_other / FRAME_SIZE);
    printf("║                                                              ║\n");
    printf("║   通道0平均值: %.1f (如果是ARGB，这是Alpha)                  ║\n", 
           (float)byte_sum[0] / pixels);
    printf("║   通道1平均值: %.1f (如果是ARGB，这是Red)                    ║\n", 
           (float)byte_sum[1] / pixels);
    printf("║   通道2平均值: %.1f (如果是ARGB，这是Green)                  ║\n", 
           (float)byte_sum[2] / pixels);
    printf("║   通道3平均值: %.1f (如果是ARGB，这是Blue)                   ║\n", 
           (float)byte_sum[3] / pixels);
    
    printf("╚══════════════════════════════════════════════════════════════╝\n");
}

/* ==================== 保存帧数据 ==================== */

int save_frame_to_file(int frame_index, const char *filename) {
    if (!frame_buffer) {
        printf("帧缓冲未初始化\n");
        return -1;
    }
    
    uint8_t *frame = (uint8_t*)frame_buffer + frame_index * FRAME_SIZE;
    
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror("无法创建文件");
        return -1;
    }
    
    size_t written = fwrite(frame, 1, FRAME_SIZE, f);
    fclose(f);
    
    if (written != FRAME_SIZE) {
        printf("写入不完整: %zu / %d\n", written, FRAME_SIZE);
        return -1;
    }
    
    printf("✓ 帧 #%d 已保存到 %s (%d 字节)\n", frame_index, filename, FRAME_SIZE);
    printf("  可以用以下命令查看:\n");
    printf("    hexdump -C %s | head -100\n", filename);
    printf("  或者复制到PC分析:\n");
    printf("    scp root@<board_ip>:%s .\n", filename);
    
    return 0;
}

/* ==================== 监控模式 ==================== */

void watch_mode(void) {
    printf("\n持续监控模式 (按 Ctrl+C 退出)\n");
    printf("========================================\n\n");
    
    uint32_t last_frame_count = 0;
    
    while (running) {
        if (vdma_base) {
            volatile uint32_t *reg = (volatile uint32_t*)vdma_base;
            uint32_t status = reg[0x34/4];
            uint32_t frame_count = (status >> 16) & 0xFF;
            
            printf("\rVDMA: Status=0x%08X, FrameCount=%d, Halted=%d  ", 
                   status, frame_count, status & 1);
            
            if (frame_count != last_frame_count) {
                printf("(帧变化: %d -> %d)", last_frame_count, frame_count);
                last_frame_count = frame_count;
            }
            
            fflush(stdout);
        }
        
        usleep(100000);  /* 100ms */
    }
    
    printf("\n");
}

/* ==================== 帮助信息 ==================== */

void print_usage(const char *prog) {
    printf("视频处理链路诊断工具\n\n");
    printf("用法: %s [选项]\n\n", prog);
    printf("选项:\n");
    printf("  -v, --vpss       显示 VPSS 详细寄存器\n");
    printf("  -d, --vdma       显示 VDMA 详细寄存器\n");
    printf("  -f, --frame N    分析帧缓冲 N (0, 1, 2)\n");
    printf("  -a, --all        显示所有诊断信息\n");
    printf("  -s, --save FILE  保存帧 0 到文件\n");
    printf("  -w, --watch      持续监控模式\n");
    printf("  -h, --help       显示帮助\n");
    printf("\n示例:\n");
    printf("  %s -a                    # 显示所有诊断\n", prog);
    printf("  %s -f 0 -f 1 -f 2        # 分析所有帧缓冲\n", prog);
    printf("  %s -s frame0.bin         # 保存帧数据\n", prog);
    printf("  %s -w                    # 监控帧计数变化\n", prog);
}

/* ==================== 主函数 ==================== */

int main(int argc, char **argv) {
    int show_vpss = 0, show_vdma = 0, show_all = 0, watch = 0;
    int frame_indices[3] = {-1, -1, -1};
    int frame_count = 0;
    char *save_file = NULL;
    
    static struct option long_options[] = {
        {"vpss",  no_argument,       0, 'v'},
        {"vdma",  no_argument,       0, 'd'},
        {"frame", required_argument, 0, 'f'},
        {"all",   no_argument,       0, 'a'},
        {"save",  required_argument, 0, 's'},
        {"watch", no_argument,       0, 'w'},
        {"help",  no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "vdf:as:wh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'v': show_vpss = 1; break;
            case 'd': show_vdma = 1; break;
            case 'f': 
                if (frame_count < 3) {
                    frame_indices[frame_count++] = atoi(optarg);
                }
                break;
            case 'a': show_all = 1; break;
            case 's': save_file = optarg; break;
            case 'w': watch = 1; break;
            case 'h':
            default:
                print_usage(argv[0]);
                return 0;
        }
    }
    
    /* 如果没有指定选项，显示所有 */
    if (!show_vpss && !show_vdma && frame_count == 0 && !show_all && !save_file && !watch) {
        show_all = 1;
    }
    
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║              视频处理链路诊断工具                            ║\n");
    printf("║              ZynqMP IR Camera Debug                          ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");
    
    signal(SIGINT, signal_handler);
    
    /* 初始化硬件 */
    printf("初始化硬件...\n");
    if (init_hardware() < 0) {
        printf("硬件初始化失败\n");
        return 1;
    }
    printf("\n");
    
    /* 执行诊断 */
    if (show_all || show_vpss) {
        dump_vpss_full();
    }
    
    if (show_all || show_vdma) {
        dump_vdma_full();
    }
    
    if (show_all) {
        /* 分析所有帧缓冲 */
        for (int i = 0; i < NUM_FRAMES; i++) {
            analyze_frame_buffer(i);
        }
    } else {
        /* 分析指定帧缓冲 */
        for (int i = 0; i < frame_count; i++) {
            if (frame_indices[i] >= 0 && frame_indices[i] < NUM_FRAMES) {
                analyze_frame_buffer(frame_indices[i]);
            }
        }
    }
    
    /* 保存帧数据 */
    if (save_file) {
        save_frame_to_file(0, save_file);
    }
    
    /* 监控模式 */
    if (watch) {
        watch_mode();
    }
    
    /* 清理 */
    cleanup();
    printf("\n诊断完成\n");
    
    return 0;
}
