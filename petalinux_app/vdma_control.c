/**
 * @file vdma_control.c
 * @brief VDMA (Video DMA) 控制实现
 * 
 * 数据流架构：
 *   CameraLink(PL, portA[7:0]+portB[7:0] → 16-bit)
 *     → Video In to AXI4-Stream (16-bit)
 *     → AXI4-Stream Data Width Converter (16-bit → 32-bit)
 *     → VDMA S2MM (32-bit总线写入DDR)
 *     → DDR (YUV422/YUYV字节序存储)
 * 
 * 重要说明：
 *   1. VDMA使用S2MM通道（Stream to Memory Mapped）接收视频流
 *   2. 数据宽度转换器将16-bit像素扩展为32-bit（2像素打包）
 *   3. DDR中按YUYV格式存储：Y0 U Y1 V（每4字节表示2像素）
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "vdma_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/mman.h>

/*============================================================================
 * 内部辅助宏
 *============================================================================*/

/** 寄存器读取 */
#define REG_READ(ctx, offset) \
    (*(volatile uint32_t*)((uint8_t*)(ctx)->regs + (offset)))

/** 寄存器写入 */
#define REG_WRITE(ctx, offset, value) \
    (*(volatile uint32_t*)((uint8_t*)(ctx)->regs + (offset)) = (value))

/** 日志宏 */
#define LOG_INFO(fmt, ...)  printf("[VDMA] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[VDMA ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) printf("[VDMA DEBUG] " fmt "\n", ##__VA_ARGS__)

/*============================================================================
 * 内部辅助函数
 *============================================================================*/

/**
 * 在 /sys/class/uio 中查找匹配指定物理地址的UIO设备
 */
static int find_uio_device(uint32_t target_addr, char *dev_path, size_t path_size)
{
    DIR *dp;
    struct dirent *entry;
    char addr_path[256];
    char addr_str[64];
    
    dp = opendir("/sys/class/uio");
    if (!dp) {
        LOG_ERROR("无法打开 /sys/class/uio");
        return -1;
    }
    
    while ((entry = readdir(dp)) != NULL) {
        /* 只处理 uioX 目录 */
        if (strncmp(entry->d_name, "uio", 3) != 0) {
            continue;
        }
        
        /* 读取物理地址 */
        snprintf(addr_path, sizeof(addr_path), 
                 "/sys/class/uio/%s/maps/map0/addr", entry->d_name);
        
        FILE *f = fopen(addr_path, "r");
        if (!f) continue;
        
        if (fgets(addr_str, sizeof(addr_str), f)) {
            uint32_t addr = (uint32_t)strtoul(addr_str, NULL, 0);
            if (addr == target_addr) {
                snprintf(dev_path, path_size, "/dev/%s", entry->d_name);
                fclose(f);
                closedir(dp);
                LOG_INFO("找到VDMA UIO设备: %s (物理地址: 0x%08X)", dev_path, addr);
                return 0;
            }
        }
        fclose(f);
    }
    
    closedir(dp);
    LOG_ERROR("未找到物理地址为 0x%08X 的UIO设备", target_addr);
    return -1;
}

/**
 * 等待VDMA复位完成
 */
static int wait_for_reset(vdma_context_t *ctx, int timeout_ms)
{
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        uint32_t ctrl = REG_READ(ctx, VDMA_S2MM_DMACR);
        if (!(ctrl & VDMA_DMACR_RESET)) {
            return 0;  /* 复位完成 */
        }
        usleep(1000);
        elapsed++;
    }
    return -1;  /* 超时 */
}

/*============================================================================
 * API 实现
 *============================================================================*/

