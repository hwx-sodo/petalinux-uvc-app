#!/bin/bash
# diagnose_usb_issue.sh - 深度诊断 USB Device Controller 问题
#
# 针对错误: "failed to start g1: -19" (ENODEV)

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "========================================="
echo "   USB Device Controller 深度诊断"
echo "========================================="
echo ""

# ===============================================
# 1. 检查 USB PHY 状态
# ===============================================
echo -e "${YELLOW}[1/7] USB PHY 状态检查${NC}"
echo "----------------------------------------"

# 检查 PHY 设备
echo "USB PHY 设备:"
if [ -d "/sys/class/phy" ]; then
    ls -la /sys/class/phy/ 2>/dev/null | grep -i usb || echo "  (未找到 USB PHY 设备)"
else
    echo "  /sys/class/phy 目录不存在"
fi

# 检查 xilinx-psgtr (ZynqMP GT PHY)
echo ""
echo "Xilinx PS-GTR PHY:"
if ls /sys/devices/platform/*/phy* 2>/dev/null | head -5; then
    :
else
    echo "  未找到 PS-GTR PHY 设备"
fi
echo ""

# ===============================================
# 2. 检查 dr_mode 配置
# ===============================================
echo -e "${YELLOW}[2/7] USB dr_mode 配置${NC}"
echo "----------------------------------------"

# 查找所有 dr_mode 文件
DR_MODE_FILES=$(find /proc/device-tree -name "dr_mode" 2>/dev/null)
if [ -n "$DR_MODE_FILES" ]; then
    for f in $DR_MODE_FILES; do
        MODE=$(cat "$f" 2>/dev/null | tr -d '\0')
        echo "  $f: $MODE"
        
        if [ "$MODE" = "host" ]; then
            echo -e "  ${RED}⚠️  警告: 此 USB 配置为 Host 模式，无法用于 Gadget！${NC}"
        elif [ "$MODE" = "peripheral" ] || [ "$MODE" = "device" ]; then
            echo -e "  ${GREEN}✓ Peripheral 模式正确${NC}"
        elif [ "$MODE" = "otg" ]; then
            echo -e "  ${YELLOW}⚠️  OTG 模式 - 需要 ID 引脚接地才能工作在 Device 模式${NC}"
        fi
    done
else
    echo "  未找到 dr_mode 配置"
fi
echo ""

# ===============================================
# 3. 检查 USB 控制器的详细状态
# ===============================================
echo -e "${YELLOW}[3/7] USB 控制器详细状态${NC}"
echo "----------------------------------------"

for udc in /sys/class/udc/*; do
    if [ -d "$udc" ]; then
        UDC_NAME=$(basename "$udc")
        echo "UDC: $UDC_NAME"
        
        # 状态
        STATE=$(cat "$udc/state" 2>/dev/null || echo "unknown")
        echo "  当前状态: $STATE"
        
        case "$STATE" in
            "not attached")
                echo -e "  ${YELLOW}→ USB 线可能未连接到主机${NC}"
                echo -e "  ${YELLOW}→ 或 PHY 未正确初始化${NC}"
                ;;
            "attached")
                echo -e "  ${GREEN}→ USB 已连接${NC}"
                ;;
            "powered")
                echo -e "  ${GREEN}→ USB 已供电${NC}"
                ;;
            "configured")
                echo -e "  ${GREEN}→ USB 已配置 (正常)${NC}"
                ;;
        esac
        
        # 当前速度
        SPEED=$(cat "$udc/current_speed" 2>/dev/null || echo "unknown")
        echo "  当前速度: $SPEED"
        
        # 最大速度
        MAX_SPEED=$(cat "$udc/maximum_speed" 2>/dev/null || echo "unknown")
        echo "  最大速度: $MAX_SPEED"
        
        # 查看设备属性
        echo "  设备属性:"
        if [ -d "$udc/device" ]; then
            # 检查驱动
            DRIVER=$(readlink "$udc/device/driver" 2>/dev/null | xargs basename 2>/dev/null || echo "unknown")
            echo "    驱动: $DRIVER"
            
            # 检查 VBUS 相关
            if [ -f "$udc/device/is_selfpowered" ]; then
                echo "    自供电: $(cat $udc/device/is_selfpowered 2>/dev/null)"
            fi
        fi
    fi
done
echo ""

# ===============================================
# 4. 检查 USB 线连接状态 (VBUS)
# ===============================================
echo -e "${YELLOW}[4/7] USB 连接检测${NC}"
echo "----------------------------------------"

# 检查是否有 VBUS 信号
echo "检查 VBUS (USB 电源)..."

# 方法1: 通过 extcon 检查
if [ -d "/sys/class/extcon" ]; then
    echo "Extcon 设备:"
    for ext in /sys/class/extcon/*; do
        if [ -d "$ext" ]; then
            EXT_NAME=$(basename "$ext")
            echo "  $EXT_NAME:"
            for state in "$ext"/cable.*/state; do
                if [ -f "$state" ]; then
                    CABLE=$(dirname "$state")
                    CABLE_NAME=$(cat "$CABLE/name" 2>/dev/null || basename "$CABLE")
                    echo "    $CABLE_NAME: $(cat $state 2>/dev/null)"
                fi
            done
        fi
    done
