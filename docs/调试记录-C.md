# 任务C 调试记录（服务器端）

> 记录格式：**现象 → 定位 → 原因 → 解决**
> 记录人：任务C执行 Agent　起始日期：2026年09月21日
> 对应规划：`docs/任务C-执行规划.md`　接口依据：`docs/接口规范.md`（未做任何字段改动）

---

## 环境基线（S0 完成时）

| 项 | 值 |
|---|---|
| 宿主机 | Windows 11 家庭中文版，WLAN `10.202.48.182/20` |
| 虚拟机软件 | VMware Workstation 17（`D:\Virtual Machine`） |
| 客户机 | Ubuntu 20.04.6 LTS，16 vCPU / 4 GB，用户名 `lilinglong` |
| 客户机网络 | `ens33` 桥接 `10.202.57.4/20`（校园网，给队友联调用）；`ens37` NAT `192.168.48.130/24`（走宿主机网络，专供 apt 等外网访问） |
| 工具链 | gcc 9.4.0、gdb、make、MySQL 8.0.42、libmysqlclient-dev、openssh-server |

> **说明**：任务书 C1.1 写的是 Ubuntu 22.04，本机已有现成的 Ubuntu 20.04.6 虚拟机，经确认后沿用。
> 20.04 上 MySQL 同为 8.0、gcc 9.4，`init.sql` 与全部源码均不受影响；如需严格对齐 22.04，可后续 `do-release-upgrade`。

---

## 一、S0 阶段问题记录

### 问题 1：VMware 客户机输入通道全部失效（最关键）

**现象**
自动化工具无法向 Ubuntu 客户机发送任何键盘/鼠标输入：
- `System.Windows.Forms.SendKeys` 发送的按键客户机无反应；
- `mouse_event` / `SendInput`（含 `MOUSEEVENTF_ABSOLUTE|VIRTUALDESK`）无法移动客户机指针；
- 向 VMware 控制台窗口 `PostMessage` 的 `WM_KEYDOWN` **只有裸键（Esc/Enter）偶尔生效**，带修饰键的组合键（Ctrl+Alt+T、Alt+F2、Ctrl+Alt+F3）全部无效。

**定位**
1. 确认窗口焦点正常：`GetForegroundWindow()` 返回的正是 `Ubuntu - VMware Workstation`；
2. 确认非 UIPI 拦截：读取 `vmware.exe` 进程令牌，`TokenElevation = 0`（未提权），同级进程不会被 UIPI 挡；
3. 确认坐标非错位：宿主机为 150% DPI 缩放（物理 2560×1600，逻辑 1695×1087），改用 `SetProcessDPIAware()` 后坐标一致，问题依旧；
4. 检索确认这是 VMware Workstation 的已知行为 —— 官方论坛与 AutoHotkey/AutoIt 社区多篇帖子均指出 **VMware 不接收宿主侧模拟输入**（`ControlSend`/`ControlClick`/`SendInput` 均无效），且 Workstation 17 的 `vmcli MKS sendKeyEvent` 也是静默无效。

**原因**
VMware Workstation 出于防作弊/防自动化考虑，不把宿主进程注入的合成输入转发给客户机；只有**真实硬件输入**或**独立于 Windows 输入队列的通道**（VNC/RFB）才能送达。

**解决**
启用 VMware 内置 VNC 控制台（已事先取得用户明确授权），用标准 RFB 协议（RFC 6143）自建客户端 `vmvnc.py` 直连，实现按键/鼠标注入与画面抓取：

```
# Ubuntu.vmx 增加（S0 完成后已移除）
RemoteDisplay.vnc.enabled = "TRUE"
RemoteDisplay.vnc.port    = "5901"
RemoteDisplay.vnc.password = "********"
```

自此获得稳定通道：`python vmvnc.py key "ctrl+alt+t"` / `type "..."` / `shot out.png`。

> 注：该通道仅用于"破门"（取得第一条可用的客户机 shell）。S0 收尾时已把这三行配置从 `Ubuntu.vmx` 删除并重启虚拟机验证，`5901` 端口不再监听；此后全部操作走 SSH。若后续 SSH 因网络故障不可达，把这三行加回 vmx 重启一次即可恢复该应急通道。

---

### 问题 2：`vmrun runProgramInGuest` 报 `VIX_E_FILE_NOT_FOUND`

**现象**
```bash
vmrun -gu lilinglong -gp ******** runProgramInGuest Ubuntu.vmx /bin/echo hello
# Error: 未找到文件
```
连 `/bin/echo`、`/usr/bin/id` 这样必然存在的程序都报"文件不存在"；`createDirectoryInGuest /tmp/vmtest` 报"没有访问权限"；而 `directoryExistsInGuest /tmp` 返回"目录不存在"（`/tmp` 必然存在）。

**定位**
`vmware.log` 中可见：
```
Vix: [guestCommands.c:321]: Error VIX_E_FILE_NOT_FOUND in VMAutomationOnRunProgramStarted(): 未找到文件
```
排除了"用户未交互登录""凭据错误"（该两类错误在修复桌面会话后已消失，错误码变为 FILE_NOT_FOUND）。同一时刻 VMware Tools 的**分辨率 RPC 是正常工作的**（日志有 `Tools_SetGuestResolution: Sending rpcMsg`），说明 tools 主体存活。