int vdma_init(vdma_context_t *ctx, 
              int width, int height, int bpp, 
              int num_bufs, uint32_t phys_addr)
{
    char uio_path[64];
    
    LOG_INFO("========== VDMA 初始化 ==========");
    LOG_INFO("分辨率: %dx%d", width, height);
    LOG_INFO("每像素字节数: %d (YUV422)", bpp);
    LOG_INFO("帧缓冲数量: %d", num_bufs);
    LOG_INFO("帧缓冲物理地址: 0x%08X", phys_addr);
    
    /* 初始化结构体 */
    memset(ctx, 0, sizeof(vdma_context_t));
    ctx->width = width;
    ctx->height = height;
    ctx->bytes_per_pixel = bpp;
    ctx->num_buffers = num_bufs;
    ctx->frame_size = (size_t)width * height * bpp;
    ctx->line_stride = (size_t)width * bpp;
    ctx->frame_buffer_phys = phys_addr;
    ctx->uio_fd = -1;
    ctx->mem_fd = -1;
    
    LOG_INFO("帧大小: %zu bytes (%.2f KB)", ctx->frame_size, ctx->frame_size / 1024.0);
    LOG_INFO("行跨度: %zu bytes", ctx->line_stride);
    
    /*----------------------------------------------------------------------
     * 步骤1: 打开UIO设备并映射VDMA寄存器
     *----------------------------------------------------------------------*/
    if (find_uio_device(VDMA_BASE_ADDR, uio_path, sizeof(uio_path)) < 0) {
        return -1;
    }
    
    ctx->uio_fd = open(uio_path, O_RDWR);
    if (ctx->uio_fd < 0) {
        LOG_ERROR("无法打开UIO设备 %s: %s", uio_path, strerror(errno));
        return -1;
    }
    
    ctx->regs = mmap(NULL, VDMA_REG_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED,
                     ctx->uio_fd, 0);
    
    if (ctx->regs == MAP_FAILED) {
        LOG_ERROR("映射VDMA寄存器失败: %s", strerror(errno));
        close(ctx->uio_fd);
        ctx->uio_fd = -1;
        return -1;
    }
    
    LOG_INFO("VDMA寄存器已映射到: %p", ctx->regs);
    
    /*----------------------------------------------------------------------
     * 步骤2: 通过 /dev/mem 映射帧缓冲区
     *----------------------------------------------------------------------*/
    ctx->mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (ctx->mem_fd < 0) {
        LOG_ERROR("无法打开 /dev/mem: %s", strerror(errno));
        LOG_ERROR("提示: 需要root权限，且内核需启用 CONFIG_DEVMEM");
        vdma_cleanup(ctx);
        return -1;
    }
    
    size_t total_buffer_size = ctx->frame_size * num_bufs;
    
    ctx->frame_buffers = mmap(NULL, total_buffer_size,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED,
                              ctx->mem_fd, phys_addr);
    
    if (ctx->frame_buffers == MAP_FAILED) {
        LOG_ERROR("映射帧缓冲区失败: %s", strerror(errno));
        LOG_ERROR("物理地址: 0x%08X, 大小: %zu bytes", phys_addr, total_buffer_size);
        vdma_cleanup(ctx);
        return -1;
    }
    
    LOG_INFO("帧缓冲区已映射到: %p (大小: %zu bytes)", 
             ctx->frame_buffers, total_buffer_size);
    
    /*----------------------------------------------------------------------
     * 步骤3: 复位VDMA
     *----------------------------------------------------------------------*/
    LOG_INFO("复位VDMA...");
    REG_WRITE(ctx, VDMA_S2MM_DMACR, VDMA_DMACR_RESET);
    
    if (wait_for_reset(ctx, 1000) < 0) {
        LOG_ERROR("VDMA复位超时");
        vdma_cleanup(ctx);
        return -1;
    }
    LOG_INFO("VDMA复位完成");
    
    /*----------------------------------------------------------------------
     * 步骤4: 配置帧缓冲数量 (必须在 Run 之前!)
     *----------------------------------------------------------------------*/
    LOG_INFO("配置帧缓冲...");
    
    /* 
     * 根据 Xilinx PG020 文档：
     * FRMSTORE 寄存器存储的是 (帧数 - 1)
     * 例如：3个帧缓冲，应该写入 2
     */
    uint32_t frmstore_value = num_bufs - 1;
    REG_WRITE(ctx, VDMA_S2MM_FRMSTORE, frmstore_value);
    
    /* 验证写入 */
    uint32_t actual_frmstore = REG_READ(ctx, VDMA_S2MM_FRMSTORE);
    LOG_INFO("  FRMSTORE 写入: %d, 读回: %d", frmstore_value, actual_frmstore);
    
    if (actual_frmstore != frmstore_value) {
        LOG_INFO("  ⚠ FRMSTORE 写入未生效，使用默认值");
    }
    
    /* 实际使用的帧缓冲数 = FRMSTORE + 1 */
    ctx->num_buffers = (int)actual_frmstore + 1;
    LOG_INFO("  实际帧缓冲数: %d", ctx->num_buffers);
    
    /*----------------------------------------------------------------------
     * 步骤5: 配置帧缓冲地址
     *----------------------------------------------------------------------*/
    uint32_t addr_offsets[] = {
        VDMA_S2MM_START_ADDR_0,
        VDMA_S2MM_START_ADDR_1,
        VDMA_S2MM_START_ADDR_2,
        VDMA_S2MM_START_ADDR_3
    };
    
    for (int i = 0; i < ctx->num_buffers && i < 4; i++) {
        uint32_t buf_addr = phys_addr + i * ctx->frame_size;
        REG_WRITE(ctx, addr_offsets[i], buf_addr);
        LOG_INFO("  帧缓冲[%d]: 0x%08X", i, buf_addr);
    }
    
    /*----------------------------------------------------------------------
     * 步骤6: 配置视频参数
     *----------------------------------------------------------------------*/
    LOG_INFO("配置视频参数...");
    
    /* HSize: 每行字节数 */
    uint32_t hsize = width * bpp;
    REG_WRITE(ctx, VDMA_S2MM_HSIZE, hsize);
    LOG_INFO("  HSize (每行字节数): %d", hsize);
    
    /* Stride: 行跨度 (通常等于HSize，除非有填充) */
    uint32_t stride = hsize;
    REG_WRITE(ctx, VDMA_S2MM_FRMDLY_STRIDE, stride);
    LOG_INFO("  Stride (行跨度): %d", stride);
    
    /* 注意: VSize在启动时写入，用于触发传输 */
    LOG_INFO("  VSize (将在启动时设置): %d", height);
    
    LOG_INFO("========== VDMA 初始化完成 ==========");
    return 0;
}

