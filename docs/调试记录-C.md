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

---

## 十七、S5 阶段问题记录（阈值告警）

### 问题 32：验收脚本自己先崩了两次——证据工具没自检

**现象**
S5-1 第一次跑：`UnboundLocalError: cannot access local variable 'buf' where it is not associated with a value`；修掉再跑：TCP 连上了、LOGIN_ACK 也收到了，但 **DATA 一条都没发出去**，脚本静默结束。

**定位**
两个都是临时验收脚本（仓库外 `s5_probe.py`）自身的 Python 错误，与服务器无关：
① 在嵌套函数 `drain()` 里写 `buf += chunk`，漏了 `nonlocal buf` → 局部变量未绑定即使用；
② `args = sys.argv[4:]` 已经切过片，循环下标却仍从 4 起 → `while i < len(args)` 直接不成立 → 消息列表恒为空。

**原因**
与问题 28、30 同一类：**采集证据的工具本身没验过**。第 ② 个尤其危险——脚本不报错、连接正常、连 LOGIN_ACK 都正常收到，唯一的问题是"什么都没发"；输出里那句"没有告警"完全可以被读成"服务器不告警"。

**解决**
改脚本（补 `nonlocal`、下标从 0 起）。经验：验收脚本改完先做一次**反证**——故意发一条必然超标的 DATA，**必须先看到 ALARM 才认可脚本**；安静通过 = 工具不可信，先查工具。

### 问题 33：ALARM 的 ts 比同批 DATA 的 ts 晚 2 秒——宿主/虚拟机时钟差

**现象**
probe 发出的 `#2` 是 `"ts": "2026-09-22 18:45:47"`，服务器推回的 ALARM 是 `"ts":"2026-09-22 18:45:49"`，看起来"告警迟了 2 秒才到"。

**定位**
probe 跑在**宿主机**上（ts 是宿主机时钟），而 ALARM 的 ts 由服务器用**虚拟机**时钟 `strftime` 生成。两者相差约 2 秒，与问题 31 实测的 +2.02s 完全吻合。服务器侧其实是毫秒级响应——日志里同一时刻相邻两行就是 `告警：sensor=2…` 与 `DATA：… 入库 4/4 行`。

**原因**
不是缺陷，是问题 31 的同一环境偏差第一次以"两个时间戳同屏出现"的形式暴露。而且**规划 S5 任务 2 本就规定 ALARM 的 ts 用服务器当前时间**，它与客户端采集时刻（DATA 的 ts）允许不同——即使两端时钟分秒不差，网络与入库耗时也会让二者差几十毫秒。

**解决**
不修代码。判读口径沿用问题 31：**只在同一侧时钟内部比对**（ALARM 的 ts 对服务器日志时间戳、DATA 的 ts 对宿主机侧记录），跨侧比较时先把 2 秒偏差算进去。联调前建议在虚拟机执行 `sudo timedatectl set-ntp true`。

---

## 十八、S5 验收结论

**产出**：新增 `src/alarm.c` / `src/alarm.h`（阈值判定、ALARM 组包与推送、告警入库）；改动 `db.c`/`db.h`（`db_insert_alarm`、`db_load_thresholds`、新增 `db_threshold_t` 与 `db_threshold_get`、新增带结果集的 `db_query` 复用重连逻辑）、`proto.c`（DATA 分支逐值接入 `alarm_check`）、`main.c`（启动时加载阈值、版本行）。`make clean && make` **零警告零错误**。**接口规范 §3.1/§3.2/§4 字段零改动**（DATA 仍不回包；ALARM 字段与顺序逐字对齐 §3.1）。

**验收命令与实测输出**（同一台服务器、一份连续日志 `/tmp/s5.log`；验收前 `alarm_log` 原有 **0** 行、`sensor_data` 最大 id 为 **204**；客户端为仓库外临时脚本 `s5_probe.py`，登录后逐条发 DATA 并打印全部回包）

| # | 命令 | 预期 | 实际 |
|---|---|---|---|
| S5-1 | `python -u s5_probe.py 10.202.57.4 9000 S5-A '{"type":"DATA","ts":"2026-09-22 18:44:30","sensors":[{"id":1,"v":35.2}]}'` | 立即收到 ALARM：level=HIGH、sensor=1、value=35.2、threshold=26、msg 含"温度"；`alarm_log` 新增一行且字段一致 | ✅ 收到 `{"type":"ALARM","level":"HIGH","sensor":1,"value":35.2,"threshold":26,"msg":"温度超上限","ts":"2026-09-22 18:44:20"}`；库中 id=1 `S5-A / 1 / 35.20 / 26.00 / HIGH / 2026-09-22 18:44:20` —— 与推送逐字段一致（threshold=26 即被越过的**上限**） |
| S5-2 | 同上脚本发 `{"id":1,"v":10.0}` | ALARM 推送，msg 为低于下限方向 | ✅ 收到 `{"type":"ALARM","level":"HIGH","sensor":1,"value":10,"threshold":18,"msg":"温度低于下限","ts":"2026-09-22 18:44:40"}`；库中 id=2 `10.00 / 18.00 / HIGH` —— threshold 换成**下限** 18 |
| S5-3 | 发 `{"id":3,"v":500}` 与 `{"id":4,"v":10000}` | 烟雾超 400 → level=HIGH 告警；光照 → **无**告警 | ✅ 烟雾：`{"type":"ALARM","level":"HIGH","sensor":3,"value":500,"threshold":400,"msg":"烟雾超上限","ts":"2026-09-22 18:44:47"}`，库中 id=3 同值；光照：**无回包**、`alarm_log` 无新行（该值仍正常入库 `sensor_data` id=208） |
| S5-4 | 连发 3 条 `{"id":1,"v":30}` | 收到 3 条 ALARM，`alarm_log` 增 3 行（不去重） | ✅ 3 条 ALARM（ts 18:44:59 / 18:45:00 / 18:45:02，逐条对应 3 条 DATA）；库中 id=4/5/6 三行全为 `30.00 / 26.00 / HIGH` —— 未做任何去重 |
| S5-5 | 发 §3.3 标准向量 `25.6 / 55.0 / 312 / 356`（全在阈值内） | 无 ALARM、`alarm_log` 无新行 | ✅ **无回包**；`alarm_log` 仍为 6 行（id 1~6）；该条 4 个值正常入库（`sensor_data` id 212~215，collect_time = `2026-09-28 15:00:01`） |

**附加验证（超出验收标准的部分）**