**原因**
该客户机的 open-vm-tools 未提供 VIX 执行程序所需的 guest program runner / 文件传输插件（版本或打包裁剪所致），导致仅"取客户机信息/设分辨率"类 RPC 可用，而"执行程序/传文件"类 RPC 缺失。

**解决**
不使用该通道，改用问题 1 的 VNC 通道 + SSH（`openssh-server` 本就属于 C1.2 必装项）。

---

### 问题 3：客户机画面周期性全黑

**现象**
截图时不时整屏全黑，键盘输入也唤不醒。用户补充："虚拟机过一会不点后会黑屏，需要点一下唤醒"。

**定位**
黑屏时 `vmrun getGuestIPAddress` 仍能返回，说明客户机未死；对照时间点，黑屏均出现在闲置数分钟后。

**原因**
GNOME 空闲熄屏/锁屏（DPMS）。

**解决**
1. 通过 VNC 注入按键/点击唤醒；
2. 在客户机内关闭自动锁屏（同时避免自动化打字被锁屏密码框截获）：
```bash
gsettings set org.gnome.desktop.screensaver lock-enabled false
gsettings set org.gnome.desktop.session idle-delay 0
```

---

### 问题 4：客户机全部网卡 `unmanaged`、开机不配网（**无网的根因**）

**现象**
```bash
$ nmcli -t device status
ens33:ethernet:unmanaged:
lo:loopback:unmanaged:
$ ip -br link
ens33  DOWN  00:0c:29:2d:52:01 <BROADCAST,MULTICAST>
```
网卡处于 DOWN 且不受 NetworkManager 管理，**连 `lo` 都是 unmanaged**；`nmcli device set ens33 managed yes` 无效；`/etc/network/interfaces` 不存在，netplan 只有默认的 `01-network-manager-all.yaml`，`systemctl is-active NetworkManager` 却是 active。

**定位**
`lo` 也是 unmanaged 是决定性线索 —— 单个网卡异常不会波及 loopback，只有"NetworkManager 被整体停用"才会如此。执行：
```bash
$ nmcli networking
disabled
```

**原因**
NetworkManager 的全局开关处于 `disabled`（`/var/lib/NetworkManager/NetworkManager.state` 中的 `NetworkingEnabled=false`，该状态**跨重启保持**）。此状态下所有设备一律 unmanaged，开机也不会拉 DHCP。

**解决**
```bash
sudo nmcli networking on      # 开启后 ens33 立即变为 connected
```
后续将两块网卡都交给 NM 自动连接（见问题 6）。

---

### 问题 5：桥接模式下拿不到 IPv4 地址

**现象**
把 `ethernet0.connectionType` 由 `nat` 改为 `bridged`（任务书 C1.1 要求）后，虚拟机始终无 IPv4 地址；`vmrun getGuestIPAddress` 报 `Unable to get the IP address`，宿主机 ARP 表里也找不到虚拟机 MAC。

**定位**
1. 先用 IPv6 链路本地地址验证二层是否真的通：
```powershell
ping -6 "fe80::20c:29ff:fe2d:5201%8"     # WLAN 接口索引 8
# 3/3 回复，<1ms
```
→ **二层桥接是通的**，宿主机与虚拟机在同一广播域。
2. 再查客户机侧：`ip -br link` 显示 `ens33` 为 `DOWN`，且 NM 完全不管理它。

**原因**
不是桥接本身的问题，而是问题 4（NM 全局关闭）导致网卡没被拉起、没发 DHCP 请求。

**解决**
```bash
sudo ip link set ens33 up
sudo dhclient ens33        # 立即拿到 10.202.56.223/20
sudo nmcli networking on   # 让 NM 接管，开机自动 DHCP
```

---

### 问题 6：客户机访问不了外网，`apt` 全部失败

**现象**
```bash
$ sudo apt-get update
错误:4 http://security.ubuntu.com/ubuntu focal-security InRelease
  明文签署文件不可用，结果为'NOSPLIT'（您的网络需要认证吗？）
E: 仓库 "http://cn.archive.ubuntu.com/ubuntu focal InRelease" 的签名不再生效。
```
同时 `ping 223.5.5.5` 100% 丢包。

**定位**
- 客户机有默认路由（`default via 10.202.63.254 dev ens33`），DNS 也能解析（能拿到 `archive.ubuntu.com` 的 AAAA 记录）；
- 但 HTTP 响应被网关替换成认证页 —— 客户机浏览器访问任意 http 站点会自动跳到 `http://10.254.100.106/a79.htm`（第一师范校园网认证门户）。

**原因**
校园网按 **MAC 认证**：宿主机 MAC 已认证可上网，虚拟机的 MAC（`00:0c:29:2d:52:01`）未认证，网关拦截其外网流量。

**解决（重要决策）**
用户确认校园网账号**同时只允许一台设备在线**，若在虚拟机内认证会将**宿主机踢下线**（而宿主机的网络正是执行 Agent 工作所依赖的），因此**放弃在虚拟机内做校园网认证**，改用**双网卡**方案：

| 网卡 | 模式 | 地址 | 用途 |
|---|---|---|---|
| `ens33` | 桥接 | `10.202.57.4/20` | 满足 C1.1 "桥接模式"，队友可从校园网直连虚拟机的 9000 端口 |
| `ens37` | NAT | `192.168.48.130/24` | 走宿主机已认证的网络，专供 `apt` 等外网访问 |

