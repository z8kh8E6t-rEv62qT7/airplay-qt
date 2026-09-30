# AirPlayQt

发现模式的设备列表按 **IPv4 数值升序，同 IP 再按端口数值升序**显示（例如 `.9` 在 `.10` 前，端口 `900` 在 `10000` 前），独立应用和 VST3 共用规则。新发现的设备直接插入对应位置，保留现有勾选和当前条目；目标记忆仍按名称及地址匹配。

排序回归已覆盖乱序发现、跨 IP 段、同 IP 不同位数端口、重复结果及勾选／当前条目保留；当前完整 CTest **10/10** 通过，通用配置／面板检查增至 **10 项**。排序版本打包日志：`build/macos-release/sort-package.log`；完整包仍输出到 `dist/macos-arm64`。

## VST3 自动重连与独立应用目标记忆（2026-09-30）

新增修复任务不改变下面已完成的移植／打包清单。VST3 原本已在发送时，宿主停用音频处理会结束旧会话并等待恢复；首次中断起 **5 秒内**收到有效的非空实时音频回调、且仍为 44.1 kHz 和相同样本精度，才自动重建一次 AirPlay 会话。握手、PTP 等待和预缓冲使用现有设置，不计入等待宿主的 5 秒，不保证无声中断。规则也适用于 DAW 停止／继续播放引起的停用回调，不猜测宿主停用原因。

等待期间保留单发送实例占用，“停止”始终可用。手动停止、旁路、格式变化、非实时处理、状态载入、真正的输入／网络故障或卸载均取消恢复；超时、重连失败或重连过程中再次暂停后需手动开始，不循环重试。关闭编辑器不取消恢复意图，重开显示当前会话状态。每次重连创建新采集队列和协议会话，旧音频及部分块不会继续发送。宿主暂停不再报告为输入故障 101。

独立应用在发现模式点击“开始”并通过目标校验后，先保存勾选的接收端，再进行权限、采集及连接步骤；连接失败不丢失选择。仅修改勾选不保存。下次启动或刷新按名称、IPv4、端口完全一致恢复；保存两台时须两台齐全才自动勾选，缺失时提示且保留记忆。当前扫描中用户手动修改后，不再自动覆盖其选择。手动输入模式不保存地址，也不会覆盖发现模式的记忆；恢复勾选不会自动播放。

配置版本仍为 1，新增可选数组 `receiverSelection`（最多两项，每项为 `name` 与规范化的 `endpoint`）；旧配置缺少该字段时视为空选择。损坏配置禁止覆盖；保存继续使用 QSaveFile 原子提交，失败时保留原文件。VST3 工程状态不保存接收端，不访问独立应用的配置。

固定执行清单：

1. **完成：区分宿主中断与真正故障。** 将 `setProcessing(false)`／`setActive(false)` 引发的临时中断与格式变化、旁路、非实时处理等故障分开记录。使用单调时钟记录首次中断时间和递增序号，确保快速停用／恢复不会被轮询遗漏；后续真正故障不能被原来的 101 遮蔽。音频回调只更新有界、无阻塞状态，不建立网络连接、不分配内存。
2. **完成：实现 VST3 自动重连生命周期。** 保存本次已成功发送所使用的目标和时间参数；中断时停止旧会话、丢弃旧音频及部分块，等待旧会话清理完成。首次中断后 5 秒内，宿主恢复启用状态，并提供 44.1 kHz、相同样本精度的有效非空实时回调，才允许创建新队列和新 AirPlay 会话，重新执行正常握手、PTP 等待及预缓冲。重复停用不延长等待期限；重连失败或重连过程中再次中断后转为手动恢复，不循环重试。
3. **完成：统一停止意图与界面状态。** 将共享面板的停止按钮改为通知所属控制器，由独立应用或插件处理停止。插件等待期间显示明确状态，保持“停止”可用，保留单发送实例占用，避免其他实例抢占后自动恢复冲突。手动停止、旁路、格式变化、非实时处理、状态载入、组件终止／卸载及其他故障立即取消恢复资格；旧异步事件不得重新启动已取消的请求。关闭编辑器不取消既有发送意图，重开时显示当前状态。宿主正常暂停不再显示笼统的 101 故障。
4. **完成：保存和恢复独立应用的目标勾选。** 在现有配置中新增可选的 `receiverSelection` 数组，每项保存 `name` 和规范化的 `endpoint`，最多两项；保留 JSON 版本 1、现有字段及配置路径，旧配置缺少该字段时按空选择处理。点击“开始”且目标校验通过后保存选择，不要求连接成功；仅修改勾选不落盘。启动和刷新后按完整匹配恢复，缺失时保留记忆并提示，不猜测替代目标；用户手动修改勾选后，当前扫描不再自动覆盖其选择。手动输入模式不新增记忆功能，也不覆盖发现模式的已保存选择。配置损坏或写入失败明确提示，保留原文件。VST3 工程状态不保存目标。
5. **完成：回归验证、重新打包和交付。** 增加宿主暂停／恢复与目标记忆测试，运行适用的 CTest、SDK validator、自测和原生宿主生命周期验证。通过现有 `package-macos` 目标生成完整包，复核依赖、签名、迁移路径及最终 ZIP，更新说明、校验值和验证报告。

恢复策略有确定性 5 秒边界测试；回环接收端测试覆盖协议握手、音频发送、会话清理及自动重连，不向 HomePod 播放。Qt 信号仅传递内置类型，恢复策略与接收端记忆类型不注册为 Qt 元类型。配置／面板通用测试独立为 `common_app`，Windows 原生发现及 ACL 测试保留。

本轮最终结果：完整 CTest **10/10**；通用配置／面板检查 **9 项**、VST3 检查 **29 项**（含 **13 种回环恢复场景**）通过；打包后 SDK validator **47/47**、SDK 自测 **51/51**。应用启动和正常退出、包内依赖、中文／空格迁移路径、四种原生宿主模式、签名及最终 ZIP 解压检查通过。最新完整产物位于 `dist/macos-arm64`；验证报告为该目录的 `verification.txt` 和 `ctest.xml`，构建／打包日志为 `build/macos-release/recovery-package.log`。CLI 为 OFF，无自动安装。

**未验证：** 实际 DAW 名称／版本尚未提供，“添加其他插件”的真实宿主复现仍需实机验收；Windows 实机回归、其他系统版本及 Claude 三维审查未执行。模拟回调和回环测试不替代 DAW／HomePod 验收；Claude 审查仍为上线门槛。本轮不自动安装插件。

## macOS 完整打包（用户追加授权，2026-09-30）

新增打包任务独立于下方原有移植计划。输出 **arm64、macOS 27.0 及以上** 的完整 `.app` 和 `.vst3`，两者分别内置 Qt、OpenSSL、libplist 及间接运行依赖，无须目标机器安装 Homebrew。实测系统为 macOS 27.2；CLI 关闭，使用 ad-hoc 本地签名，不提供 Developer ID 签名、公证或自动安装。

在项目根目录执行：

```sh
cmake --preset macos-release -DBUILD_TESTING=ON -DAIRPLAY_CLI_TEST=OFF
cmake --build --preset macos-release --target package-macos
```

目标要求 Release、arm64、部署目标 27.0 和 CLI 关闭。它依赖应用、VST3 和测试程序完成构建，执行 CTest，再在 `build/macos-release/package-macos-stage` 暂存、部署、签名及验证，全部通过后才替换 `dist/macos-arm64`。失败保留此前交付和暂存诊断。运行需要已登录的 macOS 图形会话；首次收集 Qt 许可证时按本机 Homebrew 配方的 URL 和 SHA-256 下载精确版本源码，缓存于 `build/macos-release/package-sources`。

交付文件：

- `dist/macos-arm64/AirPlayQt.app`
- `dist/macos-arm64/AirPlayQt.vst3`
- `dist/macos-arm64/AirPlayQt-macos-arm64.zip`：包含两个完整 bundle 与使用说明，保留 framework 符号链接。
- `dist/macos-arm64/SHA256SUMS`、`verification.txt`、`ctest.xml` 及应用／原生宿主的动态加载日志：作为归档旁的验证材料。

Qt 官方 `macdeployqt` 部署框架与 Cocoa 平台插件；所有非系统动态库引用改为相对于加载模块自身的路径，移除构建时 rpath。每个 bundle 的 `Contents/Resources/ThirdPartyLicenses` 包含许可证、Qt 第三方归属记录及上游源码来源。VST3 自建 QApplication 时定位自身模块的 `Contents/PlugIns`，缺失 Cocoa 后端时明确失败，卸载恢复原 Qt 路径；借用 QApplication 不修改宿主路径。

