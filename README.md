# AirPlayQt

[中文](README.md) | [English](README.en.md)

通过独立应用或 VST3 效果插件，将实时音频发送到 AirPlay 接收器或已有的 AirPlay 立体声组合。

使用 C++20 和 Qt Widgets 开发，支持 Windows x64、macOS 和 64 位 Linux。

![AirPlayQt 主界面](doc/readme/gui.png)

## 功能

- **独立应用采集：** Windows 支持 ASIO 输入；macOS 支持 Core Audio 输入和输出设备自动回环采集。
- **Linux 采集：** 支持 PipeWire 输入，包括由系统解码的蓝牙 AAC/SBC；输入中断时发送静音，所选音源恢复后自动接流。
- **VST3 输入：** 接收 DAW 的立体声音频，本地 Float32/Float64 音频保持原样直通。
- **接收器选择：** 支持自动发现或手动输入 IPv4 地址。
- **音量控制：** 支持接收器原生音量和静音。
- **中英文界面：** 即时切换，独立应用与各插件实例分别保存语言设置。

## 快速开始

Linux 蓝牙音频接收与 AirPlay 播放操作请参阅 [Linux 使用教程](doc/linux/README.md)。

1. 打开独立应用，或在 DAW 的立体声轨道或总线上插入 AirPlayQt 效果插件。
2. 独立应用中选择采集设备和两个不同的声道。macOS 输出设备标注为 `(auto loopback)`。使用 VST3 时，将工程采样率设为 44.1 kHz，并启用实时处理。
3. 选择一个接收器，或已有 AirPlay 立体声组合的两个成员。手动地址格式为 `IPv4[:port]`，默认端口为 `7000`。
4. 按需选择网卡，然后点击“开始”。
5. 播放期间可调整接收器音量或静音。点击“停止”结束会话。

发送需要 **44.1 kHz** 音频。Linux 由 PipeWire 转换输入采样率并校正时钟；Windows/macOS 输入本身必须提供 44.1 kHz。AirPlayQt 将输入转换为立体声 16 位 PCM，再通过 ALAC 传输。立体声组合的左右声道由 AirPlay 分组信息决定，与选择顺序无关。能够发现接收器不代表一定兼容。

VST3 插件保持本地音频直通，并向宿主报告零本地延迟；远端播放仍有缓冲延迟。关闭插件编辑窗口后仍会继续发送。发送中的实例若在暂停后五秒内恢复有效的宿主音频，可尝试重连一次。

日志仅保留在当前编辑窗口中。关闭窗口后丢弃日志，重新打开时日志区为空，只显示此后产生的新日志，不补放关闭期间的日志。

macOS 上需允许输入设备的麦克风权限、自动回环的系统音频录制权限，以及 AirPlay 所需的本地网络权限。

> **macOS 音频采集：** 请使用虚拟回环音频设备。已测试 **Rogue Amoeba 的 Loopback**；**BlackHole** 尚未测试。在 Loopback 中将音频输入连接到输出声道。AirPlayQt 内置的 **auto loopback** 可能出现音频失真。
>
> **Windows 音频采集：** 请安装支持 ASIO 的虚拟音频设备，例如 **VB-Audio Matrix**，并在其路由配置中将音频输入连接到输出，操作类似 macOS 上的 Loopback。

自动回环通过原生 Core Audio taps 采集正在所选输出设备上播放的应用音频，无需虚拟音频驱动。采集期间，被采集音频的本地播放会静音，停止采集后恢复；仅选择设备不会使其静音。AirPlayQt 不修改系统默认输出、设备音量或采样率。开始前请在“音频 MIDI 设置”中将输出设备设为 **44.1 kHz**。多音频流设备的输出声道按设备流顺序列出，请选择两个不同的声道。少于两个声道的设备会显示在列表中，但无法启动立体声会话。设备移除或格式变化会停止发送，重新开始前需再次选择设备。原生资源清理失败时会报告错误，切换采集设备前必须重试清理。

Windows 应用会请求实时进程优先级，若请求失败则退出；可能需要以管理员权限运行。

## 构建

### 环境要求

