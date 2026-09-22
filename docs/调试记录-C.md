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

### 问题 22：valgrind 装不上、ThreadSanitizer 链接失败 —— 最终免 root 跑通了 valgrind

**现象**
```bash
$ which valgrind || echo 'valgrind 未安装'
valgrind 未安装

$ sudo -n true
sudo: 需要密码                      ← 无免密，apt install 走不通

$ gcc -fsanitize=thread ...
/usr/bin/ld: 找不到 libtsan_preinit.o: 没有那个文件或目录
```

**定位**
1. TSan 路线：`/usr/lib/gcc/x86_64-linux-gnu/9/` 下有 `libtsan.a`/`libtsan.so`（运行库在），**唯独缺 `libtsan_preinit.o`**；`-static-libtsan` 也绕不过；`dpkg -L libgcc-9-dev` 显示该包**本来就不提供**这个文件（Ubuntu 20.04 的 gcc-9 包装配如此），全盘 `find` 亦无结果 —— 这条路彻底堵死。
2. valgrind 路线：`apt-get download valgrind` **不需要 root**（只下载到当前目录），于是下载 + `dpkg-deb -x` 解包到 `/tmp/vg`，绕开安装。

**原因**
本机 sudo 有密码而执行 Agent 不持有，所以"装包"这类操作走不通；但"下载 deb + 解包"全程是普通用户权限，**不需要 sudo**。真正的障碍在后面：Debian 把 valgrind 的工具库放在非默认路径，直接运行解包出来的 `valgrind` 会报 `failed to start tool 'memcheck'`。

**解决**
定位到工具库的真实位置后用 `VALGRIND_LIB` 指过去，valgrind 即正常工作（无需安装、无需 root）：
```bash
mkdir -p /tmp/vgpkg && cd /tmp/vgpkg && apt-get download valgrind
mkdir -p /tmp/vg && dpkg-deb -x valgrind_*.deb /tmp/vg
find /tmp/vg -name 'memcheck-amd64-linux'      # → /tmp/vg/usr/lib/x86_64-linux-gnu/valgrind/
export VALGRIND_LIB=/tmp/vg/usr/lib/x86_64-linux-gnu/valgrind
/tmp/vg/usr/bin/valgrind --version             # → valgrind-3.15.0
```
> 注：`/tmp/vg/usr/bin/valgrind` 是个 shell wrapper，`--version` 在**没设** `VALGRIND_LIB` 时会失败（工具库找不到），设上即通。此后 helgrind/drd/memcheck 三个工具都可直接用。

顺带保留 ASan 作为交叉验证手段（免 root、编译期开关，见上）：
```bash
make CFLAGS='-Wall -Wextra -O1 -g -Isrc -fsanitize=address -fno-omit-frame-pointer'
./server_asan          # 官方 2×50 与加压 8×200 各跑一轮，最后 SIGINT 优雅退出
grep -E 'ERROR: (Address|Leak)Sanitizer|SUMMARY|leaked' /tmp/asan.log   # 无任何报告
```

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

### 问题 24：Helgrind 抓到 cJSON 全局变量的数据竞争（**本阶段唯一的真实并发缺陷**）

**现象**
补上 valgrind 后，用 Helgrind 跑 S2-5 并发用例：
```bash
VALGRIND_LIB=/tmp/vg/usr/lib/x86_64-linux-gnu/valgrind \
  /tmp/vg/usr/bin/valgrind --tool=helgrind --log-file=/tmp/hg.log ./server
# 跑完 2×50 并发后：
$ grep -c 'Possible data race' /tmp/hg.log
2
$ grep 'ERROR SUMMARY' /tmp/hg.log
==5888== ERROR SUMMARY: 148 errors from 2 contexts (suppressed: 34127 from 59)
```
两条报告堆栈完全相同，都指向 cJSON：
```
Possible data race during write of size 8 at 0x195340 by thread #5
   at 0x10D735: cJSON_ParseWithLengthOpts (cJSON.c:1110)
   by 0x10F461: proto_handle (proto.c:121)
   by 0x10AEE7: net_task_run (net.c:271)
   by 0x10F09D: pool_worker (pool.c:58)
   by 0x4864322: start_thread (pthread_create.c:477)
This conflicts with a previous write of size 8 by thread #2   ← 同一个函数的另一个工作线程
   ...（同样的堆栈）
Address 0x195340 is 0 bytes inside data symbol "global_error"
```

**定位**
- 写方是**两个不同的工作线程**，都停在 `cJSON.c:1110`（`global_error.json = NULL;`）与 `:1111`（`global_error.position = 0;`）；
- 两条报告对应这个全局结构体的两个字段（地址 0x195340 与 0x195348，相差 8 字节）；
- 看 cJSON 源码：`static error global_error = { NULL, 0 };` 是**进程级全局**，每次 `cJSON_Parse*` 进来都重置它，解析失败时（`fail:` 分支）还会写入错误位置；
- 我们的 `proto_handle` 由 4 个工作线程并发调用，谁都没对这个全局加锁——**不是我们漏了锁，是这个全局本来就不该被多线程共享**。

