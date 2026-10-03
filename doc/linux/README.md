# Linux 使用教程

[中文](README.md) | [English](README.en.md)

将手机的蓝牙音频通过 AirPlayQt 发送到 AirPlay 接收器或 AirPlay 立体声组合。

## 1. 安装依赖

以下命令适用于已更新的 Arch Linux x86_64。需要 PipeWire 1.4+、WirePlumber 0.5+；GUI 需要运行中的桌面用户会话，无桌面 CLI 见第 7 节。手机和电脑通过蓝牙连接；电脑与 AirPlay 需要在能够互相通信的局域网中。

```sh
sudo pacman -S --needed qt6-base openssl libplist pipewire pipewire-audio wireplumber bluez bluez-utils avahi libcap
sudo systemctl enable --now bluetooth.service avahi-daemon.service
systemctl --user start pipewire.service wireplumber.service
```

检查服务状态。AAC 接收还需要 PipeWire 的 AAC 插件；其他发行版的软件包可能不同。

```sh
systemctl status bluetooth.service avahi-daemon.service
systemctl --user status pipewire.service wireplumber.service
pacman -Ql pipewire-audio | grep libspa-codec-bluez5-aac.so
```

## 2. 设置程序权限

以下步骤假设 AirPlayQt 已安装到 `/usr/local/bin/AirPlayQt`。若安装位置不同，请替换命令中的路径。

```sh
sudo setcap cap_net_bind_service=ep /usr/local/bin/AirPlayQt
getcap /usr/local/bin/AirPlayQt
```

最后一条命令应显示 `cap_net_bind_service=ep`，用于绑定 AirPlay 所需的 UDP 319/320 端口。每次重新安装或替换程序后，重新执行 `setcap`。

## 3. 配对手机

在 KDE 蓝牙设置中启用蓝牙并允许被发现。在手机蓝牙设置中选择电脑，确认双方验证码，然后在电脑上将手机设为信任。

也可以在终端运行 `bluetoothctl`，逐行输入以下命令。配对完成前保持该终端打开：

```text
power on
agent KeyboardDisplay
default-agent
pairable on
discoverable on
```

若提示 agent 已注册，继续执行 `default-agent`。手机发起配对后，电脑若显示 `Confirm passkey`，核对一致后输入 `yes`；若显示 `Enter passkey`，输入手机显示的六位数字。

配对成功后，在同一终端输入以下命令。将 `PHONE_MAC` 替换为手机蓝牙地址，例如 `AA:BB:CC:DD:EE:FF`；可用 `devices` 查看地址。

```text
trust PHONE_MAC
connect PHONE_MAC
info PHONE_MAC
quit
```

确认设备信息包含 `Paired: yes`、`Trusted: yes` 和 `Connected: yes`。AirPlayQt 的输入列表用于选择音源，配对和连接在系统蓝牙设置中完成。

## 4. 将蓝牙音频设为采集输入

如果系统已配置所有手机作为采集输入（例如定制的 archiso 镜像），跳过本节。

复制已安装的配置示例，然后编辑复制的文件。如果目标文件已存在，直接编辑它，保留已有规则。

```sh
mkdir -p ~/.config/wireplumber/wireplumber.conf.d
cp -n /usr/local/share/doc/AirPlayQt/51-airplayqt-bluetooth.conf.example ~/.config/wireplumber/wireplumber.conf.d/51-airplayqt-bluetooth.conf
nano ~/.config/wireplumber/wireplumber.conf.d/51-airplayqt-bluetooth.conf
```

选择一种匹配方式，保留文件中的 `bluez5.media-source-role = "input"`：

|用途| `node.name` |
| --- | --- |
|仅指定手机；地址中的冒号改成下划线| `"~bluez_input.AA_BB_CC_DD_EE_FF.*"` |
|所有手机| `"~bluez_input.*"` |

停止音频播放后应用配置，再重新连接手机。这会短暂中断桌面音频。

```sh
systemctl --user restart wireplumber
```

该设置使手机音频供应用采集，避免同时从电脑音箱播放。可用 `wpctl status` 查看输入设备；某些设备需要手机开始播放后才出现。

## 5. 发送到 AirPlay

以普通桌面用户启动已安装的程序，不要使用 `sudo`：

```sh
/usr/local/bin/AirPlayQt
```

1. 在手机上播放音乐，将音频输出选为电脑（例如 `archlive`）。
2. 在 AirPlayQt 输入列表中选择该手机，左右输入声道选择 FL/FR。
3. 选择连接 AirPlay 所在局域网的网卡，扫描并选择接收器。使用立体声组合时，先通过接收器支持的应用建立组合，再在 AirPlayQt 中选择组合的两个成员并确认左右声道。
4. 点击“开始”，调整音量；需要时使用静音。
5. 更换输入设备前先点击“停止”；结束播放时点击“停止”或关闭程序。

可以先开始发送，再让手机播放。手机暂停或断开时程序保持发送静音；同一手机恢复后自动接流。默认预缓冲为 40 ms，可先保持默认值。

