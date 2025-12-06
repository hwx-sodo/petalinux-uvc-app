#!/bin/bash
#
# run_network_stream.sh - 网络视频流传输启动脚本
#
# 用法:
#   ./run_network_stream.sh <PC的IP地址> [端口] [协议]
#
# 示例:
#   ./run_network_stream.sh 10.72.43.219           # UDP模式，默认端口5000
#   ./run_network_stream.sh 10.72.43.219 8000      # UDP模式，端口8000
#   ./run_network_stream.sh 10.72.43.219 5000 tcp  # TCP模式
#

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 默认参数
DEFAULT_PORT=5000
DEFAULT_PROTOCOL="udp"
DEFAULT_PC_IP="10.72.43.219"    # PC的IP地址
BOARD_IP="10.72.43.10"          # 开发板的IP地址
NETMASK="255.255.0.0"           # 子网掩码
APP_PATH="/usr/bin/network-stream-app"

# 检查参数
if [ $# -lt 1 ]; then
    echo -e "${RED}错误: 请指定PC的IP地址${NC}"
    echo ""
    echo "用法: $0 <PC_IP> [端口] [协议]"
    echo ""
    echo "参数:"
    echo "  PC_IP     PC端的IP地址（必需）"
    echo "  端口      网络端口（默认: ${DEFAULT_PORT}）"
    echo "  协议      udp 或 tcp（默认: ${DEFAULT_PROTOCOL}）"
    echo ""
echo "示例:"
echo "  $0 10.72.43.219"
echo "  $0 10.72.43.219 8000"
echo "  $0 10.72.43.219 5000 tcp"
echo ""
echo "PC端接收命令:"
echo "  python receive_stream.py -p 5000        # UDP模式"
echo "  python receive_stream.py -p 5000 -t     # TCP模式"
echo ""
echo "开发板IP配置:"
echo "  ifconfig eth0 ${BOARD_IP} netmask ${NETMASK} up"
exit 1
fi

TARGET_IP=$1
TARGET_PORT=${2:-$DEFAULT_PORT}
PROTOCOL=${3:-$DEFAULT_PROTOCOL}

echo "========================================"
echo "网络视频流传输"
echo "========================================"
echo ""

# 验证IP地址格式
if ! echo "$TARGET_IP" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo -e "${RED}错误: 无效的IP地址格式: $TARGET_IP${NC}"
    exit 1
fi

# 验证端口
if [ "$TARGET_PORT" -lt 1 ] || [ "$TARGET_PORT" -gt 65535 ]; then
    echo -e "${RED}错误: 无效的端口号: $TARGET_PORT${NC}"
    exit 1
fi

# 验证协议
if [ "$PROTOCOL" != "udp" ] && [ "$PROTOCOL" != "tcp" ]; then
    echo -e "${RED}错误: 无效的协议: $PROTOCOL (请使用 udp 或 tcp)${NC}"
    exit 1
fi

echo "目标IP: $TARGET_IP"
echo "端口:   $TARGET_PORT"
echo "协议:   $PROTOCOL"
echo ""

# 配置开发板IP（如果没有配置）
echo -e "${YELLOW}检查开发板网络配置...${NC}"
CURRENT_IP=$(ifconfig eth0 2>/dev/null | grep 'inet ' | awk '{print $2}' | sed 's/addr://')
if [ -z "$CURRENT_IP" ]; then
    echo "开发板eth0未配置IP，正在配置..."
    ifconfig eth0 ${BOARD_IP} netmask ${NETMASK} up
    sleep 1
    echo -e "${GREEN}✓ 已配置开发板IP: ${BOARD_IP}${NC}"
else
    echo -e "${GREEN}✓ 开发板IP: ${CURRENT_IP}${NC}"
fi
echo ""

# 检查网络连接
echo -e "${YELLOW}检查网络连接...${NC}"
if ping -c 1 -W 2 "$TARGET_IP" > /dev/null 2>&1; then
    echo -e "${GREEN}✓ 可以访问目标主机${NC}"
else
    echo -e "${YELLOW}⚠ 无法ping通目标主机（可能是防火墙阻止）${NC}"
    echo "  继续尝试连接..."
fi
echo ""

# 检查应用程序
if [ ! -f "$APP_PATH" ]; then
    # 尝试本地路径
    if [ -f "./network-stream-app" ]; then
        APP_PATH="./network-stream-app"
    elif [ -f "/home/root/network-stream-app" ]; then
        APP_PATH="/home/root/network-stream-app"
    else
        echo -e "${RED}错误: 找不到 network-stream-app${NC}"
        echo "请先编译应用程序:"
        echo "  cd /path/to/petalinux_app"
        echo "  make network-stream-app"
        exit 1
    fi
fi

echo "使用应用程序: $APP_PATH"
echo ""

# 提醒用户在PC端启动接收程序
echo "========================================"
echo -e "${YELLOW}重要: 请先在PC端启动接收程序!${NC}"
echo "========================================"
echo ""
echo "PC端命令:"
if [ "$PROTOCOL" = "tcp" ]; then
    echo "  python receive_stream.py -p $TARGET_PORT -t"
else
    echo "  python receive_stream.py -p $TARGET_PORT"
fi
echo ""
echo "按 Enter 继续，或 Ctrl+C 取消..."
read -r

# 构建命令
CMD="$APP_PATH -H $TARGET_IP -p $TARGET_PORT"
if [ "$PROTOCOL" = "tcp" ]; then
    CMD="$CMD -t"
fi

echo ""
echo "启动命令: $CMD"
echo "========================================"
echo ""

# 执行
exec $CMD