**原因**
cJSON 的"最近一次解析错误"（`cJSON_GetErrorPtr` 的返回值来源）设计成进程级全局，隐含"单线程使用"的假设。本服务器把解析放进了线程池（S2 的核心改动），4 个工作线程同时解析，于是每次解析都在无锁地写同一个全局变量。虽然我们**从不调用** `cJSON_GetErrorPtr`、该值不会被读，但写-写竞争本身就是未定义行为，且会一直污染后续所有并发检测结果。

**解决**
给这个全局加 `__thread`，改为线程局部（与上游唯一差异，共 8 行，1 行代码 + 7 行注释）：
```c
-static error global_error = { NULL, 0 };
+static __thread error global_error = { NULL, 0 };
```
语义上更正确：每个线程各记各的最后一次解析错误，`cJSON_GetErrorPtr` 在多线程下才真正可用；单线程行为与上游完全一致。无需加锁，也不影响解析性能（线程局部反而免去了缓存行争用）。

复测（同样的用例与加压）：
```bash
$ grep -c 'Possible data race' /tmp/hg2.log
0
$ grep 'ERROR SUMMARY' /tmp/hg2.log
==6432== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 159562 from 59)
```
功能回归同步复跑：`basic` + `concurrent 50` + `concurrent 100 4` + `concurrent 200 8` 全部通过。

> **给 S8 的提醒**：`src/cJSON.c` 现在带着这一处本地补丁（cJSON.h 未改）。**不要**再整份覆盖成上游版本，否则这个竞争会复活；差异可用 `diff /tmp/cJSON.c src/cJSON.c` 随时复核。

---

### 问题 25：Memcheck 报 `possibly lost: 1,152 bytes in 4 blocks`

**现象**
```bash
$ VALGRIND_LIB=... valgrind --tool=memcheck --leak-check=full --show-leak-kinds=possible --log-file=/tmp/mc2.log ./server
$ grep -A 12 'loss record' /tmp/mc2.log
1,152 bytes in 4 blocks are possibly lost in loss record 1 of 1
   at 0x483DD99: calloc (...)
   by 0x40149DA: allocate_dtv (dl-tls.c:286)
   by 0x40149DA: _dl_allocate_tls (dl-tls.c:532)
   by 0x4864322: allocate_stack (allocatestack.c:622)
   by 0x4864322: pthread_create@@GLIBC_2.2.5 (pthread_create.c:660)
   by 0x10F103: pool_init (pool.c:80)
   by 0x10A6CB: main (main.c:52)
```
`definitely lost: 0`、`indirectly lost: 0`、`still reachable: 0`，只有这 4 块。

**定位**
堆栈直指 `pool_init` 里的 `pthread_create`，再往下是 glibc 的 `_dl_allocate_tls`——分配的是**每个线程的 TLS/线程描述符**。块数 4 恰等于 `POOL_THREADS`，大小 288 字节/块。

**原因**
这 4 个工作线程按设计**常驻到进程结束**（`pool.h` 写明"线程随进程退出，没有销毁接口"），进程退出时它们仍在运行，Valgrind 无法通过寄存器/栈找到指向这些块的唯一指针，于是归类为 `possibly lost`。**不是内存泄漏**，是活跃线程的正常开销。

**解决**
无需处理（若强行消除，就得给线程池加一套 shutdown/join 机制，属于规划未要求的复杂度）。本条记录在案，是为了让 S7 复跑压测时能一眼认出这条报告、不至于误判为新泄漏——**判读标准：只要 `definitely lost` 为 0 就没有泄漏**。

## 九、S2 验收结论