通过 VMX 增加第二块网卡：
```
ethernet1.present        = "TRUE"
ethernet1.connectionType = "nat"
ethernet1.addressType    = "generated"
```

---

### 问题 7：双网卡默认路由冲突（外网会被校园网路由抢走）

**现象**
两块网卡都起来后存在两条默认路由，若校园网那条优先级更高，外网流量会走被拦截的校园网网关，`apt` 依然失败（浏览器还会反复弹出认证页）。

**定位**
```bash
$ ip route
default via 10.202.63.254 dev ens33 proto dhcp metric 20101
```
校园网 DHCP 下发的 metric 为 20101；NAT 侧若不给更优 metric，两者会竞争。

**原因**
NetworkManager 对每条连接独立赋 metric，未显式指定时由 NM 自动分配，不可控。

**解决**
为 NAT 连接显式指定更优（更小）的 metric，使外网固定走 NAT；校园网仍保留其链路路由（`10.202.48.0/20 dev ens33`），队友访问不受影响：
```bash
sudo nmcli con add type ethernet ifname ens37 con-name nat \
     ipv4.method auto ipv4.route-metric 50
sudo nmcli con up nat
```
结果：
```bash
$ ip route
default via 192.168.48.2 dev ens37 proto dhcp metric 50      ← 外网走这里
default via 10.202.63.254 dev ens33 proto dhcp metric 20101
10.202.48.0/20 dev ens33 proto kernel scope link src 10.202.57.4 metric 101
192.168.48.0/24 dev ens37 proto kernel scope link src 192.168.48.130 metric 50
```
`ping 223.5.5.5` 由 100% 丢包变为 0% 丢包，`apt-get update` 返回 0。

---

### 问题 8：`sudo -S` 的密码被输入重定向覆盖

**现象**
```bash
echo <密码> | sudo -S mysql < sql/init.sql
# [sudo] lilinglong 的密码： 对不起，请重试。 ×3
# sudo: 3 次错误密码尝试
```

**定位**
`sudo -S` 从**标准输入**读密码，而同一命令行里的 `< sql/init.sql` 把标准输入改成了 SQL 文件，两者冲突，sudo 实际拿到的是 SQL 文件的首行。

**原因**
shell 重定向的优先级高于管道，`<` 覆盖了 `|`。

**解决**
把重定向挪到内层 shell 里执行：
```bash
echo <密码> | sudo -S bash -c 'mysql < /home/lilinglong/server-linux/sql/init.sql'
```

---

### 问题 9：虚拟机 ping 不通宿主机（S0-1 方向二）

**现象**
宿主机能 ping 通虚拟机，反向 `ping 10.202.48.182` 100% 丢包。

**定位**
- 虚拟机侧路由、防火墙均正常（`ufw` 未启用），`ip route` 有到 `10.202.48.0/20` 的直连路由；
- 该包的目标是**宿主机自身**，入站方向由宿主机 Windows 防火墙裁决。

**原因**
Windows 防火墙三个配置文件（域/专用/公用）均为启用状态，默认**拦截 ICMPv4 入站回显请求**，与虚拟机无关。

**解决**
在宿主机以管理员身份放行 ICMPv4 入站：
```
netsh advfirewall firewall add rule name="Allow ICMPv4 In" protocol=icmpv4:8,any dir=in action=allow
```
放行后反向 ping 3/3 通过（0.95~1.10 ms）。

---

### 问题 10：`timeout` 掩盖了程序的真实退出码

**现象**
S0-6 用 `timeout -s INT 3 ./server` 验证 Ctrl-C 退出，程序日志显示"收到退出信号，服务器正常退出"，但退出码是 **124**。

**定位**
`timeout` 在计时到期并发送信号后，无论被调用程序如何退出，默认都返回 124。

**原因**
`timeout` 的默认行为不是透传被调用程序的退出码。

**解决**
加 `--preserve-status` 透传真实退出码：
```bash
timeout --preserve-status -s INT 3 ./server     # 退出码 0
```

---

## 二、工具链问题记录（宿主机侧，不影响交付物）

### 问题 11：PowerShell 脚本报"缺少 using 指令"

**现象**
`powershell -File xxx.ps1` 解析报错：`'+' 运算符后缺少表达式`、`缺少 using 指令`，而同一段 C# 内联代码用 `-Command` 传却正常。

**定位**
对比文件字节：脚本由本机工具写入时为 **LF** 换行。

**原因**
Windows PowerShell 5.1 的 here-string（`@'...'@`）终止符**要求 CRLF**，LF 文件下 here-string 不被识别，C# 代码被当成 PowerShell 代码执行。

**解决**
写出脚本后统一转换：`sed -i 's/\r*$/\r/' xxx.ps1`。

---

### 问题 12：含中文注释的 .ps1 解析错乱

**现象**
某个带中文注释的 PowerShell 脚本报 `[System.Object[]] 不包含名为 op_Addition 的方法` 等与代码无关的语法错误。

**定位**
该文件是**无 BOM 的 UTF-8**，而 Windows PowerShell 5.1 对无 BOM 文件按**系统 ANSI（中文为 GBK）**解码。

**原因**
中文注释的 UTF-8 字节被按 GBK 解码后产生乱码字符，破坏了后续代码的词法结构。