| 检查 | 结果 |
|---|---|
| **MID 级别路径**（S5-1~5 只覆盖了 HIGH） | ✅ 湿度 80（上限 60）→ `{"type":"ALARM","level":"MID","sensor":2,"value":80,"threshold":60,"msg":"湿度超上限",…}` —— level 确实取自 `threshold_cfg` 而非硬编码 |
| **边界值用严格不等号** | ✅ 湿度 60.0（=上限）不告警；probe 实测烟雾 400（=上限）也不告警 —— 只有严格越界才算超标 |
| **NULL 侧不检查** | ✅ 烟雾发 0（低于任何"下限"）不告警 —— `min_value` 为 NULL 时该侧判定天然不成立 |
| **一条 DATA 多个值同时超标** | ✅ `{"id":1,"v":30},{"id":2,"v":80}` → 同一次收到 2 条 ALARM（温度 HIGH + 湿度 MID），库中 id=8/9 两行 |
| **真实数据流（端到端）** | ✅ 13 秒 probe（`--id S5-C`）共 7 条 DATA → 收到 6 条 ALARM，与逐值手算完全一致（#4、#5 两条全在阈值内，确实无告警）；库中 `alarm_log` 分组统计 `sensor 1 HIGH ×2 / sensor 2 MID ×3 / sensor 3 HIGH ×1` = 6 条，`sensor_data` 28 行 = 7×4 |
| **DATA 依然不回包**（接口规范 §3.1 未定义应答） | ✅ S5-5 的正常值 DATA 与 S5-4 的 3 条超标 DATA 均未收到任何非 ALARM 回包；S5 只新增了规范定义的 ALARM 主动推送 |
| **并发告警 + helgrind 数据竞争检测** | ✅ 两个客户端并发（HG-1 温度/湿度、HG-2 烟雾/光照）同时触发告警互不干扰；在 helgrind 下跑完整轮收发与告警后优雅退出，报告 `ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 457 from 87)` —— **零数据竞争**（S5 新增的"工作线程只读阈值缓存"路径无竞争，与 S2 那次的判读口径一致） |
| 服务器错误日志 | ✅ 全程 `grep -c '\[ERROR\]'` = **0**；9 条告警 INFO 日志分布在不同 tid（工作线程）上 |
| 优雅退出回归（S0-6/S1~S4） | ✅ SIGINT 后依次打印 `select 主循环已退出` → `数据库连接已关闭` → `收到退出信号，服务器正常退出`；`pgrep -ax server` 无残留、9000 端口释放 |

---

## 十九、S5 实现要点（供 S6/S7 参考，避免踩坑）

1. **阈值缓存由 db 模块持有、启动时一次填好、此后只读**：`db_load_thresholds()` 在主线程（`pool_init` 之前）把 `threshold_cfg` 转成 `db_threshold_t` 数组，`alarm.c` 经 `db_threshold_get()` 读。只读 + `pthread_create` 的 happens-before = 无需加锁；**S6 若也要用阈值（over_count 要判超限），直接调 `db_threshold_get` 即可**，不要再查一次库、也不要自己缓存一份。
2. **`db_query()` 是 S5 为"带结果集的 SELECT"新增的**（`db_exec` 只处理无结果集的语句），带与 `db_exec` 相同的"断线重连一次"。S6 的聚合查询走它。
3. **告警判定在 DATA 入库循环里逐值进行，且无条件调用**：即使某个值 `db_insert_sensor` 失败也照做阈值判定——ALARM 是给客户端的实时通知，不该被 `sensor_data` 的写入失败连累；两边各自记日志。
4. **判定口径**：严格不等号（等于边界不算超标）、NULL 侧不检查、"没有配置行"= 该传感器不告警（光照）。`threshold` 字段给**被越过的那个边界**。
5. **ALARM 的 ts 是服务器时间**（`strftime` 直接生成，不走客户端输入专用的 `proto_datetime_ok`），与 DATA 里的 ts（客户端采集时刻）**允许不同**——宿主机/虚拟机的 2 秒偏差会让这两个数字看起来"对不上"，判读见问题 33。
6. **`alarm_send` 与 `proto_reply` 同构但各写各的**：JSON 文本与 `'\n'` 必须在同一次 `net_send` 里发出（拆开会被别的线程插队）。这是刻意保留的十行重复，不要为了"复用"去改 proto 的对外契约。
7. 本步跑过一次 **helgrind**（并发告警场景，`ERROR SUMMARY: 0 errors`，见 §十八），主要是为了验证 S5 新增的"工作线程只读阈值缓存"这条跨线程路径——结论是安全，靠的是"启动阶段填好 + 之后只读 + `pthread_create` 的 happens-before"，**不要**日后为了"保险"给它加锁（加锁反而会引入锁序问题）。memcheck 仍未跑（属 S7）；已知形态与 S4 相同：常驻工作线程的 TLS 表现为 `still reachable`，判读只看 `definitely lost` 是否为 0。
8. 日志里每条告警一行 `client_id=X 告警：sensor=n(名称) value=… >/< threshold=… level=… msg="…" ts=…`，S7 压测统计告警条数直接 `grep -c '告警：'` 即可（与 `alarm_log` 的行数应一致）。

---

## 二十、S6 阶段问题记录（统计分析与 ANALYSIS 推送）

### 问题 34：helgrind "进程已退出" 是误判 —— `pkill -x valgrind` 什么也没杀到

**现象**
第一次 helgrind 跑完（probe 收发正常），执行 `pkill -INT -x valgrind; pgrep -a valgrind` → **无输出**，据此判定"进程已退出"，但 `/tmp/hg6.log` 里只有 6 行头部、没有 `ERROR SUMMARY`。紧接着再起一个 helgrind 实例，服务器日志报：

```
[ERROR] bind() 端口 9000 失败：Address already in use
```

**定位**
`ss -ltnp | grep 9000` 显示端口属于 **pid=8288 且仍在监听**，`ps -ef` 里它跑得好好的；`cat /proc/8288/comm` 得到 `helgrind-amd64-`。

**原因**
pgrep/pkill 默认按**进程名（comm，最长 15 字符）**匹配，而 valgrind 载入 helgrind 工具后进程名被改写成 `helgrind-amd64-` —— `-x valgrind` 与 `-x server` **都匹配不到它**。这次没筛到任何进程，`pgrep` 也同样筛不到，我把它读成了"已退出"，于是一台仍在运行的服务器被当成了僵尸；报告没写完的真正原因是**进程根本还活着**（valgrind 的报告要等进程退出才落盘）。