**产出**：新增 `src/pool.c/h`（线程池）、`src/proto.c/h`（cJSON 解析与分发）、`src/cJSON.c/h`（官方 v1.7.18：cJSON.h 与上游逐字节一致，MD5 `a9c6c607caeee91e5fe70dfd349b4ddc`；cJSON.c 基于上游 MD5 `630deb131a520d63b8913492d982b589`，带一处 8 行本地补丁——`global_error` 加 `__thread`，见问题 24）；改动 `net.c`（echo 占位 → 深拷贝投递线程池、`net_send` 加发送锁、accept 设 `SO_SNDTIMEO`）、`session.c`（会话表锁）、`main.c`（`pool_init`）、`config.h`（新增 `SEND_TIMEOUT_SEC`）。`make clean && make` 零警告零错误（含 cJSON.c）。

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
| **数据竞争检测（S2-5 点名的 valgrind 项）**<br>`VALGRIND_LIB=… valgrind --tool=helgrind ./server` 跑完 basic + 2×50 + 4×100 + 8×200 | ✅ `grep -c 'Possible data race'` = **0**；`ERROR SUMMARY: 0 errors from 0 contexts`。<br>修复前为 **2 个上下文 / 148 次**（cJSON 全局 `global_error`，已用 `__thread` 修掉，见问题 24） |
| **内存泄漏检测**<br>`valgrind --tool=memcheck --leak-check=full ./server` | ✅ `definitely lost: 0`、`indirectly lost: 0`、`still reachable: 0`；仅 `possibly lost: 1,152 bytes in 4 blocks` —— 4 个常驻工作线程的 TLS 块，非泄漏（见问题 25） |
| AddressSanitizer + LeakSanitizer（免 root 的交叉验证，见问题 22） | ✅ 走完官方 2×50 与加压 8×200 后 SIGINT 优雅退出，**无任何 ASan/LSan 报告** |
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
7. **cJSON 要点**：`cJSON_PrintUnformatted` 得到的字符串要用 `cJSON_free` 释放；`cJSON_Parse` 失败返回 NULL，此时**没有**对象可释放（不要对 NULL 调 `cJSON_Delete`）；`cJSON_AddNumberToObject(obj, "code", 1)` 输出的是 `1` 而非 `1.0`（S2-1~S2-3 已逐字节比对）。**`src/cJSON.c` 带一处本地补丁（`global_error` 加 `__thread`，见问题 24），不要再从上游整份覆盖**。
8. **日志的 `[tid:…]` 是 S2-4 的证据来源**：新增模块请继续用 `log_info/log_warn/log_err`，不要直接 `printf`——否则会丢线程号，且多线程下可能交错。
9. **并发检测的做法（S7 直接复用）**：虚拟机没装 valgrind 也能跑——`apt-get download valgrind` + `dpkg-deb -x` 解包到 `/tmp/vg`，再用 `VALGRIND_LIB=/tmp/vg/usr/lib/x86_64-linux-gnu/valgrind` 启动（见问题 22）。S7 压测建议照此跑一轮 `--tool=helgrind`（盯 `Possible data race`）与一轮 `--tool=memcheck --leak-check=full`（**只认 `definitely lost`，`possibly lost` 里那 1152 字节是常驻线程的 TLS，不是泄漏**）。

---

## 十一、S3 阶段问题记录（会话协议：LOGIN / HEARTBEAT / 超时）

### 问题 26：照规划字面"从工作线程关旧连接"会 double close，可能误关新接入的连接

**现象**
规划 S3 任务 1 写"同 client_id 已有旧连接 → **关旧 fd**"，S1 要点 4 也写"net_close 会被工作线程调用（踢旧连接）"。按字面实现（工作线程处理新连接 LOGIN 时直接 `net_close(旧fd)`）在推演中就暴露出冲突路径：

```
场景：旧连接 A(fd=5) 正被顶替，同时 A 自己也断开了
  工作线程：处理 B 的 LOGIN → net_close(5) → session_remove(5) + close(5)
  主线程  ：select 看到 fd=5 可读 → recv 返回 0 → net_close(5) → close(5)  ← 第二次
```

两次 `close(5)` 之间若有新连接 D 被 accept，内核会把 **5 这个号复用**给 D，于是第二次 `close(5)` 关掉的是 D——一个刚接入、完全无辜的新连接。这正是 S1 要点 4 想防的"误踢新连接"，只是那个要点只说了 `session_remove` 与 `close` 的**先后顺序**，没覆盖"两个线程各关一次"。

**定位**
把 S3 新增/既有的全部 `net_close` 调用点逐条列出来，标上执行线程：

| 调用点 | 线程 | 触发条件 |
|---|---|---|
| `net_accept` 会话表满 | 主线程 | 新连接但没有空槽 |
| `net_handle_client` | 主线程 | `recv` 返回 0 / 出错 |
| `net_shutdown_all` | 主线程 | 进程退出 |
| **踢旧连接（S3 新增）** | **工作线程** | 同 client_id 重复登录 |

只有最后一行是工作线程——它就是"两个线程关同一个 fd 号"的唯一来源。进一步确认：`session_remove` 是幂等的（重复摘同一个槽无害），但 **`close(fd)` 不幂等**（关一个已经关闭/已被复用的 fd 号是未定义行为），两者被包在同一个 `net_close` 里，缺少"谁有权 close"的仲裁。

另外还有一条死路：若让 `session_login` 在持有会话表锁时直接调 `net_close`，`net_close` → `session_remove` 会再取同一把**非递归**锁 → 自死锁（`session.h` 早已写明"不得在持有会话表锁的状态下再回调本模块"）。

**原因**
`close(fd)` 的语义是"关闭**这个数字**指向的文件描述符"，而 fd 号会被内核回收复用；只要存在两个线程各自以"连接"为单位发起 close，就必然存在"按连接关闭"与"按 fd 号关闭"的错配窗口。根治办法不是加锁，而是**让 close 有唯一的执行者**。

**解决**
把踢旧拆成"标记"与"回收"两段，close 全部收敛到主线程：

