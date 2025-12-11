/**
 * @file vdma_control.c
 * @brief VDMA (Video DMA) 控制实现
 */

#include "vdma_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <dirent.h>
/**
 * 打开UIO设备并映射VDMA寄存器
 * 
 * @param vdma VDMA控制结构指针
 * @return 0成功，-1失败
 */
static int vdma_open_uio(vdma_control_t *vdma)
{
    struct dirent *entry;
    DIR *dp;
    char addr_path[128];
    char addr_str[64];
    unsigned long addr;

    dp = opendir("/sys/class/uio");
    if (dp == NULL) {
        fprintf(stderr, "无法打开 /sys/class/uio 目录\n");
        return -1;
    }

    // 遍历所有 uioX 目录
    while ((entry = readdir(dp))) {
        // 跳过 . 和 .. 以及非 uio 开头的目录
        if (strncmp(entry->d_name, "uio", 3) != 0) continue;

        // 读取 map0/addr (物理地址)
        // 路径格式: /sys/class/uio/uioX/maps/map0/addr
        snprintf(addr_path, sizeof(addr_path), "/sys/class/uio/%s/maps/map0/addr", entry->d_name);
        
        FILE *f = fopen(addr_path, "r");
        if (!f) continue;

        if (fgets(addr_str, sizeof(addr_str), f)) {
            // 将十六进制字符串转为长整型
            addr = strtoul(addr_str, NULL, 0);
            
            // 【核心判断】地址是否匹配 0x80020000
            if (addr == VDMA_BASE_ADDR) {
                char dev_path[64];
                snprintf(dev_path, sizeof(dev_path), "/dev/%s", entry->d_name);
                
                printf("成功找到 VDMA: %s (物理地址 0x%08lX)\n", dev_path, addr);
                
                vdma->uio_fd = open(dev_path, O_RDWR);
                if (vdma->uio_fd < 0) {
                    fprintf(stderr, "无法打开设备 %s: %s\n", dev_path, strerror(errno));
                    fclose(f);
                    closedir(dp);
                    return -1;
                }
                
                fclose(f);
                closedir(dp);
                return 0; // 成功返回
            }
        }
        fclose(f);
    }

    closedir(dp);
    fprintf(stderr, "错误: 未找到物理地址为 0x%08X 的 VDMA UIO 设备\n", VDMA_BASE_ADDR);
    return -1;
}

/**
 * 初始化VDMA
 */
int vdma_init(vdma_control_t *vdma, int width, int height,
              int bytes_per_pixel, int num_frames,
              uint32_t frame_buffer_phys)
{
    printf("初始化VDMA控制器...\n");
    
    memset(vdma, 0, sizeof(vdma_control_t));
    vdma->width = width;
    vdma->height = height;
    vdma->bytes_per_pixel = bytes_per_pixel;
    vdma->num_frames = num_frames;
    vdma->frame_buffer_phys = frame_buffer_phys;
    vdma->frame_buffer_size = (size_t)width * height * bytes_per_pixel * num_frames;
    vdma->uio_fd = -1;
    
    /* 打开UIO设备 */
    if (vdma_open_uio(vdma) < 0) {
        return -1;
    }
    
    /* 映射VDMA寄存器 */
    vdma->base_addr = mmap(NULL, VDMA_ADDR_SIZE,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED,
                           vdma->uio_fd, 0);
    
    if (vdma->base_addr == MAP_FAILED) {
        fprintf(stderr, "映射VDMA寄存器失败: %s\n", strerror(errno));
        close(vdma->uio_fd);
        return -1;
    }
    
    printf("VDMA寄存器映射成功: %p\n", vdma->base_addr);
    
    /* 映射帧缓冲（通过/dev/mem） */
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "打开/dev/mem失败: %s\n", strerror(errno));
        fprintf(stderr, "提示: 确保内核启用了 /dev/mem 支持\n");
        munmap(vdma->base_addr, VDMA_ADDR_SIZE);
        close(vdma->uio_fd);
        return -1;
    }
    
    vdma->frame_buffer = mmap(NULL, vdma->frame_buffer_size,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED,
                              mem_fd, frame_buffer_phys);
    close(mem_fd);
    
    if (vdma->frame_buffer == MAP_FAILED) {
        fprintf(stderr, "映射帧缓冲失败: %s\n", strerror(errno));
        fprintf(stderr, "物理地址: 0x%08X, 大小: %zu bytes\n", 
                frame_buffer_phys, vdma->frame_buffer_size);
        fprintf(stderr, "提示: 检查设备树reserved-memory配置\n");
        munmap(vdma->base_addr, VDMA_ADDR_SIZE);
        close(vdma->uio_fd);
        return -1;
    }
    
    printf("帧缓冲映射成功: %p (物理地址: 0x%08X)\n", 
           vdma->frame_buffer, frame_buffer_phys);
    
    /* 复位VDMA */
    printf("复位VDMA...\n");
    *(volatile uint32_t*)(vdma->base_addr + VDMA_S2MM_CONTROL) = VDMA_CTRL_RESET;
    usleep(10000);
    
    /* 等待复位完成 */
    int timeout = 1000;
    while ((*(volatile uint32_t*)(vdma->base_addr + VDMA_S2MM_CONTROL) & VDMA_CTRL_RESET) && timeout > 0) {
        usleep(100);
        timeout--;
    }
    
    if (timeout <= 0) {
        fprintf(stderr, "VDMA复位超时\n");
        return -1;
    }
    
    /* 配置VDMA参数 */
    printf("配置VDMA参数...\n");
    uint32_t hsize = width * bytes_per_pixel;
    uint32_t stride = hsize;
    
    *(volatile uint32_t*)(vdma->base_addr + VDMA_S2MM_HSIZE) = hsize;
    *(volatile uint32_t*)(vdma->base_addr + VDMA_S2MM_STRIDE) = stride;
    
    /* 配置帧缓冲地址
     * 注意：虽然VDMA硬件NUM_FSTORES=1，但我们仍然在DDR中分配多个帧缓冲
     * 通过软件逻辑实现多缓冲，避免读写冲突
     */
    uint32_t frame_size = width * height * bytes_per_pixel;
    *(volatile uint32_t*)(vdma->base_addr + VDMA_S2MM_START_ADDR1) = frame_buffer_phys;
    
    /* 配置多个帧缓冲地址（即使硬件只支持1个，也配置以支持软件多缓冲） */
    if (num_frames >= 2) {
        *(volatile uint32_t*)(vdma->base_addr + VDMA_S2MM_START_ADDR2) = frame_buffer_phys + frame_size;
    }
    
    if (num_frames >= 3) {
        *(volatile uint32_t*)(vdma->base_addr + VDMA_S2MM_START_ADDR3) = frame_buffer_phys + frame_size * 2;
    }
    
    printf("VDMA初始化完成\n");
    printf("  分辨率: %dx%d\n", width, height);
    printf("  HSize: %d bytes\n", hsize);
    printf("  Stride: %d bytes\n", stride);
    printf("  帧缓冲数: %d\n", num_frames);
    printf("  每帧大小: %d bytes\n", frame_size);
    
    return 0;
}

