# Reeden Kobo 兼容层 (0.7.8 Experimental)

在 **Kobo Clara Colour（产品代码 393）** 上原字节运行官方 **Reeden Kindle（同时支持 `1.43.2+791` 与 `1.43.1+780`）** 核心的开源 C 宿主与硬件兼容层。

> **合规与版权说明**：本仓库仅包含 MIT 协议的 Kobo 宿主适配代码、预编译适配层二进制及 SIL OFL 开源字体，**不包含任何 Reeden 官方闭源二进制或 Debian 发行版运行库文件**。安装脚本会在本地电脑上结合你自己下载的 `Reeden-kindle.zip`（原字节不改）与 Debian 官方镜像源一键完成组包与安装。

> **非官方 / Experimental**：这是社区兼容层，不是 Reeden 或 Kobo 官方产品，不代表官方支持或背书。运行时拥有设备 root 权限并控制硬件；开始前请备份重要书籍和笔记，不能保证断电、损坏或所有配置都可恢复。
>
> **下载与电脑要求**：[最新 v0.7.8 一键安装/卸载包](https://github.com/SSNQ/reeden-kobo/releases/tag/v0.7.8)，需要 **Python 3.10+**。固定的 Bullseye 依赖免费 LTS 已结束，哈希校验不是持续安全更新；见 [依赖维护方案](docs/依赖安全维护.md)。

---

## 当前主分支进展（2026-11-05）

v0.7.8 针对安装后资源缺失导致无法启动的问题，加入受校验保护的本地资源恢复归档、每次启动的完整性复核，以及显示实际版本和失败文件的自检。仅补回缺失资源，已有文件内容变化、恢复归档损坏或程序文件缺失仍会阻止启动；不重算校验清单掩盖错误。详见[本次发布说明](docs/releases/v0.7.8.md)与[资源缺失排查和恢复](docs/资源缺失排查与恢复-20261105.md)。

v0.7.6/v0.7.7 的下载恢复、官方核心校验及卸载修复继续保留。791 使用完整文件 SHA 清单；780 的验证边界没有扩大。

v0.7.5 已整合 Wi-Fi 桥接、夜间彩字分区刷新、翻页动画与文字残影诊断。开发细节见[刷新与翻页动画开发记录](docs/刷新与翻页动画开发记录.md)及[文字下缘横向拖影诊断](docs/文字下缘横向拖影诊断-20261102.md)。

**2026-11-03 用户反馈**：隔夜休眠问题已解决，续航实测可接受。这是用户反馈，本轮未进行设备测试，也未新增功耗测量。文字残影尚未完全消除；保留已撤回 Q（动画后整页复绘）的状态，不将其作为默认修复，相关实验选项仍默认关闭。

## 安装前必读

- 这是 **Experimental / GitHub Pre-release**，已测设备是 Kobo Clara Colour（393），固件 4.46.23836。仅凭同型号不能保证所有配置都正常；其他机型拒绝安装和启动。
- 安装器和启动脚本检查产品代码；`fb-guard` 另检查竖屏 `1072×1448`、32bpp、rotation=3 及帧缓冲布局，在交接原生界面之前拒绝不匹配的显示配置。
- 程序及阅读数据安装在用户分区 `.adds/reeden-kobo`，菜单在 `.adds/nm/reeden-kobo`。不替换固件、系统库或开机启动项，不设置开机自启。运行时需要控制屏幕、触摸、电源和网络；恢复网络时可能回写 `/etc/resolv.conf`，恢复 DNS。
- 开始前备份重要书籍和笔记。升级会保留旧安装和数据副本，需要足够剩余空间；不要在 Reeden 运行期间升级，也不要在写入时拔线。
- 默认菜单不再提供开启 SSH 的入口，只保留 **Reeden SSH - Stop** 以便关闭旧版监听服务。可选诊断工具仅允许密钥认证，见 [SSH 说明](docs/SSH.md)。升级不自动终止现有 SSH 会话，旧服务需手动停止或重启设备。
- [下载 v0.7.8 安装包](https://github.com/SSNQ/reeden-kobo/releases/tag/v0.7.8)。v0.7.3 及以前已被替代；不要混用旧标签自动生成的源码包和新版附件。

### 退出和故障恢复

1. 正常退出：同时按住屏幕左上角和右上角约 2 秒，返回 Kobo 原生界面。
2. 软件恢复：若原生菜单可用，选择 **Reeden Kobo - Return Home**；已有 SSH 时可运行 `sh /mnt/onboard/.adds/reeden-kobo/recover.sh`。
3. 若软件完全无响应，按设备的硬件强制关机/开机方式恢复。长按电源键可能需要较长时间，必要时接 USB 充电后重试；不能保证所有内核或硬件故障均可由软件恢复。
4. 需卸载时先退出 Reeden，再连接 USB，双击电脑端「一键卸载」脚本。默认保留完整备份，可选择删除当前安装及阅读数据；详见下方卸载步骤。

---

## 一、支持环境与前提条件

| 项目 | 要求 |
|---|---|
| **支持设备** | Kobo Clara Colour（产品代码 `393`，MT8113T，1072×1448 32bpp，旋转 `3`） |
| **已测固件** | `4.46.23836`（ARMHF Linux 4.9.77） |
| **前置插件** | Kobo 已安装 [NickelMenu](https://pgaskin.net/NickelMenu/)（存在 `/mnt/onboard/.adds/nm/` 目录） |
| **官方核心** | 支持官网**Kindle `1.43.2+791`** 与 **`1.43.1+780`** (`Reeden-kindle.zip`，可自行下载或由安装脚本自动从官网拉取并校验) |
| **电脑环境** | macOS / Windows / Linux，安装有 **Python 3.10+**（仅用标准库，无需安装 Zig、C 编译器或第三方包） |

---

### 实测兼容性

| 组合 | 状态 |
|---|---|
| Clara Colour（393）/ 固件 4.46.23836 / 官方 Kindle 1.43.2+791 | 本轮 0.7.2 基本实体验收通过；新工具与版本的验证范围另列 |
| 同设备 / 官方 Kindle 1.43.1+780 | 历史测试通过，本轮未全面复测 |
| 其他型号、固件或非白名单核心 | 不支持或未验证，不能推断兼容 |
| macOS / Windows / Linux，Python 3.10/3.12 | USB 工具和启动脚本 CI；本轮实体 USB 升级仅测 macOS |

详细 [兼容性测试表与真实设备截图](docs/兼容性.md) 区分实机、模拟盘、QEMU 和尚未验证的项目。

## 二、一键安装与升级（普通用户）

**已经安装过？** 下载新版完整 ZIP 并解压，退出 Kobo 上的 Reeden、连接 USB 并在设备上点“连接”，然后双击 **一键更新-Windows.bat** / **一键更新-macOS.command**（Linux：`sh 一键更新-Linux.sh`）。自动保留数据与旧版备份，不需要输入 `--upgrade`，缺少官方包时自动获取并校验。完成后安全弹出再启动。更新入口安装的是这个 ZIP 中的版本，不会自动下载新版工具；首次安装仍按下方步骤。详见 [更新说明](docs/安装与卸载.md#保留数据升级)。


### 第 1 步：准备官方包（或交由脚本自动下载）
从 [Reeden 官方下载页](https://reeden.app/cn/download/kindle) 下载 `Reeden-kindle.zip`（支持 `1.43.2+791` 及 `1.43.1+780`），直接放在本目录或电脑的「下载 (`Downloads`)」文件夹中（无需手动解压）；如果在交互模式下未找到本地包，直接按回车即可由脚本自动从 Reeden 官网拉取最新包并校验 SHA-256。

### 第 2 步：选择任一方式一键安装

> **macOS 提示**：从压缩包解出的脚本带有隔离属性，首次双击 `一键安装-macOS.command` 若被系统拦截，请在「终端」中执行一次：
> `xattr -d com.apple.quarantine ~/Downloads/reeden-kobo/一键安装-macOS.command`（路径按实际解压位置调整），或右键该文件选择「打开」。
> 若解压工具丢失了可执行权限，先执行 `chmod +x 一键安装-macOS.command`；也可以直接在「终端」里运行 `python3 install.py --usb`。
>
> **网络提示**：依赖包从 `deb.debian.org` 与 `raw.githubusercontent.com` 下载。若下载缓慢或失败，可配置代理后重试；已下载的依赖会缓存在 `cache/` 目录，重复运行不会重新下载。

#### 方式 A：USB 数据线一键安装 / 升级（推荐，免 SSH）
1. 用 USB 数据线将 Kobo Clara Colour 连接到电脑，在 Kobo 屏幕点击「连接」（电脑出现 `KOBOeReader` 盘符）。
2. 直接双击运行对应系统的启动脚本：
   - **macOS**：双击 `一键安装-macOS.command`
   - **Windows**：双击 `一键安装-Windows.bat`
   - **Linux / 终端**：运行 `python3 install.py --usb`（若已安装旧版需保留书库与阅读进度升级，可加 `--upgrade`，交互模式下会自动识别并保留数据）
3. 脚本会自动完成：
   - 校验官方 `Reeden-kindle.zip`：791 核对全部 243 个文件的 SHA-256 及精确文件集合；780 核对两库白名单、版本元数据、必要资源和字体引用（尚无全文件清单）
   - 下载并校验 10 个固定版本的 Debian Bullseye ARMHF 运行库与 Mozilla 根证书
   - 合成 `ui-assets` 中文字体与数学符号覆盖层
   - 校验 Kobo 机型码（`393`），将清单中的全部文件写入 `.adds/reeden-kobo` 并逐文件校验 SHA-256（升级时自动备份旧版并保留 `userdata/` 阅读进度、笔记、冷暖光设置及 `storage/` 书库）
   - 写入 NickelMenu 启动项 `.adds/nm/reeden-kobo`
4. 安全弹出 USB，回到 Kobo 原生主界面，点击右下角 **NickelMenu → `Reeden Kobo (test)`** 即可启动。

#### 方式 B：Wi-Fi SSH 无线一键安装 / 升级
若 Kobo 已配置可用的 SSH 连接（如 KOReader 密钥认证，端口 `2222`；可选 Reeden 密钥认证端口 `2223`），可使用下列命令。阅读器本体和 USB 安装不依赖 KOReader。
```sh
# 全新安装
python3 install.py --ssh 192.168.1.100 --port 2222

# 保留书库、阅读进度与冷暖光设置升级
python3 install.py --ssh 192.168.1.100 --port 2222 --upgrade
```

#### 方式 C：仅在电脑生成完整运行包 (`package/`)
```sh
python3 install.py --official-core /path/to/Reeden-kindle.zip --output package
```

---

## 三、核心适配特性 (0.7.5)

- **原字节复用官方核心（双版本支持）**：同时支持 **`1.43.2+791`** 与 **`1.43.1+780`**，`libflutter_engine.so`、`libapp.so` 及原始资源保持原字节不变；不修改账号、授权或付费逻辑；禁用 Kindle 自更新覆盖 Kobo 宿主。
- **墨水屏刷新策略（对齐 KOReader MTK）**：
  - **白底灰阶**：局部 `GLR16 (4)` / 清屏 `GC16 (2)`
  - **纯灰阶黑底（夜间模式）**：局部 `GLKW16 (9)` / 清屏 `GCK16 (8)`，配合 KOReader MTK `night_mode 4` 硬件反相与互补灰阶提交，改善黑底全刷白闪；实际效果需按内容和主题验证
  - **真实彩色与旧色擦除**：局部 `GLRC16 (11)` / 清屏 `GCC16 (10)` + `CFA G2 (0x600)`，抑制 2 级以内 RGB 舍入噪声误触发彩色模式
  - **帧同步与边界对齐**：16 像素对齐 + 边缘余量，RAM 帧缓存跳过重复帧，每次 FULL REAGL/CFA 请求等待刷新完成同步
- **前光与冷暖光控制（自适应 `1.43.2+` 原生滑块与 `1.43.1` 叠画滑块）**：
  - **`1.43.2+791`**：直接对接 Reeden 官方 UI 原生新增的「暖光」滑块（`currentAmberLevel` `0..24` 映射至底层 `lm3630a_led/color` `10..0` 反向混光），自动持久化至 `userdata/kobo-light.conf`
  - **`1.43.1+780`**：在「书页中央 → 显示」前光行下方合成硬件「冷暖」控制行（0 最冷 ～ 100 最暖，10% 步进）
- **电源键锁屏（含 `1.43.2+` 彩色与透明叠底锁屏）、深度休眠与唤醒恢复 (0.7.5)**：
  - 支持「自定义锁屏画面」（在 `1.43.2+791` 下支持 3/4 通道 **RGB 彩色锁屏封面**与**透明区域叠在当前书页上**），短按电源键锁屏并进入 Linux `mem` 深度休眠（充电时按 MTK 限制仅锁屏关灯、跳过内核深睡），再按直接唤醒回到当前书页
  - 使用独立非 `SIGCHLD` (`__WCLONE`) 监督进程等待休眠脚本，避免与 Dart 3.12.2 全局 `wait()` 回收线程竞争导致唤醒卡死
  - 记录 `preparing / suspended / resuming / complete` 阶段时间戳；准备或唤醒恢复超过 45 秒由 `guardian.sh` 自动恢复 Kobo 原生界面
- **安全交接与退出恢复**：
  - 首帧在内存中渲染完成后才暂停 Nickel 并接管屏幕与触控
  - **同时按住屏幕左上角和右上角约 2 秒**即可正常退出 Reeden，还原硬件反相与前光，并以断电干净状态将 Wi-Fi 交还给新启动的 Kobo 原生界面

---

## 四、日常使用与故障恢复

1. **启动**：先从 KOReader 退出回到 Kobo 原生主页并连接 Wi-Fi，在 NickelMenu 点击 `Reeden Kobo (test)`。
2. **导书**：应用内 `/mnt/us` 映射到 `/mnt/onboard/.adds/reeden-kobo/storage/`（如放入 `storage/documents/` 或使用 Wi-Fi 传书）。
3. **锁屏与唤醒**：建议在「我的 → 墨水屏」开启「自定义锁屏画面」。短按电源键休眠，再按唤醒继续阅读。
4. **退出回原生系统**：同时按住屏幕 **左上角 + 右上角约 2 秒**。
5. **紧急恢复与卸载**：
   - NickelMenu 点击 `Reeden Kobo - Return Home`，或 SSH 运行 `sh /mnt/onboard/.adds/reeden-kobo/recover.sh`
   - 卸载：退出后连接 USB，双击电脑端「一键卸载」脚本，默认保留完整备份（见下一节）

## 五、一键卸载（USB，免 SSH）

1. 先退出 Reeden，回到 Kobo 原生主界面。如果开启过 Reeden SSH，先重启 Kobo 再卸载，以关闭仍在内存中的服务器和已有会话。
2. 用 USB 连接电脑，在 Kobo 屏幕点「连接」，电脑出现 `KOBOeReader` 盘。
3. 在本页安装 ZIP 解压目录双击 `一键卸载-macOS.command` 或 `一键卸载-Windows.bat`；Linux 运行 `sh 一键卸载-Linux.sh`。
4. 按提示选择卸载方式，完成后安全弹出 USB。

| 方式 | 程序与数据处理 | 磁盘空间 |
|---|---|---|
| 默认：保留完整备份 | 将整个当前安装改名为 `.adds/reeden-kobo-uninstalled-日期-随机码/`，移除未被修改的标准菜单 | 备份仍占空间；可复制到电脑后手动删除 |
| 删除当前安装及数据 | 选择「2」，再输入 `DELETE REEDEN`；删除当前应用及其中的书籍、笔记、进度、诊断密钥 | 释放当前安装空间；以前升级/卸载的备份保持不动 |

macOS 若拦截卸载脚本，右键打开或使用下方终端命令；也可参照安装脚本的隔离属性说明。

卸载工具不需要官方安装包或联网，只需同目录的 `install.py` 和 Python 3.10+。它不移除 NickelMenu、KOReader、原生书库和固件。自定义菜单会保留并提示手动处理；如果同时接了多台 Kobo，工具会停止，要求明确指定设备。运行中的会话锁和符号链接路径会拒绝卸载。

终端用法：

```sh
# 自动查找 USB 设备，交互确认（默认保留备份）
python3 uninstall.py --usb

# 手动指定设备，例如 Windows 的 E:\ 或 macOS 的 /Volumes/KOBOeReader
python3 uninstall.py --kobo-mount /Volumes/KOBOeReader

# 删除当前安装及阅读数据，仍需输入 DELETE REEDEN 确认
python3 uninstall.py --usb --purge
```

备份恢复、残留目录清理和安装常见问题见 [安装与卸载指南](docs/安装与卸载.md)。备份含私人书籍、笔记和可能的诊断密钥，请勿上传到 GitHub 或发给别人。卸载中途拔线、磁盘损坏或断电仍可能需要人工恢复。

详细验收记录与已知限制请参阅 [docs/测试说明.md](docs/测试说明.md)。

---

## 六、从源码交叉编译与运行测试（开发者）

本仓库已内置 `prebuilt/` 预编译二进制，普通用户无需编译。开发者如需自行从 C 源码交叉编译：

1. 准备 [Zig 0.13.0](https://ziglang.org/download/)。
2. 编译 ARMHF 宿主、兼容库、静态帧缓冲守卫与目标机测试程序（支持无官方二进制时通过 `src/flutter_engine_stub.c` 独立完成编译，产物哈希与连接官方库完全一致）：
   ```sh
   ZIG=/path/to/zig sh scripts/build.sh
   sha256sum -c prebuilt/PREBUILT_SHA256SUMS
   ```
3. 运行电脑端隔离回归测试：
   ```sh
   python3 tests/launcher_test.py
   python3 tests/native_network_test.py
   python3 tests/power_guard_test.py
   python3 tests/wake_guard_test.py
   ```

---

## 七、验证范围与版本发布

[0.7.5 发布说明](docs/releases/v0.7.5.md) 和 [真机验收记录](docs/真机验收-0.7.2.md) 区分自动化验证与已完成的设备测试。隔离测试及 QEMU 不能替代隔夜休眠和真实功耗测量。

新 Release 由标签 CI 构建：标签、源码 ZIP、安装 ZIP 的代码来自同一提交，附件提供 `COMMIT.txt` 和 `SHA256SUMS`。请勿忽略 Experimental 标记。旧历史中的路径及私人示例地址已清理，已有克隆请参阅 [历史清理说明](docs/历史清理.md)。

## 八、安全反馈与实机截图

安全漏洞使用 [私密报告](https://github.com/SSNQ/reeden-kobo/security/advisories/new)，普通问题用 [Bug 表单](https://github.com/SSNQ/reeden-kobo/issues/new?template=bug_report.yml)。请勿公开书库、原始备份、账号或密钥。维护范围见 [SECURITY.md](SECURITY.md)，本轮扫描范围和边界见 [安全检查记录](docs/安全检查-2026-11-01.md)。

![Clara Colour 实际运行的 Reeden 首页](docs/images/clara-colour-home-20261101.png)

2026-11-01 从用户准备的可公开页面读取设备帧缓冲，原始像素编码，无内容改绘；软件组合与限制见 [截图说明](docs/兼容性.md)。帧缓冲截图不能反映实际面板残影、白闪或前光，阅读统计不是续航测量。

## 九、开源许可与致谢

- 适配层代码采用 [MIT License](LICENSE)。
- 官方 Reeden 核心归 Reeden 官方所有，遵循其自身条款，严禁在公开仓库或衍生包中默认分发官方闭源二进制。
- 包含 Flutter Embedder Header (`vendor/Flutter-LICENSE`)、cJSON (`vendor/cJSON-LICENSE`)、Noto CJK / Math (`vendor/NotoSans*-LICENSE`)、musl (`vendor/musl-LICENSE`) 与 Zig (`vendor/Zig-LICENSE`) 许可证声明。
- 硬件接口参考：[KOReader Kobo device](https://github.com/koreader/koreader/blob/master/frontend/device/kobo/device.lua)、[KOReader MTK framebuffer](https://github.com/koreader/koreader-base/blob/master/ffi/framebuffer_mxcfb.lua)、[Kobo MT8113 Kernel](https://github.com/kobolabs/Kobo-Reader/tree/master/hw/mt8113-libraC_vision7)。
