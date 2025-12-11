#!/bin/bash
# 以太网PHY诊断脚本
# 用法: ./diagnose_eth.sh

echo "========================================"
echo "   以太网 PHY 诊断工具"
echo "========================================"
echo ""

# 1. 检查设备树中GEM3状态
echo "1. 检查设备树 GEM3 配置:"
echo "----------------------------------------"
if [ -d "/proc/device-tree/axi/ethernet@ff0e0000" ]; then
    echo "   GEM3节点: 存在 ✓"
    
    # 检查status
    if [ -f "/proc/device-tree/axi/ethernet@ff0e0000/status" ]; then
        STATUS=$(cat /proc/device-tree/axi/ethernet@ff0e0000/status 2>/dev/null | tr -d '\0')
        echo "   status: $STATUS"
    fi
    
    # 检查phy-mode
    if [ -f "/proc/device-tree/axi/ethernet@ff0e0000/phy-mode" ]; then
        PHY_MODE=$(cat /proc/device-tree/axi/ethernet@ff0e0000/phy-mode 2>/dev/null | tr -d '\0')
        echo "   phy-mode: $PHY_MODE"
    fi
    
    # 检查MDIO子节点
    if [ -d "/proc/device-tree/axi/ethernet@ff0e0000/mdio" ]; then
        echo "   MDIO节点: 存在 ✓"
        echo "   MDIO内容:"
        ls /proc/device-tree/axi/ethernet@ff0e0000/mdio/ 2>/dev/null | sed 's/^/      /'
    else
        echo "   MDIO节点: 不存在 ✗"
    fi
else
    echo "   GEM3节点: 不存在 ✗"
    echo "   尝试查找其他位置..."
    find /proc/device-tree -name "*ethernet*" -o -name "*gem*" 2>/dev/null | head -10
fi
echo ""

# 2. 检查MACB驱动状态
echo "2. 检查 MACB 驱动状态:"
echo "----------------------------------------"
if [ -d "/sys/bus/platform/drivers/macb" ]; then
    echo "   MACB驱动: 已加载 ✓"
    echo "   绑定的设备:"
    ls /sys/bus/platform/drivers/macb/ 2>/dev/null | grep -v "bind\|unbind\|uevent" | sed 's/^/      /'
else
    echo "   MACB驱动: 未加载 ✗"
fi
echo ""

# 3. 检查GPIO状态（可能需要复位PHY）
echo "3. 检查 GPIO 状态 (寻找可能的PHY复位引脚):"
echo "----------------------------------------"
if [ -f "/sys/kernel/debug/gpio" ]; then
    echo "   GPIO概览 (前30行):"
    cat /sys/kernel/debug/gpio 2>/dev/null | head -30 | sed 's/^/      /'
else
    echo "   无法读取GPIO状态 (需要debugfs)"
    echo "   尝试: mount -t debugfs none /sys/kernel/debug"
fi
echo ""

# 4. 完整的以太网相关dmesg
echo "4. 完整以太网相关 dmesg:"
echo "----------------------------------------"
dmesg | grep -i "gem\|macb\|eth\|mdio\|phy" | grep -v "physical\|usbphy" | tail -20 | sed 's/^/   /'
echo ""

# 5. 检查regulator（PHY可能需要电源）
echo "5. 检查电源管理 (Regulators):"
echo "----------------------------------------"
if [ -d "/sys/class/regulator" ]; then
    ls /sys/class/regulator/ 2>/dev/null | head -10 | sed 's/^/      /'
else
    echo "   无regulator信息"
fi
echo ""

# 6. 硬件信息
echo "6. 系统硬件信息:"
echo "----------------------------------------"
if [ -f "/proc/device-tree/model" ]; then
    MODEL=$(cat /proc/device-tree/model 2>/dev/null | tr -d '\0')
    echo "   设备型号: $MODEL"
fi
if [ -f "/proc/device-tree/compatible" ]; then
    COMPAT=$(cat /proc/device-tree/compatible 2>/dev/null | tr '\0' ' ')
    echo "   兼容性: $COMPAT"
fi
echo ""

echo "========================================"
echo "   诊断完成"
echo "========================================"
echo ""
echo "如果MDIO总线为空，可能的原因:"
echo "  1. PHY芯片需要GPIO复位"
echo "  2. PHY电源未开启"
echo "  3. MDIO信号线硬件问题"
echo "  4. PHY地址配置错误"
echo "  5. PHY芯片损坏"
echo ""