包验证包含：完整 CTest、打包后的 SDK validator、分别位于中文／空格路径下的应用启动与正常退出、VST3 自建／借用／不兼容 Qt 与缺失平台插件的生命周期检查。测试宿主重定位到被测插件自己的 Qt；通过 dyld 加载日志逐项检查非系统镜像来自被测包。递归审计每份 Mach-O 的架构、最低系统版本、依赖闭包及符号链接，逐层签名后验证，并对最终 ZIP 解压副本再次审计与验签。缺库及签名损坏的副本必须被门禁拒绝。测试不自动发送 HomePod 音频，也不隐藏或修改 Homebrew。

本次结果：CTest **9/9**、打包后 SDK validator **47/47**，CTest 中 SDK 自测 **51/51**。四种原生宿主模式各完成五轮加载／卸载和每轮两次窗口重开；应用显示窗口后正常退出 0。两个包各有 **29 个 arm64 Mach-O 文件**，实际加载、迁移路径、签名与最终 ZIP 解压审计全部通过。另以独立 CLI=ON 配置执行发布脚本，确认在暂存／发布前拒绝；日志为 `build/macos-release/package-cli-guard.log`。最终常规构建缓存仍为 CLI=OFF。

新增打包计划的固定清单仅更新完成状态：

1. **完成：建立可重复打包入口。** 新增 CMake `package-macos` 目标，依赖 Release 应用及插件构建完成；通过独立暂存目录打包，成功验证后发布至 `dist/macos-arm64`。明确最低系统版本 27.0，拒绝 CLI 开启的发布配置。
2. **完成：完整部署运行依赖。** 使用 Qt 官方部署工具处理应用，递归收集 Qt、OpenSSL、libplist 及其非系统依赖；每个包独立包含自己的 Frameworks、Qt 插件和必要资源。重写动态库引用及搜索路径，移除对 Homebrew、源码目录和构建目录的运行依赖；保留 framework 符号链接、权限声明及第三方许可证。
3. **完成：补齐 VST3 包内路径处理。** 从已加载插件模块的位置定位自身 Qt 插件目录；创建 QApplication 前配置路径，失败及卸载时恢复此前状态。借用兼容 QApplication 时保持宿主路径不变；保留不兼容时明确失败的行为。插件名称、FUID、状态版本及单发送实例约束不变。
4. **完成：签名与验证。** 完成所有文件修改后，从内部库到外层 bundle 逐层签名，核验签名和依赖闭包。运行现有 CTest、SDK validator 和原生宿主测试；针对打包副本验证窗口重开、反复加载卸载及 Qt 自建／借用／不兼容场景。将两个包分别复制到含空格和中文的路径再验证，确认运行时实际加载包内依赖，不修改或隐藏本机 Homebrew。
5. **完成：归档交付。** 输出两个完整 bundle、`AirPlayQt-macos-arm64.zip`、SHA-256 校验文件及验证报告；解压 ZIP 后复核结构和签名。更新构建说明、系统要求、打包命令与交付路径。

用户已确认打包前实机播放通过；打包后的真实 DAW／播放、其他机器及系统版本、长时同步与 Windows 尚未重新验收。ad-hoc 签名不保证其他机器通过 Gatekeeper。**[blocked] 正式上线：Claude 可维护性／边界条件／回归风险三维审查仍未执行。**