/**
 * 启动VDMA
 */
int vdma_start(vdma_control_t *vdma)
{
    if (!vdma || !vdma->base_addr) {
        fprintf(stderr, "VDMA未初始化\n");
        return -1;
    }
    
    printf("启动VDMA...\n");
    
    /* 启动VDMA：Run + Circular模式 */
    uint32_t ctrl = VDMA_CTRL_RUN | VDMA_CTRL_CIRCULAR;
    *(volatile uint32_t*)(vdma->base_addr + VDMA_S2MM_CONTROL) = ctrl;
    usleep(1000);
    
    /* 写VSize触发传输 */
    *(volatile uint32_t*)(vdma->base_addr + VDMA_S2MM_VSIZE) = vdma->height;
    usleep(10000);
    
    /* 检查状态 */
    uint32_t status = *(volatile uint32_t*)(vdma->base_addr + VDMA_S2MM_STATUS);
    printf("VDMA状态: 0x%08X\n", status);
    
    if (status & VDMA_STATUS_HALTED) {
        fprintf(stderr, "警告: VDMA处于HALTED状态\n");
        fprintf(stderr, "可能原因: 数据源未准备好或配置错误\n");
        return -1;
    }
    
    printf("VDMA启动成功\n");
    return 0;
}

/**
 * 停止VDMA
 */
int vdma_stop(vdma_control_t *vdma)
{
    if (!vdma || !vdma->base_addr) {
        return -1;
    }
    
    printf("停止VDMA...\n");
    
    /* 清除Run位 */
    *(volatile uint32_t*)(vdma->base_addr + VDMA_S2MM_CONTROL) = 0;
    usleep(10000);
    
    return 0;
}

/**
 * 获取当前VDMA正在写入的帧编号
 * 
 * 注意：根据Xilinx AXI VDMA文档，S2MM Status Register (0x34)的位定义：
 *   - 位0: Halted
 *   - 位1: VDMAIntErr  
 *   - 位4: VDMASlvErr
 *   - 位5: VDMADecErr
 *   - 位16-23: Frame_Count (当前帧号)
 *   - 位24-31: Delay_Count
 */
int vdma_get_current_frame(vdma_control_t *vdma)
{
    if (!vdma || !vdma->base_addr) {
        return -1;
    }
    
    uint32_t status = *(volatile uint32_t*)(vdma->base_addr + VDMA_S2MM_STATUS);
    /* Frame Count在位16-23，不是位24-25！ */
    int frame = (status >> 16) & 0xFF;
    
    /* 限制在帧缓冲数量范围内 */
    return frame % vdma->num_frames;
}

/**
 * 清理VDMA资源
 */
void vdma_cleanup(vdma_control_t *vdma)
{
    if (!vdma) return;
    
    printf("清理VDMA资源...\n");
    
    vdma_stop(vdma);
    
    if (vdma->frame_buffer && vdma->frame_buffer != MAP_FAILED) {
        munmap(vdma->frame_buffer, vdma->frame_buffer_size);
        vdma->frame_buffer = NULL;
    }
    
    if (vdma->base_addr && vdma->base_addr != MAP_FAILED) {
        munmap(vdma->base_addr, VDMA_ADDR_SIZE);
        vdma->base_addr = NULL;
    }
    
    if (vdma->uio_fd >= 0) {
        close(vdma->uio_fd);
        vdma->uio_fd = -1;
    }
}
