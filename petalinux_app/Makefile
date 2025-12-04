# Makefile for USB UVC Camera Application
# PetaLinux/Yocto 环境专用版本
# 
# 重要说明：
# 1. 本Makefile不设置CFLAGS和LDFLAGS，完全使用Recipe通过环境变量传递的值
# 2. Recipe中的CFLAGS已经包含了系统头文件路径（如-I/usr/include等）
# 3. 编译选项（如-Wall -O2 -std=c99）在Recipe中追加到CFLAGS

# 目标程序名
TARGET = uvc-camera-app

# 源文件
SRCS = main.c vpss_control.c vdma_control.c
OBJS = $(SRCS:.c=.o)

# 默认目标
all: $(TARGET)

# 链接目标程序
# 使用Recipe传递的CC、CFLAGS、LDFLAGS
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	@echo "编译完成: $(TARGET)"

# 编译源文件
# CFLAGS由Recipe通过环境变量传递，包含：
# - 系统头文件路径（-I/usr/include等）
# - 编译选项（-Wall -O2 -std=c99等）
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 清理
clean:
	rm -f $(TARGET) $(OBJS)
	@echo "清理完成"

# 安装
install:
	install -d $(DESTDIR)/usr/bin
	install -m 0755 $(TARGET) $(DESTDIR)/usr/bin/
	@echo "安装完成: $(DESTDIR)/usr/bin/$(TARGET)"

.PHONY: all clean install