# 虚拟机服务器固定 IP 操作指南

> 适用场景：机房环境监测系统 · 服务器跑在 VMware 虚拟机（Ubuntu 20.04）里，
> 队友需要从校园网访问服务器，但桥接网卡每次开关机 IP 都变。
> 本文记录「为什么变 → 怎么固定 → 涉及哪些知识 → 出问题怎么排查」。
>
> 最终结果：桥接网卡 ens33 固定为 **10.202.49.200**，NAT 网卡 ens37 保持 **192.168.48.130**。

---

## 一、背景与问题

服务器在 VMware 虚拟机里，通过**桥接模式**接入校园网，让队友能从自己的机器访问。

问题：桥接网卡走校园网 **DHCP**，每次开关机，分配到的 IP 都可能变。于是队友联调前总得先问一句「今天服务器 IP 是多少」，再改客户端配置，非常低效。

---

## 二、核心原理（为什么 IP 会变，怎么治）

### 2.1 IP 地址为什么需要「分配」

网络里每台设备要通信，必须有一个**在当前网络内唯一**的 IP 地址（类比：快递要有唯一门牌号）。这个地址要么**手动指定**（静态），要么**自动领取**（DHCP）。

### 2.2 DHCP 动态分配为什么会变

**DHCP**（Dynamic Host Configuration Protocol，动态主机配置协议）的工作流程：

```
客户端开机                        DHCP 服务器
    │  ── DISCOVER（广播：谁给我 IP？）──►
    │  ◄── OFFER（我有个 10.202.49.66 给你）──
    │  ── REQUEST（我就要这个）──►
    │  ◄── ACK（成交，租约 8 小时）──
```

关键在两点：

1. **租约（lease）有期限**。校园网 DHCP 租约通常几小时到一天，到期后客户端要重新申请；
2. **池（pool）是共享的**。校园网 DHCP 服务器手里有一大段地址（比如 `10.202.48.0/20` 整段），
   谁先申请就给谁。虚拟机关机期间，这个 IP 可能被分给了别的同学；下次开机再申请，就拿到另一个。

所以「每次开关机 IP 变」的根因是：**地址的所有权在 DHCP 服务器手里，虚拟机只是在「借」**。

### 2.3 静态 IP：把地址「买」下来

静态 IP 就是**不再问 DHCP 借**，而是直接告诉网卡：「你就用 10.202.49.200，别去找 DHCP」。

- 优点：地址永久固定，队友配置一次即可；
- 风险：如果这个地址恰好也在校园网 DHCP 的池子里，DHCP 服务器**不知道**你占用了它，
  可能把它分给别的设备 → **IP 冲突**（两台设备同一个 IP，通信互相干扰）。

> 降低冲突概率的办法：选一个「高位、不常被自动分配」的地址（DHCP 一般从低位开始发），
> 比如 `10.202.49.200` 这种，比 `10.202.49.10` 安全得多。学生一般没权限在校园网做
> DHCP 保留（Reservation），所以只能这样「赌一个不太会被用到的地址」，并保留 NAT 兜底。

### 2.4 NAT 与桥接：两个网卡 = 两个入口

这是理解整个方案的关键。虚拟机里有**两张网卡**，走两条完全不同的路：

| | 网卡 ens37（NAT） | 网卡 ens33（桥接） |
|---|---|---|
| 谁分配地址 | VMware 自带的虚拟 DHCP | 校园网 DHCP |
| 地址 | 192.168.48.130（固定） | 10.202.49.200（原来会变，现已固定） |
| 谁能访问 | **只有宿主机** | 宿主机 + **队友**（同校园网） |
| 出外网 | 走宿主机做地址转换 | 直接走校园网网关 |
| 作用 | 你自己的「后门」，永远稳定 | 给队友的「正门」 |

