# 以太网MACB驱动调试指南

本文档用于诊断和解决 ZynqMP GEM3 以太网 + KSZ9031 PHY 的连接问题。

---

## 问题描述

dmesg显示以下错误：
```
mdio_bus ff0e0000.ethernet-ffffffff: MDIO device at address 3 is missing.
```

这表示：
- ✅ MACB MAC驱动已正常加载
- ✅ MDIO总线已探测
- ❌ 在地址3找不到PHY设备

---

## 诊断步骤

### 第一步：扫描MDIO总线寻找PHY

PHY的实际地址可能不是3。运行以下命令扫描所有可能的PHY地址：

```bash
# 方法1：检查sysfs中的PHY信息
ls -la /sys/bus/mdio_bus/devices/

# 方法2：查看完整dmesg PHY信息
dmesg | grep -i "phy\|mdio\|ethernet"

# 方法3：如果有mii-tool
mii-tool -v eth0

# 方法4：使用ethtool检查
ethtool eth0
```

### 第二步：检查内核是否加载了PHY驱动

```bash
# 检查Micrel PHY驱动是否加载
lsmod | grep micrel
# 或
cat /proc/modules | grep -i micrel

# 检查所有PHY驱动
dmesg | grep -i "phy\|ksz\|micrel"

# 查看可用的PHY驱动
ls /lib/modules/$(uname -r)/kernel/drivers/net/phy/
```

**如果没有看到 `micrel.ko`，需要在内核配置中启用 Micrel PHY 驱动。**

### 第三步：检查设备树状态

```bash
# 检查GEM3状态
cat /proc/device-tree/axi/ethernet@ff0e0000/status
# 应该显示 "okay"

# 检查PHY节点
ls /proc/device-tree/axi/ethernet@ff0e0000/mdio/
# 应该看到 ethernet-phy@3 目录

# 检查PHY地址配置
cat /proc/device-tree/axi/ethernet@ff0e0000/mdio/ethernet-phy@3/reg
# 显示PHY地址
```

### 第四步：检查网络接口状态

```bash
# 查看所有网络接口
ip link show
# 或
ifconfig -a

# 查看接口详情
ip -d link show eth0
```

---

## 常见原因与解决方案

### 原因1：PHY地址配置错误

KSZ9031的地址由其PHYAD[2:0]引脚决定，可能的地址是0-7。

**排查方法：**
```bash
# 使用phytool扫描（如果可用）
for i in $(seq 0 31); do
    echo "Checking PHY address $i..."
    # 尝试读取PHY ID寄存器
done
```

**解决方法：**
修改 `system-user.dtsi` 中的PHY地址：

```dts
/* 如果PHY实际在地址0 */
phy0: ethernet-phy@0 {
    reg = <0>;  /* 改为实际地址 */
    /* ... */
};

/* 同时更新phy-handle */
&gem3 {
    phy-handle = <&phy0>;
    /* ... */
};
```

### 原因2：PHY需要GPIO复位

某些板子上PHY的复位引脚连接到GPIO，需要先复位PHY才能通信。

**排查方法：**
```bash
# 检查是否有PHY复位相关的GPIO
dmesg | grep -i reset
dmesg | grep -i gpio

# 查看GPIO状态
cat /sys/kernel/debug/gpio
```

**解决方法：**
在设备树中添加复位GPIO配置：

```dts
phy3: ethernet-phy@3 {
    reg = <3>;
    compatible = "micrel,ksz9031", "ethernet-phy-ieee802.3-c22";
    
    /* 添加复位GPIO - 根据实际硬件修改GPIO号 */
    reset-gpios = <&gpio 38 GPIO_ACTIVE_LOW>;
    reset-assert-us = <10000>;    /* 复位保持10ms */
    reset-deassert-us = <1000>;   /* 复位释放后等待1ms */
};
```

**如果不知道GPIO号，尝试手动复位：**
```bash
# 假设复位GPIO是38（根据原理图修改）
echo 38 > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio38/direction
echo 0 > /sys/class/gpio/gpio38/value   # 拉低复位
sleep 0.1
echo 1 > /sys/class/gpio/gpio38/value   # 释放复位
sleep 0.1

# 然后重新探测
echo "1" > /sys/bus/platform/drivers/macb/unbind 2>/dev/null
echo "ff0e0000.ethernet" > /sys/bus/platform/drivers/macb/bind
```

### 原因3：Micrel PHY驱动未编译

**排查方法：**
```bash
# 检查内核配置
zcat /proc/config.gz | grep -i MICREL
# 应该看到 CONFIG_MICREL_PHY=y 或 =m
```

**解决方法：**
在内核配置中启用：

```bash
petalinux-config -c kernel
```

导航到：
```
Device Drivers  --->
    [*] Network device support  --->
        -*- PHY Device support and infrastructure  --->
            <*>   Micrel PHYs              # 必须启用！
            <*>   Generic PHY              # 建议也启用
```