else
    echo "  没有 extcon 设备 (可能不支持动态检测)"
fi

# 方法2: 检查 USB 状态寄存器
echo ""
echo "DWC3 控制器状态:"
DWC3_DEV=$(find /sys/devices -name "dwc3" -type d 2>/dev/null | head -1)
if [ -n "$DWC3_DEV" ]; then
    echo "  路径: $DWC3_DEV"
    if [ -f "$DWC3_DEV/mode" ]; then
        echo "  模式: $(cat $DWC3_DEV/mode 2>/dev/null)"
    fi
else
    echo "  未找到 DWC3 设备节点"
fi
echo ""

# ===============================================
# 5. 检查时钟配置
# ===============================================
echo -e "${YELLOW}[5/7] 时钟配置${NC}"
echo "----------------------------------------"

# 检查 USB 相关时钟
echo "USB 相关时钟:"
if [ -d "/sys/kernel/debug/clk" ]; then
    for clk in /sys/kernel/debug/clk/*usb* /sys/kernel/debug/clk/*dwc* /sys/kernel/debug/clk/*ref*; do
        if [ -d "$clk" ]; then
            CLK_NAME=$(basename "$clk")
            ENABLE=$(cat "$clk/clk_enable_count" 2>/dev/null || echo "?")
            RATE=$(cat "$clk/clk_rate" 2>/dev/null || echo "?")
            echo "  $CLK_NAME: enable=$ENABLE, rate=$RATE Hz"
        fi
    done 2>/dev/null
    
    # 没有找到匹配的时钟
    if ! ls /sys/kernel/debug/clk/*usb* /sys/kernel/debug/clk/*dwc* /sys/kernel/debug/clk/*ref* 2>/dev/null | head -1 > /dev/null; then
        echo "  (未找到 USB 相关时钟，可能需要 mount debugfs)"
    fi
else
    echo "  debugfs 未挂载或时钟调试不可用"
    echo "  尝试: mount -t debugfs none /sys/kernel/debug"
fi
echo ""

# ===============================================
# 6. 检查内核错误详情
# ===============================================
echo -e "${YELLOW}[6/7] 关键内核错误${NC}"
echo "----------------------------------------"

echo "PHY 相关错误:"
dmesg 2>/dev/null | grep -iE "(phy|usb3-phy)" | tail -5 || echo "  (无)"

echo ""
echo "时钟相关错误:"
dmesg 2>/dev/null | grep -iE "(clk|clock).*fail" | tail -5 || echo "  (无)"

echo ""
echo "DWC3 错误:"
dmesg 2>/dev/null | grep -iE "dwc3.*fail|dwc3.*error|dwc3.*-[0-9]+" | tail -5 || echo "  (无)"

echo ""

# ===============================================
# 7. 检查硬件设计 (Vivado) 配置
# ===============================================
echo -e "${YELLOW}[7/7] 硬件设计检查${NC}"
echo "----------------------------------------"

# 检查 USB 控制器地址
echo "USB 控制器地址映射:"
echo "  fe200000.dwc3 - 这是 PS USB3 控制器"
echo "  ff9d0000.usb0 - 这是 USB 包装器"

# 检查设备树中的 USB 配置
echo ""
echo "设备树 USB 节点:"
for usb_node in /proc/device-tree/axi/usb* /proc/device-tree/amba/usb*; do
    if [ -d "$usb_node" ]; then
        echo "  $usb_node:"
        [ -f "$usb_node/status" ] && echo "    status: $(cat $usb_node/status 2>/dev/null | tr -d '\0')"
        
        # 检查子节点 (dwc3)
        for dwc in "$usb_node"/dwc3*; do
            if [ -d "$dwc" ]; then
                echo "    $(basename $dwc):"
                [ -f "$dwc/status" ] && echo "      status: $(cat $dwc/status 2>/dev/null | tr -d '\0')"
                [ -f "$dwc/dr_mode" ] && echo "      dr_mode: $(cat $dwc/dr_mode 2>/dev/null | tr -d '\0')"
                [ -f "$dwc/maximum-speed" ] && echo "      max-speed: $(cat $dwc/maximum-speed 2>/dev/null | tr -d '\0')"
            fi
        done
    fi
done
echo ""

# ===============================================
# 总结
# ===============================================
echo "========================================="
echo -e "${YELLOW}诊断结论与建议${NC}"
echo "========================================="
echo ""

# 分析日志中的错误
if dmesg 2>/dev/null | grep -q "Can't find usb3-phy"; then
    echo -e "${RED}❌ 发现问题: USB3 PHY 未找到${NC}"
    echo "   这是导致 -19 (ENODEV) 错误的主要原因！"
    echo ""
    echo "   解决方案:"
    echo "   1. 检查设备树中 USB 节点的 phy-names 和 phys 属性"
    echo "   2. 确保 PHY 驱动正确加载"
    echo "   3. 检查 Vivado 硬件设计中 USB PHY 是否正确配置"
    echo ""
fi

if dmesg 2>/dev/null | grep -q "Failed to get clk 'ref'"; then
    echo -e "${RED}❌ 发现问题: 缺少 'ref' 时钟${NC}"
    echo "   USB 控制器无法获取参考时钟"
    echo ""
    echo "   解决方案:"
    echo "   1. 检查设备树中时钟配置"
    echo "   2. 确保 PS 时钟配置正确"
    echo ""
fi

# 检查 UDC 状态
UDC_STATE=$(cat /sys/class/udc/*/state 2>/dev/null | head -1)
if [ "$UDC_STATE" = "not attached" ]; then
    echo -e "${YELLOW}⚠️  UDC 状态: not attached${NC}"
    echo "   可能原因:"
    echo "   1. USB 线未连接到 PC/Host"
    echo "   2. USB PHY 未正确初始化 (很可能是这个)"
    echo "   3. 使用了错误的 USB 端口"
    echo ""
fi

# 检查 dr_mode
DR_MODE=$(find /proc/device-tree -name "dr_mode" -exec cat {} \; 2>/dev/null | tr -d '\0' | head -1)
if [ "$DR_MODE" = "host" ]; then
    echo -e "${RED}❌ 发现问题: USB 配置为 Host 模式${NC}"
    echo "   需要修改设备树，将 dr_mode 改为 \"peripheral\" 或 \"otg\""
    echo ""
fi

echo "----------------------------------------"
echo "下一步操作建议:"
echo ""
echo "1. 首先确认 USB 线已连接到电脑"
echo ""
echo "2. 如果 PHY 错误，需要检查/修改设备树:"
echo "   在 Petalinux 项目中修改 system-user.dtsi:"
echo ""
echo "   &dwc3_0 {"
echo "       status = \"okay\";"
echo "       dr_mode = \"peripheral\";"
echo "   };"
echo ""
echo "3. 如果问题仍然存在，可能需要检查 Vivado 硬件设计"
echo "   确保 USB 控制器已启用且配置正确"
echo ""
echo "4. 重新生成并部署 boot.bin 和 image.ub"
echo ""