int vdma_start(vdma_context_t *ctx)
{
    if (!ctx || !ctx->regs) {
        LOG_ERROR("VDMA未初始化");
        return -1;
    }
    
    LOG_INFO("启动VDMA...");
    
    /* 清除所有错误状态位 (写1清除) */
    REG_WRITE(ctx, VDMA_S2MM_DMASR, VDMA_DMASR_ERR_MASK);
    
    /* 配置控制寄存器:
     * - RS (Run/Stop) = 1: 启动
     * - Circular Mode = 1: 循环缓冲模式
     */
    uint32_t ctrl = VDMA_DMACR_RS | VDMA_DMACR_CIRCULAR;
    REG_WRITE(ctx, VDMA_S2MM_DMACR, ctrl);
    
    /* 短暂等待控制寄存器生效 */
    usleep(1000);
    
    /* 写入VSize触发传输
     * 根据Xilinx文档，写入VSize是启动传输的最后一步
     */
    REG_WRITE(ctx, VDMA_S2MM_VSIZE, ctx->height);
    
    /* 等待VDMA启动 */
    usleep(10000);
    
    /* 检查状态 */
    uint32_t status = REG_READ(ctx, VDMA_S2MM_DMASR);
    
    if (status & VDMA_DMASR_HALTED) {
        LOG_ERROR("VDMA启动失败，处于HALTED状态");
        LOG_ERROR("状态寄存器: 0x%08X", status);
        
        if (status & VDMA_DMASR_DMA_INT_ERR) LOG_ERROR("  - DMA内部错误");
        if (status & VDMA_DMASR_DMA_SLV_ERR) LOG_ERROR("  - DMA从设备错误");
        if (status & VDMA_DMASR_DMA_DEC_ERR) LOG_ERROR("  - DMA解码错误");
        if (status & VDMA_DMASR_SOF_EARLY)   LOG_ERROR("  - SOF提前错误");
        if (status & VDMA_DMASR_EOL_EARLY)   LOG_ERROR("  - EOL提前错误");
        if (status & VDMA_DMASR_SOF_LATE)    LOG_ERROR("  - SOF延迟错误");
        if (status & VDMA_DMASR_EOL_LATE)    LOG_ERROR("  - EOL延迟错误");
        
        LOG_ERROR("可能原因: 视频输入源未连接或时序不匹配");
        return -1;
    }
    
    ctx->is_running = 1;
    LOG_INFO("VDMA启动成功");
    LOG_INFO("  控制寄存器: 0x%08X", REG_READ(ctx, VDMA_S2MM_DMACR));
    LOG_INFO("  状态寄存器: 0x%08X", status);
    
    return 0;
}