### 手机音量键

发送期间，当前所选手机的蓝牙音量请求会直接调整 AirPlay 音量，立体声组合两端同步。绝对音量 `0` 对应静音（`-144 dB`），`1–127` 映射为 `-30 + 30 × 值 / 127 dB`；只有加减按键时，每次按下或重复请求调整 `1 dB`，释放不重复调整。相对按键等待 100 ms，同一窗口的绝对音量优先。应用不额外缩放 PCM，也不把 AirPlay 音量反向写回手机。

初次连接和重连只建立音量状态，不覆盖 AirPlay 当前音量；停止、换设备或断连会丢弃待处理按键。设备必须通过 BlueZ 暴露绝对音量或 AVRCP 按键；如果手机只在本机改变音频幅度，应用无法从音频中还原按键。

绝对音量不需要读取输入设备。仅加减按键需要读取对应的 AVRCP 输入节点；若日志提示 `Cannot read AVRCP volume keys`，可安装以下可选规则，授权当前本地桌面用户访问蓝牙 AVRCP 节点：

```sh
sudo install -m 0644 /usr/local/share/doc/AirPlayQt/70-airplayqt-avrcp.rules.example /etc/udev/rules.d/70-airplayqt-avrcp.rules
sudo udevadm control --reload-rules
```

随后断开并重新连接手机。应用不会自行修改系统权限，也无需以 root 运行或加入可读取所有键盘的 `input` 组。无法读取或唯一识别 AVRCP 设备时会记录日志，音频继续播放。同一适配器上的同名设备可能无法区分；普通电脑键盘不参与透传。应用不独占按键，桌面环境仍可能响应相同媒体键。

## 6. 常见问题

### iPhone 搜不到电脑

确认电脑已开启“可被发现”，并让 iPhone 停留在“设置 → 蓝牙”页面。发现窗口超时后，重新执行 `discoverable on`。

若仍搜不到，可临时将适配器类别设为音响。先用 `sudo btmgmt info` 确认适配器编号；以下 `0` 对应 `hci0`。随后重新关闭、开启可被发现并搜索。

```sh
sudo btmgmt --index 0 class 4 20
```

这是临时设置，重启后可能恢复。已固化音响类别的 archiso 镜像无需此步骤。

### 手机连上即断

检查配对及信任状态，再重新连接。若日志出现 `a2dp.c:auth_cb() Access denied`，同时检查 KDE 是否有待确认的授权提示。

```sh
bluetoothctl info PHONE_MAC
bluetoothctl trust PHONE_MAC
bluetoothctl connect PHONE_MAC
journalctl -b -u bluetooth.service --no-pager -n 80
```

### 已连接但没有声音

- 将手机输出切回手机本身，再选电脑并恢复播放。
- 用 `wpctl status` 检查输入；确认第 4 节规则匹配正确，并在 AirPlayQt 中选择该手机而非耳机麦克风。
- 查看 AirPlayQt 日志：`codec=aac` 表示当前使用 AAC；SBC 也可接收。客户端实际采样率应为 44100 Hz；源端采样率可不同，`unknown` 表示未取得该信息。
- 若提示速率控制不支持或客户端格式不匹配，检查 PipeWire 是否为 1.4+，并检查自定义格式／重采样配置。

### 找不到 AirPlay 或无法开始

确认 Avahi 正常运行、网卡选择正确、局域网允许组播通信；检查是否启用了访客网络或客户端隔离。手动填写接收器地址时也需要 Avahi。

```sh
systemctl status avahi-daemon.service
getcap /usr/local/bin/AirPlayQt
sudo ss -lunp 'sport = :319 or sport = :320'
```

UDP 319/320 绑定失败时，确认程序权限正确且端口未被其他进程占用。换网卡或网络地址变化后，重新选择网卡并开始发送。

### 配置文件位置

AirPlayQt 设置保存在 `$XDG_CONFIG_HOME/AirPlayQt.json`；未设置该环境变量时为 `~/.config/AirPlayQt.json`。手动编辑前先关闭程序。

## 7. 独立 CLI（前台运行）

`AirPlayQtCli` 不读写 GUI 配置，必须手工指定输入设备 ID 和接收端 IPv4。

### 单独构建和安装