**解决**
工具脚本一律只写 ASCII 注释（或保存为 UTF-8 with BOM）。

---

### 问题 13：150% DPI 缩放下截图与坐标不一致

**现象**
脚本按截图量出的坐标点击，落点整体偏移。

**定位**
```powershell
DPI-unaware 矩形: L=-7 T=-7 R=1688 B=1080
DPI-aware  矩形: L=-11 T=-11 R=2531 B=1619
```
宿主机为 150% 缩放（物理 2560×1600 / 逻辑 1695×1087）。

**原因**
进程未声明 DPI 感知时，Windows 对窗口坐标做虚拟化缩放，与屏幕像素不是 1:1。

**解决**
脚本开头先调 `SetProcessDPIAware()`，之后所有坐标一律按物理像素计算。

---

### 问题 14：VMware 内置 VNC 下画面全黑

**现象**
启用内置 VNC 后能握手、能认证、能拿到 `FramebufferUpdate`，但画面全黑（640×480 与 2318×1440 都一样）。

**定位**
VMX 中 `mks.enable3d = "TRUE"`；客户机为 GNOME 桌面，使用 3D 加速渲染。

**原因**
VMware 的内置 VNC 控制台不抓取 3D 加速后的画面。

**解决**
VMX 改为 `mks.enable3d = "FALSE"`（重启生效）。GNOME 退回软件渲染（llvmpipe），终端与桌面显示正常。该改动对服务器开发无影响。

---

## 三、S0 验收结论

| # | 验收项 | 结果 |
|---|---|---|
| S0-1 | 宿主机 ↔ 虚拟机双向 ping | ✅ 宿主机→虚拟机 3/3（`10.202.57.4`，<1ms）；虚拟机→宿主机 3/3（`10.202.48.182`，0.95~1.10ms） |
| S0-2 | `gcc --version` / `systemctl status mysql` | ✅ gcc 9.4.0；mysql active (running) |
| S0-3 | `mysql -uiot -piot123456 machine_room -e "SHOW TABLES;"` | ✅ 4 张表 |
| S0-4 | `mysql -uiot -piot123456 machine_room -e "SELECT * FROM threshold_cfg;"` | ✅ 3 行（温度/湿度/烟雾） |
| S0-5 | `cd server-linux && make clean && make` | ✅ 零警告零错误，产出 `server` |
| S0-6 | `./server` 后 Ctrl-C | ✅ 打印启动日志，收到 SIGINT 后正常退出，退出码 0 |

---

## 四、环境持久性验证（S0 收尾）

上面所有手工修复如果只存在于运行时内存里，重启就前功尽弃。因此收尾时做了一次**冷重启验证**：

**操作**
1. 把 `RemoteDisplay.vnc.*` 三行从 `Ubuntu.vmx` 删除（回收临时通道）；
2. `sudo systemctl reboot` 重启客户机；
3. 不做任何手工干预，等它自己起来后复查。

**结果（全部自动恢复，无需人工干预）**

```
运行时间: up 2 min
--- 网卡 ---
ens33            UNKNOWN        10.202.57.4/20        ← 桥接，自动 DHCP
ens37            UNKNOWN        192.168.48.130/24     ← NAT，自动 DHCP
--- 默认路由 ---
default via 192.168.48.2 dev ens37 proto dhcp metric 50     ← 外网走 NAT
default via 10.202.63.254 dev ens33 proto dhcp metric 20100
--- 服务 ---
mysql: active   ssh: active
--- 外网 ---
2 个包，0% 包丢失
--- VNC 端口 ---
5901 未监听
```

**关键配置落点**（换台机器重现时照此检查）
| 配置 | 存放位置 |
|---|---|
| NM 全局开关 enabled | `/var/lib/NetworkManager/NetworkManager.state` |
| `nat` 连接（ens37，metric 50） | `/etc/NetworkManager/system-connections/nat.nmconnection` |
| `配置 1` 连接（ens33，自动 DHCP） | `/etc/NetworkManager/system-connections/配置 1.nmconnection` |
| mysql / ssh 开机自启 | `systemctl is-enabled mysql ssh` → enabled |

重启后再次复跑 S0-3~S0-6 仍全部通过。

---

## 五、S1 阶段问题记录（select 回显骨架）

### 问题 15：执行 S1 前虚拟机 SSH 完全不通，而 ping 却"看起来是通的"

**现象**
```bash
$ ssh vmserver "uname -a"          # 配置指向 NAT 192.168.48.130
ssh: connect to host 192.168.48.130 port 22: Connection timed out

$ ssh -v vmserver-lan "echo OK"    # 桥接 10.202.57.4，卡在 Connecting 后被杀
debug1: Connecting to 10.202.57.4 [10.202.57.4] port 22
（timeout 退出码 124）

$ ping -n 2 10.202.57.4            # ← 迷惑点
已发送 = 2，已接收 = 2，丢失 = 0 (0% 丢失)
```

**定位**
1. 先怀疑网络配置回退（桥接失效 / NM 又变 unmanaged），但 S0 收尾时做过冷重启验证，配置是持久的；
2. 查虚拟化进程：`tasklist | grep -i vmware` 只有 `vmware-authd.exe`（后台服务），**没有 `vmware-vmx.exe`**（虚拟机进程）；
3. 决定性一步：`vmrun list` → `Total running VMs: 0`；
4. 回头细看 ping 的**正文**而不是统计行：真正的回复内容是"无法访问目标主机"，那是本机路由返回的 ICMP 不可达，并非虚拟机应答——统计口径（"0% 丢失"）在这里会骗人。