**解决**
改用命令行匹配取 PID，再显式发信号：

```bash
PID=$(pgrep -f '[v]algrind.bin'); kill -INT $PID
```

（`[v]` 是为了让 pgrep 不匹配到承载这条命令的 ssh/bash 自己）。valgrind 会把 SIGINT 转发给服务器，服务器走完优雅退出后 valgrind 才写完整报告。判读口径已写进 `运行说明.md` §6。

### 问题 35：helgrind 抓到分析线程退出标志的数据竞争（**真实缺陷**）

**现象**
S6 首轮 helgrind：`ERROR SUMMARY: 2 errors from 2 contexts`，两条堆栈互为读写对：

```
Possible data race during write of size 4 at 0x19F7E0 by thread #1
   at 0x10E354: analysis_stop (analysis.c:250)   by main (main.c:190)
This conflicts with a previous read of size 4 by thread #6
   at 0x10E312: analysis_thread (analysis.c:233)
Address 0x19f7e0 is 0 bytes inside data symbol "g_stop"
```

**定位**
`g_stop` 是分析线程的退出标志：主线程在 `analysis_stop()` 里写、分析线程在循环里读，两者之间**没有任何同步**（当时的写法是 `static volatile int` + "分段 sleep 检查"）。

**原因**
`volatile` 只保证"这次访问不会被编译器优化掉"，**不构成线程间同步**、不建立 happens-before；C 标准下这是未定义行为。helgrind 报得没错，这是真实的并发缺陷，与 S2 那次 cJSON 全局是同一类。

**解决**
改成互斥锁 + 条件变量，用 `pthread_cond_timedwait` 做"定时等待 + 可被打断"：

```c
static pthread_mutex_t g_stop_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_stop_cond = PTHREAD_COND_INITIALIZER;
static int             g_stop      = 0;      /* 受 g_stop_lock 保护 */
```

`analysis_stop()` 里 `pthread_mutex_lock` → `g_stop = 1` → `pthread_cond_signal` → `unlock`。顺带把"退出最多等 1 秒"变成立即生效。重跑 helgrind 后这条竞争消失。

### 问题 36：并发首次建连时 libmysqlclient 内部被报数据竞争（真实缺陷，落在库的路径上）

**现象**
修掉问题 35 后重跑，仍有 `ERROR SUMMARY: 2 errors from 2 contexts`，但堆栈换了：

```
Possible data race during read/write of size 8 at 0x4F8BE40
   at 0x489CD0F: ??? (in libmysqlclient.so.21.2.42)        ← 库内部
   by mysql_real_connect
   by 0x10C870: db_conn (db.c:162)
   by db_insert_sensor … proto_handle_data                  （thread #5，工作线程）
This conflicts with a previous write … by thread #6
   by db_query … db_run_analysis … analysis_thread           （分析线程）
Address 0x4f8be40 is in the BSS segment of libmysqlclient.so.21.2.42
```

**定位**
两条堆栈的调用方分别是**工作线程**与**分析线程**的**首次**建连（`db_conn()` 里的 `mysql_real_connect`）。S4/S5 只有工作线程，且它们被"第一条 DATA"触发的时刻分散，没撞上；S6 加了分析线程，它在启动后第一次统计时必然建连。

**原因**
两个线程各自的首次建连之间**没有 happens-before 关系**（它们是兄弟线程；主线程在 `db_init` 里的那次建连只随"创建线程"这个动作建立了同步，管不到兄弟线程彼此）。helgrind 判定的是"无同步关系的两次访问"，不要求时间上真的重叠。竞争位置在 libmysqlclient 自己的 BSS 全局，不是我们的代码——但只要并发走这条库路径，就会被判定为竞争，而它是真实的未同步访问。

**解决**
给 db.c 的建连路径加一把锁 `g_conn_lock`（`mysql_close` 共用同一把），把"首次建连/关闭"串行化：

- 已有连接的**快路径完全不碰锁**（`if (t_conn != NULL) return t_conn;`），查询/写入性能不受影响；
- 建连是每个线程**一生一次**的罕见事件，串行化没有实际代价；
- 锁内不获取任何其它锁，**不引入锁序问题**。

重跑 helgrind（两个客户端并发收发 + 分析线程同时统计推送）：`ERROR SUMMARY: 0 errors from 0 contexts`。

### 问题 37：ANALYSIS 推送的统计值与库里存的值对不上（精度）

**现象**
首轮推送里出现：

```json
{"id":1,"avg":24.282609,…,"std":3.8810187113933439,"over_count":9}
```

而 `analysis_result` 里同一组存的是 `24.28 / 3.88`。

**定位**
`analysis_result` 的列是 `DECIMAL(10,2)`，入库时被舍到 2 位小数；推送走的是 cJSON 的 double 打印——cJSON 先用 `%1.15g` 尝试，往返不成立就退到 `%1.17g`，于是 `std` 打成 17 位有效数字的长串。

**原因**
不是缺陷（JSON 数值合法，误差也远小于验收要求的 0.05），但"客户端显示的数字"与"SQL 查出来的数字"不一致，联调时极易被当成 bug；客户端界面上也不好看。

**解决**
推送前按列精度规整（新增 `analysis_round2()`，舍入方向与 MySQL 的 DECIMAL 一致：远离零）。现在推送值与库值**逐位相同**，客户端的数字与数据库自查结果对得上。

### 问题 38：`ssh "mysql -e \"…\""` 引号嵌套，命令根本没执行

**现象**

