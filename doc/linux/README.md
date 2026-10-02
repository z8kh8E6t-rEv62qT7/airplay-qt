# Linux 使用教程

[中文](README.md) | [English](README.en.md)

将手机的蓝牙音频通过 AirPlayQt 发送到 AirPlay 接收器或 HomePod 立体声组合。

## 1. 安装依赖

以下命令适用于已更新的 Arch Linux x86_64。需要 PipeWire 1.4+、WirePlumber 0.5+，以及运行中的桌面用户会话。手机和电脑通过蓝牙连接；电脑与 HomePod 需要在能够互相通信的局域网中。

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

最后一条命令应显示 `cap_net_bind_service=ep`，用于绑定 HomePod 所需的 UDP 319/320 端口。每次重新安装或替换程序后，重新执行 `setcap`。

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

配对成功后，在同一终端输入以下命令。将 `PHONE_MAC` 替换为手机蓝牙地址，例如 `08:C7:B5:48:7D:0C`；可用 `devices` 查看地址。

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
|仅指定手机；地址中的冒号改成下划线| `"~bluez_input.08_C7_B5_48_7D_0C.*"` |
|所有手机| `"~bluez_input.*"` |

停止音频播放后应用配置，再重新连接手机。这会短暂中断桌面音频。

```sh
systemctl --user restart wireplumber
```

该设置使手机音频供应用采集，避免同时从电脑音箱播放。可用 `wpctl status` 查看输入设备；某些设备需要手机开始播放后才出现。

## 5. 发送到 HomePod

以普通桌面用户启动已安装的程序，不要使用 `sudo`：

```sh
/usr/local/bin/AirPlayQt
```

1. 在手机上播放音乐，将音频输出选为电脑（例如 `archlive`）。
2. 在 AirPlayQt 输入列表中选择该手机，左右输入声道选择 FL/FR。
3. 选择连接 HomePod 所在局域网的网卡，扫描并选择接收器。使用立体声组合时，先在 Apple 家庭 App 中建立组合，再在 AirPlayQt 中选择组合的两个成员并确认左右声道。
4. 点击“开始”，调整音量；需要时使用静音。
5. 更换输入设备前先点击“停止”；结束播放时点击“停止”或关闭程序。

可以先开始发送，再让手机播放。手机暂停或断开时程序保持发送静音；同一手机恢复后自动接流。默认预缓冲为 40 ms，可先保持默认值。

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

### 找不到 HomePod 或无法开始

确认 Avahi 正常运行、网卡选择正确、局域网允许组播通信；检查是否启用了访客网络或客户端隔离。手动填写接收器地址时也需要 Avahi。

```sh
systemctl status avahi-daemon.service
getcap /usr/local/bin/AirPlayQt
sudo ss -lunp 'sport = :319 or sport = :320'
```

UDP 319/320 绑定失败时，确认程序权限正确且端口未被其他进程占用。换网卡或网络地址变化后，重新选择网卡并开始发送。

### 配置文件位置

AirPlayQt 设置保存在 `$XDG_CONFIG_HOME/AirPlayQt.json`；未设置该环境变量时为 `~/.config/AirPlayQt.json`。手动编辑前先关闭程序。
