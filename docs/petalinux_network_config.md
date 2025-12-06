# PetaLinux 网络配置指南

本文档说明如何在PetaLinux中配置GEM3以太网接口。

---

## 一、设备树配置

### 1.1 创建/修改 system-user.dtsi

文件位置：`project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi`

```dts
/include/ "system-conf.dtsi"
/ {
};

/* ========== 启用GEM3以太网 ========== */
&gem3 {
    status = "okay";
    
    /* 接口类型 - 根据实际硬件修改 */
    phy-mode = "rgmii-id";  /* 常见选项: rgmii, rgmii-id, rgmii-rxid, rgmii-txid, gmii, mii, sgmii */
    
    /* PHY句柄 - 指向下面定义的PHY节点 */
    phy-handle = <&phy0>;
    
    /* MDIO总线定义 */
    mdio {
        #address-cells = <1>;
        #size-cells = <0>;
        
        /* PHY节点 - 根据实际PHY修改 */
        phy0: ethernet-phy@0 {
            /* 
             * PHY地址 - 根据硬件修改
             * 常见值: 0, 1, 4, 7
             */
            reg = <0>;
            
            /* 
             * PHY芯片兼容性字符串 - 根据实际芯片修改
             * 
             * 常见PHY:
             * - Marvell 88E1512:  "marvell,88e1510", "ethernet-phy-ieee802.3-c22"
             * - Marvell 88E1111:  "marvell,88e1111", "ethernet-phy-ieee802.3-c22"
             * - Realtek RTL8211:  "realtek,rtl8211f", "ethernet-phy-ieee802.3-c22"
             * - Micrel KSZ9031:   "micrel,ksz9031", "ethernet-phy-ieee802.3-c22"
             * - TI DP83867:       "ti,dp83867", "ethernet-phy-ieee802.3-c22"
             * - 通用PHY:          "ethernet-phy-ieee802.3-c22"
             */
            compatible = "ethernet-phy-ieee802.3-c22";
            
            /* 
             * PHY复位GPIO（可选）
             * 如果PHY有复位引脚连接到GPIO，取消注释并修改
             */
            /* reset-gpios = <&gpio 38 GPIO_ACTIVE_LOW>; */
            /* reset-assert-us = <100>; */
            /* reset-deassert-us = <100>; */
        };
    };
};

/* ========== 保留内存用于视频缓冲 ========== */
&reserved_memory {
    #address-cells = <2>;
    #size-cells = <2>;
    ranges;
    
    /* 视频帧缓冲区 - 用于VDMA */
    video_buffer: video_buffer@20000000 {
        compatible = "shared-dma-pool";
        reg = <0x0 0x20000000 0x0 0x10000000>;  /* 256MB at 0x20000000 */
        no-map;
    };
};

/* ========== VDMA配置 (使用UIO) ========== */
&axi_vdma_0 {
    compatible = "generic-uio";
    status = "okay";
};

/* ========== VPSS配置 (使用UIO) ========== */
&v_proc_ss_0 {
    compatible = "generic-uio";
    status = "okay";
};
```

---

## 二、phy-mode 选择指南

| phy-mode值 | 说明 | 使用场景 |
|------------|------|----------|
| `rgmii` | 标准RGMII | PHY内部处理延迟 |
| `rgmii-id` | RGMII + 内部TX/RX延迟 | **最常用**，由MAC添加延迟 |
| `rgmii-txid` | RGMII + 仅TX延迟 | 部分PHY需要 |
| `rgmii-rxid` | RGMII + 仅RX延迟 | 部分PHY需要 |
| `gmii` | GMII接口 | 老式千兆 |
| `mii` | MII接口 | 百兆网络 |
| `sgmii` | 串行GMII | 高端应用 |

**如何选择：**
- 大多数情况下先尝试 `rgmii-id`
- 如果网络不通，查看PHY数据手册确认延迟配置
- 或者逐一尝试其他rgmii变体

---

## 三、内核配置

运行 `petalinux-config -c kernel`，确保以下选项启用：

### 3.1 网络核心

```
[*] Networking support  --->
    Networking options  --->
        [*] TCP/IP networking
        [*]   IP: kernel level autoconfiguration
        [*]     IP: DHCP support
        [*]     IP: BOOTP support
```

### 3.2 以太网驱动

```
Device Drivers  --->
    [*] Network device support  --->
        [*]   Ethernet driver support  --->
            [*]   Cadence devices
            <*>     Cadence MACB/GEM support
```

### 3.3 PHY驱动

根据您的PHY芯片选择：

