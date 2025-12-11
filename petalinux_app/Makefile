# Makefile for Ethernet Camera Application
# 支持 PetaLinux/Yocto 环境和本地编译
# 
# 使用方法：
#   make           - 编译主程序 (eth-camera-app)
#   make all       - 编译主程序
#   make diag      - 编译诊断工具 (需要 video_diag.c)
#   make full      - 编译所有程序
#   make clean     - 清理编译产物
#   make install   - 安装到目标系统
#
# 目标程序：
#   eth-camera-app - 以太网视频流传输
#   video-diag     - 视频处理链路诊断工具 (可选)

# 编译器设置（如果环境变量未定义，使用默认值）
CC ?= gcc
CFLAGS ?= -Wall -O2 -std=c99
LDFLAGS ?=

# 目标程序名
TARGET = eth-camera-app
DIAG_TARGET = video-diag

# 源文件
SRCS = network_stream.c vpss_control.c vdma_control.c
OBJS = $(SRCS:.c=.o)

# 诊断工具源文件
DIAG_SRCS = video_diag.c
DIAG_OBJS = $(DIAG_SRCS:.c=.o)

# 头文件依赖
DEPS = vpss_control.h vdma_control.h

# 默认目标 - 只编译主程序（兼容PetaLinux）
all: $(TARGET)

# 链接主程序
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "========================================="
	@echo "编译完成: $(TARGET)"
	@echo "========================================="

# 链接诊断工具（可选，需要 video_diag.c）
$(DIAG_TARGET): $(DIAG_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "========================================="
	@echo "编译完成: $(DIAG_TARGET)"
	@echo "========================================="

# 只编译诊断工具
diag: $(DIAG_TARGET)

# 编译所有程序（包括诊断工具）
full: $(TARGET) $(DIAG_TARGET)

# 编译源文件
%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

video_diag.o: video_diag.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 清理
clean:
	rm -f $(TARGET) $(DIAG_TARGET) $(OBJS) $(DIAG_OBJS)
	@echo "清理完成"

# 安装程序（只安装主程序，兼容PetaLinux）
install: $(TARGET)
	install -d $(DESTDIR)/usr/bin
	install -m 0755 $(TARGET) $(DESTDIR)/usr/bin/
	@echo "安装完成: $(DESTDIR)/usr/bin/$(TARGET)"

# 显示帮助信息
help:
	@echo "可用目标:"
	@echo "  all     - 编译主程序 $(TARGET) (默认，兼容PetaLinux)"
	@echo "  diag    - 编译诊断工具 $(DIAG_TARGET)"
	@echo "  full    - 编译所有程序"
	@echo "  clean   - 清理编译产物"
	@echo "  install - 安装到 \$$(DESTDIR)/usr/bin/"
	@echo "  help    - 显示此帮助信息"
	@echo ""
	@echo "编译示例:"
	@echo "  make                    # 编译主程序"
	@echo "  make full               # 编译所有（包括诊断工具）"
	@echo "  make CC=aarch64-linux-gnu-gcc  # 使用交叉编译器"
	@echo ""
	@echo "诊断工具使用:"
	@echo "  ./video-diag -a         # 显示所有诊断信息"
	@echo "  ./video-diag -s frame.bin  # 保存帧数据到文件"
	@echo "  ./video-diag -w         # 持续监控VDMA状态"

.PHONY: all diag full clean install help