1. `session_login`（工作线程，持会话表锁）：只给旧会话打 `s->kick = 1` 标记并记 WARN 日志，**绝不在锁内关闭任何东西**；
2. `net_loop` 每轮开头（主线程，重建 fd_set 之前）调 `session_collect_dead()`：锁内把 `kick` 标记的、以及超时的会话槽**摘除**（槽位清空 → 该 fd 立刻退出 select 集合，不会再被 recv），返回 fd 列表；
3. 主线程紧接着逐个 `net_close(fd)`——此时"摘会话 → close"全程串行，且同一 fd 号不可能有第二条 close 路径。

顺带把 `net_close` 的 `close(fd)` 段放进**发送锁**内（`net_send` 全程持锁），消除"close 释放 fd 号 → accept 复用 → 另一个线程的迟到 send 写到新连接上"的交错窗口。

复验（`s3_probe.py stress 60`，5 个 client_id 轮转、旧连接有时在踢旧前后主动断开以逼出 fd 复用）：

```
压力轮次 60，异常 0 轮          ← 每轮都校验"新连接仍能收到 LOGIN_ACK 与 HEARTBEAT_ACK"
61 次重复登录 / 61 次回收        ← 标记与回收一一对应，无漏收
0 条 [ERROR]                    ← 全程无异常
```

### 问题 27：在持会话表锁的函数里调 `session_count()` —— 非递归锁自死锁

**现象**
`session_login` 成功分支原打算打一条"登录成功（当前会话 N 个）"的日志，直接写了 `session_count()`。写完复查时发现：本函数**正持有** `g_session_lock`，而 `session_count()` 第一件事就是 `pthread_mutex_lock(&g_session_lock)`。

**定位**
对照 `session.c` 的锁约定——"static 辅助函数假定调用时已持锁，公开函数一律自己加锁"。`session_login` 是公开函数（自己加了锁），它去调另一个公开函数，等于对同一把非递归 mutex 连续加锁两次。

**原因**
`PTHREAD_MUTEX_INITIALIZER` 初始化的是**默认类型（非递归）**互斥锁，同一线程重复 lock 是未定义行为，Linux/glibc 下表现为直接死锁。且这类死锁只在这个分支被触发，压测若没覆盖"登录成功"就会漏网——上线后才炸。

**解决**
把计数逻辑抽成 static 的 `session_count_locked()`（假定已持锁），`session_count()` 与 `session_login()` 各自复用它：公开函数仍负责加锁，锁内一律只调 `*_locked` 版本。

```c
static int session_count_locked(void);     /* 调用方必须已持锁 */
int        session_count(void);            /* 公开：内部加锁后转调 */
```

同类检查同步做了一遍：`session_sweep_timeouts` / `session_collect_dead` 里都只用锁内安全的 `session_slot_clear()`，没有再出现"锁内调公开函数"。

### 问题 28：验收脚本每轮都"等满超时"，60 轮压测超出工具时限

**现象**
踢旧压力脚本跑到 240s 仍未结束，被工具转为后台任务（最终 250s 左右完成，结果正常）：

```
Command did not complete within its 240s timeout and was moved to the background
```

**定位**
脚本每轮固定消耗：等 A 的 ACK 1s + 等 B 的 ACK 1s + 探测 A 是否断开 0.2s + 等 B 的 HEARTBEAT_ACK 2s ≈ 4.2s。60 轮 ≈ 250s。而其中真正必要的等待只有"回包到达"那几毫秒。

**原因**
读应答的辅助函数 `recv_lines()` 是"**收满 wait 秒**才返回"（循环 `recv` 直到 timeout/EOF），而不是"**收到数据就返回**"。写一次性用例时无感（只等 2s），一旦进循环就被放大 60 倍。

**解决**
验收脚本改成"收到数据即返回"（保留 timeout 作为上限，用于区分"没有应答"与"有应答"）；等待时间只作为兜底上限。与问题 10（`timeout` 掩盖退出码）、问题 21（`grep` 匹配自由文本假阳性）同类——**验收工具本身的口径要精确，否则会得出与事实相反的结论或白等**。

---

## 十二、S3 验收结论

**产出**：改动 `session.c/h`（`session_login` 含踢旧标记、`session_is_logged`、`session_client_id`、新增 `session_client_id_copy` / `session_collect_dead`、`session_sweep_timeouts` 转为真断开、`kick` 字段）、`proto.c`（LOGIN/DATA/HEARTBEAT 三个 handler 填实、登录态拦截、错误码 3/4）、`net.c`（主线程每轮巡检回收、`net_close` 的 close 段入发送锁）、`main.c`（版本行）。`make clean && make` 零警告零错误。**接口规范 §3.1/§3.2/§3.3 字段零改动**（新增的 code=3/4 只出现在规范未定义的路径上）。

**验收命令与实测输出**（同一台服务器、一份连续日志）