依据：[Qt macOS 部署](https://doc.qt.io/qt-6/macos-deployment.html)、[Qt 库搜索路径](https://doc.qt.io/qt-6/qcoreapplication.html#libraryPaths)、[Apple 嵌套代码签名](https://developer.apple.com/library/archive/documentation/Security/Conceptual/CodeSigningGuide/Procedures/Procedures.html)。

## macOS arm64 移植状态（2026-09-30，本机开发构建完成）

原移植目标为 Release 本机开发构建，独立应用使用 Core Audio 输入设备，插件使用 DAW 音频；采样率不是 44.1 kHz 时只报错拒绝，不引导、不自动切换、不重采样。常规构建运行依赖本机 Qt 6.11.1、OpenSSL 3.6.4 和 libplist 2.7.0。只针对当前 macOS 27.2 / arm64 环境；本机 OpenSSL、libplist 二进制最低系统版本为 27.0。新增打包任务将 macOS 预设的部署目标明确为 27.0；旧系统兼容性未验证。

固定执行清单保持原顺序与范围，状态如下：

1. **完成：建立平台边界与构建入口。** 保留共享协议、队列、会话和界面，分离 Windows／macOS 采集、发现和插件窗口实现；公共接口移除 Windows 类型依赖。新增 `macos-release` 配置、构建及测试预设，使用 Apple Clang、Ninja、arm64，输出至 `build/macos-release`，保留既有 Windows 预设。
2. **完成：实现 Core Audio 独立应用。** 以设备 UID 保存选择，枚举输入声道，接入现有 `CaptureStream`；回调使用预分配缓冲和有界队列，不执行阻塞操作。处理权限拒绝、设备移除、格式变化、无效声道和溢出，异常停止后需手动重启。界面采用输入设备／声道文案，控制面板打开“音频 MIDI 设置”；配置存入用户配置目录，保留现有 JSON 字段和版本。
3. **完成：移植发现与计时。** 使用 Bonjour DNS-SD，保留扫描期限、IPv4 筛选、去重、取消和刷新语义，正确释放回调资源。Mac 提供纳秒墙钟实现，调度继续使用单调计时；保留现有 PTP 端口和协议行为，移除 Mac 路径上的 Windows 优先级及计时器调用。应用补齐音频输入和本地网络用途声明。
4. **完成：移植 VST3 编辑器与生命周期。** 共享 Processor、状态、音频透传及会话逻辑，使用 macOS SDK 入口和 NSView 嵌入。处理主线程事件派发、Retina 逻辑尺寸、焦点、编辑器重开及卸载清理；保留兼容 QApplication 借用和不兼容时明确失败的行为。保持插件名称、FUID、状态版本和单发送实例约束。
5. **完成：构建、验证并交付。** 编译完整 `.app` 和 `.vst3`，运行适用的 CTest、SDK validator、自测及 Mac 原生宿主测试，验证启动与动态依赖。更新构建说明，提供产物绝对路径、测试结果和未完成验收项。

第 4 项原阻塞已解决：接收端列表与发现信号只传递 Qt 内置字符串，业务层继续使用普通 C++ 类型；界面启动／参数变更信号改为同线程通知，由调用方读取类型化数据。移除自定义元类型声明，同时避免 Qt 6.11 `QListWidgetItem::checkState()` 内联枚举转换产生的模块内类型注册，直接读取 `CheckStateRole` 整数。不依赖 Qt 内部注销接口，不改变插件 FUID、状态版本或配置 JSON。

第 5 项验证结果：完整 CTest 在 CLI 开启时 **10/10 组通过**，包含协议、Bonjour、Core Audio、VST 音频／状态及界面回归；SDK validator **47/47**，SDK 自测 **51/51**。Mac 原生宿主分别在自建、借用和不兼容 Qt 模式下完成五轮真实加载／卸载，每轮两次编辑器打开／关闭，并验证逻辑尺寸与焦点。自建与借用模式每轮向发现信号注入确定性结果，覆盖选择、读取、清空、启动拒绝和参数通知，不依赖局域网是否有接收端。参数恢复、窗口重开和多实例单发送限制也已在 Mac 执行；多实例测试只连接本机回环测试端口。

独立应用实际启动、显示主窗口、完成 Bonjour 发现并正常退出（退出码 0）通过；`file` 确认两份二进制均为 arm64，`otool -L` 确认依赖 Homebrew 与系统动态库。SDK 对齐分配测试已按 macOS 的 C `aligned_alloc` 约束验证非对齐倍数返回空指针，保留 Windows 原检查；依据 [WG14 DR 460](https://open-std.org/jtc1/sc22/wg14/www/docs/n1986.htm)。未修改 VST3 SDK。

已实测：Loopback Audio 第 1、2 声道、44.1 kHz，向两台 HomePod 连续发送 30 秒并正常停止，详见下方 CLI 记录。未验证：物理音频设备的权限拒绝／采集／拔插、真实 DAW、HomePod 左右定位与长时同步、Windows 编译及回归。其他 Core Audio 格式、声道映射、分块、溢出和属性变化仍仅通过注入测试验证。**[blocked] 正式上线：** 仍需实际宿主与设备验收，以及 Claude 可维护性／边界条件／回归风险三维审查；本任务不执行 Claude。

当前产物（仅限当前 Mac 的本机开发构建）：

- `/Users/langzhuo/Projects/airplay-qt/build/macos-release/AirPlayQt.app`
- `/Users/langzhuo/Projects/airplay-qt/build/macos-release/VST3/Release/AirPlayQt.vst3`

复现命令（项目根目录）：

```sh
cmake --preset macos-release -DBUILD_TESTING=ON
cmake --build --preset macos-release
ctest --preset macos-release --output-junit macos-tests.xml
```

`vst3` 和 `vst3_module_*` 测试需要 macOS 图形会话；模块宿主会扫描设备，但不会开始音频发送。结果在 `build/macos-release/macos-tests.xml` 和 `build/macos-release/Testing/Temporary/LastTest.log`；构建日志为 `build/macos-release/build.log`。编辑器截图位于 `build/macos-release/bin/Release/VstEmbedded-owned.png` 和 `VstEmbedded-borrowed.png`。本节记录原开发构建；完整包见上方新增打包任务。没有自动安装插件或制作公证安装包。


### macOS CLI 实机测试（用户追加授权）

麦克风权限后端构建遗漏已修复：对 macOS 独立目标执行 `qt_finalize_target(AirPlayQt)`，使 Qt 根据 Info.plist 导入静态 Darwin 麦克风权限插件及请求入口。已从产物符号确认两者存在，并确认 AVFoundation 链接。

CLI 由 `#ifdef AIRPLAY_CLI_TEST` 控制，CMake 选项默认 `OFF`。关闭时不编译 `Cli.cpp`、不定义 CLI 入口，也不注册 `cli_cases`；本次最终产物已关闭 CLI。开关仅适用于 macOS 独立应用。CMake 缓存会保留显式选择，测试后需显式设回 `OFF`。

在项目根目录开启并构建：

```sh
cmake --preset macos-release -DAIRPLAY_CLI_TEST=ON
cmake --build --preset macos-release
```

测试后关闭并构建：

```sh
cmake --preset macos-release -DAIRPLAY_CLI_TEST=OFF
cmake --build --preset macos-release
```

仅开启时，同一个 `AirPlayQt.app` 支持 `--cli`，不显示主窗口，复用 Controller、Core Audio、权限检查与 AirPlay 会话。设备按 UID 选择，声道参数从 1 开始，只接受 44.1 kHz。CLI 读取已有时间设置，不保存设备／声道选择，不执行旧 Windows 实测程序的音量变化和静音测试；协议仍沿用现有会话的初始音量处理。

```sh
/Users/langzhuo/Projects/airplay-qt/build/macos-release/AirPlayQt.app/Contents/MacOS/AirPlayQt --cli --help
/Users/langzhuo/Projects/airplay-qt/build/macos-release/AirPlayQt.app/Contents/MacOS/AirPlayQt --cli --list-devices
/Users/langzhuo/Projects/airplay-qt/build/macos-release/AirPlayQt.app/Contents/MacOS/AirPlayQt --cli --device 'com.rogueamoeba.Loopback::9B38442C-0A0F-4BDD-B24C-A170833A3FB0' --left 1 --right 2 --seconds 30 --receiver 192.168.8.9 --receiver 192.168.8.10
```

接收端必须显式指定；重复 `--receiver` 可选择已有立体声组的两台设备。`--seconds` 范围 1～3600，从进入发送状态开始计时，默认 30；`--startup-timeout` 范围 1～3600，默认 60，包含授权等待。Ctrl+C／SIGTERM 请求有界正常停止。标准输出为 JSON Lines，包含阶段、组合、初始音量、每秒统计和最终结果；最终计数是最后一次遥测采样值。退出码 0 表示完成规定时长且有发送包，1 表示运行失败，2 表示参数错误，130／143 表示中断。非零峰值用于区分输入音频与静音；发送成功不能代替听音确认。

新增自动回归 `cli_cases` 覆盖 16 项帮助、参数边界、重复接收端、非法地址和不存在设备的失败退出；不向 HomePod 发送。CLI 开启状态完整 CTest **10/10 通过**，关闭状态 **9/9 通过**；两份报告分别为 `build/macos-release/macos-cli-on-tests.xml` 和 `macos-tests.xml`。关闭状态已验证构建命令不包含 `Cli.cpp`、最终二进制不存在 `runCli` 符号，且 `cli_cases` 未注册。实际发送仅由人工明确调用，不加入自动 CTest。

**CLI 实机发送通过（2026-09-30 14:49，America/Denver）：** 用户确认本地网络权限已允许后，通过系统 `open -n -W` 启动同一应用完成了既定测试：Loopback Audio 第 1、2 声道，目标 `192.168.8.9:7000` 和 `192.168.8.10:7000`。立体声组校验、PTP 等待、输入缓冲、30 秒发送及正常停止均完成。最终 `exit_code=0`，含停止清理的发送阶段计时为 30123 ms；最后一次遥测每台 3750 包，重传 0、过期 0，最大积压 55.873 ms，左右声道峰值均为 0.096527，确认输入不是静音。未执行额外音量调节；会话报告初始音量约 -10.5 dB。用户随后确认 HomePod 已实际出声。此结果验证实际采集、发送及出声；左右定位和长期同步尚未验收。

成功日志：`build/macos-release/homepod-cli-retry.jsonl`，标准错误：`homepod-cli-retry.stderr.log`。之前的 `homepod-cli.jsonl` 保留两次失败记录：第一次直接命令启动的麦克风权限归属 Codex 而被拒绝；第二次系统启动已通过麦克风检查，但本地网络首次授权期间连接立即失败。用户确认允许后的本次重试成功，未修改签名或系统权限数据库。`AirPlayQt` 签名标识与 `org.airplayqt.app` 包标识差异仍属于开发构建的已知信息，不能据此断言它是此前错误的根因。构建日志：`build/macos-release/cli-build.log`。


macOS 会按启动环境判断权限归属；从命令行启动与从 Finder 启动的授权行为可能不同。参见 [Apple 本地网络隐私说明](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)。同一应用的 CLI 现已通过实测；GUI 尚未在此次授权生效后重新开始发送验证。


Windows x64 / C++20 / Qt 6 Widgets：读取一个 ASIO 驱动的任意两个输入声道，通过 PoC 的实时 ALAC/PTP 路径发送到一台接收端或已有 HomePod 立体声组。

另提供同进程 Qt 编辑器的 Windows x64 VST3 效果插件：直接接收 DAW 立体声音频，本地 Float32／Float64 原样透传，复用同一套发现、协议和发送实现。当前为 0.1.0 开发构建，尚未满足正式上线门槛。

接收端可通过 Windows 原生 DNS-SD 发现或纯手动输入 IPv4 地址选择，生产代码不再包含固定目标。单台不要求立体声组合；双台启动读取设备身份及 `tsid`，要求两个不同设备属于同一个已有组合。两台均接收完整立体声，播放左右角色由 HomePod 已有组合决定，勾选顺序不指定左右。

只接受 44.1 kHz；支持 ASIO 整数及浮点 PCM 输入，显式转换为 16-bit 双声道，不重采样、不对 PCM 调整 gain。不包含 WASAPI、APAT、失败后单台降级或自动重连。仅支持既有 ALAC/PTP 协议路径，发现到设备并不表示该设备一定兼容。

## 构建与运行

### MSVC Release（当前构建）

MSVC 与 CLANG64 共用 Fusion 样式设置：独立程序及 VST3 编辑器均使用 Fusion。插件借用宿主 QApplication 时只设置自身控件样式，不改变宿主应用的全局样式。

速率诊断由 `#ifdef AIRPLAY_VST_RATE_DIAGNOSTICS` 控制，CMake 选项默认 `OFF`，关闭时不编译诊断字段、计时、统计与日志。需要排查时配置 `cmake --preset msvc -DAIRPLAY_VST_RATE_DIAGNOSTICS=ON`（CLANG64 使用 `--preset release`）并重新构建；关闭时重新配置为 `OFF` 并构建。启用后，VST 在连接完成、开始采集后输出 `[VST速率]` 诊断日志：每秒记录实际统计窗口、输入／发送帧率、积压净增长（ms/s）、队列及 PCM 积压、网络 poll 最大间隔／耗时和累计帧数。停止或报错前补充 `[VST速率·停止]`，不足一秒也会报告。发送帧率按每台接收端共享的音频时间线计算，不乘接收端数量；输入计数只包含已入队的完整 352 帧块，首个窗口包含启动预缓冲。统计在网络线程完成，独立 ASIO 程序不启用这组日志，不改变发送节奏。实际连接后，可复制连续几条速率日志及最终报错用于判断输入是否快于实时或网络线程是否落后。

使用已安装的 Visual Studio 2026 + v143 x64 工具集（MSVC 19.44），Qt 6.11.2、OpenSSL 3.6.4 及传递依赖来自 conda-forge 的 win-64 预编译包。依赖环境在 `build/msvc-deps`，不修改系统 PATH。已有 CLANG64 的 `libplist-2.0.dll` 通过 C ABI 复用：配置时从 DLL 导出表生成 MSVC `.lib`，仅复制对应 C 头文件和 DLL 到构建目录，不引入 MinGW CRT 头文件或 C++ 包装库。

本机 `build/Library/bin/micromamba.exe` 已准备好。复现依赖安装及构建，在 MSYS Bash 执行：

```bash
cd /e/Projects/airplay-qt
./build/Library/bin/micromamba.exe --no-rc create -y \
  -r E:/Projects/airplay-qt/build/mamba \
  -p E:/Projects/airplay-qt/build/msvc-deps \
  --override-channels -c conda-forge 'qt6-main=6.11.2' 'openssl=3.6.4'
/clang64/bin/cmake.exe --preset msvc
/clang64/bin/cmake.exe --build --preset msvc-release
/clang64/bin/ctest.exe --preset msvc-release
```

精确包 URL 与校验值记录于 `build/diagnostics/conda-msvc-win64-explicit.txt`；可使用 micromamba 的 `--file` 重建相同依赖集。CMake/CTest preset 限定 DLL 和 Qt 插件搜索路径，避免默认 PATH 中的 FFmpeg／CLANG64 DLL 混入。所有项目 C++ 目标显式启用 `/EHsc`，确保 VST3 SDK 调整全局编译选项后，异常路径仍正确执行 RAII 清理。

产物：`build/msvc/Release/AirPlayQt.exe`；完整插件包 `build/msvc/VST3/Release/AirPlayQt.vst3/`。MSVC 独立程序的默认构建会执行 `AirPlayQtRuntime`，将递归解析的非系统 DLL、`platforms/qwindows.dll` 和 `qt.conf` 放到 EXE 旁，清单为同目录 `runtime-dependencies.txt`。独立程序无需开发环境 PATH；由于现有 Realtime 优先级要求，请右键 EXE“以管理员身份运行”，或使用 Windows sudo。保持 EXE 与旁边 DLL、platforms 目录一起使用。

已通过 sudo 验证：清除 Qt 环境变量、PATH 仅保留 Windows System32，主窗口出现并正常关闭（退出码 0），没有开始音频发送。记录为 `build/diagnostics/msvc-standalone-start-admin.txt`。本地部署后的依赖检查为 `build/diagnostics/msvc-exe-local-ldd.txt`。

MSVC VST3 包现包含运行依赖，无需设置 PATH、QT_PLUGIN_PATH 或 QT_QPA_PLATFORM_PLUGIN_PATH。尚未自动安装到系统插件目录。正常退出 Nuendo 后，手动将整个 `build/msvc/VST3/Release/AirPlayQt.vst3/` 目录包复制到 `C:/Program Files/Common Files/VST3/`（需要该目录写入权限）。不要只复制最里面的同名二进制文件，也不要与旧 CLANG64 包混合。

```text
AirPlayQt.vst3/
  Contents/Resources/moduleinfo.json
  Contents/x86_64-win/
    AirPlayQt.vst3               # 静态 MSVC CRT 加载入口，仅导入系统 DLL
    runtime/
      AirPlayQtEngine.dll       # 原 SDK 工厂、Qt 和 AirPlay 实现
      Qt6*.dll、libcrypto*.dll、libplist-2.0.dll 等递归依赖
      runtime-dependencies.txt
      runtime-sha256.txt
      platforms/qwindows.dll
```

重新启动 Nuendo，在 VST Plug-in Manager 中执行“Scan for new and blocked plug-ins”；按住 Shift 可重扫全部插件。若 AirPlayQt 在 Blocklist，选中后执行 Reactivate。见 [Nuendo 15 重扫说明](https://www.steinberg.help/r/nuendo/15.0/en/cubase_nuendo/topics/installing_and_managing_plugins/installing_and_managing_plugins_vst_plug_in_manager_toolbar_r.html)及[恢复被阻止插件](https://www.steinberg.help/r/nuendo/15.0/en/cubase_nuendo/topics/installing_and_managing_plugins/installing_and_managing_plugins_plug-in_manager_reactivating_from_blacklist_t.html)。从音频轨道的效果插槽中搜索 AirPlayQt；这是效果插件，不在乐器列表中。

默认构建的 `AirPlayQtVst3Runtime` 目标每次重新收集和部署依赖，再运行 SDK 元数据工具和 validator。单独构建目标时也应选择 `AirPlayQtVst3Runtime`，而不是只构建外层二进制。EXE 与 VST3 共用 `cmake/CollectRuntime.cmake` 的递归依赖收集逻辑，分别保持原有布局。部署清单包含来源与 SHA-256，缺失／冲突会让构建失败。

加载入口通过自身模块的绝对路径使用 `LoadLibraryExW` 的 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32` 加载 Engine，不改变宿主 PATH、全局 DLL 搜索规则或优先级。初始化前校验包内文件；已加载同名第三方 DLL 只有内容一致时才允许复用，MSVC/UCRT 系统运行库不做内容一致比较。不同 Qt 等同名库已被宿主加载时，会明确拒绝初始化，不能保证任意宿主／插件组合均兼容。自建 QApplication 只搜索本包平台插件；借用兼容宿主 QApplication 不修改其路径。最后退出时检查 SDK 工厂、组件和 Qt 清理状态；尚在使用则返回失败并保留 Engine，避免释放活动代码。

若扫描失败，先确认完整包及 `runtime-sha256.txt` 未被拆散或混用。加载器通过 `OutputDebugStringW` 输出具体原因，并提供 `AirPlayQtLoaderError` 调用方缓冲诊断接口，不弹扫描对话框。原生诊断宿主可直接运行：

```bash
./build/msvc/bin/Release/test_vst3_native.exe \
  'E:/Projects/airplay-qt/build/msvc/VST3/Release/AirPlayQt.vst3/Contents/x86_64-win/AirPlayQt.vst3' normal
```

此工具清除开发环境、从包外目录加载，创建组件、关闭／重开隐藏编辑器，检查五轮卸载；不激活音频。它自身不链接 Qt。具体路径与失败记录在 `build/msvc/native-tests/*/result.txt`。Qt 开启编辑器后会做设备发现；扫描及仅创建组件不启动发现或 AirPlay。

依赖报告：`build/diagnostics/msvc-loader-dependents.txt` 和 `msvc-loader-ldd.txt` 为外层，`msvc-engine-dependents.txt` 和 `msvc-engine-ldd.txt` 为 Engine。MSYS ldd 不接受 `.vst3` 扩展名，外层报告检查的是逐字节相同的 `.dll` 诊断副本；Engine 的动态加载另由冷启动宿主确认实际包内路径。依赖包含 Qt、OpenSSL、libplist、MSVC/UCRT 和 Windows 系统 DLL，不使用 `libc++.dll`。

2026-09-30 最终 MSVC Release 回归：19 组 CTest 全部通过（116.72 秒），结果记录于 `build/diagnostics/msvc-loader-ctest.log` 和 `.xml`；构建日志为 `msvc-loader-build.log`。保留原协议／采集／会话／发现测试、validator 47 项、SDK 自测 51 项、自建／借用／不兼容 Qt 测试，并增加正常／中文空格路径、强制 Engine 换址重载、缺失 Engine／依赖／平台插件、错误入口、不同同名 DLL 拒绝及相同副本复用测试。另有 45 次完整冷加载／卸载循环通过，记录于 `msvc-loader-stress.log`。测试不向 HomePod 发送音频。模块截图仍在 `build/msvc/bin/Release/VstEmbedded-owned.png` 和 `VstEmbedded-borrowed.png`。

验证过程中，中间 `/MD` 加载入口构建曾出现两次访问异常；修正为计划要求的 `/MT` 后，上述重复测试和强制换址测试未再复现。尚未单独证明此前异常与 CRT 选择的因果关系，不能将这些自动化结果表述为 Nuendo 的卸载安全验收。

本轮包内依赖部署计划状态：

1. **建立明确的加载边界：完成。** 外层使用静态 CRT，实际导入只有 KERNEL32 和 bcrypt；Engine 保留原产品版本与双组件 FUID。
2. **部署依赖并处理 Qt 路径：完成。** 共用递归部署、清单／校验值、同名库冲突检查和包内平台插件路径已实现；部署后运行元数据工具和 validator。
3. **完善失败与卸载处理：完成。** 初始化失败回滚、重复 Init／Exit、活动组件／工厂拒绝卸载、Qt 和发现清理、外层及 Engine 实际卸载均纳入测试。

**验证与交付：完成本轮 Release 构建及自动化验证。** 未向 HomePod 发送音频，也未复制到系统插件目录。下述实机与审查门槛仍为 blocked。

**[blocked] MSVC Debug：** conda-forge Qt 包仅提供 Release 库，没有 Qt Debug 库；当前 MSVC preset 只开放 Release，不把 Release Qt 冒充 Debug 依赖。原有 CLANG64 Debug 产物仍保留。Nuendo/Cubase 实机验收、Windows 10 和 Claude 三维审查仍未完成，不能据此宣称可发布。

### 既有 CLANG64 构建

在 `E:\MSYS\usr\bin\bash.exe` 中执行：

```bash
cd /e/Projects/airplay-qt
export PATH=/clang64/bin:/usr/bin:/c/Windows/System32:$PATH
cmake --preset debug
cmake --build --preset debug -j 4
ctest --preset debug
cmake --preset release
cmake --build --preset release -j 4
ctest --preset release
./build/release/AirPlayQt.exe
```

需要 CLANG64 的 Qt 6（Core、Widgets、Network、Test）、OpenSSL、libplist 2.7+、CMake 和 Ninja。ASIO SDK 不复制到工程，通过 `ASIO_SDK_ROOT` 指定，当前预设为 `E:/MSYS/clang64/include/asiosdk`。修改 SDK 路径可使用：

```bash
cmake --preset release -DASIO_SDK_ROOT=E:/path/to/asiosdk
```

当前交付为开发环境构建，不是便携包。运行时必须能找到 `/clang64/bin` 中的 DLL 与 Qt 插件。

两个构建选项 `AIRPLAY_BUILD_STANDALONE`、`AIRPLAY_BUILD_VST3` 默认均为 ON。VST3 SDK 通过 `VST3_SDK_ROOT` 指定，默认使用已下载的 `external/vst3sdk`。本机 SDK 已按用户指定的对齐版 `operator new` 修复 CLANG64 编译；在重新下载 SDK 后，须应用下文记录的补丁。

仅构建独立程序可加 `-DAIRPLAY_BUILD_VST3=OFF`；仅构建插件可运行：

```bash
cmake --preset release -DAIRPLAY_BUILD_STANDALONE=OFF -DAIRPLAY_BUILD_VST3=ON
cmake --build --preset release -j 4
ctest --preset release
```

插件单独构建不需要 ASIO SDK。若之后要恢复同时构建两者，重新配置时显式加 `-DAIRPLAY_BUILD_STANDALONE=ON`。

## 既有 CLANG64 VST3 开发包与宿主使用

生成的完整目录包为：

- Debug：`build/debug/VST3/Debug/AirPlayQt.vst3/`
- Release：`build/release/VST3/Release/AirPlayQt.vst3/`

真正的 Windows x64 模块位于包内 `Contents/x86_64-win/AirPlayQt.vst3`，`Contents/Resources/moduleinfo.json` 由 SDK 工具生成。应复制整个外层目录包。不要同时安装相同 FUID 的 Debug 和 Release 包。

未自动安装或链接插件目录。用户可自行把 Release 目录包复制到 `%LOCALAPPDATA%/Programs/Common/VST3/`，或具有写入权限时使用 `C:/Program Files/Common Files/VST3/`；路径依据 [Steinberg 插件位置规范](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Locations%2BFormat/Plugin%2BLocations.html)。

开发包没有携带 CLANG64、Qt、OpenSSL、libplist 等运行库。启动宿主及其扫描子进程前，须让它们继承正确的运行环境。可在 MSYS Bash 中设置当前终端环境，再运行本机实际安装的 Cubase／Nuendo 可执行文件：

```bash
export PATH=/clang64/bin:/usr/bin:/c/Windows/System32:$PATH
export QT_PLUGIN_PATH='E:/MSYS/clang64/share/qt6/plugins'
export QT_QPA_PLATFORM_PLUGIN_PATH='E:/MSYS/clang64/share/qt6/plugins/platforms'
# 在同一终端运行本机实际的宿主 exe 路径。
```

上述设置只作用于当前终端及子进程，不修改系统 PATH。宿主已运行时应先正常退出，再从此环境启动。插件借用 Qt 时要求兼容的 QApplication 和同一 UI 线程；不兼容时编辑器显示错误，本地音频继续透传。

在 Cubase／Nuendo 的 VST Plug-in Manager 中扫描新插件；若曾因缺少 DLL 进入 Blocklist，应先修复运行环境，再在该页面重新启用并扫描。不同宿主版本按钮文字可能不同，参见 [Cubase 官方重启用说明](https://www.steinberg.help/r/cubase-pro/15.0/en/cubase_nuendo/topics/installing_and_managing_plugins/installing_and_managing_plugins_plug-in_manager_reactivating_from_blacklist_t.html)。插件名称为 AirPlayQt，分类 Fx / Tools。当前检测到 Nuendo 15，完整小版本号及实机验收结果尚未记录。

本机 Nuendo 15 扫描排查（2026-09-29）：安装目录包结构及 SHA-256 与 Release 一致。扫描日志包含 `LoadLibraryW failed`，并因错误文本编码导致 `Unable to parse scanner output`；不能把前面的 `Scanning: AirPlayQt OK` 单独当作加载成功。本机默认环境下加载返回错误 127，而 CLANG64 优先的环境可加载。独立进程预加载 `E:/Projects/ffmpeg/libc++.dll` 后可复现同一错误；该文件在默认 PATH 中先于 CLANG64。正确环境下，已安装包通过 47 项 validator 检查，日志为 `build/diagnostics/nuendo-installed-validator.log`。

先保存工程并彻底退出 Nuendo，再在 MSYS Bash 运行以下命令；仅重扫已经运行的旧进程不会更新其环境或替换已加载 DLL：

```bash
PATH=/clang64/bin:/usr/bin:/c/Windows/System32:$PATH \
QT_PLUGIN_PATH='E:/MSYS/clang64/share/qt6/plugins' \
QT_QPA_PLATFORM_PLUGIN_PATH='E:/MSYS/clang64/share/qt6/plugins/platforms' \
'/d/Audio/Steinberg/Nuendo 15/Nuendo15.exe'
```

随后在 VST Plug-in Manager 重新扫描，从音频轨道的效果插槽搜索 AirPlayQt。未修改系统 PATH、Nuendo 缓存或任何已安装 DLL。Nuendo 内重扫与实际播放结果仍待用户确认；检测到 Nuendo 15 不等于完成宿主验收。

1. 将 AirPlayQt 插入立体声轨道或总线。只有工程为 44,100 Hz、宿主启用实时处理且未旁路时，才能开始 AirPlay。其他工程采样率仍原样透传，不重采样。
2. 打开编辑器后扫描；在发现模式勾选一／两台，或在手动模式填写 IPv4[:端口]。选择后手动开始；加载插件、扫描插件和恢复工程均不会自动发送。
3. 本地报告 0 samples 延迟、无尾音。AirPlay 提前量只影响远端播放，不计入 DAW 延迟补偿。宿主停止走带但继续实时监听时可持续发送；跳转、循环不重置网络时间线。
4. 标准 Bypass 可由宿主控制。旁路、预处理、离线导出、格式变化、输入断流、NaN/Inf 或队列溢出会停止 AirPlay，本地透传继续；条件恢复后须手动开始。宿主暂时停用处理时，仅已在发送的实例按上方 5 秒规则自动重连一次。
5. 关闭编辑器不停止当前发送。重新打开恢复时间设置、会话状态和原生音量；界面可缩放、调整大小，较小窗口可滚动访问全部控件。停止后可重新选择接收端。
6. 多个实例仅允许一个持有发送会话。另一实例会显示占用；与独立程序或其他进程发生 PTP 端口冲突时启动失败，不抢占端口。插件只配对申请／释放 1 ms 计时精度，不更改宿主优先级或电源策略。

插件工程状态采用版本 1 的固定小端格式：标记、版本、13 项时间值及标准旁路状态。标准旁路持久化用于保持宿主 Processor／Controller 一致；其余参数不支持自动化。截断、未知版本、非法时间或旁路值整体拒绝，时间参数不会部分应用。载入状态会停止发送，即使载入失败也不会自动恢复。插件不访问独立程序的 AirPlayQt.json，不保存接收端、模式、临时实例标识、密钥、音量或发送状态。

### SDK 对齐分配修复

SDK 3.8.1 的 `alignedalloc.h` 在 MinGW 路径调用了 UCRT 缺失的 `std::aligned_alloc`。按用户指定，MinGW 非零对齐分支改为 `::operator new(size, std::align_val_t(alignment), std::nothrow)`，释放使用相同 alignment 的 `::operator delete`。零对齐保留 malloc/free，其他平台分支不变。没有排除 Data Exchange 源文件。

这是本轮经用户指定方法处理的 SDK 修改，补丁保存在 `patches/vst3sdk-clang64-aligned-allocation.patch`。本机已应用，勿重复应用。新 SDK 工作副本可从项目根目录运行：

```bash
git -C external/vst3sdk/public.sdk apply --check ../../../patches/vst3sdk-clang64-aligned-allocation.patch
git -C external/vst3sdk/public.sdk apply ../../../patches/vst3sdk-clang64-aligned-allocation.patch
```

对齐 new/delete 必须配对，不能用 free 释放；nothrow 保留分配失败返回空指针的行为。依据 [C++ 对齐分配与释放约定](https://eel.is/c++draft/new.delete.single)。测试覆盖零大小、非对齐倍数大小、8～4096 字节对齐及空指针释放。

### 自动化与实机验收

CTest 保留 audio、airplay、app、discovery 四组，新增 vst3、SDK validator/selftest 及三个实际 DLL 宿主测试。插件测试覆盖 f32/f64 位级透传、原地缓冲、静音、跨块分包、PoC ALAC 向量、故障与新会话队列隔离、状态截断、双组件销毁顺序、多实例占用和活动连接清理。隐藏 Win32 测试宿主检查自建／借用／不兼容 Qt 应用、HWND 嵌入与缩放、发现取消和重复 DLL 加载卸载。它可能发出只读 DNS-SD 查询，不激活音频处理或向接收端播放。

截图由测试生成：`build/debug/bin/VstEmbedded-owned.png`、`VstEmbedded-borrowed.png`、`VstStreamingPanel.png`；独立界面为 `build/debug/MainWindow.png`。构建日志位于 `build/diagnostics/vst3-debug-build.log`、`vst3-release-build.log` 和 `vst3-only-build.log`。

**[blocked] 实机验收**：用户仍需记录 Cubase／Nuendo 的实际版本，验证扫描、嵌入界面、发现／手动单／双机、反向选择顺序、原生音量／静音、旁路、工程恢复、关闭窗口继续播放，以及至少 30 分钟同步和缓冲趋势。隐藏测试宿主与 validator 不代表真实 DAW 兼容性。

**[blocked] Windows 10 兼容性**与 **[blocked] Claude 可维护性／边界条件／回归风险审查** 尚无结果。本轮不宣称可正式发布。MSVC 包内依赖部署按后续独立确认的范围交付，CLANG64 开发包仍依赖开发环境。

按用户要求，主程序启动时通过 `SetPriorityClass` 将当前进程设为 `REALTIME_PRIORITY_CLASS`，并通过 `GetPriorityClass` 读回验证；设置或验证失败会显示错误并退出，不静默降级。设置发生在主窗口、控制器与网络线程创建之前，仅作用于 `AirPlayQt.exe`，不写入 JSON，也不改变诊断/测试程序的优先级。该级别可能使鼠标响应和系统任务受影响，详见 [Windows SetPriorityClass 文档](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setpriorityclass)。

## 使用

1. 默认“发现模式”自动扫描 10 秒，勾选一台或两台接收端后开始；不自动勾选或播放。“刷新”清空列表和勾选，等待旧扫描取消后再开始新扫描。也可切换到“手动模式”，第一个输入框必填、第二个可空，格式为 `IPv4[:端口]`，省略端口使用 7000，例如 `192.168.8.9`。手动模式不预填、不提供发现候选或自动补全。
2. 选择 ASIO 驱动，再选择左右输入声道。编号从 1 开始，例如 1/2 或 31/32。两个声道必须不同。驱动控制面板只能在停止时打开；应用请求 44100 Hz 并再次查询确认，失败直接报错。
3. 点击“开始”。依次连接、读取身份、校验单／双机条件、瞬时配对、等待 PTP、开始采集并预缓冲。单台仍需要协议认证，不代表跳过瞬时配对。开始后锁定接收端模式、选择、刷新、驱动、声道和时间设置。
4. 音量设置和静音通过接收端原生 `SET_PARAMETER volume`。单台启动取自身初始音量，双台取较低值；取消静音恢复静音前音量。
5. 点击“停止”或关闭窗口会停止采集，并在 TEARDOWN 等待上限内清理所选会话。任一所选接收端失败都会停止整个会话，需手动再次开始。关闭窗口也等待发现请求取消，不阻塞界面线程。

发现仅浏览 `_airplay._tcp.local`，使用系统 `dnsapi`，无需 Bonjour。浏览所有网卡，只保留有效 IPv4 和实际服务端口；同一端点合并，同名不同端点保留。每轮最多 128 个候选、同时解析最多 4 个，其余解析排队；达到候选上限明确提示。扫描、解析失败可切换手动模式。解析结果仅供选择，连接时仍读取 `/info` 校验真实身份。

接收端选择与手动输入不写入 JSON，重启后必须重新选择。手动文本只在当前进程内保留；切换模式仅使用当前模式的目标。扫描超时、刷新、切换手动、开始播放和退出都会取消原生请求；取消未结束前不创建新一轮扫描，迟到结果不会进入新列表。取消 API 失败时显示错误并保留上下文，等待原生操作结束，不提前释放仍可能被系统访问的内存。

开始准备采集时，程序自动申请 1 ms Windows 计时精度，并显式禁止 Windows 忽略本进程的计时精度请求；日志显示申请成功。停止、启动失败或正常退出时释放请求并恢复原进程策略。申请或验证失败会阻止启动，恢复失败会报告错误。它不改变 ASIO 缓冲大小、采样率或 PCM，也不增加 JSON 设置。尚未实测所有 Windows 10/11 版本；若系统不支持该策略，程序明确报错，不静默跳过。

### VB-Matrix 的方向

本程序是普通 ASIO 宿主，只读取驱动的 **ASIO input** 缓冲。Matrix 界面的虚拟设备方向是从 Matrix 看：客户端输入对应 Matrix 的输出侧。

如果音源在 Matrix 的 `VASIO32 in 31/32`，需要在 Matrix 中把它路由到 `VASIO32 out 31/32`，本程序选择输入 31/32 才能读取。仅上排 in 有电平不代表客户端输入已有信号。ASIO 的输出缓冲由宿主写入，不能用它读取其他客户端输出。

VASIO 跟随 Matrix 主时钟。若报 `ASE_NoClock (-995)` 且查询仍为 48000 Hz，先在 Matrix 将主时钟改为 44100 Hz 并重启音频引擎。本程序不会自行改动 Matrix 的路由。

## 时间与配置

所有协议/采集时间设置可在界面编辑，显示和输入单位统一为毫秒（ms），支持三位小数。内部计时及 JSON 数值仍使用秒，旧配置无需修改。

| 设置 | 默认（ms） | 范围（ms） |
|---|---:|---:|
| 播放提前量 | 2000 | 0～2000 |
| 启动 PTP 等待 | 2000 | 0～30000 |
| 采集预缓冲 | 40 | 0～500 |
| 最大积压 | 100 | 10～1000 |
| 最大发送落后 | 100 | 10～1000 |
| 采集断流超时 | 1000 | 100～30000 |
| TCP 连接超时 | 8000 | 100～60000 |
| RTSP 请求超时 | 8000 | 100～60000 |
| TEARDOWN 等待上限 | 1000 | 100～10000 |
| PTP Sync 周期 | 125 | 1000 × 2ⁿ，15.625～1000 |
| PTP Announce 周期 | 1000 | 1000 × 2ⁿ，125～8000 |
| 音频同步包周期 | 500 | 50～2000 |
| RTSP OPTIONS 保活周期（经用户补充授权） | 10000 | 1000～20000 |

预缓冲必须小于最大积压。低提前量可能使接收端晚到；不保证零延迟。时钟漂移导致的积压、断流或发送落后会显式报错，不通过丢帧或重采样掩盖。

macOS 配置写入 **`~/.config/AirPlayQt.json`**，Windows 仍写入 **exe 同目录的 `AirPlayQt.json`**，均与启动工作目录无关。macOS 原 `~/Library/Preferences/AirPlayQt/AirPlayQt.json` 已按用户要求删除，不迁移、不读取旧路径。成功准备采集后保存设备标识（macOS UID／Windows 驱动 CLSID）、从 0 开始的声道索引及时间参数；发现模式点击“开始”时另行保存目标名称和规范化地址，规则见上方新增修复任务。不保存手动模式地址、选择模式、配对密钥或 HomePod 音量。配置版本保持 1，旧字段不变。

配置使用 `QSaveFile` 原子替换，并关闭直接写入降级。配置缺失使用默认值；损坏、版本无效或参数无效时提示，并在本次运行中禁止覆盖原文件。修复或移走文件后重启可恢复保存。无法保存时明确报错。已保存驱动或声道不存在时保持未选择，不静默替换。

## 代码边界

- `src/ui`：中文 Qt Widgets 界面、有界日志、电平及统计显示。
- `src/app`：配置校验、原子保存、主线程驱动生命周期、会话代次和网络线程协调。
- `src/audio`：x64 注册表驱动枚举、COM/IASIO 所有权、`CaptureTiming` 计时资源管理、回调采集、有界 SPSC 队列及 PCM 转换。
- `src/airplay`：接收端地址模型、Windows DNS-SD、SRP/HKDF/ChaCha20-Poly1305、增量 RTSP/HAP 解析、plist、PTP、共用单／双机状态机、RTP 与有界重传缓存。

ASIO 回调只复制到预分配队列，并通过无锁原子值报告错误。格式转换、加密、网络和日志均不在 ASIO 回调执行。网络对象在所属线程中创建和销毁，不使用同步 socket 等待或嵌套事件循环。

NaN/Inf、驱动重置、采样率/时钟变化、回调重入、采样位置跳变、队列溢出、认证失败和任一接收端失败均显式停止整组。HAP nonce 耗尽拒绝复用；RTP 序号/时间戳按协议位宽回绕，重传保存原密文。

## 验证

自动测试包含 PCM 类型、字节序、限幅、非有限浮点数、声道交织、队列容量与并发；配置校验/损坏保护/写入失败；PoC 确定性向量；本机单／双接收端的真实 SRP、加密 RTSP、SETUP 字段、同组／异组／重复设备、初始化部分失败、超时、取消、音量失败、活跃会话析构、采集故障和断流清理。发现使用模拟原生 API 检查结果去重、IPv6-only、解析失败、128/4 上限、10 秒截止、反复刷新、迟到回调和析构后的原生资源释放。GUI 检查两种模式隔离、最多勾选两台、运行锁定、关闭等待取消，并使用 offscreen 截图生成 `build/<配置>/MainWindow.png`。

`ctest` 生成 `test_audio.txt/xml`、`test_airplay.txt/xml`、`test_app.txt/xml`、`test_discovery.txt/xml`。所有会话显式接收端点列表；`SessionEnvironment` 仅用于测试时钟注入。自动化测试不采集真实 ASIO 输入、不向 HomePod 播放音频。

PoC 保持原样。对照数据位于 `tests/airplay/vectors.json`，可重新生成：

```bash
"/d/Python/python.exe" -m venv venv
./venv/Scripts/python.exe -m pip install cryptography
./venv/Scripts/python.exe tests/airplay/generate_vectors.py
```

显式实机验收工具（不纳入 CTest）固定选择 VASIO-32，参数为从 1 开始的左右通道、运行秒数及一至两个接收端地址：

```bash
./build/debug/test_live.exe 31 32 1800 192.168.8.9 192.168.8.10
./build/debug/test_live.exe 1 2 15 192.168.8.9
```

运行至少 10 秒时会执行原生音量降低 3 dB、静音和恢复；不会修改 PCM。工具调用生产 `Controller`，日志输出状态、电平、积压、包数及重传统计，配置保存在该测试 exe 同目录。

### 本轮设备发现计划执行状态（2026-09-29）

1. 建立共用接收端模型：已完成。
2. 实现 Windows 原生发现：已完成。
3. 扩展共用会话为单／双机：已完成。
4. 接入两种 GUI 模式：已完成。

构建与自动化验收状态在本节记录，原定任务与范围不变。已确认用户反馈旧版稳定；不据此认定本轮新增功能通过听音验收。本轮未改 ASIO、计时精度、Realtime 优先级、编码或发送节奏。

- 原生 API 的只读扫描已完成：发现 `192.168.8.9:7000`、`192.168.8.10:7000` 两台 HomePod 和 `192.168.8.4:7000` MacBook，正常结束扫描；没有执行配对、采集或播放。
- Debug/Release 构建及两套四组 CTest 全部通过，每套 QtTest 结果为 audio 21、airplay 26、app 10、discovery 8 项（包含各组初始化／清理项），无失败或跳过。日志与 XML 位于各构建目录。
- Release 主程序更新已完成：此前旧程序占用导致的链接阻塞已解除，旧进程退出后完成同一 Release 构建；`build/release/AirPlayQt.exe` 与 `build/debug/AirPlayQt.exe` 均为本轮版本。
- **[blocked] 第 2/3/4 项实机验收**：仍需用户在 GUI 验证发现／手动模式、单台／双台、反向选择后的左右播放、原生音量与静音、刷新与重启，以及双台至少 30 分钟回归。
- **[blocked] Windows 10 兼容性**：本轮仅在当前 Windows 11 开发环境验证。
- **[blocked] Claude 上线审查**：当前未发现可用 Claude 工具或命令，尚未完成可维护性、边界条件、回归风险三项审查。本轮未发布 1.0.0。

### 既有版本验收记录（历史）

- Debug/Release 构建及三组自动化测试通过，包含保活与音量串行、保活失败停机、Windows 目录禁止创建文件时保留原配置的测试。
- 19 项 PoC 对照数据通过，包括 RTP 序号/时间戳回绕及负时间偏移的同步包。
- 两台实机 `/info` 返回相同立体声组，支持 44.1 kHz ALAC。
- VASIO-32 31/32：15 秒静音流测试正常开始及停止。
- 两台实机确认原生音量降低、静音和恢复；接收端重传请求已响应。
- **未完成：** 30 分钟持续运行。第一次长测约 34 秒时接收端关闭控制连接，程序停止整组。记录：`build/debug/live-31-32.log`。
- VASIO-32 1/2：15 秒静音流及原生音量/静音/恢复测试正常结束。
- **未完成：** 有声输入的左右听音确认。专门的 31/32 原始缓冲诊断收到 215 次回调、220160 帧，两个 Float32 输入的原始字节全部为零。截图上排 in 31/32 有信号，下排 out 有信号的是 9/10；需将音源路由到 out 31/32。
- 保活版本第二次长测已运行超过 4 分钟，随后为原始缓冲诊断主动中止；不计作 30 分钟通过。日志：`build/debug/live-31-32-keepalive.log`。

上述实机阻塞属于既有计划第 2/3/4 项及验证条件；未以构建成功替代实机验收，也未更改原计划。

用户另行明确授权在第 3 项补充 OPTIONS 保活及高级时间参数；保活与音量串行，请求失败停止整组。对照诊断中，10 秒 OPTIONS 使两台控制连接保持到 45 秒并正常退出；正式实现仍需重新进行 30 分钟验收。

`test_probe.exe` 是用户授权的只读诊断工具，固定检查 VASIO-32 客户端输入 31/32，运行 5 秒，输出通道名、格式、回调/帧数、非零原始字节数和转换峰值。不扫描其他通道、不播放、不保存音频。运行前须停止占用该 ASIO 驱动的验收程序。

### 采集破音诊断

用户随后完成 Matrix 路由并确认命令行 AirPlay 发送通过。在相同 Internal Clock / 44100 Hz / 1024 帧配置下，用户报告 DAW 经 VASIO32 监听正常，而本程序采集的 WAV 偶发破音。后续 1 ms 计时对照中用户确认无破音，结果及正式接入状态见下文。

一次性录音工具固定读取 VASIO-32 输入 31/32，不启动 AirPlay、不修改 GUI。先停止 GUI 发送并释放驱动，再在 MSYS Bash 运行：

```bash
PATH=/clang64/bin:/usr/bin:$PATH ./build/release/test_record.exe 60 build/diagnostics/capture-trace-31-32
```

`60` 为诊断录音秒数（允许 1～300）。工具生成三个同前缀文件：

- `-raw-f32.wav`：原始 Float32 数据，仅将左右平面数据交织，保留样本位模式。
- `-pcm16.wav`：使用生产 PCM 转换器得到的 16-bit 数据，与原始文件逐帧对齐。
- `-callbacks.csv`：双缓冲索引、QPC 回调时间/耗时、线程 ID、驱动样本位置/时间标志，以及复制前、队列内、复制后和消费端的数据校验值。

回调诊断使用预分配内存，停止采集后才写 CSV。GUI 中默认不开启回调诊断。CSV 中 `time_flags` 标记驱动样本位置/时间是否有效；未提供时间信息的旧式回调不能据其零值判断断流。`copy_matches_source` 和 `queue_matches_consumer` 应为 1；录音停止时尚未消费的尾部块相应字段为空。`trace_overflow` 应为 0。哈希一致仅说明所检查的字节一致，不能证明驱动交来的波形本身正常。

输出文件不覆盖已有文件，再次录音需使用新前缀。工具退出时才完成 WAV 头部，强制终止可能留下未完成文件。模拟 IASIO 测试覆盖双缓冲重用、左右映射、旧式/时间信息回调、独立生产线程与消费线程、样本位置跳变及诊断容量上限；这些测试不能替代 VASIO 实机结果。

用户确认带轨迹的 Float32 录音仍有破音后，15 秒 CSV 中的 646 次回调未见样本位置跳变、双缓冲顺序错误或数据校验差异；645 个完整 WAV 块也与回调数据一致。但回调间隔集中在 15～16 ms 和 30～32 ms，最长 46.4463 ms，明显偏离均匀的 1024/44100 秒节拍。该现象是后续对照依据，尚不能单独证明破音根因。

经用户明确授权，最初仅在 `test_record` 加载 ASIO 前申请 1 ms 计时精度并禁用忽略计时精度请求的策略。用户运行 15 秒对照录音并确认没有破音。两份 CSV 均为 646 次回调，均没有数据校验差异、样本位置跳变或轨迹溢出；回调间隔中位数由 16.7105 ms 变为 23.1707 ms，最长间隔由 46.4463 ms 变为 25.8563 ms，超过 30 ms 的间隔由 307 次降为 0 次。理论块间隔为 1024/44100 = 23.21995 ms。这支持计时精度/进程策略与本次破音有关，不能单凭短测保证长期无破音。

用户随后确认接入正式 GUI。现在 `audio::CaptureTiming` 统一封装 `timeBeginPeriod(1)`、`SetProcessInformation` 和恢复校验，由 `AsioCapture` 在准备采集、查询/设置采样率之前持有，在驱动停止并释放缓冲后恢复。录音工具共用该生命周期，不重复申请。单会话保护先于计时资源申请；重复启动和其他采集对象不能覆盖原策略快照。恢复失败时保留资源供后续停止重试，GUI 与录音工具显式报告错误。析构路径作最后的恢复尝试并记录失败；强制杀进程不等同于正常退出。

本次 Debug/Release 构建与两套三组 CTest 均通过。新增测试覆盖计时资源异常释放、重复恢复、准备后取消、连续启停、驱动采样率/缓冲创建/空缓冲/启动失败、析构停止、第二采集对象拒绝启动；离线工具验证两种 WAV 和 CSV 输出。本轮未自动打开 ASIO 或向 HomePod 发送音频。

上述为计时修复阶段的历史记录。用户后续已反馈旧版稳定；本轮设备发现版本的验收和上线阻塞见“本轮设备发现计划执行状态”，不以旧版反馈替代新增功能验收。

对照录音请用新前缀，避免覆盖未申请 1 ms 精度时的基线文件：

```bash
PATH=/clang64/bin:/usr/bin:$PATH ./build/release/test_record.exe 60 build/diagnostics/capture-1ms-31-32
```

控制台应显示 `TIMER request_ms=1 ignore_timer_resolution_disabled=1`，正常结束显示 `TIMER restored=1`；CSV 新增两个设置状态列。1 ms 是进程的计时精度请求，不是对实际 ASIO 回调间隔的保证；效果仍需通过波形和 CSV 验证。`--check-output` 离线检查同时验证这些系统 API 的申请、读回和恢复，不会打开 ASIO 设备。

## Windows VST3 计划执行状态

沿用已确认的五项计划及顺序，仅更新执行状态。开发包与自动化结果不替代实机上线门槛。

1. **接入 SDK 与插件构建：完成。** 双构建开关、标准 SDK 工厂／入口／目录包、固定 FUID 已接入；保留完整 SDK 目标。对齐分配阻塞已按用户后续指定的 operator new/delete 方法修复，SDK 补丁单独保存。Debug／Release 及 ASIO 路径无效时的 VST3-only 构建通过。
2. **建立可复用的音频与会话边界：完成。** CaptureStream、SessionController、StreamingPanel 由独立程序与插件共用；原 ASIO 生命周期与独立 EXE 策略保留。
3. **实现 VST3 音频与处理状态：完成。** 立体声 f32/f64 透传、352 帧入队、实时条件与故障停止、标准旁路及确定性转换／ALAC 对照已实现并测试。
4. **实现 Qt 编辑器和插件生命周期：未完成。[blocked]** 代码及隐藏 Win32 宿主测试已完成：自建／借用／不兼容 Qt、嵌入／缩放、窗口关闭与重开、多实例占用、发现取消、活动连接清理、实际 DLL 反复加载卸载通过。仍缺少 Cubase／Nuendo 的嵌入与卸载实机结果，不能据此认定宿主兼容。
5. **工程状态与交付：完成。** 版本化状态、整体校验、Debug／Release 开发包、构建／运行／安装／重扫说明及测试截图已提供；未安装系统插件目录、未制作自包含包。

上线门槛仍为 **[blocked] Windows 10 兼容性**、**[blocked] Cubase／Nuendo 实机版本与验收结果**、**[blocked] Claude 可维护性／边界条件／回归风险审查**。没有自动向 HomePod 播放音频，也没有安装到系统插件目录。

SDK validator 对插件执行 47 项检查、SDK 自测执行 51 项；两者均通过。原有协议／采集测试仍保留。当前插件使用已确认的进程内状态与有界队列，不使用 SDK Data Exchange 辅助类，但完整编译该文件，不通过排除文件规避兼容问题。

2026-09-29 最终回归：Debug 和 Release 各 10 组 CTest 全部通过。JUnit 结果保存于 `build/diagnostics/vst3-debug-ctest.xml` 和 `vst3-release-ctest.xml`。VST3-only 构建将 `ASIO_SDK_ROOT` 指向不存在的 `E:/does-not-exist`，仍成功生成插件并通过 validator；未依赖本机可用 ASIO SDK 掩盖依赖问题。实际 DLL 测试另确认 FreeLibrary 后模块不再加载，没有通过固定 DLL 常驻规避卸载测试。

## 实现参考

- 项目原始 `airplay_poc.py`：AirPlay 协议与确定性向量依据。
- 官方 ASIO SDK 的 `common/asio.h`、`common/iasiodrv.h`：缓冲、回调与驱动 ABI。
- [VB-Matrix 官方手册](https://vb-audio.com/Matrix/VBMatrix_UserManual.pdf)：路由及 VASIO 主时钟。
- [Qt QSaveFile](https://doc.qt.io/qt-6/qsavefile.html)、[Qt QAbstractSocket](https://doc.qt.io/qt-6/qabstractsocket.html)：原子保存与异步 socket。
- [Windows DnsServiceBrowse](https://learn.microsoft.com/en-us/windows/win32/api/windns/nf-windns-dnsservicebrowse)、[DnsServiceResolve](https://learn.microsoft.com/en-us/windows/win32/api/windns/nf-windns-dnsserviceresolve)：Windows 10+ 原生异步 DNS-SD；[BrowseCancel](https://learn.microsoft.com/en-us/windows/win32/api/windns/nf-windns-dnsservicebrowsecancel) 的终止回调与 [ResolveComplete](https://learn.microsoft.com/en-us/windows/win32/api/windns/nc-windns-dns_service_resolve_complete) 的实例释放约定。
- [Windows timeBeginPeriod](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod)、[SetProcessInformation](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation)：配对释放计时请求及显式控制忽略计时精度请求的策略。
- [OpenSSL EVP](https://docs.openssl.org/3.6/man3/EVP_EncryptInit/) 与 [HKDF](https://docs.openssl.org/3.6/man3/EVP_PKEY_CTX_set_hkdf_md/)：认证加密和密钥派生。

ASIO SDK、Qt、OpenSSL、libplist 的许可证由各上游提供；本工程未复制 SDK 实现源码。macOS 完整包内附运行依赖的许可证与来源信息；不提供自动安装程序或公证安装包。Windows 开发构建的交付边界不变。
