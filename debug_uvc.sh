#!/bin/bash
# debug_uvc.sh - UVC Gadget 调试工具
#
# 用于诊断 UVC Gadget 配置失败的原因

echo "========================================="
echo "       UVC Gadget 调试工具"
echo "========================================="
echo ""

# 1. 系统信息
echo "[1/6] 系统信息"
echo "----------------------------------------"
echo "内核版本: $(uname -r)"
echo "系统架构: $(uname -m)"
echo ""

# 2. ConfigFS 检查
echo "[2/6] ConfigFS 状态"
echo "----------------------------------------"
if [ -d "/sys/kernel/config" ]; then
    echo "ConfigFS 路径: /sys/kernel/config"
    echo "USB Gadget: $(ls /sys/kernel/config/usb_gadget/ 2>/dev/null || echo '(空)')"
else
    echo "❌ ConfigFS 未挂载"
    echo "运行: mount -t configfs none /sys/kernel/config"
fi
echo ""

# 3. UDC 控制器检查
echo "[3/6] USB Device Controller (UDC)"
echo "----------------------------------------"
if [ -d "/sys/class/udc" ]; then
    UDC_LIST=$(ls /sys/class/udc 2>/dev/null)
    if [ -n "$UDC_LIST" ]; then
        for udc in $UDC_LIST; do
            echo "UDC: $udc"
            echo "  状态: $(cat /sys/class/udc/$udc/state 2>/dev/null || echo 'unknown')"
            echo "  速度: $(cat /sys/class/udc/$udc/current_speed 2>/dev/null || echo 'unknown')"
            echo "  最大速度: $(cat /sys/class/udc/$udc/maximum_speed 2>/dev/null || echo 'unknown')"
            
            # 检查 OTG 模式
            if [ -f "/sys/class/udc/$udc/device/role" ]; then
                echo "  角色: $(cat /sys/class/udc/$udc/device/role 2>/dev/null)"
            fi
            
            # 检查驱动
            DRIVER=$(readlink /sys/class/udc/$udc/device/driver 2>/dev/null | xargs basename 2>/dev/null || echo "unknown")
            echo "  驱动: $DRIVER"
        done
    else
        echo "❌ 未找到任何 UDC 控制器"
        echo ""
        echo "可能的原因:"
        echo "  1. USB 控制器未配置为 Device 模式"
        echo "  2. 设备树配置错误"
        echo "  3. USB 驱动未加载"
    fi
else
    echo "❌ /sys/class/udc 目录不存在"
fi
echo ""

# 4. 已加载的 USB Gadget 模块
echo "[4/6] USB Gadget 内核模块"
echo "----------------------------------------"
GADGET_MODULES=$(lsmod 2>/dev/null | grep -E "(usb_f_uvc|libcomposite|udc|gadget|dwc3)" || echo "")
if [ -n "$GADGET_MODULES" ]; then
    echo "$GADGET_MODULES"
else
    echo "未找到 USB Gadget 相关模块 (可能是内核内置)"
fi
echo ""

# 5. 设备树检查
echo "[5/6] 设备树 USB 配置"
echo "----------------------------------------"
if [ -d "/proc/device-tree" ]; then
    # 查找 DWC3 节点
    DWC3_PATHS=$(find /proc/device-tree -name "dwc3*" -o -name "*usb*" 2>/dev/null | head -10)
    if [ -n "$DWC3_PATHS" ]; then
        for path in $DWC3_PATHS; do
            if [ -f "$path/dr_mode" ]; then
                echo "USB 节点: $path"
                echo "  dr_mode: $(cat $path/dr_mode 2>/dev/null | tr -d '\0')"
            fi
        done
    else
        echo "未找到 USB 设备树节点"
    fi
    
    # 检查 ZynqMP USB
    if [ -d "/proc/device-tree/axi" ]; then
        USB_NODES=$(find /proc/device-tree/axi -maxdepth 2 -name "status" 2>/dev/null | while read f; do
            if grep -q "okay" "$f" 2>/dev/null; then
                dirname $f
            fi
        done | grep -i usb || true)
        if [ -n "$USB_NODES" ]; then
            echo ""
            echo "活动的 USB 节点:"
            echo "$USB_NODES"
        fi
    fi
else
    echo "无法访问 /proc/device-tree"
fi
echo ""

# 6. 最近的内核日志
echo "[6/6] 内核日志 (USB 相关)"
echo "----------------------------------------"
dmesg 2>/dev/null | grep -iE "(udc|dwc3|usb|gadget|uvc)" | tail -30 || echo "无法读取 dmesg"
echo ""

# 7. 总结和建议
echo "========================================="
echo "诊断总结"
echo "========================================="
echo ""

# 检查常见问题
ISSUES=0

# 检查 UDC
if [ -z "$(ls /sys/class/udc 2>/dev/null)" ]; then
    echo "❌ 问题: 没有可用的 UDC 控制器"
    echo "   建议: 检查设备树中 USB dr_mode 是否设置为 \"peripheral\""
    ISSUES=$((ISSUES+1))
fi

# 检查 ConfigFS
if [ ! -d "/sys/kernel/config/usb_gadget" ]; then
    echo "❌ 问题: ConfigFS USB Gadget 不可用"
    echo "   建议: 确保内核启用了 CONFIG_USB_CONFIGFS 和 CONFIG_USB_CONFIGFS_F_UVC"
    ISSUES=$((ISSUES+1))
fi

# 检查 UVC 模块
if ! (lsmod 2>/dev/null | grep -q usb_f_uvc) && ! (grep -q USB_F_UVC /proc/config.gz 2>/dev/null); then
    echo "⚠️  注意: usb_f_uvc 模块可能未加载"
    echo "   建议: 尝试 modprobe usb_f_uvc (如果是模块编译)"
fi

if [ $ISSUES -eq 0 ]; then
    echo "✅ 基本检查通过"
    echo ""
    echo "如果仍然遇到 -19 (ENODEV) 错误，请检查:"
    echo "  1. USB 线是否正确连接"
    echo "  2. USB PHY 是否正确初始化"
    echo "  3. Vivado 硬件设计中 USB 是否配置正确"
fi

echo ""