| # | 命令 | 预期 | 实际 |
|---|---|---|---|
| S3-1 | `python s3_probe.py basic <IP>` 第一条 | 回 `{"type":"LOGIN_ACK","status":0}` | ✅ 回 `{"type":"LOGIN_ACK","status":0}` |
| S3-2 | 同连接发 §3.3 的 DATA 向量 | 正常接收（日志打印 4 个传感器值），不回 ERROR | ✅ 无任何应答（符合规范：DATA 未定义应答）；日志 `sensors=4 [1=25.6 2=55 3=312 4=356]`，与向量 25.6℃/55.0%/312/356lux 逐一相符 |
| S3-3 | 同连接发 `{"type":"HEARTBEAT"}` | 回 `{"type":"HEARTBEAT_ACK"}` | ✅ 回 `{"type":"HEARTBEAT_ACK"}` |
| S3-4 | 连接 A、B 依次用 `PC-01` 登录 | A 被断开，B 收 LOGIN_ACK status=0 | ✅ B 回 `{"type":"LOGIN_ACK","status":0}`；A 收到 EOF（被服务器断开）；B 随后 HEARTBEAT 仍得 ACK。日志：`重复登录：旧连接 fd=4 标记待踢除（新连接 fd=5）` → 1s 后 `回收被顶替的旧连接：fd=4` → `连接已关闭 fd=4` |
| S3-5 | 登录后静置不发任何消息 | 约 90s 被服务器断开，日志有超时回收记录 | ✅ 13:37:34 登录 → 13:39:04 断开（**整 90s**），客户端在静置 88.2s 时收到 EOF。日志：`会话超时：fd=4 已 90s 无任何消息（阈值 90s）` → `会话超时断开：fd=4 …回收连接` → `连接已关闭 fd=4` |
| S3-6 | 未登录直接发 DATA | 回 ERROR 且不断连；随后 LOGIN 成功 | ✅ 回 `{"type":"ERROR","code":3,"msg":"not logged in"}`；紧接着 LOGIN 回 `{"type":"LOGIN_ACK","status":0}`、再发 HEARTBEAT 回 ACK（证明连接全程未断） |

> S3-5 的两个数字不矛盾：服务器从"最后一字节到达"起算满 90s 断开；客户端从收到 LOGIN_ACK 后才开始计时，且巡检节拍为 1s，故观察到 88.2s。验收要求"约 90s"，符合。

**附加验证（超出验收标准的部分）**

| 检查 | 结果 |
|---|---|
| 非法 client_id（规划任务 1 的校验要求，5 种输入） | ✅ 含空格/感叹号、32 字符超长、空串、缺字段、非字符串——**全部回 `code=4 invalid client_id` 且不注册**（紧接着发 DATA 仍被 `code=3` 拦截） |
| 踢旧 + 旧连接同时断开 的并发压力 60 轮<br>（5 个 client_id 轮转、旧连接有时主动断开以逼出 fd 复用） | ✅ **异常 0 轮**：每轮新连接都正常收到 LOGIN_ACK 与 HEARTBEAT_ACK，无一被误关（问题 26 的修复经此验证） |
| **数据竞争检测**<br>`valgrind --tool=helgrind ./server` 跑 basic + kick + nologin + badid + stress 12 | ✅ `grep -c 'Possible data race'` = **0**；`ERROR SUMMARY: 0 errors from 0 contexts`（S3 新增的"工作线程打标记 / 主线程回收"跨线程路径无竞争） |
| **内存泄漏检测**<br>`valgrind --tool=memcheck --leak-check=full ./server` 跑同一组用例 | ✅ `definitely lost: 0`、`indirectly lost: 0`、`still reachable: 0`；`possibly lost: 1,152 bytes in 4 blocks` 仍是问题 25 已定性的常驻线程 TLS（非泄漏）；无 invalid read/write |
| 服务器错误日志 | ✅ 连续日志 `[ERROR]` = **0**；4 条 WARN 恰好对应 1 次踢旧 + 1 次未登录拦截 + 2 条超时巡检（告警 + 回收） |
| 优雅退出回归（S0-6/S1/S2） | ✅ SIGINT 后 `select 主循环已退出` → `收到退出信号，服务器正常退出`，无残留进程、端口释放 |

---

## 十三、S3 实现要点（供 S4/S5 参考，避免踩坑）

