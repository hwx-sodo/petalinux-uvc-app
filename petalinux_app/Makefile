# Makefile for Ethernet Camera Application
# 支持 PetaLinux/Yocto 环境和本地编译
# 
# 使用方法：
#   make           - 编译 eth-camera-app
#   make all       - 编译 eth-camera-app
#   make clean     - 清理编译产物
#   make install   - 安装到目标系统
#
# 目标程序：
#   eth-camera-app - 以太网视频流传输（含诊断功能）
#
# 诊断功能使用：
#   ./eth-camera-app -D              # 诊断模式
#   ./eth-camera-app -D -s frame.bin # 诊断并保存帧数据

# 编译器设置（如果环境变量未定义，使用默认值）
CC ?= gcc
# 说明：
# - clock_gettime/usleep 等POSIX接口在部分编译器/标准库下需要显式开启特性宏
# - 这里通过编译选项统一开启，避免各文件里重复写宏定义
CFLAGS ?= -Wall -O2 -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS ?=

# 目标程序名
TARGET = eth-camera-app

# 源文件
SRCS = network_stream.c vpss_control.c vdma_control.c
OBJS = $(SRCS:.c=.o)

# 头文件依赖
DEPS = vpss_control.h vdma_control.h

# 默认目标
all: $(TARGET)

# 链接程序
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "========================================="
	@echo "编译完成: $(TARGET)"
	@echo "========================================="

# 编译源文件
%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

# 清理
clean:
	rm -f $(TARGET) $(OBJS)
	@echo "清理完成"

# 安装程序
install: $(TARGET)
	install -d $(DESTDIR)/usr/bin
	install -m 0755 $(TARGET) $(DESTDIR)/usr/bin/
	@echo "安装完成: $(DESTDIR)/usr/bin/$(TARGET)"

# 显示帮助信息
help:
	@echo "可用目标:"
	@echo "  all     - 编译 $(TARGET)"
	@echo "  clean   - 清理编译产物"
	@echo "  install - 安装到 \$$(DESTDIR)/usr/bin/"
	@echo "  help    - 显示此帮助信息"
	@echo ""
	@echo "编译示例:"
	@echo "  make                    # 使用默认编译器"
	@echo "  make CC=aarch64-linux-gnu-gcc  # 使用交叉编译器"
	@echo ""
	@echo "诊断功能使用:"
	@echo "  ./$(TARGET) -D              # 仅诊断模式"
	@echo "  ./$(TARGET) -D -s frame.bin # 诊断并保存帧数据"
	@echo "  ./$(TARGET) -d -f           # 调试+强制发送"

.PHONY: all clean install help