**NAT（网络地址转换）**：虚拟机发出去的包，被宿主机「翻译」成宿主机的地址再出去，
外面只看到宿主机，看不到虚拟机。所以 NAT 地址对队友不可见，但对宿主机自己永远固定。

**桥接（Bridged）**：虚拟机直接「插」到和宿主机同一张物理网卡的网络上，和宿主机平级，
从校园网 DHCP 拿一个真实地址，队友能直接 ping 到它。

> 方案的精髓：**两条路互相独立，互为兜底**。桥接静态 IP 万一冲突失效，你还能通过
> NAT 的 `192.168.48.130` ssh 进去改回来——不会「把自己锁在门外」。

### 2.5 子网掩码、网关、DNS 各管什么

配静态 IP 时，光有 IP 不够，还得配齐三样：

| 参数 | 本次的值 | 作用 |
|---|---|---|
| **子网掩码** `/20` | `255.255.240.0` | 决定「哪些 IP 和我在同一网段，可以直接通信」。`10.202.49.200/20` 表示本网段是 `10.202.48.0 ~ 10.202.63.255` |
| **网关** | `10.202.63.254` | 去**别的网段**时的「出口」。队友如果在别的子网，服务器回包要先发给网关 |
| **DNS** | `223.5.5.5`（阿里）/ `114.114.114.114` | 把域名翻译成 IP。服务器自己一般用不到，但配了能保证它自己能上网 |

### 2.6 路由表与 metric（为什么出外网还走 NAT）

两台网卡各有各的「默认路由」，Linux 用 **metric（度量值）** 决定走哪条，**数值越小优先级越高**：

```
default via 192.168.48.2  dev ens37  metric 50      ← NAT，metric 小，优先
default via 10.202.63.254 dev ens33  metric 40100   ← 桥接，metric 大，兜底
```

于是：出外网走 NAT（快、稳定），桥接只负责「校园网内可达」。两条默认路由各司其职，不打架。

---

## 三、所用知识与技术栈

### 3.1 网络分层与 IP 协议（OSI/TCP-IP）

IP 属于**网络层**（OSI 第 3 层），负责「把包从 A 送到 B」，靠 IP 地址寻址。
上面跑 TCP（传输层，负责可靠传输，本项目服务器监听 **TCP 9000** 端口），
下面靠以太网（数据链路层，用 **MAC 地址** 在同一网段内传输）。

一个包要跨网段出去，链路是：`本机 → 网关（数据链路层用网关的 MAC）→ ... → 目的地`。

### 3.2 VMware 虚拟网络

VMware 在宿主机上虚拟出几种「网络」：

- **桥接网络（VMnet0）**：虚拟机网卡绑定物理网卡，与宿主机同网络平级；
- **NAT 网络（VMnet8）**：虚拟出一段私有网段（`192.168.48.0/24`），内置虚拟网关
  （`192.168.48.2`）和虚拟 DHCP；
- **仅主机（VMnet1）**：只和宿主机互通，不能出外网（本项目没用）。

### 3.3 Linux 网络配置体系（netplan / NetworkManager / nmcli）

Ubuntu 20.04 的网络配置是「三层结构」：

```
netplan（.yaml 顶层配置）
   │  renderer: networkd 或 NetworkManager
   ▼
systemd-networkd  /  NetworkManager（实际干活的守护进程）
   │
   ▼
nmcli / ip / ifconfig（命令行工具，操作或查看）
```

- **netplan**：YAML 描述「每个网卡怎么配」，是 Ubuntu 新一代的配置入口；
- **NetworkManager**：桌面系统常用的网络管理服务，本项目虚拟机 renderer 就是它；
- **nmcli**：NetworkManager 的命令行工具，比 netplan 更「实时」，改完立即生效。

本次为什么用 **nmcli** 而不是改 netplan yaml：
因为这台虚拟机 renderer 是 `NetworkManager`，网卡实际由 NetworkManager 管理，
直接用 `nmcli` 改最贴合现状、不会引入 renderer 切换的副作用，且**改动会持久化到**
`/etc/NetworkManager/system-connections/`，重启后仍生效。