1. **close 只有一个执行者（主线程）**：工作线程要关连接时，只能"打标记"（`session_login` 的 `kick`），由主线程 `session_collect_dead` + `net_close` 统一回收。S4/S5 若新增"某类消息要主动断开客户端"的需求，照此办——**不要在工作线程里直接调 `net_close`**（问题 26）。
2. **`session_collect_dead` 是"摘槽 + 交 fd"**，返回的 fd 只应交给 `net_close`；摘槽后该 fd 当轮就退出了 select 集合，不会再被 recv。调用点必须在**重建 fd_set 之前**（`net_loop` 循环开头），顺序反了会让本轮的 fds 快照里还留着已摘除的 fd。
3. **锁内绝不调公开函数**：`session.c` 的公开函数各自加锁，锁内只能调 `*_locked` / `session_slot_clear` 这类静态辅助（问题 27）。这条对 S4 的 `db_*` 同样适用——`db` 那边计划用 `__thread` 连接，天然规避跨线程竞争，但"锁内套锁"的自死锁仍要自查。
4. **`session_client_id()` 返回内部指针**（冻结签名如此），出锁即可能失效；**S4 入库前一律用 `session_client_id_copy()`** 拷到本地缓冲再用（proto 的 DATA 分支已是这个写法，直接照抄）。`alarm_check` / `db_insert_*` 的 `client_id` 参数也应来自这个本地副本。
5. **DATA 分支目前"只记录、不回包"**：S4 接入库时不要把回包加进来——接口规范 §3.1 没有为 DATA 定义应答，客户端也不等应答；日志里的 `[1=25.6 2=55 3=312 4=356]` 汇总格式可直接复用来核对入库值。
6. **未登录拦截的三个入口已就位**：DATA/HEARTBEAT 在各自 handler 开头判 `session_is_logged(fd)`，未登录回 `code=3` 且**不 touch 活跃时间**（`session_touch` 在判定之后调用）。S4/S5 新增的消息类型同样要先过这道判定。
7. **刷新活跃时间有两处**：`net.c` 在 recv 到字节时无条件 `session_touch`（"任何字节到达即活跃"的兜底），proto 在处理**合法且已登录**的消息时再显式 touch 一次（对齐规划 S3 任务 4 的表述）。重复 touch 无害，静置场景（S3-5）不受影响。
8. **验收脚本要"收到即返回"**：本轮压测脚本因"每轮等满超时"把 60 轮拖到 250s（问题 28）。S7 压测请用"收到数据即返回"的读法，超时只作为"没有应答"的判据。

---

## 十四、S4 阶段问题记录（DATA 入库）

### 问题 29：`strptime` 隐式声明——返回的指针被当成 `int` 截断

**现象**
新增 ts 校验后 `make` 出现两条警告（S4 首次编译）：

```
src/proto.c: In function ‘proto_datetime_ok’:
src/proto.c:164:12: warning: implicit declaration of function ‘strptime’; did you mean ‘strftime’? [-Wimplicit-function-declaration]
  164 |     rest = strptime(ts->valuestring, "%Y-%m-%d %H:%M:%S", &tmv);
src/proto.c:164:10: warning: assignment to ‘const char *’ from ‘int’ makes pointer from integer without a cast [-Wint-conversion]
```

**定位**
`strptime` 是 X/Open（XSI）扩展，不在 C 标准与 POSIX 基础集里；glibc 只在定义了 `_XOPEN_SOURCE` 时才在 `<time.h>` 里声明它。本工程 Makefile 用 gcc 默认的 `-std=gnu17`（`_DEFAULT_SOURCE`），恰好不含这一项，于是编译器"没见过"这个函数，按 C89 老规矩当作返回 `int` 的函数放行。

**原因**
隐式声明的返回类型是 `int`，而 `strptime` 实际返回 `char *`——指向**未解析部分**的指针。64 位机上指针被截成低 32 位，再拿去做 `*rest != '\0'` 的判断属于未定义行为；即使侥幸不崩，也会把合法 ts 判成非法（一律回落到 NOW()），S4-3/S4-4 的判定口径就整体失效。这条警告不是"风格提示"，它本身就是 UB 报告。

**解决**
在 `proto.c` **任何 `#include` 之前**加一行：

```c
#define _XOPEN_SOURCE 700   /* strptime() 属于 XSI 扩展，glibc 默认不声明 */
```

重编后零警告零错误。经验：特性宏只在用到它的那个 `.c` 顶部定义，不要塞进 Makefile 的 `CFLAGS`——那会影响全部源文件（例如 `_XOPEN_SOURCE` 会关掉 `__USE_MISC`，可能让别处依赖的 BSD 扩展消失）。

### 问题 30：`timeout 30 python probe.py > 文件` 拿到空文件——S4-2 的比对证据被缓冲吞了

**现象**
S4-1 跑完（行数 0 → 64，增长正确），但为 S4-2 准备的"probe 自己发出去的记录"文件是空的，`tail -2` 无任何输出。

**定位**
命令是 `timeout 30 python simulator/server_probe.py client ... > s4_probe_out.txt`。probe 用 `print` 输出到 stdout：**stdout 是终端时是行缓冲**（所以 S3 手测从没遇到过），**一旦重定向到文件就变成块缓冲**（4~8KB）。30 秒的 16 条记录还填不满一个缓冲块，全留在用户态缓冲里；`timeout` 到点发 SIGTERM 直接杀进程，缓冲随之丢弃。

**原因**
与问题 28 同类——**证据的采集方式不严谨，会得出与事实相反的结论**：数据其实已经正确入库（服务器日志与 SQL 都能证明），但"发送侧原始记录"没落盘，S4-2 就无法做逐值比对。