依赖为 Qt 6.5+、OpenSSL、libplist 2.7+ 和第 1 节的 Linux 音频组件。

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf qt6-base openssl libplist pipewire pipewire-audio wireplumber bluez bluez-utils avahi libcap
cmake -S . -B build/linux-cli -G Ninja -DCMAKE_BUILD_TYPE=Release -DAIRPLAY_BUILD_STANDALONE=OFF -DAIRPLAY_BUILD_CLI=ON -DAIRPLAY_BUILD_VST3=OFF -DBUILD_TESTING=OFF
cmake --build build/linux-cli --target AirPlayQtCli
sudo cmake --install build/linux-cli --prefix /usr/local
sudo setcap cap_net_bind_service=ep /usr/local/bin/AirPlayQtCli
getcap /usr/local/bin/AirPlayQtCli
```

每次替换可执行文件后重新设置权限。手动 IP 发送仍需要运行 Avahi 来发布会话控制服务，并需要 UDP 319/320 端口权限。

### 无桌面音频环境

先按第 1、3、4 节启用系统蓝牙及 Avahi、配对并信任手机、配置 `bluez5.media-source-role = "input"`。

```sh
systemctl --user start pipewire.service wireplumber.service
```

纯 SSH 会话没有活动的本地 seat 时，可由用户在 `~/.config/wireplumber/wireplumber.conf.d/52-airplayqt-headless.conf` 中添加以下配置；若文件已经存在，合并配置而不是覆盖。此设置针对专门用于音频的用户，避免其他用户的音频会话同时争用蓝牙。

```text
wireplumber.profiles = {
  main = {
    monitor.bluez.seat-monitoring = disabled
  }
}
```

这是 WirePlumber 提供的[无桌面蓝牙配置](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/bluetooth.html#logind-integration)。修改后停止播放并执行 `systemctl --user restart wireplumber`，然后重新连接手机。应用不会自动创建配置、启动系统服务、执行配对或安装自启动服务。

第 5 节 AVRCP 示例中的 `uaccess` 面向活动本地用户，不能保证为 SSH 用户授权。无桌面环境若需要相对音量按键，由管理员单独授予运行用户读取对应蓝牙 AVRCP 输入节点的权限；不要授予所有键盘节点权限。绝对音量仍经 BlueZ D-Bus 转发；按键不可读时记录日志，音频继续运行。

### 查询与发送

```sh
/usr/local/bin/AirPlayQtCli --help
/usr/local/bin/AirPlayQtCli --version
/usr/local/bin/AirPlayQtCli --list-devices
/usr/local/bin/AirPlayQtCli --list-interfaces
```

`--list-devices` 输出 ID、名称及连接状态。启动和设备列表查询都只检查首轮系统查询结果：指定设备不存在、查询失败或超过 5 秒未完成就退出，不等待设备以后上线。查询过程中设备目录变化而导致首轮 BlueZ 快照失效时也退出，请重新运行。已配对且仍在目录中的断开手机可以选中，之后保持与 GUI 相同的静音等待行为。

将下面的设备 ID 与 IP 换成实际值。蓝牙 ID 包含适配器地址和手机地址，使用列表中的完整值。单台接收端：

```sh
/usr/local/bin/AirPlayQtCli --device 'bluez:00:11:22:33:44:55/AA:BB:CC:DD:EE:FF' --receiver 192.168.8.9
```

已在接收端系统中建立的双机立体声组合：

```sh
/usr/local/bin/AirPlayQtCli --device 'bluez:00:11:22:33:44:55/AA:BB:CC:DD:EE:FF' --receiver 192.168.8.9:7000 --receiver 192.168.8.10:7000
```

可选显式网卡绑定及交换输入声道：

```sh
/usr/local/bin/AirPlayQtCli --device 'bluez:00:11:22:33:44:55/AA:BB:CC:DD:EE:FF' --receiver 192.168.8.9 --interface eth0 --local-ip 192.168.8.20 --left 2 --right 1
```

| 参数 | 行为 |
| --- | --- |
| `--device ID` | 发送时必填；精确匹配稳定设备 ID，不自动换设备 |
| `--receiver IPv4[:port]` | 发送时必填；一至两个不同端点，默认端口 7000；不扫描接收端 |
| `--interface NAME --local-ip IPv4` | 必须成对提供；省略时使用系统路由；指定后失效不切换其他网卡 |
| `--left N --right N` | 默认 1/2（FL/FR）；左右均可选择 1 或 2，允许相同 |
| `--list-devices`、`--list-interfaces` | 分别单独使用，输出查询结果后退出 |
| `--help`、`--version` | 分别单独使用，不初始化音频 |

除 `--receiver` 外不允许重复参数；拒绝未知参数、位置参数及查询与发送参数混用。时间参数沿用现有默认值，不加载 GUI 保存的高级设置。列表与帮助写到 stdout；带 UTC 时间的英文状态、错误和重试日志写到 stderr，不持续输出音频统计。

首次设备检查、采集初始化或 AirPlay 建连失败，直接退出。首次进入发送状态（包括发送静音）后，AirPlay 会话中断、失败或被其他发送端抢占时，清理旧会话后等待 2 秒，用原参数无限重试整个接收端组合。手机暂停、断开或音频节点暂时失效，沿用 GUI 的静音及采集恢复逻辑，不视作 AirPlay 会话失败。重建会话的音量初始化也沿用现有会话行为。

Ctrl+C 或 SIGTERM 取消重试，停止采集并清理网络会话后退出。退出码为：查询成功 `0`，初始化/运行失败 `1`，参数错误 `2`，SIGINT `130`，SIGTERM `143`。