### 原因4：MDIO总线信号问题

**排查方法：**
```bash
# 检查MDIO总线状态
dmesg | grep -i mdio
```

**可能的硬件问题：**
- MDC/MDIO信号线连接问题
- 上拉电阻缺失
- 电压电平不匹配

---

## 尝试通用PHY驱动

如果Micrel特定驱动有问题，可以尝试使用通用PHY驱动：

修改 `system-user.dtsi`：

```dts
phy3: ethernet-phy@3 {
    reg = <3>;
    /* 只使用通用兼容性字符串 */
    compatible = "ethernet-phy-ieee802.3-c22";
};
```

---

## 尝试不同的PHY地址

创建一个测试脚本来尝试不同的PHY地址：

```bash
#!/bin/bash
# save as: test_phy_address.sh

echo "=== PHY Address Scanner ==="

# 检查sysfs
echo ""
echo "1. Checking /sys/bus/mdio_bus/devices/:"
ls -la /sys/bus/mdio_bus/devices/ 2>/dev/null || echo "   (not available)"

# 检查dmesg
echo ""
echo "2. PHY-related dmesg entries:"
dmesg | grep -i "phy\|mdio" | tail -20

# 检查网络接口
echo ""
echo "3. Network interfaces:"
ip link show

# 检查PHY驱动模块
echo ""
echo "4. Loaded PHY drivers:"
lsmod | grep -i phy

# 检查内核配置
echo ""
echo "5. Kernel PHY config:"
if [ -f /proc/config.gz ]; then
    zcat /proc/config.gz | grep -i "MICREL\|GENERIC_PHY"
else
    echo "   /proc/config.gz not available"
fi
```

---

## 完整调试命令汇总

在开发板上依次运行：

```bash
# 1. 基本信息
echo "=== System Info ===" && uname -a

# 2. 网络接口
echo "=== Network Interfaces ===" && ip link show

# 3. dmesg网络相关
echo "=== dmesg Ethernet ===" && dmesg | grep -i "eth\|macb\|phy\|mdio\|gem"

# 4. 设备树状态
echo "=== Device Tree ===" && ls /proc/device-tree/axi/ | grep -i eth

# 5. PHY驱动
echo "=== PHY Drivers ===" && ls /lib/modules/$(uname -r)/kernel/drivers/net/phy/ 2>/dev/null

# 6. MDIO设备
echo "=== MDIO Devices ===" && ls /sys/bus/mdio_bus/devices/ 2>/dev/null

# 7. GPIO状态（如果有复位GPIO）
echo "=== GPIO Status ===" && cat /sys/kernel/debug/gpio 2>/dev/null | head -50
```

---

## 快速修复尝试

### 方案A：尝试PHY地址0

修改 `system-user.dtsi`：

```dts
&gem3 {
    status = "okay";
    phy-mode = "rgmii-id";
    phy-handle = <&phy0>;
    
    mdio: mdio {
        #address-cells = <1>;
        #size-cells = <0>;
        
        /* 尝试地址0 */
        phy0: ethernet-phy@0 {
            reg = <0>;
            compatible = "ethernet-phy-ieee802.3-c22";
        };
    };
};
```

### 方案B：让内核自动扫描PHY

```dts
&gem3 {
    status = "okay";
    phy-mode = "rgmii-id";
    /* 不指定phy-handle，让内核自动扫描 */
    
    mdio: mdio {
        #address-cells = <1>;
        #size-cells = <0>;
    };
};
```

### 方案C：添加PHY复位

```dts
&gem3 {
    status = "okay";
    phy-mode = "rgmii-id";
    phy-handle = <&phy3>;
    
    mdio: mdio {
        #address-cells = <1>;
        #size-cells = <0>;
        
        phy3: ethernet-phy@3 {
            reg = <3>;
            compatible = "micrel,ksz9031", "ethernet-phy-ieee802.3-c22";
            
            /* 添加复位配置 - 修改GPIO号为实际值 */
            reset-gpios = <&gpio 38 GPIO_ACTIVE_LOW>;
            reset-assert-us = <10000>;
            reset-deassert-us = <1000>;
        };
    };
};
```

---

## 需要的硬件信息

为了进一步诊断，请提供：

1. **开发板型号**：（例如：ZCU102、Ultra96、自定义板）
2. **PHY型号确认**：（确认是否真的是KSZ9031）
3. **原理图信息**：
   - PHY的MDIO地址配置（PHYAD[2:0]引脚连接）
   - PHY复位引脚是否连接到GPIO
   - PHY复位引脚GPIO编号
4. **Vivado硬件设计**：确认GEM3是否正确配置

---

## 联系信息

如果以上步骤都无法解决问题，请提供：
- 完整的 `dmesg` 输出
- `ip link show` 输出
- `/sys/bus/mdio_bus/devices/` 内容
- 内核配置（`/proc/config.gz`）

---

*最后更新: 2025-12-09*