```
$ ssh vmserver-lan "mysql -uiot -piot123456 machine_room -e \"
SELECT … FROM analysis_result;\""
/usr/bin/bash: eval: line 4: unexpected EOF while looking for matching `"'
```

**定位**
外层双引号（ssh 参数）+ 内层双引号（mysql -e）+ SQL 里的单引号（字符串字面量），三层嵌套把本地 shell 绕晕，远程拿到的是一条截断的命令。

**原因**
与问题 32 同类：**采集证据的方式本身没验过**。这次 shell 直接报错还算幸运；若它"恰好"执行了一条残缺但合法的查询，就会拿到错误证据而不自知。

**解决**
SQL 一律走 heredoc，既不碰引号嵌套、也不在命令行暴露密码：

```bash
ssh vmserver-lan "MYSQL_PWD=iot123456 mysql -uiot machine_room -t" <<'SQL'
SELECT …;
SQL
```

### 问题 39：`grep 'ANALYSIS'` 把脚本自己的汇总行也算进去（假阳性）

**现象**
S6-3 验收里 `grep 'ANALYSIS' s6d2.txt | tail -1` 打出来的"那条 ANALYSIS"，其实是脚本收尾的汇总行：

```
=== 汇总：发 DATA 0 条、指定消息 3 条；收到 ANALYSIS 2 条、其它回包 9 条 ===
```

**定位**
probe 的汇总行正文里含 "ANALYSIS" 三个字，被 `grep 'ANALYSIS'` 一并匹配，`tail -1` 取到的正是这最后一行。

**原因**
与问题 21（`grep 'ERROR'` 匹到正文里的"回 ERROR"）**完全同类**：用关键词筛日志时没把"正文里也会出现这个词"算进去。这次还叠加了一个更坏的后果——若我不核对，就会得出"S6-D2 没收到 ANALYSIS"的错误结论（实际收到了 2 条）。

**解决**
筛报文一律带上 JSON 的引号：`grep '"ANALYSIS"' 文件`。经验沿用问题 21：**用关键词提取证据前，先把"这个词还可能出现在哪里"想一遍**。

---

## 二十一、S6 验收结论

**产出**：新增 `src/analysis.c` / `src/analysis.h`（分析线程：周期统计 + 逐 client 组包推送 ANALYSIS + 可中断的条件变量等待）；改动 `db.c`/`db.h`（`db_run_analysis` 聚合查询与入库、结果内存副本与 `db_analysis_count/row/period` 三个读接口、`db_set_analysis_window`、新增建连锁 `g_conn_lock`）、`session.c`/`session.h`（新增 `session_snapshot_logged`：fd 与 client_id 同锁取齐）、`main.c`（getopt `-p/-t/-w`、创建分析线程、退出时 `analysis_stop` + `pthread_join` 再关库）。`make clean && make` **零警告零错误**。**接口规范 §3.1/§3.2/§4 零改动**（ANALYSIS 字段名与顺序逐字对齐 §3.1）。

**验收命令与实测输出**（同一台服务器；两台客户端脚本为仓库外 `s6_probe.py`，登录后按秒发 DATA 并打印全部回包）

| # | 命令 | 预期 | 实际 |
|---|---|---|---|
| S6-1 | 默认参数启动 `./server`，再 `python -u s6_probe.py 10.202.57.4 9000 S6-C --data 75`（75s ≥ 一个 60s 周期） | probe 每周期收到一条 ANALYSIS，JSON 字段与 §3.1 逐一对齐 | ✅ 收到 1 条（窗口 19:12:58~19:22:58）：`{"type":"ANALYSIS","period_start":"2026-09-22 19:12:58","period_end":"2026-09-22 19:22:58","sensors":[{"id":1,"avg":24.1,"max":29.3,"min":18.3,"std":3.56,"over_count":8},{"id":2,"avg":56.88,"max":78.6,"min":40.2,"std":11.03,"over_count":8},{"id":3,"avg":300,"max":538,"min":111,"std":123.09,"over_count":5},{"id":4,"avg":456.14,"max":779,"min":140,"std":206.66,"over_count":0}]}` —— 字段名、顺序、类型与 §3.1 示例逐项一致；窗口跨度 600s |
| S6-2 | `SELECT * FROM analysis_result WHERE client_id='S6-C' AND period_end='2026-09-22 19:22:58';` 与同窗口手工 SQL（`AVG/MAX/MIN/STDDEV_POP` + 阈值 CASE 求和，窗口取该行的 `period_start/period_end`）比对 | 每周期每个 client×sensor 一行；avg/max/min 与手工 SQL 误差 < 0.05 | ✅ 库中 id 133~136 四行：`24.10/29.30/18.30/3.56/8`、`56.88/78.60/40.20/11.03/8`、`300.00/538/111/123.09/5`、`456.14/779/140/206.66/0`；手工 SQL 同窗口得 `24.100000 / 56.881818 / 300.000000 / 456.136364` 与 `std 3.55911122715668 / 11.027842299612555 / 123.08792127433284 / 206.66008442819276`、`over_cnt 8/8/5/0` —— **max/min/over_count 逐位相同，avg/std 最大误差 0.0036 ≪ 0.05**；推送值与库值逐位相同 |
| S6-3 | `./server -t 15 -w 60`，`S6-D2` 连发 3 条 `{"type":"DATA","sensors":[{"id":1,"v":30.0},{"id":2,"v":70.0},{"id":3,"v":500},{"id":4,"v":600}]}` 后等一个周期 | 该组 over_count = 3 | ✅ 推送 `{"id":1,"avg":30,…,"over_count":3},{"id":2,…,"over_count":3},{"id":3,…,"over_count":3},{"id":4,…,"over_count":0}`（光照不告警恒 0）；库中 id 173~176 的 `over_count` = `3/3/3/0`，与推送一致 |
| S6-4 | 全部客户端断开后连等多个周期（窗口内旧数据过期后） | 不崩溃；无统计行、不推送（或推空数组，二选一并在运行说明注明） | ✅ 19:25:21 / 19:25:36 / 19:25:51 连续三个周期日志 `分析：窗口 … 内无数据，本周期不写 analysis_result`；`analysis_result` 行数稳定在 **192** 不再增长；服务器进程正常、无任何推送。口径选"**不推送**（不发空 sensors 数组）"，已注明在 `运行说明.md` §5.5 |
| S6-5 | `./server -t 15 -w 60` | 15 秒即产出统计，窗口为 1 分钟 | ✅ 启动横幅 `分析周期/窗口: 15s / 60s`；客户端连上后 **15.0s / 21.9s** 各收到一条 ANALYSIS；库中 `TIMESTAMPDIFF(SECOND, period_start, period_end)` = **60**；每 15 秒一期（19:23:21 → 19:23:36 → …） |

**附加验证（超出验收标准的部分）**

| 检查 | 结果 |
|---|---|
| **禁止把 A 的统计发给 B**（任务要求的红线） | ✅ 两个客户端并发：`S6-D1` 发 `20/50/300/500`、`S6-D2` 发 `30/70/500/600`，同一期（19:23:36~19:24:36）各自收到的 sensors 只含自己的值（D1 的 `avg` 全是对应的 20/50/300/500 且 `over_count` 全 0；D2 是 30/70/500/600 且超标组为 3） |
| **不在线的 client 只入库、不推送** | ✅ 19:24:36 期两个客户端都在线 → 各推 1 条；客户端 19:24:3x 断开后，19:24:51 期日志为 `共 8 组、入库 8 行；当前无在线客户端，不推送` —— 数据进了 `analysis_result`，但没人收推送 |
| **一期多 client 混合时只推在线的那一个** | ✅ 19:22:58 期窗口内有 5 个 client（HG-6/HG-6B/HG-6C/HG-6D/S6-C）共 **20 组**入库，而只给当时唯一在线的 S6-C 推了 1 条（日志 `在线 1 个客户端，推送 1 个（跳过 0）`） |
| **单样本 std 给 0 不给 null** | ✅ `S6-D1/D2` 每组只有一个样本，推送与库中 `std` 均为 **0**（`0` 而非 `null`，也不是空值） |
| **数据竞争检测（helgrind）** | ✅ `valgrind --tool=helgrind ./server -t 20 -w 60` + 两个客户端并发收发 + 分析线程统计推送，优雅退出后 `grep -c 'Possible data race'` = **0**、`ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 8217 from 186)`。过程中修掉两处真实竞争：退出标志（问题 35）、并发首次建连（问题 36） |
| **getopt 参数校验** | ✅ `-t abc`／`-p 99999`／`-h` 均打印"参数错误…" + 用法并以 `rc=1` 退出，不启动服务器；`-p/-t/-w` 默认值与 `分析周期/窗口` 横幅一致 |
| **优雅退出回归（S0~S5）** | ✅ SIGINT 后依次打印 `select 主循环已退出` → `分析线程已收到退出请求，退出` → `数据库连接已关闭（mysql_library_end 已调用）` → `收到退出信号，服务器正常退出`；`pgrep -ax server` 无残留、9000 端口释放 |
| 服务器错误日志 | ✅ 两个实例全程 `grep -c '\[ERROR\]'` = **0** |

---

## 二十二、S6 实现要点（供 S7/S8 参考，避免踩坑）

1. **分析线程用自己那条 MySQL 连接**：db 模块的 `__thread` 惰性建连会在它第一次调 `db_*` 时自动建立，**不要**把主线程的连接对象传过去（S4 要点 2 的老规矩）。
2. **`g_conn_lock` 只保护"首次建连/关闭"，快路径无锁**：不要为了"省一把锁"删掉它——删掉问题 36 的竞争就复活（helgrind 会报）。也不要把它扩成"保护整个查询"（那会把每线程一条连接的收益全抹掉）。
3. **退出顺序：`analysis_stop()` → `pthread_join()` → `db_shutdown()`**（main.c 收尾）。分析线程有自己的连接，必须在 `mysql_library_end()` 之前停稳；工作线程没有 join 机制，仍随进程退出（§九 的同一取舍）。
4. **over_count 与 ALARM 是同一套判定口径**（严格不等号、NULL 侧不检查、无配置不判），但**分别实现**在两处：`alarm.c` 的 C 判定与 `db.c` 的 `db_build_over_expr()` SQL 片段。改判定口径时**两处都要改**；S7 若发现两者不一致，先查这里。
5. **S6-4 的口径是"无数据不推送"**（不是推空 `sensors` 数组），对外承诺已写进 `运行说明.md` §5.5——客户端（任务B）不需要处理空数组。
6. **推送前复核 fd 归属**（`session_client_id_copy` 再比一次）：快照到推送之间 fd 可能被回收复用，这是"不把 A 的统计发给 B"的最后一道闸，别当成冗余代码删掉。
7. **valgrind 的启停姿势**：进程名是 `helgrind-amd64-`，用 `pgrep -f '[v]algrind.bin'` 取 PID 再 `kill -INT`，否则会像问题 34 那样"杀掉了一个不存在的进程"还浑然不觉。
8. **筛证据带引号**：报文用 `grep '"ANALYSIS"'`，日志级别用 `grep '\[ERROR\]'`（问题 21/39 是同一类假阳性）。
9. 本步的 getopt 只在启动时解析一次、之后参数只读，因此 `analysis_cfg_t` 可以直接指向 main 的栈变量：`pthread_create` 建立 happens-before，分析线程读到的一定是初始化好的值（与阈值缓存同一套理由）。

---

## 二十三、S7 阶段问题记录（压测与异常测试）

### 问题 40：分析线程的 MySQL 连接从未关闭 —— memcheck 报 definitely lost（**真实缺陷，本阶段唯一一处**）

**现象**

S7 补跑内存检测（S2 以来第一次在**完整功能**下跑，含 db.c 与分析线程）：

```bash
VALGRIND_LIB=/tmp/vg/usr/lib/x86_64-linux-gnu/valgrind \
  /tmp/vg/usr/bin/valgrind --tool=memcheck --leak-check=full --log-file=/tmp/mc7.log ./server