```
Device Drivers  --->
    [*] Network device support  --->
        -*- PHY Device support and infrastructure  --->
            
            # Marvell PHY
            <*>   Marvell PHYs
            
            # Realtek PHY  
            <*>   Realtek PHYs
            
            # Micrel/Microchip PHY
            <*>   Micrel PHYs
            
            # TI PHY
            <*>   Texas Instruments PHYs
            
            # 通用PHY（建议始终启用）
            <*>   Generic PHY
```

### 3.4 网络工具支持

```
[*] Networking support  --->
    Networking options  --->
        [*] Network packet filtering framework (Netfilter)  --->
        [*] Unix domain sockets
        <*> The IPv6 protocol
```

---

## 四、Rootfs软件包配置

运行 `petalinux-config -c rootfs`：

### 4.1 必需的网络工具

```
Filesystem Packages  --->
    base  --->
        [*] net-tools
        [*] iproute2
    console/network  --->
        [*] ethtool
        [*] iperf3        # 可选，用于测试带宽
        [*] netcat        # 可选，用于调试
    console/utils  --->
        [*] iputils-ping  # 或 busybox 中的 ping
```

### 4.2 推荐的调试工具

```
Filesystem Packages  --->
    console/network  --->
        [*] tcpdump       # 网络抓包
        [*] curl          # HTTP测试
    misc  --->
        [*] socat         # Socket工具
```

---

## 五、完整配置步骤

### 步骤1：修改设备树

```bash
# 进入PetaLinux项目目录
cd <petalinux-project>

# 编辑设备树
nano project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi
```

粘贴上面的设备树内容，根据您的PHY修改。

### 步骤2：配置内核

```bash
petalinux-config -c kernel
```

按上述3.1-3.4进行配置，保存退出。

### 步骤3：配置Rootfs

```bash
petalinux-config -c rootfs
```

按上述4.1-4.2添加软件包，保存退出。

### 步骤4：编译

```bash
petalinux-build
```

### 步骤5：生成镜像

```bash
petalinux-package --boot --fsbl images/linux/zynqmp_fsbl.elf \
    --u-boot images/linux/u-boot.elf \
    --pmufw images/linux/pmufw.elf \
    --fpga images/linux/*.bit \
    --force
```

---

## 六、验证网络配置

启动系统后：

### 6.1 检查网口识别

```bash
# 查看网络接口
ifconfig -a

# 应该看到 eth0 或类似接口
```

### 6.2 检查PHY连接

```bash
# 查看PHY状态
cat /sys/class/net/eth0/carrier   # 1=连接, 0=断开

# 查看链路速度
ethtool eth0
```

### 6.3 配置IP

```bash
# 手动配置IP
ifconfig eth0 10.72.43.10 netmask 255.255.0.0 up

# 或使用DHCP
udhcpc -i eth0
```

### 6.4 测试连通性

```bash
ping 10.72.43.219
```

---

## 七、常见问题

### 问题1：看不到eth0接口

**可能原因：**
- 设备树中gem3未正确启用
- PHY驱动未加载

**排查：**
```bash
# 检查设备树
cat /proc/device-tree/axi/gem@ff0e0000/status
# 应该显示 "okay"

# 检查驱动加载
dmesg | grep -i macb
dmesg | grep -i eth
```

### 问题2：网口不通

**可能原因：**
- phy-mode配置错误
- PHY地址错误
- PHY需要复位

**排查：**
```bash
# 检查PHY检测
dmesg | grep -i phy

# 检查链路状态
ethtool eth0
```

### 问题3：速度只有100Mbps

**可能原因：**
- 网线问题
- PHY配置问题

**排查：**
```bash
# 查看协商结果
ethtool eth0

# 强制千兆（测试用）
ethtool -s eth0 speed 1000 duplex full autoneg off
```

---

## 八、常见开发板配置参考

### ZCU102/ZCU104 (Marvell 88E1512)

```dts
&gem3 {
    status = "okay";
    phy-mode = "rgmii-id";
    phy-handle = <&phy0>;
    mdio {
        #address-cells = <1>;
        #size-cells = <0>;
        phy0: ethernet-phy@12 {
            reg = <12>;
            compatible = "marvell,88e1510", "ethernet-phy-ieee802.3-c22";
        };
    };
};
```

### Ultra96 (Microchip LAN8742)

```dts
&gem3 {
    status = "okay";
    phy-mode = "rgmii-id";
    phy-handle = <&phy0>;
    mdio {
        #address-cells = <1>;
        #size-cells = <0>;
        phy0: ethernet-phy@0 {
            reg = <0>;
            compatible = "ethernet-phy-ieee802.3-c22";
        };
    };
};
```

---

*配置完成后，请参考 NETWORK_STREAMING_GUIDE.md 进行视频流传输测试。*