### 3.4 涉及的 Linux 命令

| 命令 | 用途 |
|---|---|
| `ip -br addr` | 看所有网卡的地址 |
| `ip route show default` | 看默认路由（含 metric） |
| `nmcli connection show` | 列出 NetworkManager 的连接 |
| `nmcli connection modify <conn> ...` | 修改连接配置 |
| `nmcli connection up <conn>` | 重新激活连接 |
| `ping` / `ss -ltn` | 连通性 / 端口监听检查 |
| `ssh` | 远程登录（走 NAT 或桥接） |

---

## 四、详细操作步骤（可复现）

> 下面命令在**虚拟机里**执行（先 `ssh vmserver` 进去）。
> `sudo` 需要管理员密码；`ssh vmserver` 的 NAT 地址 192.168.48.130 是固定兜底，全程不依赖桥接。

### 第 0 步：确认当前网络状态

```bash
ip -br addr                      # 看 ens33 / ens37 当前地址
ip route show default            # 看默认路由和 metric
nmcli -t -f NAME,DEVICE,UUID connection show   # 拿到 ens33 对应的「连接」和 UUID
```

### 第 1 步：选一个空闲 IP（降低冲突概率）

在宿主机（或同网段任何机器）ping 候选地址，无响应即可用：

```bash
ping -n 1 -w 800 10.202.49.200   # 无响应 = 大概率空闲
```

### 第 2 步：备份当前连接配置（出问题可回滚）

```bash
CONN_UUID=$(nmcli -t -f UUID,DEVICE connection show | grep ":ens33" | cut -d: -f1)
nmcli connection show "$CONN_UUID" > /tmp/ens33_conn_backup.txt
```

### 第 3 步：改为静态 IP

```bash
sudo nmcli connection modify "$CONN_UUID" \
  ipv4.method manual \
  ipv4.addresses 10.202.49.200/20 \
  ipv4.gateway 10.202.63.254 \
  ipv4.route-metric 20100 \
  ipv4.dns "223.5.5.5 114.114.114.114"
```

字段说明：
- `ipv4.method manual`：手动（静态），不再走 DHCP；
- `ipv4.addresses`：IP + 子网掩码（`/20`）；
- `ipv4.gateway`：网关；
- `ipv4.route-metric`：这条默认路由的 metric（保持比 NAT 的 50 大，出外网仍走 NAT）；
- `ipv4.dns`：DNS 服务器，空格分隔多个。

### 第 4 步：重新激活连接并验证

```bash
sudo nmcli connection up "$CONN_UUID"   # 重新激活，新地址立即生效
ip -br addr show ens33                   # 应显示 10.202.49.200/20
ip route show default                    # 两条默认路由，NAT metric 50 优先
```

### 第 5 步：从宿主机/队友侧验证

```bash
# 宿主机上：
ping 10.202.49.200                       # 应通
ssh vmserver-lan                          # 若 ~/.ssh/config 已配，走桥接也应通
# 队友侧：客户端连 10.202.49.200:9000
```

### 第 6 步：确认持久化（重启后仍生效）

```bash
nmcli connection show "$CONN_UUID" | grep ipv4
# 应看到 ipv4.method: manual / ipv4.addresses: 10.202.49.200/20
```

`nmcli modify` 写的是持久配置（存在 `/etc/NetworkManager/system-connections/`），
**重启后 NetworkManager 会自动用这份配置**，无需再手动设置。

---

## 五、备选方案：用 netplan 配（另一种方式）

如果哪天想统一走 netplan（YAML 管理），把 `/etc/netplan/*.yaml` 改成下面这样：