# 走完登录/DATA/告警/心跳/非法JSON/分析推送一整轮后 SIGINT 退出：
$ grep -A 6 'LEAK SUMMARY' /tmp/mc7.log
==4570==    definitely lost: 1,192 bytes in 1 blocks
==4570==    indirectly lost: 27,786 bytes in 21 blocks
==4570==      possibly lost: 117,192 bytes in 92 blocks
==4570==    still reachable: 0 bytes in 0 blocks
$ grep -c 'Invalid read\|Invalid write\|uninitialised' /tmp/mc7.log
0                              # 没有越界/未初始化读这类内存错误，只有泄漏
```

`definitely lost` 不为 0，与 S2 阶段（当时是 0）的结论**不一致**。

**定位**

① **把 45 条 loss record 按调用栈归因**（提取每条记录里含 `.c:` 的用户代码帧）：

| 归属 | 字节数 | 堆栈特征 |
|---|---|---|
| **分析线程** | **28,978（1,192 direct + 27,786 indirect）** | `analysis_thread → analysis_once → db_run_analysis → db_query → db_conn → mysql_init/mysql_real_connect/mysql_options` |
| 4 个常驻工作线程 | 115,912（possibly） | `pool_worker → net_task_run → proto_handle_data → db_insert_sensor → db_conn`，每条都写着 **"in 4 blocks"**（4 个线程各一份） |
| 4 个工作线程的 TLS | 1,280（possibly） | `pthread_create → pool_init → main`（S2 问题 25 的同一形态，只是每块从 288 涨到 320 字节——新增了 `__thread MYSQL *` 指针） |

② **代码级核对**：`db_shutdown()`（db.c:772）内只有一句 `db_conn_drop();`，注释写明"**主线程这条**"；而 `analysis_thread()` 的退出路径（analysis.c:268）只打一行日志就直接 `return NULL`——**没有任何关闭连接的调用**。

③ **对照实验**（决定性证据）：把分析周期调到 3600s，让分析线程**在整轮测试里不建连**：

```bash
$ /tmp/vg/usr/bin/valgrind --tool=memcheck --leak-check=full --log-file=/tmp/mc7b.log ./server -t 3600
# 同样跑一轮 DATA/告警（不跨分析周期），SIGINT 退出：
$ grep -A 6 'LEAK SUMMARY' /tmp/mc7b.log
==5369==    definitely lost: 0 bytes in 0 blocks          ← 归零
==5369==    indirectly lost: 0 bytes in 0 blocks          ← 归零
==5369==      possibly lost: 117,192 bytes in 92 blocks   ← 与实验组逐字节相同
```

关掉"分析线程建连"这一条路径，`definitely/indirectly lost` 双双归零，而 `possibly lost` **一字不差**。因果链闭合：泄漏的就是分析线程那一条连接，其余是常驻线程的既定形态。

**原因**

`db_conn()` 把连接存在**线程局部** `__thread MYSQL *tls_conn` 里，S4 定下的策略是"每线程一条连接，工作线程的连接随进程退出由内核回收"（§十五 要点 3）。这在 S4/S5 时是对的——那时没有线程会**真正退出**。

S6 给分析线程加了优雅退出（`analysis_stop()` → `pthread_join()`，见 §二十一），情况就变了：分析线程**真的会退出**，它的 TLS 随线程销毁，`tls_conn` 这个唯一的指针随之消失，那条 MySQL 连接（1,192 字节的 MYSQL 结构 + 27,786 字节的内部缓冲）就再没有任何指针指向它——Valgrind 归类为 `definitely lost`。

工作线程没有 join 机制、进程退出时仍然活着，它们的连接只表现为 `possibly lost`（既有口径，见问题 25）。

**影响面**：**运行期不累积**。分析线程只建一条连接（`db_conn()` 的快路径命中后不再新建），泄漏发生在它退出、进程即将结束的时刻。所以这不是"跑久了耗尽内存"，而是"退出时没释放干净"；但交付口径要求 `definitely lost` 为 0，必须修。

**解决（本次未实施——S7 按约定禁止改业务代码，此处只给出方案）**

必须在**分析线程自己**的退出路径上关（`tls_conn` 是线程局部的，线程一退出主线程就再也拿不到它了，放在 `db_shutdown()` 里够不着）：

1. db.h **新增**一个公开接口，例如 `void db_close_this_thread(void);`，实现就是现有 static 函数 `db_conn_drop()` 的直接转发；
2. `analysis_thread()` 在 `log_info("分析线程已收到退出请求，退出")` **之前**调用它一次；
3. 属"新增接口"而非改动冻结签名，与 S5 的 `db_threshold_get()`、S6 的 `db_analysis_*()` 同一套路（§2.3 的接口一字不动）。

> 修完后 `definitely lost` 与 `indirectly lost` 应双双回到 0；`possibly lost` 仍会是 117,192 字节（4 个常驻工作线程的 TLS + 连接），**那是判读口径内的正常形态，不要试图消除**（要消除就得给线程池加 shutdown/join，属规划未要求的复杂度）。

---

### 问题 41：排队的启动命令在旧实例退出后"补位"，看起来像服务器自己重启了

**现象**

S7 收尾停服务器：`pkill -INT -x server` 之后 1 秒，日志里冒出**新实例**的启动横幅，`pgrep` 显示 pid=4233 正监听 9000：

```
[2026-09-22 20:03:08] [INFO ] 收到退出信号，服务器正常退出      ← 旧实例（2581）退出
[2026-09-22 20:03:09] [INFO ] 服务器已就绪，按 Ctrl-C 退出      ← 1 秒后又起来一个
```

而我 19:38 只打算启动一次。

**定位**

```bash
$ ps -o pid,ppid,lstart,cmd -p 4233
    PID    PPID                  STARTED CMD
   4233    4232 Tue Sep 22 20:03:08 2026 ./server