int vdma_stop(vdma_context_t *ctx)
{
    if (!ctx || !ctx->regs) {
        return -1;
    }
    
    LOG_INFO("停止VDMA...");
    
    /* 清除RS位停止VDMA */
    uint32_t ctrl = REG_READ(ctx, VDMA_S2MM_DMACR);
    ctrl &= ~VDMA_DMACR_RS;
    REG_WRITE(ctx, VDMA_S2MM_DMACR, ctrl);
    
    /* 等待停止完成 */
    usleep(10000);
    
    ctx->is_running = 0;
    LOG_INFO("VDMA已停止");
    
    return 0;
}

int vdma_reset(vdma_context_t *ctx)
{
    if (!ctx || !ctx->regs) {
        return -1;
    }
    
    LOG_INFO("复位VDMA...");
    
    REG_WRITE(ctx, VDMA_S2MM_DMACR, VDMA_DMACR_RESET);
    
    if (wait_for_reset(ctx, 1000) < 0) {
        LOG_ERROR("VDMA复位超时");
        return -1;
    }
    
    ctx->is_running = 0;
    LOG_INFO("VDMA复位完成");
    
    return 0;
}

int vdma_get_write_frame(vdma_context_t *ctx)
{
    if (!ctx || !ctx->regs) {
        return -1;
    }
    
    uint32_t status = REG_READ(ctx, VDMA_S2MM_DMASR);
    int frame_count = (status & VDMA_DMASR_FRMCNT_MASK) >> VDMA_DMASR_FRMCNT_SHIFT;
    
    /* Frame Count是当前正在写入的帧编号 */
    return frame_count % ctx->num_buffers;
}

const uint8_t* vdma_get_read_buffer(vdma_context_t *ctx, int *frame_index)
{
    if (!ctx || !ctx->frame_buffers) {
        return NULL;
    }
    
    int read_frame;
    
    if (ctx->num_buffers == 1) {
        /* 单帧缓冲模式：直接读取帧0
         * 注意：可能会有轻微的画面撕裂，但数据仍然有效
         */
        read_frame = 0;
    } else {
        /* 多帧缓冲模式：选择一个不是当前写入的帧来读取 */
        int write_frame = vdma_get_write_frame(ctx);
        if (write_frame < 0) {
            write_frame = 0;
        }
        read_frame = (write_frame + 1) % ctx->num_buffers;
    }
    
    if (frame_index) {
        *frame_index = read_frame;
    }
    
    return (const uint8_t*)ctx->frame_buffers + (size_t)read_frame * ctx->frame_size;
}

const uint8_t* vdma_get_frame_buffer(vdma_context_t *ctx, int index)
{
    if (!ctx || !ctx->frame_buffers) {
        return NULL;
    }
    
    if (index < 0 || index >= ctx->num_buffers) {
        return NULL;
    }
    
    return (const uint8_t*)ctx->frame_buffers + (size_t)index * ctx->frame_size;
}