```yaml
network:
  version: 2
  renderer: NetworkManager   # 保持现状；如改成 networkd 需确保 systemd-networkd 已启用
  ethernets:
    ens33:
      dhcp4: no
      addresses:
        - 10.202.49.200/20
      routes:
        - to: default
          via: 10.202.63.254
          metric: 20100
      nameservers:
        addresses: [223.5.5.5, 114.114.114.114]
    ens37:
      dhcp4: yes             # NAT 保持 DHCP 不动
```

然后 `sudo netplan apply`。
> 注意：这台机器 renderer 是 NetworkManager，若 netplan 里同时定义了 ethernets，
> 会生成对应的 NetworkManager 连接，可能与已有连接重名。所以**当前环境优先用 nmcli**，
> netplan 方式留作「统一管理」时的备选。

---

## 六、回滚（把静态 IP 改回 DHCP）

```bash
CONN_UUID=$(nmcli -t -f UUID,DEVICE connection show | grep ":ens33" | cut -d: -f1)
sudo nmcli connection modify "$CONN_UUID" ipv4.method auto   # 恢复自动(DHCP)
sudo nmcli connection up "$CONN_UUID"
# 或：nmcli connection reload 后删掉手动加的连接，用备份文件对照恢复
```

---

## 七、常见问题排查

| 现象 | 原因 | 处理 |
|---|---|---|
| 队友连不上 10.202.49.200 | IP 冲突（被别的设备占了） | 宿主机 `ping 10.202.49.200`，若被占走 `ssh vmserver`（NAT）换地址 |
| `ssh vmserver` 也不通 | 虚拟机没开机 / NAT 网络异常 | `vmrun list` 看虚拟机状态；`ssh` 前先确认 NAT 地址 |
| 配完 ens33 起不来 | 网关/掩码写错 | 对照第 2 步备份文件逐项核对；走 NAT 进去改 |
| 出外网变慢/不通 | metric 设错，默认路由被桥接抢走 | 确认 `ip route` 里 NAT（metric 50）仍是最小 |
| 重启后 IP 又变回 DHCP | 改的是临时配置没持久化 | 确认用 `nmcli connection modify`（不是 `ip addr add`），且 `nmcli connection show` 里是 manual |

---

## 八、本次实际执行记录（真实命令与输出）

```
# 探测空闲地址（宿主机）
$ ping -n 1 -w 800 10.202.49.200   → 无响应（空闲）

# 改配置（虚拟机内，ssh vmserver）
$ CONN_UUID=$(nmcli -t -f UUID,DEVICE connection show | grep ":ens33" | cut -d: -f1)
$ echo $CONN_UUID
  cc2c8574-3857-477c-b9ee-aa8ecb56005b
$ sudo nmcli connection modify "$CONN_UUID" ipv4.method manual \
    ipv4.addresses 10.202.49.200/20 ipv4.gateway 10.202.63.254 \
    ipv4.route-metric 20100 ipv4.dns "223.5.5.5 114.114.114.114"
  修改OK
$ sudo nmcli connection up "$CONN_UUID"
  连接已成功激活

# 验证
$ ip -br addr show ens33
  ens33  10.202.49.200/20 ...
$ ip route show default
  default via 192.168.48.2  dev ens37 proto dhcp   metric 50
  default via 10.202.63.254 dev ens33 proto static metric 40100
$ ip -br addr show ens37
  ens37  192.168.48.130/24 ...       ← NAT 未动

# 宿主机验证
$ (cat < /dev/null > /dev/tcp/10.202.49.200/9000) → 可达
$ ssh vmserver-lan → LAN_SSH_OK       ← 桥接入口通
```

---

## 九、一句话总结

- **变的原因**：桥接网卡用 DHCP「借」地址，租约到期/被抢，下次开机就换一个；
- **治的办法**：桥接网卡改成静态 IP「买断」`10.202.49.200`，并保留 NAT 的固定地址做后门；
- **兜底**：NAT（192.168.48.130）永远能 ssh 进去，任何配置失误都能救回来。