- CMake 3.25+、支持 C++20 的编译器；使用 Ninja 预设时还需安装 Ninja。
- Qt 6.5+，包含 Core、Widgets、Network 和 Test。
- OpenSSL 和 libplist 2.7+；非 MSVC 构建还需 pkg-config。
- 插件需要 Steinberg VST3 SDK，通过 Git 子模块提供。
- Windows 独立应用需要 Steinberg ASIO SDK。

在仓库根目录初始化 SDK：

```sh
git submodule update --init --recursive
```

[CMakePresets.json](CMakePresets.json) 中的预设包含特定机器的路径。配置前请根据本机安装位置调整编译器、依赖、SDK 和环境路径。以下命令均在仓库根目录执行。

### Linux

蓝牙输入、安装权限及断流恢复操作见 Linux 使用教程（[中文](doc/linux/README.md) / [English](doc/linux/README.en.md)）。Linux 支持独立 GUI 和可选的无界面 `AirPlayQtCli`，不支持 VST3。

```sh
cmake --preset linux-release
cmake --build --preset linux-release -j "$(nproc)"
ctest --preset linux-release --output-on-failure
```

### macOS

提供的预设面向 **Apple Silicon 和 macOS 27.0+**，使用 Apple Clang 和 Homebrew 依赖路径。

```sh
cmake --preset macos-release -DBUILD_TESTING=ON -DAIRPLAY_CLI_TEST=OFF
cmake --build --preset macos-release
ctest --preset macos-release
```

输出文件：

- 应用：`build/macos-release/AirPlayQt.app`
- 插件：`build/macos-release/VST3/Release/AirPlayQt.vst3`

开发构建使用本机依赖。生成包含运行时库的应用与插件包：

```sh
cmake --build --preset macos-release --target package-macos
```

产物写入 `dist/macos-arm64`，使用 ad-hoc 签名。

### Windows

`msvc` 预设使用 Visual Studio 2026、v143 x64 工具集，以及 `build/msvc-deps/Library` 下的 Release 版 Qt/OpenSSL 库。将 `ASIO_SDK_ROOT` 指向 ASIO SDK，将 `LIBPLIST_ROOT` 指向包含 C 头文件和 DLL 的 libplist 安装目录。

```sh
cmake --preset msvc
cmake --build --preset msvc-release
ctest --preset msvc-release
```

输出文件：

- 应用：`build/msvc/Release/AirPlayQt.exe`
- 插件：`build/msvc/VST3/Release/AirPlayQt.vst3`

MSVC 构建会将运行时依赖部署到应用旁边及插件包内。请保持这些文件在一起，将完整的 `.vst3` 目录安装到宿主的插件位置，然后重新扫描。

也可通过 `debug` 和 `release` 预设使用 MSYS2 CLANG64。其 VST3 SDK 需要[对齐内存分配补丁](patches/vst3sdk-clang64-aligned-allocation.patch)；构建前对新检出的 SDK 检查并应用补丁：

```sh
git -C external/vst3sdk/public.sdk apply --check ../../../patches/vst3sdk-clang64-aligned-allocation.patch
git -C external/vst3sdk/public.sdk apply ../../../patches/vst3sdk-clang64-aligned-allocation.patch
cmake --preset release
cmake --build --preset release
ctest --preset release
```

使用 CLANG64 构建时，应用和宿主进程必须能够找到所需的运行时 DLL 与 Qt 插件。

### 构建选项

| 选项 | 默认值 | 用途 |
| --- | --- | --- |
| `AIRPLAY_BUILD_STANDALONE` | `ON` | 构建输入采集应用。 |
| `AIRPLAY_BUILD_VST3` | `ON`（Linux 为 `OFF`） | 构建 VST3 插件。 |
| `AIRPLAY_CLI_TEST` | `OFF` | 启用 macOS 应用的命令行测试入口。 |
| `AIRPLAY_VST_RATE_DIAGNOSTICS` | `OFF` | 启用 VST 输入／发送速率诊断。 |
| `AIRPLAY_RELEASE_VERSION` | `0.1.0` | `X.Y.Z` 格式的应用、macOS bundle 和 VST3 版本。 |

配置时传入选项，例如 `cmake --preset macos-release -DAIRPLAY_BUILD_VST3=OFF`。仅构建插件时不需要 ASIO SDK。