void vdma_dump_registers(vdma_context_t *ctx)
{
    if (!ctx || !ctx->regs) {
        printf("VDMA未初始化\n");
        return;
    }
    
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                    VDMA 寄存器状态                           ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    /* S2MM通道寄存器 */
    uint32_t dmacr = REG_READ(ctx, VDMA_S2MM_DMACR);
    uint32_t dmasr = REG_READ(ctx, VDMA_S2MM_DMASR);
    uint32_t vsize = REG_READ(ctx, VDMA_S2MM_VSIZE);
    uint32_t hsize = REG_READ(ctx, VDMA_S2MM_HSIZE);
    uint32_t stride = REG_READ(ctx, VDMA_S2MM_FRMDLY_STRIDE);
    uint32_t frmstore = REG_READ(ctx, VDMA_S2MM_FRMSTORE);
    
    printf("║ S2MM 通道 (Stream → Memory):                                ║\n");
    printf("║   控制寄存器 (0x30): 0x%08X                               ║\n", dmacr);
    printf("║     - Run/Stop:    %d                                        ║\n", (dmacr >> 0) & 1);
    printf("║     - Circular:    %d                                        ║\n", (dmacr >> 1) & 1);
    printf("║     - Reset:       %d                                        ║\n", (dmacr >> 2) & 1);
    printf("║   状态寄存器 (0x34): 0x%08X                               ║\n", dmasr);
    printf("║     - Halted:      %d                                        ║\n", (dmasr >> 0) & 1);
    printf("║     - Idle:        %d                                        ║\n", (dmasr >> 1) & 1);
    printf("║     - FrameCount:  %d                                        ║\n", (dmasr >> 16) & 0xFF);
    printf("║   VSize (0xA0):    %-6d (期望: %d)                        ║\n", vsize, ctx->height);
    printf("║   HSize (0xA4):    %-6d (期望: %d)                      ║\n", hsize, ctx->width * ctx->bytes_per_pixel);
    printf("║   Stride (0xA8):   %-6d                                     ║\n", stride);
    printf("║   FrmStore (0x48): %-6d (表示 %d 个帧缓冲)                ║\n", frmstore, frmstore + 1);
    printf("║                                                              ║\n");
    printf("║ 帧缓冲地址 (实际使用: %d 个):                                  ║\n", ctx->num_buffers);
    printf("║   [0]: 0x%08X %s                                    ║\n", 
           REG_READ(ctx, VDMA_S2MM_START_ADDR_0), 
           ctx->num_buffers >= 1 ? "✓" : " ");
    if (frmstore >= 1) {
        printf("║   [1]: 0x%08X %s                                    ║\n", 
               REG_READ(ctx, VDMA_S2MM_START_ADDR_1),
               ctx->num_buffers >= 2 ? "✓" : " ");
    }
    if (frmstore >= 2) {
        printf("║   [2]: 0x%08X %s                                    ║\n", 
               REG_READ(ctx, VDMA_S2MM_START_ADDR_2),
               ctx->num_buffers >= 3 ? "✓" : " ");
    }
    printf("║                                                              ║\n");
    
    /* 错误检测 */
    printf("║ 状态诊断:                                                    ║\n");
    if (dmasr & VDMA_DMASR_HALTED) {
        printf("║   ❌ VDMA处于HALTED状态                                     ║\n");
    } else if (dmacr & VDMA_DMACR_RS) {
        printf("║   ✓ VDMA正在运行                                            ║\n");
    } else {
        printf("║   ⚠ VDMA已停止                                              ║\n");
    }
    
    if (dmasr & VDMA_DMASR_DMA_INT_ERR) printf("║   ❌ DMA内部错误                                            ║\n");
    if (dmasr & VDMA_DMASR_DMA_SLV_ERR) printf("║   ❌ DMA从设备错误                                          ║\n");
    if (dmasr & VDMA_DMASR_DMA_DEC_ERR) printf("║   ❌ DMA解码错误                                            ║\n");
    if (dmasr & VDMA_DMASR_SOF_EARLY)   printf("║   ⚠ SOF提前错误                                             ║\n");
    if (dmasr & VDMA_DMASR_EOL_EARLY)   printf("║   ⚠ EOL提前错误                                             ║\n");
    if (dmasr & VDMA_DMASR_SOF_LATE)    printf("║   ⚠ SOF延迟错误                                             ║\n");
    if (dmasr & VDMA_DMASR_EOL_LATE)    printf("║   ⚠ EOL延迟错误                                             ║\n");
    
    printf("╚══════════════════════════════════════════════════════════════╝\n");
}