**原因**
虚拟机根本没开机。S0 收尾做冷重启验证后就关机了，本次 S1 开始时它处于关机状态；Windows 的 `ping` 在收到"目标主机不可达"时仍按"收到回复"计数，导致 `0% 丢失` 的假象。

**解决**
```bash
"D:/Virtual Machine/vmrun.exe" start "D:/VMs/Ubuntu/Ubuntu.vmx" nogui
```
随后轮询 SSH，约 90 秒后 `ssh vmserver-lan` 恢复正常（`up 0 min`，两块网卡 IP 与 S0 一致）。经验：**判断 Linux 机器是否在线，以端口连通（ssh）为准，不要只信 ping 的统计行**。

---

### 问题 16：S1-4 的验收命令照字面执行会得到错误结论（假失败）

**现象**
规划 S1-4 写的操作是 `printf 'hel' | nc <IP> 9000` 与 `printf 'lo\n' | nc <IP> 9000`。若把它理解成"先后跑两条命令"，第二条发完 `lo\n` 后收不到任何回显，看起来像"半行留存没实现"。

**定位**
两条命令各自启动一个 `nc`，建立的是**两条互不相干的 TCP 连接**：`hel` 留在连接 A 的会话接收缓冲里，A 一断开该会话即被回收；`lo\n` 走的是连接 B，B 的缓冲里从来没有过 `hel`，自然拼不出 `hello`。

**原因**
分帧缓冲是**每连接独立**的（`session_t` 按 fd 分槽），S1-4 要验证的是"同一条连接内、跨 TCP 段的半行留存"，跨连接不可能拼接。规划原文的括号里其实给了正确做法（"或 python 分两次写同一行"），字面照抄前半句则跑偏。

**解决**
在**同一个 socket** 上分两次写：先 `sendall(b'hel')`，`sleep 1`，再 `sendall(b'lo\n')`，服务器正确回显 `hello`（见 S1-4 验收记录）。后续复验 S1-4 一律按此方式，避免误判为失败。

---

### 问题 17：宿主机没有 nc，缺手测工具