**解决**
凡是重定向抓输出，一律加 `-u`（无缓冲）：`timeout 11 python -u simulator/server_probe.py ... > s4_probe_pc02.txt`。重跑后 probe 记录的
`[-> #6] {"type": "DATA", "ts": "2026-09-22 18:33:14", "sensors": [{"id": 1, "v": 30.1}, {"id": 2, "v": 66.1}, {"id": 3, "v": 184}, {"id": 4, "v": 384}]}`
与库中 id 85~88 行逐值对上（见 §十五 S4-2）。

### 问题 31：虚拟机时钟比宿主机快约 2 秒（吃掉联调点 §6.5"误差<3s"的大半余量）

**现象**
用宿主机时间给 probe 的输出打点、用 MySQL 的 `NOW()` 给服务器侧打点，同一动作的两串时间对不上，差 2~3 秒。

**定位**
单次往返测量（两端各记宿主机时刻，取中点作为虚拟机采样时刻）：

```
宿主机 发起=1790073232.008870600 返回=1790073232.709718200
虚拟机 时刻=2026-09-22 18:33:54.382315386
→ 单程约 0.35 s；虚拟机时钟 - 宿主机时钟 ≈ +2.02 s
```

同一时刻虚拟机的 `date` 与 MySQL 的 `NOW()` 完全一致（都是 18:33:46），说明偏差在**宿主机 ↔ 虚拟机**之间，不在 MySQL。

**原因**
虚拟机时间同步（VMware Tools / chrony）在上电后未及时校正，宿主机与客户机各自走时钟。属环境问题，不是代码问题。

**解决**
不修代码。判读口径改为**同一侧时钟内部比对**：S4-3/S4-4 的"collect_time ≈ 服务器当前时间"用服务器日志时间戳（服务器侧时钟）与该行 collect_time 比（实测同一秒，误差 ≤1s），不掺宿主机时间。记录在案是因为它直接关系接口规范 §6.5「三端时间戳一致，误差 < 3s」——2s 的系统性偏差仍在容差内，但已占掉大半余量，联调时若发现时间戳成片偏移，先查这里；建议联调前在虚拟机执行 `sudo timedatectl set-ntp true`（或手工校准）后再测。

---

## 十五、S4 验收结论

**产出**：新增 `src/db.c` / `src/db.h`（每线程一条 MySQL 连接的惰性建连、`db_insert_sensor` 转义+写入、`db_shutdown`）；改动 `proto.c`（DATA 分支拆包入库、`proto_datetime_ok` 校验 ts）、`main.c`（`db_init` 排在 `pool_init` 之前、退出前 `db_shutdown`、版本行）。`make clean && make` **零警告零错误**。**接口规范 §3.1/§3.2/§4 字段零改动**（DATA 依然不回包，也没有提前实现 S5 的告警）。

**验收命令与实测输出**（同一台服务器、一份连续日志 `/tmp/s4.log`，库中原有 0 行）

| # | 命令 | 预期 | 实际 |
|---|---|---|---|
| S4-1 | ① `mysql -uiot -piot123456 machine_room -e 'SELECT COUNT(*) FROM sensor_data;'`<br>② `PYTHONIOENCODING=utf-8 timeout 30 python simulator/server_probe.py client -H 10.202.57.4 --id PC-01`<br>③ 再查一次 COUNT | 持续增长，约每 2s 增 4 行 | ✅ 跑前 **0**、跑后 **64**（16 条 DATA × 4 传感器，30 秒 2s 一条）。前 8 行 `collect_time` = 18:32:21×4 → 18:32:23×4，正是"每 2 秒 4 行" |
| S4-2 | probe（PC-02，`python -u` 留发送记录）+ `SELECT * FROM sensor_data WHERE client_id='PC-02' ORDER BY id DESC LIMIT 4;` | 数值与 probe 发送一致 | ✅ probe 最后一条发出的原文：`{"id": 1, "v": 30.1}, {"id": 2, "v": 66.1}, {"id": 3, "v": 184}, {"id": 4, "v": 384}`，ts=18:33:14；库中 id 85~88 为 `30.10 / 66.10 / 184.00 / 384.00`，collect_time 全为 `2026-09-22 18:33:14` —— 4 个值逐一对上（温度湿度 1 位小数、烟雾光照整数） |
| S4-3 | 手工发 `{"type":"DATA","sensors":[{"id":1,"v":23.4}]}`（无 ts） | 该行 collect_time ≈ 服务器当前时间（误差<3s） | ✅ 日志 `[2026-09-22 18:33:27] … ts=(缺省，用服务器 NOW()) 入库 1/1 行`；库中 id=89 `sensor_value=23.40, collect_time=2026-09-22 18:33:27` —— 与服务器日志同一秒（误差 ≤1s） |
| S4-4 | 手工发 `{"type":"DATA","ts":"not-a-time","sensors":[{"id":2,"v":48.8}]}` | 不报错，fallback 到 NOW() | ✅ 日志 `[2026-09-22 18:33:27] … ts=(非法，改用服务器 NOW()) 入库 1/1 行`；库中 id=90 `48.80 / 2026-09-22 18:33:27`；全程 `[ERROR]` 0 条、连接未断（随后继续发消息仍被处理） |
| S4-5 | 手工发 `{"id":3,"v":25}`（int）与 `{"id":4,"v":25.6}`（float） | 两种都正确入库 | ✅ 库中 id=91 `sensor_type=3, sensor_value=25.00`、id=92 `sensor_type=4, sensor_value=25.60` —— int 与 float 都落库，DECIMAL(10,2) 按列定义补足两位小数 |