void vdma_dump_frame_info(vdma_context_t *ctx, int frame_index)
{
    if (!ctx || !ctx->frame_buffers) {
        printf("帧缓冲未初始化\n");
        return;
    }
    
    if (frame_index < 0 || frame_index >= ctx->num_buffers) {
        printf("无效的帧索引: %d\n", frame_index);
        return;
    }
    
    const uint8_t *frame = vdma_get_frame_buffer(ctx, frame_index);
    if (!frame) return;
    
    uint32_t phys_addr = ctx->frame_buffer_phys + frame_index * ctx->frame_size;
    
    printf("\n");
    printf("┌──────────────────────────────────────────────────────────────┐\n");
    printf("│ 帧缓冲 #%d  物理地址: 0x%08X                              │\n", 
           frame_index, phys_addr);
    printf("├──────────────────────────────────────────────────────────────┤\n");
    
    /* 打印多个位置的数据 */
    struct { size_t offset; const char *name; } positions[] = {
        {0, "行0 (开头)"},
        {ctx->line_stride, "行1      "},
        {ctx->line_stride * 100, "行100    "},
        {ctx->frame_size / 2, "中间     "},
        {ctx->frame_size - ctx->line_stride, "最后一行 "}
    };
    
    for (int p = 0; p < 5; p++) {
        size_t offset = positions[p].offset;
        if (offset >= ctx->frame_size) continue;
        
        printf("│ %s [0x%06zX]: ", positions[p].name, offset);
        for (int i = 0; i < 16 && (offset + i) < ctx->frame_size; i++) {
            printf("%02X ", frame[offset + i]);
        }
        printf("│\n");
        
        /* YUV422解析 (YUYV: Y0 U Y1 V) */
        printf("│   YUYV解析: ");
        for (int g = 0; g < 4 && (offset + g * 4 + 3) < ctx->frame_size; g++) {
            uint8_t y0 = frame[offset + g * 4 + 0];
            uint8_t u  = frame[offset + g * 4 + 1];
            uint8_t y1 = frame[offset + g * 4 + 2];
            uint8_t v  = frame[offset + g * 4 + 3];
            printf("(Y0=%3d U=%3d Y1=%3d V=%3d) ", y0, u, y1, v);
        }
        printf("│\n");
    }
    
    /* 统计分析 */
    printf("├──────────────────────────────────────────────────────────────┤\n");
    
    int count_ff = 0, count_00 = 0;
    for (size_t i = 0; i < ctx->frame_size; i++) {
        if (frame[i] == 0xFF) count_ff++;
        else if (frame[i] == 0x00) count_00++;
    }
    
    double pct_ff = 100.0 * count_ff / ctx->frame_size;
    double pct_00 = 100.0 * count_00 / ctx->frame_size;
    
    printf("│ 统计:                                                        │\n");
    printf("│   0xFF 字节: %7d (%.1f%%)                                 │\n", count_ff, pct_ff);
    printf("│   0x00 字节: %7d (%.1f%%)                                 │\n", count_00, pct_00);
    
    if (pct_ff > 95.0) {
        printf("│   ❌ 几乎全是0xFF - VDMA可能未写入数据                       │\n");
    } else if (pct_00 > 95.0) {
        printf("│   ⚠ 几乎全是0x00 - 可能是黑屏或无信号                        │\n");
    } else {
        printf("│   ✓ 有有效数据                                               │\n");
    }
    
    printf("└──────────────────────────────────────────────────────────────┘\n");
}

void vdma_cleanup(vdma_context_t *ctx)
{
    if (!ctx) return;
    
    LOG_INFO("清理VDMA资源...");
    
    /* 停止VDMA */
    if (ctx->is_running) {
        vdma_stop(ctx);
    }
    
    /* 解除帧缓冲映射 */
    if (ctx->frame_buffers && ctx->frame_buffers != MAP_FAILED) {
        size_t total_size = ctx->frame_size * ctx->num_buffers;
        munmap(ctx->frame_buffers, total_size);
        ctx->frame_buffers = NULL;
    }
    
    /* 解除寄存器映射 */
    if (ctx->regs && ctx->regs != MAP_FAILED) {
        munmap((void*)ctx->regs, VDMA_REG_SIZE);
        ctx->regs = NULL;
    }
    
    /* 关闭文件描述符 */
    if (ctx->mem_fd >= 0) {
        close(ctx->mem_fd);
        ctx->mem_fd = -1;
    }
    
    if (ctx->uio_fd >= 0) {
        close(ctx->uio_fd);
        ctx->uio_fd = -1;
    }
    
    LOG_INFO("VDMA资源已释放");
}

char* vdma_get_status_string(vdma_context_t *ctx, char *buf, size_t buf_size)
{
    if (!ctx || !ctx->regs || !buf) {
        snprintf(buf, buf_size, "VDMA未初始化");
        return buf;
    }
    
    uint32_t status = REG_READ(ctx, VDMA_S2MM_DMASR);
    int halted = (status & VDMA_DMASR_HALTED) ? 1 : 0;
    int frame_count = (status >> 16) & 0xFF;
    int has_error = (status & VDMA_DMASR_ERR_MASK) ? 1 : 0;
    
    if (halted) {
        snprintf(buf, buf_size, "HALTED (帧=%d, 错误=%s)", 
                 frame_count, has_error ? "是" : "否");
    } else if (ctx->is_running) {
        snprintf(buf, buf_size, "运行中 (帧=%d)", frame_count);
    } else {
        snprintf(buf, buf_size, "已停止 (帧=%d)", frame_count);
    }
    
    return buf;
}