**现象**
规划的验收标准通篇以 `nc <IP> 9000` 作为手测手段，但宿主机（Windows 11）执行 `which nc ncat` 无输出；`C:\Windows\System32\OpenSSH\` 下只有 ssh/scp/sftp 客户端，没有 netcat。

**定位**
虚拟机内虽有 `/usr/bin/nc`（OpenBSD netcat 1.206），但在虚拟机内自连（127.0.0.1）只能验证服务器进程本身，**验证不了"宿主机 → 虚拟机 9000"这条跨机链路**（而这正是 S1-1 的要求，也是队友联调时要走的路径）。

**原因**
Windows 不自带 netcat（S0 阶段也没装）。

**解决**
写了一个 40 行的 Python 最小 TCP 客户端（连接 → 发送 → 打印收到的原始字节），等价于 `nc` 的用途，**放在仓库外的临时目录，不作为交付物**：
```bash
python tcp_cli.py 10.202.57.4 9000 'hello\n'            # 直连虚拟机桥接地址
python tcp_cli.py 10.202.57.4 9000 'hello\n' --split 1  # 同一连接分两段写（S1-4）
```
S1-1/S1-4/S1-5/S1-6 由宿主机侧执行，既补上了工具缺口，也顺带证明了桥接网络的跨机可达性；S1-2/S1-3（多连接）由虚拟机内两个并发 `nc` / Python 完成。

---

## 六、S1 验收结论

**产出**：`src/net.c/h`、`src/session.c/h`、`src/main.c`（接入网络层）——共 649 行，`make clean && make` 零警告零错误。

| # | 验收项 | 结果 |
|---|---|---|
| S1-1 | 宿主机连虚拟机 9000 发 `hello`（跨机，桥接 `10.202.57.4`） | ✅ 收到 `b'hello\n'` |
| S1-2 | 两个终端同时连、各发各的 | ✅ 两个会话（fd=4 / fd=5）互不干扰，各收各的回显 |
| S1-3 | 一个终端断开 | ✅ 服务器打印"客户端 fd=5 主动断开…剩余会话 1 个"，另一连接继续正常回显，进程存活 |
| S1-4 | 同一条连接分两段写 `hel` + `lo\n` | ✅ 只产生 1 条完整消息，回显 `hello`（半行留存） |
| S1-5 | 一次发 `a\nb\nc\n` | ✅ 切出 3 条独立消息（日志 3 行"收到完整消息 1 字节：a/b/c"），回显 `b'a\nb\nc\n'` |
| S1-6 | 发 >4KB 无换行的数据，再接正常行 | ✅ 触发 2 次"单行超过 4096 字节…丢弃该行"告警；之后 `ok-after-oversize`、`ok-after-split-oversize`、`still-alive` 全部正常回显，进程无崩溃、日志 0 条 ERROR |

**附加验证（S1 任务内容第 4 条：超时巡检只告警不断开）**

| 检查 | 结果 |
|---|---|
| 连接静置 95s（>90s 阈值） | ✅ 12:27:25 打印「会话超时：fd=4 已 90s 无任何消息（阈值 90s）——S1 仅告警，S3 起断开回收」；12:27:30 该连接再发消息仍被正常回显 → 只告警不断开，符合 S1 约定 |
| Ctrl-C 退出（S0-6 回归） | ✅ `kill -INT` 后退出码 0，日志依次打印"连接已关闭 fd=4"→"select 主循环已退出"→"服务器正常退出"，端口 9000 释放（监听数 0） |
| 退出时连接仍打开 | ✅ 服务器主动回收该连接（`net_shutdown_all` 路径），不依赖对端先断开 |

---

## 七、S1 实现要点（供 S2/S3 参考，避免踩坑）

1. **`session_next_line` 的返回值语义**：`>= 0` = 取到一条消息（长度可为 0，表示空行）；`-1` = 当前无完整行或 fd 无效。调用方必须用 `>= 0` 判断"取到了"，用 `0` 判断"空行"会漏掉空行导致死循环。
2. **超长行丢弃后要"吞到下一个 `\n`"**：进入 `discarding` 态后，字节一律丢弃，直到遇到 `\n` 才恢复正常分帧。也就是说，超长行**必须由发送方补一个 `\n` 收尾**，否则紧随其后的正常消息会连同这一行一起被吞掉（这是按规划"丢弃该缓冲并标记"的语义实现的）。
3. **超时告警做了去重**：`session_sweep_timeouts` 每秒被调用一次，若不去重会每秒刷一条日志。每个会话用 `timeout_logged` 标记"本段空闲已报过警"，`session_touch` 时清除（重新活跃后可再次告警）。
4. **`net_close` 里先 `session_remove` 再 `close`**：S1 单线程下两种顺序都安全，但 S3 起 `net_close` 会被工作线程调用（踢旧连接），必须先摘除会话再关 fd——否则新连接可能立刻复用同一 fd 号，而旧会话槽位还占着它，导致误踢新连接。S3 实现踢旧逻辑时务必保持此顺序。
5. **`net_on_message` 是 S2 的唯一替换点**：S1 的 echo 全在这一个静态函数里，S2 只需把它改成"深拷贝消息 → `pool_submit`"，`net_loop` / 分帧 / 会话层都不用动。
6. **S1 未加锁**：`session.c` 全程跑在 select 主循环单线程里，故未加互斥锁；S2 引入线程池后需按规划补会话表锁与 `net_send` 发送锁。

---

## 八、S2 阶段问题记录（线程池 + JSON 解析分发）

### 问题 18：`pkill -f 'server-linux/server'` 把承载它的远程 shell 一起杀掉

**现象**
```bash
$ ssh vmserver-lan "pkill -f 'server-linux/server'; cd ~/server-linux && setsid ./server ... "
# 退出码 255，服务器没起来，/tmp/s2.log 也不存在
```

**定位**
单独复跑 `pgrep -a -f server-linux/server`：
```
4389 bash -c pgrep -a -f server-linux/server; echo '--- listen ---'; ...
```
输出里出现了**这条命令自己的 bash 进程** —— 说明 `-f` 匹配的是整条命令行，而远端命令串里正含着 `server-linux/server` 这个模式。

**原因**
`pkill -f` / `pgrep -f` 匹配整条命令行而非进程名。远端 shell 的命令行本身包含该模式，于是 pkill 杀掉了自己（连同承载它的 ssh 会话），`ssh` 因对端进程被信号终止而返回 255。

**解决**
只按进程名匹配，不带 `-f`：
```bash
pkill -x server      # 进程名精确等于 server，不会误伤 bash
```
（或维护 pid 文件 `kill $(cat /tmp/s2.pid)`。）此后巡检一律用 `pgrep -ax server`。

---

### 问题 19：宿主机 `ssh vmserver`（NAT 192.168.48.130）连不上

**现象**
```bash
$ ssh vmserver "ls /tmp/cJSON.c"
ssh: connect to host 192.168.48.130 port 22: Connection timed out

$ ssh vmserver-lan "ip -br addr"      # 同一条虚拟机，桥接地址却正常
ens37   UNKNOWN   192.168.48.130/24   ← NAT 地址确实存在
```

**定位**
虚拟机侧 NAT 网卡有地址、也有默认路由（`default via 192.168.48.2 dev ens37 metric 50`），且出网正常（用 wget 从 GitHub 拉到 cJSON）。问题只在"宿主机 → 客户机"这个反方向。

**原因**
NAT 网络（VMnet8）保证的是**客户机出向**可用；宿主机经 VMnet8 反向访问客户机是否通，取决于宿主机侧 VMnet8 虚拟网卡与 VMware NAT 服务的状态。这与 S0 记录里对 ens37 的定位一致——它本来就是"专供 apt 出网"的通道。

**解决**
本阶段全部操作走桥接 `ssh vmserver-lan`（队友联调也走这条）；需要出网的操作（拉 cJSON）在虚拟机内发起，NAT 方向正常。此条记录在案，避免下次把它误判成"虚拟机没开机"。

---

### 问题 20：宿主机 Python 输出中文乱码

**现象**
验收脚本打印的中文变成 `Ŀ�� 10.202.57.4:9000 ģʽ basic`，而其中的英文与 JSON 完全正常。

**定位**
脚本文件本身是 UTF-8，Git Bash 终端也是 UTF-8；乱码只出现在 Python 进程的输出上，说明是解释器输出编码环节的问题。

**原因**
Windows 的 Python 3 默认按**控制台代码页**（中文系统为 GBK）编码 stdout，UTF-8 字节被按 GBK 解码显示。

**解决**
运行时显式指定输出编码：
```bash
PYTHONIOENCODING=utf-8 python s2_probe.py basic
```
与问题 11、12 同属"Windows 中文环境编码"一类坑，只是这次发生在输出侧而非脚本解析侧。

---

### 问题 21：`grep -c 'ERROR'` 把 WARN 正文里的"ERROR"当成错误级别（假阳性）

**现象**
压测结束后统计服务器错误日志：
```bash
$ grep -c 'ERROR' /tmp/s2.log
108                       ← 看着像 108 条错误，其实程序一条错误都没出
```

**定位**
按级别字段重新统计：
```bash
$ sed -n 's/.*\[\(INFO \|WARN \|ERROR\)\].*/\1/p' /tmp/s2.log | sort | uniq -c
    241 INFO
    108 WARN