**附加验证（超出验收标准的部分）**

| 检查 | 结果 |
|---|---|
| **"每线程一条连接"的直接证据**（规划 S4 任务 1 的设计要点） | ✅ 服务器日志里 `本线程已建立 MySQL 连接` 共 **5 条、tid 各不相同**：主线程 `140462175754112`（与启动横幅同一个 tid）+ 4 个工作线程；跑 probe 期间查 `information_schema.processlist`，`user='iot'` 对 `machine_room` 的连接恰好 **5 条**（另 1 条是我自己的查询会话，db 为 NULL） |
| 日志计数与库中行数交叉核对 | ✅ 日志累计"入库 N 行"求和 = **204**，与 `SELECT COUNT(*)` = **204** 完全一致；分组 `PC-01 68 / PC-02 24 / PC-03 56 / PC-04 56`（4 个 client_id 并发写入互不串） |
| 容错：越界 id 与非数值 v | ✅ `{"id":7,…},{"id":1,"v":"x"}` → 两条 WARN（`id=7 不是 1~4 的整数`、`id/v 缺失或非数值`），日志 `入库 0/2 行（跳过 2）`，该连接后续消息不受影响 |
| **DATA 不回任何包**（接口规范 §3.1 未定义应答） | ✅ 一条连接上连发 5 条 DATA 后静置 2s，回包列表为空（仅 LOGIN 时回过一条 LOGIN_ACK） |
| 服务器错误日志 | ✅ 连续日志 `[ERROR]` = **0**；6 条 WARN = 4 条 `recv() 失败 … Connection reset by peer`（**是 `timeout` 杀 probe 产生 RST 的正常现象**，不是缺陷）+ 2 条上面的容错跳过 |
| 优雅退出回归（S0-6/S1~S3） | ✅ SIGINT 后依次打印 `select 主循环已退出` → `数据库连接已关闭（mysql_library_end 已调用）` → `收到退出信号，服务器正常退出`，端口 9000 释放、无残留进程 |

---

## 十六、S4 实现要点（供 S5/S6 参考，避免踩坑）

1. **`db_init` 必须在线程创建之前**（main.c 里排在 `pool_init` 前面）：`mysql_library_init()` 不是线程安全的；工作线程读到的连接参数靠 `pthread_create` 建立的 happens-before 保证可见，之后参数只读、无需加锁。
2. **每线程一条连接是惰性建立的**（`__thread MYSQL *t_conn` + 首次使用时 `mysql_thread_init`）。S6 的分析线程要写库，直接调 `db_*` 即可——那条线程首次调用时会自己建连，**不要**把主线程的连接对象传过去。
3. `db_exec()`（db.c 内的 static）带"连接断开（2006/2013）→ 重连一次重试"的兜底。S5 的 `db_insert_alarm`、S6 的 `db_run_analysis` 请复用它，不要再各写一遍 `mysql_query`。
4. **DATA 分支"不回包"在 S4 依然成立**。S5 加告警时，只允许新增 `ALARM` 推送（那是接口规范 §3.1 定义的服务器主动消息），**不要**顺手给 DATA 加个 ACK。
5. ts 的两道校验（`strptime` 整串消费 + `mktime/strftime` 往返比对）是**客户端输入专用**的，写在 proto.c 内部的 static 函数里；S5 组装 ALARM 的 `ts`、S6 的 `period_start/period_end` 都是服务器自己生成的时间，直接 `strftime` 即可，不要绕道这个函数。
6. 一条 DATA = 4 次 `mysql_query` 往返，2 秒周期余量充足。S6 的聚合查询若变慢，先看任务队列水位（§十 要点 5 的背压：`pool_submit` 队列满会顶住主线程）。
7. 本步**未跑** helgrind / memcheck（规划里属 S7）。已知形态供 S7 判读：4 个工作线程常驻不退出，它们的连接与 TLS 在进程退出时表现为 `still reachable`——判读标准仍按 §九：**只看 `definitely lost` 是否为 0**。
8. 抓 Python probe 的输出做证据时记得 `-u`（问题 30）；重定向 + 被杀 = 缓冲丢失，会得到"空文件"这种假证据。
