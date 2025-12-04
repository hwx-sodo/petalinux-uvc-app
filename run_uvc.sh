#!/bin/bash
# run_uvc.sh - UVC 相机一键启动脚本
# 
# 这个脚本会自动:
# 1. 配置 USB Gadget (如果还没配置)
# 2. 检查 UIO 设备
# 3. 启动视频流
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# 默认参数
WIDTH=640
HEIGHT=480
FPS=60
TEST_MODE=0

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -w|--width)
            WIDTH="$2"
            shift 2
            ;;
        -h|--height)
            HEIGHT="$2"
            shift 2
            ;;
        -f|--fps)
            FPS="$2"
            shift 2
            ;;
        -t|--test)
            TEST_MODE=1
            shift
            ;;
        --help)
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  -w, --width <N>    视频宽度 (默认: 640)"
            echo "  -h, --height <N>   视频高度 (默认: 480)"
            echo "  -f, --fps <N>      帧率 (默认: 30)"
            echo "  -t, --test         测试模式 (不使用 VDMA)"
            echo ""
            echo "示例:"
            echo "  $0                      # 默认 640x480@30fps"
            echo "  $0 -w 1280 -h 720       # 720p"
            echo "  $0 --test               # 测试模式"
            exit 0
            ;;
        *)
            echo "未知选项: $1"
            exit 1
            ;;
    esac
done

echo "================================================"
echo "         UVC 相机一键启动脚本"
echo "================================================"
echo ""
echo "参数:"
echo "  分辨率: ${WIDTH}x${HEIGHT}"
echo "  帧率: ${FPS} fps"
echo "  测试模式: $([ $TEST_MODE -eq 1 ] && echo '是' || echo '否')"
echo ""

# 1. 检查 root 权限
if [ "$EUID" -ne 0 ]; then
    echo "错误: 请使用 root 权限运行此脚本"
    echo "用法: sudo $0 $@"
    exit 1
fi

# 2. 检查 UIO 设备
echo "[1/4] 检查 UIO 设备..."
if [ ! -e /dev/uio0 ]; then
    echo "警告: /dev/uio0 (VDMA) 不存在"
    if [ $TEST_MODE -eq 0 ]; then
        echo "切换到测试模式..."
        TEST_MODE=1
    fi
else
    echo "  ✅ /dev/uio0 (VDMA) 存在"
fi

if [ -e /dev/uio1 ]; then
    echo "  ✅ /dev/uio1 (VPSS) 存在"
fi

# 3. 配置 UVC Gadget
echo ""
echo "[2/4] 配置 USB Gadget..."
GADGET_PATH="/sys/kernel/config/usb_gadget/g1"

if [ ! -d "$GADGET_PATH" ]; then
    echo "  -> 运行 setup_uvc.sh..."
    bash "${SCRIPT_DIR}/setup_uvc.sh"
else
    echo "  ✅ USB Gadget 已配置"
fi

# 4. 检查 /dev/video0
echo ""
echo "[3/4] 检查视频设备..."
sleep 1  # 等待设备节点创建

if [ ! -e /dev/video0 ]; then
    echo "警告: /dev/video0 不存在"
    echo "尝试重新配置..."
    # 尝试清理并重新配置
    if [ -f "$GADGET_PATH/UDC" ]; then
        echo "" > "$GADGET_PATH/UDC" || true
    fi
    bash "${SCRIPT_DIR}/setup_uvc.sh"
    sleep 2
fi

if [ -e /dev/video0 ]; then
    echo "  ✅ /dev/video0 存在"
else
    echo "  ❌ /dev/video0 仍然不存在"
    echo "提示: 检查 USB 是否连接，或者内核是否支持 UVC Gadget"
    exit 1
fi

# 5. 启动视频流
echo ""
echo "[4/4] 启动视频流..."
echo ""

# 构建可执行文件路径
UVC_STREAM="/usr/bin/uvc-camera-app"

if [ ! -x "$UVC_STREAM" ]; then
    echo "错误: uvc_stream 可执行文件不存在或没有执行权限"
    echo "请先编译: cd ${PROJECT_ROOT}/src && make CROSS=1"
    exit 1
fi

# 构建命令行参数
ARGS="-w $WIDTH -H $HEIGHT -f $FPS"
if [ $TEST_MODE -eq 1 ]; then
    ARGS="$ARGS --test"
fi

echo "运行: $UVC_STREAM $ARGS"
echo ""
echo "================================================"
echo "按 Ctrl+C 停止"
echo "================================================"
echo ""

exec "$UVC_STREAM" $ARGS