```
级别里根本没有 ERROR；而这 108 条 WARN 的**正文**里写着"回 ERROR code=…"（是日志文案在描述"已回了一条 ERROR 应答"）。

**原因**
`grep -c 'ERROR'` 匹配整行任意位置，正文里的 "ERROR" 被当成了级别字段。

**解决**
匹配**带方括号的级别字段**：
```bash
grep -c '\[ERROR\]' /tmp/s2.log      # → 0
```
教训与问题 15（ping 统计行骗人）同类：**验收统计的口径要匹配到字段，不能匹配自由文本**，否则会得出与事实相反的结论。

---

### 问题 22：valgrind 与 ThreadSanitizer 都不可用（缺 sudo 密码）

**现象**
```bash
$ which valgrind || echo 'valgrind 未安装'
valgrind 未安装

$ sudo -n true
sudo: 需要密码                      ← 无免密，装不了

$ gcc -fsanitize=thread ... 
/usr/bin/ld: 找不到 libtsan_preinit.o: 没有那个文件或目录
```

**定位**
`/usr/lib/gcc/x86_64-linux-gnu/9/` 下有 `libtsan.a`/`libtsan.so`（运行库在），**唯独缺 `libtsan_preinit.o`**；`-static-libtsan` 绕不过（链接器仍要该文件）；全盘 `find / -name libtsan_preinit.o` 无结果。

**原因**
虚拟机未安装 valgrind；gcc-9 的 TSan 运行库不完整（包被裁剪），补齐需要 `sudo apt install`，而本机 sudo 有密码、执行 Agent 不持有。

**解决**
规划里 S2-5 的 valgrind 项本就是"（可选）"，改用**免 root 的 AddressSanitizer** 覆盖同一类风险（S2 新增的任务 malloc/free 生命周期、cJSON 解析树释放），另用加重并发压测补偿做不到的竞争检测：
```bash
make CFLAGS='-Wall -Wextra -O1 -g -Isrc -fsanitize=address -fno-omit-frame-pointer'
./server_asan          # 官方 2×50 与加压 8×200 各跑一轮，最后 SIGINT 优雅退出
grep -E 'ERROR: (Address|Leak)Sanitizer|SUMMARY|leaked' /tmp/asan.log
# → 无任何报告（退出码正常，LSan 给出空报告）
```
**遗留**：真正的数据竞争检测（TSan/helgrind）本阶段没能做。若 S7 压测前需要补，在本机执行一条 `sudo apt install valgrind` 即可。

---

### 问题 23：ssh 执行"启动后台进程"的命令不返回，被误判为超时

**现象**
```bash
$ ssh vmserver-lan "cd ~/server-linux && nohup setsid ./server > /tmp/s2.log 2>&1 < /dev/null & sleep 4; ss -ltn | grep 9000"
# 长时间无输出 → 工具判定超过 120s 超时，转为后台任务
```
但另开一条 ssh 查询，服务器其实早已起来并在监听。

**定位**
新连接查询 `pgrep -ax server` / `ss -ltn | grep 9000` / `cat /tmp/s2.log` 全部正常，说明命令的实质内容早已执行完毕，只是 ssh 会话本身一直没有结束。

**原因**
sshd 要等会话内**所有**进程（包括刚 fork 出来的后台进程）都关闭继承来的标准输出/标准输入管道后，才关闭这条通道；后台进程仍持有会话的 pty/管道引用，于是 ssh 一直挂着不返回。

**解决**
启动类操作按"**启动一条、查询一条**"两步走：第一条 ssh 只负责拉起（不等返回），另开一条 ssh 查询进程与端口来判定结果；必要时加 `ssh -n` 并把 stdin 明确指向 `/dev/null`。本阶段所有服务器启停都按此方式执行。

---

## 九、S2 验收结论

**产出**：新增 `src/pool.c/h`（线程池）、`src/proto.c/h`（cJSON 解析与分发）、`src/cJSON.c/h`（官方 v1.7.18，cJSON.c MD5 `630deb131a520d63b8913492d982b589`、cJSON.h MD5 `a9c6c607caeee91e5fe70dfd349b4ddc`，与上游逐字节一致）；改动 `net.c`（echo 占位 → 深拷贝投递线程池、`net_send` 加发送锁、accept 设 `SO_SNDTIMEO`）、`session.c`（会话表锁）、`main.c`（`pool_init`）、`config.h`（新增 `SEND_TIMEOUT_SEC`）。`make clean && make` 零警告零错误（含 cJSON.c）。

**验收命令与实测输出**

| # | 命令 | 预期 | 实际 |
|---|---|---|---|
| S2-1 | `python s2_probe.py one '{bad'` | `{"type":"ERROR","code":1,"msg":"malformed json"}`，连接不断 | ✅ 回 `{"type":"ERROR","code":1,"msg":"malformed json"}`；紧接着再发一条仍收到应答，连接未被断开 |
| S2-2 | `python s2_probe.py one '{"type":"PING"}'` | code=2 `unknown type`，连接不断 | ✅ 回 `{"type":"ERROR","code":2,"msg":"unknown type"}`；再发一条仍收到应答 |
| S2-3 | `python s2_probe.py one '{"foo":1}'` | 回 code=2 | ✅ 回 `{"type":"ERROR","code":2,"msg":"unknown type"}`（type 缺失与未知 type 同码） |
| S2-4 | `grep '工作线程\] 解析消息' /tmp/s2.log` + 按 tid 统计 | 主线程只 accept/recv/投递，解析来自工作线程且 tid 不同 | ✅ 日志中出现 **5 个 tid**：`140380596881216`（主线程：新连接/收到完整消息…投递线程池/断开回收）与 4 个工作线程 tid；主线程日志里没有任何"解析"记录 |
| S2-5 | `python s2_probe.py concurrent 50` | 两个客户端并发各 50 条，无崩溃、无乱序 | ✅ 0 号客户端 50/50 条 code=1、1 号客户端 50/50 条 code=2，回包内容各归各，无串包、无交错 |

**附加验证（超出验收标准的部分）**

| 检查 | 结果 |
|---|---|
| 加压并发：`python s2_probe.py concurrent 200 8`（8 客户端 × 200 条 = 1600 条） | ✅ 8 个客户端全部 200/200 条回包正确，无串包、无交错 |
| AddressSanitizer + LeakSanitizer（替代不可用的 valgrind，见问题 22） | ✅ 走完官方 2×50 与加压 8×200 后 SIGINT 优雅退出，**无任何 ASan/LSan 报告** |
| 服务器错误日志 | ✅ `grep -c '\[ERROR\]' /tmp/s2.log` = **0**；108 条 WARN 恰好对应 108 条非法输入消息 |
| 退出行为回归（S0-6/S1） | ✅ SIGINT 后依次打印"select 主循环已退出"→"收到退出信号，服务器正常退出"，端口 9000 释放 |

---

## 十、S2 实现要点（供 S3/S4 参考，避免踩坑）

1. **任务内存归属**：`net.c` 里 malloc 的 `net_task_t`（含消息副本 `line`）由工作线程的 `net_task_run` 在处理完后释放——一个任务只有一个消费者，不需要引用计数。新增任务类型时照此约定，**谁投递谁负责不释放、谁执行谁负责释放**。
2. **分帧符必须和正文同一次发出**：`proto_reply` 把 JSON 文本与 `'\n'` 拼进同一个 buffer 后只调一次 `net_send`。若拆成两次 `net_send`，另一线程的回包会插在中间，对端收到半截 JSON。
3. **发送锁是全局的**（规划 S2 任务 5 的要求："同一 fd 的多次 send 不交错"）。实现简单，代价是一个不读数据的客户端会让阻塞的 `send()` 拖住所有连接 —— 故在 accept 时设 `SO_SNDTIMEO=10s` 兜底（超时按发送失败返回）。S7 压测若暴露问题，升级方向是"每 fd 一把锁"或"每连接发送缓冲 + 独立写线程"。
4. **锁序：会话表锁 → 发送锁**，任何路径不得反向。工作线程执行任务时**不持有池锁**（`pool.c` 在调用 `task.fn` 前已解锁），因此任务体里可以安全地调 `net_send` / `session_*`，甚至可以再 `pool_submit` 而不会自死锁。
5. **`pool_submit` 队列满时阻塞**（背压），不丢任务。S4 起任务体里会有 MySQL 查询，单条变慢会通过背压顶住主线程；那时应关注任务队列水位。
6. **S3 注意 `session_client_id` 的指针生命周期**：该函数返回的是**会话槽内部指针**，`session.c` 加锁后，出锁那一刻它就可能因连接被踢/回收而失效。S3 实现时应在锁内 `memcpy` 到调用方缓冲（或改由调用方一次性取走所需字段），不要把裸指针传出后再用。
7. **cJSON 要点**：`cJSON_PrintUnformatted` 得到的字符串要用 `cJSON_free` 释放；`cJSON_Parse` 失败返回 NULL，此时**没有**对象可释放（不要对 NULL 调 `cJSON_Delete`）；`cJSON_AddNumberToObject(obj, "code", 1)` 输出的是 `1` 而非 `1.0`（S2-1~S2-3 已逐字节比对）。
8. **日志的 `[tid:…]` 是 S2-4 的证据来源**：新增模块请继续用 `log_info/log_warn/log_err`，不要直接 `printf`——否则会丢线程号，且多线程下可能交错。
