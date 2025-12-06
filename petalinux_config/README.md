# PetaLinux 网络配置完整步骤

**适用于：KSZ9031RNX PHY + GEM3**

---

## 配置文件说明

```
petalinux_config/
├── system-user.dtsi    # 设备树配置（复制到项目中）
└── README.md           # 本文档
```

---

## 第一步：复制设备树文件

将 `system-user.dtsi` 复制到您的PetaLinux项目中：

```bash
# 进入您的PetaLinux项目目录
cd <您的PetaLinux项目路径>

# 复制设备树文件
cp /path/to/petalinux_config/system-user.dtsi \
   project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi
```

---

## 第二步：配置内核

```bash
petalinux-config -c kernel
```

### 2.1 启用网络核心功能

进入菜单，确保以下选项启用：

```
[*] Networking support  --->
    Networking options  --->
        <*> Packet socket                    # 数据包套接字
        <*> Unix domain sockets              # Unix域套接字
        [*] TCP/IP networking                # TCP/IP协议栈
        [*]   IP: multicasting               # 多播支持
        [*]   IP: advanced router            # 高级路由
        [*]   IP: kernel level autoconfiguration
        [*]     IP: DHCP support
        [*]     IP: BOOTP support
```

### 2.2 启用以太网MAC驱动

```
Device Drivers  --->
    [*] Network device support  --->
        [*]   Ethernet driver support  --->
            [*]   Cadence devices
            <*>     Cadence MACB/GEM support    # ← 必须启用！
```

### 2.3 启用PHY驱动

```
Device Drivers  --->
    [*] Network device support  --->
        -*- PHY Device support and infrastructure  --->
            <*>   Micrel PHYs                   # ← KSZ9031驱动，必须启用！
            <*>   Generic PHY                   # ← 建议也启用
```

### 2.4 保存并退出

按两次 `Esc` → 选择 `Yes` 保存

---

## 第三步：配置Rootfs

```bash
petalinux-config -c rootfs
```

### 3.1 添加网络工具

```
Filesystem Packages  --->
    base  --->
        [*] net-tools                        # ifconfig, netstat等

    console/network  --->
        [*] ethtool                          # 网络诊断工具
        [*] iperf3                           # 带宽测试（可选）
        
    misc  --->
        [*] bridge-utils                     # 网桥工具（可选）
```

### 3.2 确保基础工具存在

```
Filesystem Packages  --->
    console/utils  --->
        [*] bash                             # 或保持busybox shell
```

### 3.3 保存并退出

---

## 第四步：编译项目

```bash
# 完整编译
petalinux-build

# 如果只修改了设备树，可以单独编译
petalinux-build -c device-tree
petalinux-build -c kernel
```

---

## 第五步：打包镜像

```bash
petalinux-package --boot \
    --fsbl images/linux/zynqmp_fsbl.elf \
    --u-boot images/linux/u-boot.elf \
    --pmufw images/linux/pmufw.elf \
    --fpga images/linux/*.bit \
    --force
```

生成的文件：
- `images/linux/BOOT.BIN` - 启动文件
- `images/linux/image.ub` - 内核镜像
- `images/linux/boot.scr` - U-Boot脚本

---

## 第六步：烧录和启动

将以下文件复制到SD卡：

```
BOOT.BIN
image.ub
boot.scr
```

---

## 第七步：验证网络

启动系统后，通过串口终端验证：

### 7.1 检查网口识别

```bash
# 查看所有网络接口
ifconfig -a

# 应该看到 eth0
```

**正常输出示例：**
```
eth0      Link encap:Ethernet  HWaddr XX:XX:XX:XX:XX:XX
          UP BROADCAST MULTICAST  MTU:1500  Metric:1
          ...
```

### 7.2 检查PHY状态

```bash
# 查看详细网络状态
ethtool eth0
```

**正常输出示例：**
```
Settings for eth0:
        Supported ports: [ TP MII ]
        Supported link modes:   10baseT/Half 10baseT/Full
                                100baseT/Half 100baseT/Full
                                1000baseT/Half 1000baseT/Full
        ...
        Speed: 1000Mb/s          # 千兆连接
        Duplex: Full             # 全双工
        Link detected: yes       # 已检测到链路
```

### 7.3 配置IP地址

```bash
# 手动配置IP（修改为您的网络配置）
ifconfig eth0 192.168.1.10 netmask 255.255.255.0 up

# 添加默认网关（如果需要）
route add default gw 192.168.1.1

# 或者使用DHCP自动获取
udhcpc -i eth0
```

### 7.4 测试连通性

```bash
# Ping PC
ping 192.168.1.100 -c 4
```

---

## 第八步：运行网络视频流

确认网络正常后，运行视频流传输：

### 8.1 在PC端先启动接收

```bash
python receive_stream.py -p 5000
```

### 8.2 在开发板上启动发送

```bash
sudo ./run_network_stream.sh 192.168.1.100
```

---

## 常见问题排查

### 问题1：看不到eth0接口

**排查步骤：**
```bash
# 检查设备树是否正确加载
cat /proc/device-tree/axi/gem@ff0e0000/status
# 应该显示 "okay"

# 检查MAC驱动
dmesg | grep -i macb
# 应该看到 "macb ff0e0000.ethernet" 相关信息

# 检查PHY驱动
dmesg | grep -i ksz
dmesg | grep -i micrel
```

**可能原因：**
- 设备树未正确复制
- 内核未启用 Cadence MACB 驱动
- 内核未启用 Micrel PHY 驱动

### 问题2：Link detected: no

**排查步骤：**
```bash
# 检查PHY检测
dmesg | grep -i phy

# 检查PHY地址是否正确
# 应该看到 "PHY [ff0e0000.ethernet-ffffffff:03]" 
# 其中 03 是PHY地址
```

**可能原因：**
- PHY地址配置错误（应该是3）
- 网线未插好
- PHY需要复位

### 问题3：速度只有100Mbps

**排查步骤：**
```bash
ethtool eth0
```

**可能原因：**
- 网线是百兆线（需要千兆网线）
- PHY自协商问题
- RGMII时序问题

**尝试手动设置：**
```bash
ethtool -s eth0 speed 1000 duplex full autoneg on
```

### 问题4：网络时断时续

**可能原因：**
- RGMII时序不匹配

**解决方法：**

尝试修改设备树中的 `phy-mode`：
```dts
# 尝试以下几种配置之一
phy-mode = "rgmii-id";    # MAC添加TX/RX延迟
phy-mode = "rgmii-rxid";  # MAC只添加RX延迟
phy-mode = "rgmii-txid";  # MAC只添加TX延迟
phy-mode = "rgmii";       # PHY处理延迟
```

---

## 配置检查清单

在编译前，确认以下项目：

- [ ] `system-user.dtsi` 已复制到正确位置
- [ ] 内核启用了 `Cadence MACB/GEM support`
- [ ] 内核启用了 `Micrel PHYs`
- [ ] Rootfs 添加了 `net-tools` 和 `ethtool`
- [ ] 项目已完整编译
- [ ] 镜像已打包

---

## 快速命令参考

```bash
# 配置内核
petalinux-config -c kernel

# 配置Rootfs
petalinux-config -c rootfs

# 编译
petalinux-build

# 打包
petalinux-package --boot --fsbl images/linux/zynqmp_fsbl.elf \
    --u-boot --pmufw --fpga images/linux/*.bit --force

# 验证网络（开发板上）
ifconfig -a
ethtool eth0
ping 192.168.1.100
```

---

*配置完成后，请参考 NETWORK_STREAMING_GUIDE.md 进行视频流传输测试。*