$ ps -o pid,lstart,cmd -p 4232
   4232 Tue Sep 22 20:03:08 2026 bash -c cd ~/server-linux && setsid nohup ./server >> /tmp/s7-server.log 2>&1 < /dev/null &
```

补位实例的**父 bash 启动于 20:03:08**——正是旧实例退出的那一刻；命令行则是我 19:38 发出的**第二条**启动命令。再核对日志：全程只有 18 行的首次横幅与 4958 行的第二次横幅，**没有任何 bind 失败记录**。

**原因**

两条启动命令是**串行**发出的，而第一条的 ssh **没有 `timeout` 包裹**（第一条只是"拉起 + 立即返回"的写法，见问题 23）。后台进程持有会话（问题 23），这条 ssh 一直挂着不返回；bash 串行执行，第二条自然要排队等它。

而第一条 ssh 直到 **server 进程退出**才返回——于是：

```
19:38:04  第一条 ssh 拉起 server(2581)，ssh 会话挂着（后台进程持有它）
   …11 分钟压测、kill -9、半行重连、memcheck 全在这期间，ssh 一直挂着…
20:03:08  pkill -INT 停掉 2581 → 会话释放 → 第一条 ssh 返回
20:03:08  排队的第二条 ssh 立即执行 → 此刻端口刚好空了 → 新实例 4233 成功 bind
```

**不是"服务器自己重启"，是排队命令补位**。若旧实例没退出，排队的第二条会 bind 失败并退出（日志里没有 bind 失败记录，正反两面都印证了这条链条）。

**解决**

- 启动/停止类操作**只发一条命令**，且一律用 `timeout N ssh …` 包裹，避免第二条在队列里等着；
- 发完停止命令必须**复核**：`pgrep -ax server` 为空 **且** `ss -ltn | grep 9000` 无监听，不能只看"我已经发了停止命令"；
- 启动后同样要 `pgrep -ax server` 确认**只有一个**实例（多实例会有一个 bind 失败，日志里能看到 `Address already in use`）。

---

### 问题 42：S7-4 的判据本身出错——"半截尾巴"的回包码与预期不符

**现象**

S7-4 设计的判据是：重连后只发上一连接半截 JSON 的"尾巴" `25.5}]}`（无换行）——

- 若服务器**残留**了上次的半行，拼接后就是一条完整 DATA → 无回包（静默入库）；
- 若**没残留**，`25.5}]}` 是残缺 JSON → 应回 `ERROR code=1`。

实测收到的是 **code=2**：

```
[+   0.32s] -> （原始字节，无换行）'25.5}]}'
[+   2.33s] -> {"type": "DATA", "sensors": [{"id": 1, "v": 21.5}, ...]}
[+   2.33s] <- {"type":"ERROR","code":2,"msg":"unknown type"}
```

**定位**

这条"尾巴"与紧随其后的正常 DATA **同属一条连接**，按 `\n` 分帧必然拼成一行：

```
25.5}]}{"type":"DATA","sensors":[{"id":1,"v":21.5},...]}
```

`proto_handle`（proto.c:344）用 `cJSON_Parse` 解析它 —— cJSON 默认**不要求整行被消费完**（等价于 `require_null_terminated=0`），它解析出开头的数字 `25.5` 就成功返回；紧接着 `cJSON_GetObjectItemCaseSensitive(root, "type")` 在一个**数字节点**上当然是 NULL → 走 code=2 分支（proto.c:354）。

**原因**

判据假设"残缺 JSON 一律回 code=1"，但这个输入**并不残缺**：它是"**合法 JSON 值 + 尾部垃圾**"。接口规范 §3.1/§3.2 只定义了两条：

- `{bad` 这类**解析失败** → code=1 `malformed json`（§3.3 明文要求）
- type 缺失/非字符串/未知 → code=2 `unknown type`

本情形规范未定义，服务器按现有约定回 code=2，**不算违反规范**。真正的错在判据——我又犯了问题 28/32 那一类错误：**拿"我以为的码"当断言，而没有先用反证确认**。

**解决**

- 判据改用**服务端日志里的原始行文本**。S7-4 的决定性证据是重连后这一行：

  ```
  [2026-09-22 19:58:32] [INFO ] fd=6 [工作线程] 解析消息：{"type": "LOGIN", "client_id": "PC-10"}
  ```

  它**逐字干净、没有任何前缀残留**——若缓冲真的残留了那半行，这里打印的会是被污染的行（`{"type":"DATA","sensors":[{"id":1,"v":{"type": "LOGIN"…`），而且登录必然失败、收不到 `LOGIN_ACK`。
- 顺带确认一个**可选的收紧点（本次未改，供 S8 决定）**：若要严格区分"合法值 + 垃圾"与真正的畸形输入，把 `cJSON_Parse(line)` 换成 `cJSON_ParseWithOpts(line, &end, 1)`（要求整行消费完）即可，届时 `25.5}]}` 会归入 code=1。规范没要求，属实现细节。

---

### 观察（非问题）：统计窗口按 `collect_time` 过滤时，会把"未来时间"的行算进来

S7-1 统计中途出现 `client_id = S5-A` 的 4 行落在压测窗口内，一度以为是串包。查证后是**接口规范 §3.3 的测试向量**：

```sql
SELECT id, client_id, sensor_value, collect_time FROM sensor_data WHERE client_id='S5-A' ORDER BY id DESC LIMIT 4;
-- 212~215 行：collect_time = 2026-09-28 15:00:01   ← 规范 §3.3 里 DATA 的 ts 就是它
```

服务器按规范"ts 合法就用该时间入库"，行为正确；但用 `collect_time >= '…' AND <= '…'` 做窗口过滤时，**未来时间戳的行会一直落在窗口内**。统计口径要么显式限定 `client_id IN (...)`（本次做法），要么在文档里注明"client 时间可信"的前提。

---

## 二十四、S7 验收结论

**产出**：**本步零业务代码改动**（S7 是测试步，不改任何 `src/`）。仅在仓库外新增压测脚本 `s7_probe.py`（三个 profile 取值域互不重叠 + 半行/尾巴两条异常用例）。`make clean && make` 仍为**零警告零错误**，**接口规范 §3.1/§3.2/§4 零改动**。

**验收命令与实测输出**（同一台服务器 PID 2581；三个 probe 在宿主机上并行跑，跨机经桥接 `10.202.57.4`）

| # | 命令 | 预期 | 实际 |
|---|---|---|---|
| S7-1 | `./server` + 三个 probe 并行 600s：<br>`python -u s7_probe.py 10.202.57.4 9000 PC-0{1,2,3} --profile p{1,2,3} --data 600` | 3 客户端并发 10 分钟无崩溃；三 client 行数大致相等、数值各自正确 | ✅ 三个 probe 各 `发 DATA 300 条`（600s÷2s）；**PC-01/PC-02 各收 ANALYSIS 10 条、0 条 ALARM，PC-03 收 ANALYSIS 10 条、ALARM 900 条，三者校验失败均为 0 项**；服务器进程存活、`grep -c '\[ERROR\]'` = **0** |
| | 期间 `SELECT client_id,sensor_type,COUNT(*),MIN(sensor_value),MAX(sensor_value) … GROUP BY` | 每 client×sensor 行数一致、数值落在本机域 | ✅ **12 行全部 cnt=300**（每 client 1200 行）；数值域 `PC-01 20.00~21.90 / 42.00~44.00 / 120~180 / 200~300`、`PC-02 23.00~24.90 / 50.00~54.00 / 250~320 / 400~500`、`PC-03 30.00~31.90 / 65.00~69.00 / 450~520 / 600~700`，**与三个 profile 逐条吻合** |
| | 跨域查询（把 A 的域套到 B 上查，应 0 行） | 无串包 | ✅ 返回**空集**；`alarm_log` 增量 `PC-03 HIGH 600 + MID 300 = 900`，**PC-01/PC-02 零条**（告警也没串） |
| S7-2 | 压测中两次 `ls /proc/$(pgrep -x server)/fd \| wc -l` 与 `lsof -p <pid> \| wc -l` | 稳定不增长（±2 内） | ✅ 计数①(19:59:28) `fd=12 / lsof=28 / RSS=8504KB`；计数②(20:01:29) `fd=12 / lsof=28 / RSS=8504KB` —— **差值 0 / 0 / 0**。10 分钟大压测中另有两次 `fd=13 → 13`（19:41:31 / 19:46:11） |
| | 压测结束三连接断开后复查 | 回落到基线 | ✅ `fd=10`，与**压测前基线 10 完全一致**；`lsof 29→26`（正好少 3 条连接） |
| S7-3 | PC-04/PC-05 在线时对 PC-04 `Stop-Process -Force`（等价 kill -9） | ≤90s 回收；其他 probe 正常 | ✅ kill 于宿主机 19:52:33，**服务器 19:52:35 即回收**：`[WARN] recv() 失败 fd=6：Connection reset by peer，回收连接` → `连接已关闭 fd=6（剩余会话 1 个）`；另一客户端 PC-05 全程不受影响，跑满 `发 DATA 75 条、收 ANALYSIS 3 条、ALARM 225 条、校验失败 0 项` |
| | **补充：真静默 90s 超时**（kill -9 会发 RST，测不到超时路径）——虚拟机内起 PC-06，`kill -STOP` 冻结（socket 保持打开、进程不再响应） | 90s 后被超时回收 | ✅ 最后一条 DATA `19:55:45` → `[2026-09-22 19:57:15] [WARN] 会话超时：fd=6 已 90s 无任何消息（阈值 90s）——本轮巡检回收该连接` —— **正好 90 秒**；冻结期间分析线程仍按周期给它推 ANALYSIS（会话尚未超时），回收后 `fd` 回落至 10 |
| S7-4 | ① `python -u s7_probe.py … PC-10 --half-line`（发半截 JSON 不换行后断开）<br>② `python -u s7_probe.py … PC-10 --profile p1 --data 8`（重连并发 DATA） | 重连后 LOGIN_ACK，后续 DATA 正常入库 | ✅ 步骤①服务器**只处理了 LOGIN，无任何 DATA 处理记录**（半行只进缓冲、未成帧）；步骤②重连立即 `{"type":"LOGIN_ACK","status":0}`，随后 4 条 DATA 全部 `入库 4/4 行（跳过 0）` |
| | 服务器日志里重连那条 LOGIN 的原始文本 | 无上次半行的前缀残留 | ✅ `19:58:32 [工作线程] 解析消息：{"type": "LOGIN", "client_id": "PC-10"}` —— **逐字干净**（若缓冲残留，此处会打印被污染的行、登录必然失败）；库中 `PC-10` 共 **16 行**（4×4），数值 `20.00~21.50 / 42.10~43.80 / 152~178 / 241~287` 与脚本发送逐条一致 |

**附加验证（超出验收标准的部分）**

| 检查 | 结果 |
|---|---|
| **内存泄漏检测（memcheck，本阶段首次在完整功能下跑）** | ⚠️ **发现一处真实缺陷**：`definitely lost: 1,192 bytes in 1 blocks` + `indirectly lost: 27,786 bytes in 21 blocks`，45 条 loss record 全部归因到**分析线程的 MySQL 连接**（堆栈 `analysis_thread → db_run_analysis → db_query → db_conn → mysql_init`）。详见**问题 40** |
| **对照实验（-t 3600 让分析线程不建连）** | ✅ `definitely lost: 0`、`indirectly lost: 0`，而 `possibly lost: 117,192 bytes in 92 blocks` **与实验组逐字节相同** —— 因果链闭合，同时证明 possibly lost 与分析线程无关 |
| **possibly lost 的构成判读** | ✅ 117,192 字节 / 92 块 = 4 个常驻工作线程的 **TLS（1,280 字节，问题 25 的同一形态）** + 4 条工作线程 MySQL 连接（115,912 字节，每条记录都写着 "in 4 blocks"）。**按既定口径不是泄漏**（线程随进程退出、无 join 机制） |
| **非泄漏类内存错误** | ✅ `grep -c 'Invalid read\|Invalid write\|uninitialised'` = **0** —— 没有越界访问、没有未初始化读 |
| 服务器错误日志 | ✅ 全程（压测 + 异常测试 + memcheck 两轮）`grep -c '\[ERROR\]'` = **0** |
| 优雅退出回归（S0~S6） | ✅ SIGINT 后依次打印 `select 主循环已退出` → `分析线程已收到退出请求，退出` → `数据库连接已关闭（mysql_library_end 已调用）` → `收到退出信号，服务器正常退出`；`pgrep -ax server` 无残留 |

> **S7 结论**：S7-1~S7-4 四条验收标准**全部通过**。唯一未达标项是**附加**的内存检测：问题 40 的 `definitely lost`（分析线程的连接）须在 S8 收尾前修掉（修法是新增 `db_close_this_thread()` 并在 `analysis_thread` 返回前调用，见问题 40 的"解决"）。S7 按约定**未改业务代码**。

---

## 二十五、S7 实现要点（供 S8/S9 参考，避免踩坑）

1. **memcheck 必须在"完整功能"下跑**：S2 那次 `definitely lost = 0` 是在**还没有 db.c** 的年代得到的，不能拿来给 S7 背书。一接上分析线程的连接，问题 40 就露出来了。S8/S9 再跑泄漏检测，务必用**默认参数**让分析线程真的建连、真的退出（`-t 3600` 那种跑法只能当对照实验，不能当验收）。
2. **判读口径（复述）**：只认 `definitely lost` 与 `indirectly lost`；`possibly lost` 里的 117,192 字节是 4 个常驻工作线程的 TLS + 各自一条 MySQL 连接，**不是泄漏**。S8 看到这个数字不要当成新问题。
3. **对照实验是定位泄漏归属的利器**：把可疑路径用参数关掉（本次 `-t 3600`），看哪个计数归零。比逐条读 45 个 loss record 快得多，也比 ASan 更能证明因果。
4. **压测脚本的三个 profile 取值域互不重叠**，是"数据互不串"的判据：p1/p2 全在阈值内（**收到任何 ALARM 即为串包**），p3 三项超标（每条 DATA 恰好 3 条 ALARM）；再要求 ANALYSIS 的 avg/max/min 落在本机域内。一次压测同时覆盖入库、告警、统计三条推送路径的隔离性。
5. **"半行残留"类判据别用回包码**：`25.5}]}` 这种"合法 JSON 值 + 尾部垃圾"会被 `cJSON_Parse` 解析成功（不要求整行消费完），走 code=2 而非 code=1（问题 42）。**决定性证据是服务端日志里打印的原始行文本**（`[工作线程] 解析消息：…`）。
6. **启动/停止服务器只发一条命令并加 `timeout`**：本次两条启动命令串行排队，第一条 ssh 挂到旧实例退出才返回，第二条随即补位把服务器又拉了起来（问题 41）。停服后必须复核 `pgrep -ax server` 为空**且** `ss -ltn | grep 9000` 无监听。
7. **S7 新增的临时工具都不入库**：`s7_probe.py`（宿主机工作区根目录，仓库外）。仓库内仍只有 S0~S6 的交付物。
