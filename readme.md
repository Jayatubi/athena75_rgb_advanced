# Athena75 RGB Advanced

**YDKB / KBDFans Athena75 RGB 的第三方固件。**

这块键盘上有一片 128×128 的彩色 LCD。原厂固件用它循环播放几段内置 GIF——屏幕上会出现什么，出厂时就定死了。

这个固件把那片屏做成一个**能装、能跑自制软件的小系统**：写一个 C 程序，打包成 `.app`，通过 USB 装进键盘自己的 flash 槽里，然后在键盘的启动器里选中运行。屏幕上跑的是真正的程序——它读按键、存自己的配置、画自己的界面，而不是一段预先录好的动画。

<img src="keyboards/ydkb/athena75_rgb_advanced/docs/apps/matrix.gif" width="200" alt="MATRIX"> <img src="keyboards/ydkb/athena75_rgb_advanced/docs/apps/wfc.gif" width="200" alt="WFC"> <img src="keyboards/ydkb/athena75_rgb_advanced/docs/apps/fish.gif" width="200" alt="FISH">

键盘该有的部分一样不少：矩阵扫描、RGB Matrix、SOCD / Snap Tap，以及用 Vial 上位机随时改键，全部保持 QMK / Vial 的行为。

| 项目 | 值 |
|---|---|
| 主控 | RP2040 双核 —— core0 跑 QMK，core1 跑显示 OS |
| 屏幕 | 128×128 RGB565，GC9107 SPI |
| 存储 | 外挂 16 MiB SPI-NOR，其中 8 MiB 划给 app（32 × 256 KiB 槽） |
| 改键 | Vial（默认 keymap 为 `vial`） |
| USB | VID/PID `0x9D5B` / `0x2514`，Raw HID `0xFF60` / `0x61` |

---

## 快速开始

仓库里已经提交了编译好的产物，想直接用可以跳过前两步：`keyboards/ydkb/athena75_rgb_advanced/artifacts/` 下有固件 UF2、macOS / Windows 的 `host_tool`，以及全部 `.app`。

```bash
KB=keyboards/ydkb/athena75_rgb_advanced

# 1) 固件 —— 在固定版本的 QMK Docker 镜像里编译，产物落到 $KB/artifacts/firmware/*.uf2
bash $KB/tools/build_wsl.sh          # Windows + WSL
bash $KB/tools/build_mac.sh          # macOS
python3 $KB/tools/build.py           # 上面两个脚本都只是它的平台外壳

# 2) host_tool —— 跨平台的原生命令行上位机
cmake -S $KB/src/host -B $KB/src/host/build -DCMAKE_BUILD_TYPE=Release
cmake --build $KB/src/host/build --config Release
```

`host_tool` 出现在 `$KB/src/host/build/Release/` 下，下文都用 `host_tool` 指代它。刷固件：

```bash
host_tool upload                     # 不带参数时自动挑 artifacts/firmware/ 里的 UF2
```

键盘 LCD 上会弹出确认框，按 Enter 确认后自动重启进 BOOTSEL 并拷贝 UF2。也可以老办法：按住 BOOTSEL 上电，把 UF2 拖进出现的 U 盘。

装几个 app 试试，然后按键盘上的 **gif 键**进入 OS 模式，启动器里就能看到它们：

```bash
host_tool app install $KB/artifacts/apps/wfc.app
host_tool app install $KB/artifacts/apps/settings.app
```

---

## 架构

三块东西：跑在键盘上的固件、连接键盘的 host_tool、以及一台把整块板子搬到 PC 上的仿真器。

### 固件 — `src/firmware/`

RP2040 的两个核分工固定：

- **core0 = QMK**：矩阵扫描、USB、Vial 改键、RGB Matrix，行为与上游一致。
- **core1 = 显示 OS**：开机动画 → 桌面启动器 → slot app。菜单引擎、app 加载器、绘图库都在这一侧。

**gif 键**（键码 `0x7e04`）只做一件事：在两种输入模式之间切换。

| 模式 | 按键去哪 |
|------|---------|
| 键盘模式（默认） | 正常发给电脑 |
| OS 模式 | 除 gif 外的所有键改道进入 core0 → core1 的事件队列，驱动启动器和当前 app，**不**发给电脑 |

OS 模式闲置约 30 秒会自动退回键盘模式，避免忘了切换而打不出字。LCD 与 RGB 共用一套熄屏超时（可设 1/5/10/15 分钟或常亮），任意键唤醒，并与电脑的 USB 挂起 / 恢复同步。面板被外壳挡住一圈时，可以用固件自带的校准屏定义一个"虚拟屏幕"，之后所有内容都相对这个窗口绘制。

16 MiB flash 分三段：固件区、4 MiB 开机动画区、8 MiB app 槽区。开机动画是一张独立烧录的 QGF，换成自己的图只要 `python3 tools/png_to_uf2.py boot my_splash.png` 打一个 boot UF2 再刷进去。片内 264 KiB SRAM 的顶部切出 80 KiB 固定窗口，专给当前运行的 app 放 `.data/.bss`。完整分区见 [`docs/flash_map.md`](keyboards/ydkb/athena75_rgb_advanced/docs/flash_map.md) 与 [`docs/ram_map.md`](keyboards/ydkb/athena75_rgb_advanced/docs/ram_map.md)。

### host_tool — `src/host/`

一份 CMake 工程，Windows / macOS / Linux 同源编译，通过 Raw HID 跟键盘对话：

| 命令 | 作用 |
|------|------|
| `devices` | 列出所有可操作对象：插着的键盘、正在跑的仿真器，以及各自的固件版本 |
| `upload [uf2]` | 刷固件（LCD 确认 → BOOTSEL → 拷贝 UF2） |
| `app pack / info / install / update / launch` | 打包、检查、安装、升级、直接启动 slot app |
| `snapshot [-o s.png]` | 把 LCD 当前画面抓成 PNG |
| `synctime` / `daemon` | 把电脑的时间推给键盘（带时钟的 app 用），或常驻对时 |
| `backup` / `restore` | Vial / VIA 配置（EEPROM）的备份与恢复 |
| `diag` / `fw` | 打印 flash 与 EEPROM 布局、固件构建号与 ABI |
| `probe read / erase / prog` | JEDEC 识别与 flash 探针读写（调试用；写操作只校验地址在窗口内，别碰固件与 EEPROM 区） |

`-d, --device` 指定操作对象，可以是键盘也可以是仿真器；同时插了两把键盘时它会直接报错列出候选，不猜。

### athena_sim — `src/sim/`

一台**全系统 RP2040 仿真器**，跑的是原封不动的固件 UF2 和 `.app`：固件里没有任何 `#ifdef SIMULATOR`，也不需要为仿真单独编译。UF2 被写进一个 16 MiB 的 flash 镜像，然后从 reset 向量开始被解释执行——经过真的 boot2、真的 ChibiOS 调度器、真的 USB 枚举，最后落到建模的 GC9107 屏和 WS2812 灯带上。

- **两个前端**：`athena_sim` 是一个 SDL2 窗口（左边面板，下面虚拟键盘，右边状态与日志，键帽还会按当前灯色染色）；`athena_sim_cli` 是同一台机器去掉窗口，给脚本和 CI 用。
- **能接东西**：仿真出来的 Raw HID 可以发布到 TCP 端口，所以 `host_tool` 的每条命令都能原样对着它跑；另有控制 socket（按键、截图、整机存档）和 gdb 桩（两个核就是两个线程）。
- **可回归**：调度是确定性的，配套的像素级回归测试逐字节比对面板输出。本 readme 里的 GIF 就是它录的。

用法见 [`src/sim/README.md`](keyboards/ydkb/athena75_rgb_advanced/src/sim/README.md)，内部实现（机器模型与三档 JIT）见 [`docs/simulator.md`](keyboards/ydkb/athena75_rgb_advanced/docs/simulator.md)。

---

## 预置 app

源码在 [`src/app/`](keyboards/ydkb/athena75_rgb_advanced/src/app)，打包好的 `.app` 在 [`artifacts/apps/`](keyboards/ydkb/athena75_rgb_advanced/artifacts/apps)，装上就能用。点名字看它的说明。

| | | |
|:---:|:---:|:---:|
| [**SETTINGS**](keyboards/ydkb/athena75_rgb_advanced/src/app/settings/README.md)<br><img src="keyboards/ydkb/athena75_rgb_advanced/docs/apps/settings.gif" width="180" alt="SETTINGS"> | [**MATRIX**](keyboards/ydkb/athena75_rgb_advanced/src/app/matrix/README.md)<br><img src="keyboards/ydkb/athena75_rgb_advanced/docs/apps/matrix.gif" width="180" alt="MATRIX"> | [**LIFE**](keyboards/ydkb/athena75_rgb_advanced/src/app/life/README.md)<br><img src="keyboards/ydkb/athena75_rgb_advanced/docs/apps/life.gif" width="180" alt="LIFE"> |
| [**MAZE**](keyboards/ydkb/athena75_rgb_advanced/src/app/maze/README.md)<br><img src="keyboards/ydkb/athena75_rgb_advanced/docs/apps/maze.gif" width="180" alt="MAZE"> | [**BRICK**](keyboards/ydkb/athena75_rgb_advanced/src/app/brick/README.md)<br><img src="keyboards/ydkb/athena75_rgb_advanced/docs/apps/brick.gif" width="180" alt="BRICK"> | [**FISH**](keyboards/ydkb/athena75_rgb_advanced/src/app/fish/README.md)<br><img src="keyboards/ydkb/athena75_rgb_advanced/docs/apps/fish.gif" width="180" alt="FISH"> |
| [**WFC**](keyboards/ydkb/athena75_rgb_advanced/src/app/wfc/README.md)<br><img src="keyboards/ydkb/athena75_rgb_advanced/docs/apps/wfc.gif" width="180" alt="WFC"> | | |

---

## 写你自己的 app

一个 app 就是 `src/app/<name>/` 下的一份 C 源码，加一张 32×32 的 `icon.png`（不给就回落到 SDK 的默认图标），外带一份讲清楚它是什么的 `README.md`。它被单独编译成一个 freestanding 的小二进制，**不链接任何固件符号**，只通过一张函数表跟系统打交道——所以同一个包能装进任意一个 slot，也能在任意一版 ABI 兼容的固件上跑。

SDK 在 [`src/app/sdk/`](keyboards/ydkb/athena75_rgb_advanced/src/app/sdk)，核心就两个文件：

- [`host_api.h`](keyboards/ydkb/athena75_rgb_advanced/src/app/sdk/host_api.h) —— 你能用的全部系统服务。入口是 `const app_desc_t *app_init(const host_api_t *api)`：固件把 slot 加载好之后调它一次，你存下 `api`，返回自己的 `enter / exit / tick`。表里有 128×128 的共享画布和一整套绘图原语（清屏、矩形、圆、位图、文本、裁剪、present）、按键事件、时钟与随机数、QMK 的实时打字速度（WPM）、RGB 灯效读写、属于自己的 4 KiB 存档，以及**固件的菜单引擎**——你只提供一棵菜单内容树，导航、动画、单选与开关标记、配色都由固件来画，所以你的设置界面和系统的长得一模一样。
- `app.ld` —— 链接脚本：代码链到 slot 基址，`.data/.bss` 链到那个固定的 RAM 窗口。

打包和安装：

```bash
KB=keyboards/ydkb/athena75_rgb_advanced

bash $KB/src/app/tools/build_app.sh <name>       # -> $KB/artifacts/apps/<name>.app
host_tool app install $KB/artifacts/apps/<name>.app   # 首次安装
host_tool app update  $KB/artifacts/apps/<name>.app   # 只换代码和图标，保留存档与数据
```

需要心里有数的几条约束：

| 约束 | 上限 / 规则 |
|---|---|
| 代码 | ≤ 250 KiB（每个 slot 共 256 KiB） |
| 图标 | 32×32 RGB565，2 KiB |
| 存档 | slot 末尾 4 KiB，一个扇区 |
| RAM | `.data` + `.bss` 合计 80 KiB，固定窗口 `0x2002_C000` |
| 大数据 | 一个包可以声明多占几个**连续** slot，运行时从 `app_base() + 0x40000` 起 |
| 落盘 | 设置交给 `cfg_save` 暂存，固件在退出 app 时比较一次、需要才写 |

最后一条值得说明：flash 扇区的擦写寿命是有限的。如果菜单里每改一个值就写一次 flash，长按连发时每重复一次又写一次，一个扇区很快就废了。所以现在所有 app 的设置都只暂存，退出时统一落盘一次。

开发时不必每改一行就刷一次真键盘——仿真器可以直接把 app 装进去跑：

```bash
bash $KB/tools/build_sim.sh
bash $KB/tools/sim_app_preview.sh fish 5000 20000   # 截图到 build/sim-preview/
bash $KB/tools/sim_record.sh fish                   # 录成 GIF
```

---

## 目录结构

这是一份完整的 QMK / Vial checkout，本固件的所有东西都收在一个键盘目录下：

```
keyboards/ydkb/athena75_rgb_advanced/
├── src/firmware/   固件：显示 OS、菜单引擎、app 加载器、绘图库
├── src/host/       host_tool（CMake）
├── src/app/        slot app 源码 + SDK + build_app.sh
├── src/sim/        athena_sim 仿真器
├── artifacts/      提交的产物：固件 UF2 / host_tool / *.app / 仿真器
├── tools/          构建入口与脚本（build.py、build_*.sh、png_to_uf2.py …）
├── docs/           flash 与 RAM 分区、仿真器实现、app 预览 GIF
├── keymaps/  ld/   QMK 惯例位置
└── config.h  rules.mk …
```

## 更多文档

| 文档 | 内容 |
|------|------|
| [`docs/usage.md`](keyboards/ydkb/athena75_rgb_advanced/docs/usage.md) | 操作与参考手册：各 app 的按键、菜单、Command 模式、host_tool 全部子命令、可调宏 |
| [`src/README.md`](keyboards/ydkb/athena75_rgb_advanced/src/README.md) | 各目录的职责与全部构建入口 |
| [`docs/flash_map.md`](keyboards/ydkb/athena75_rgb_advanced/docs/flash_map.md) | 16 MiB flash 的完整分区、slot 内部布局、多 slot 包、写入安全区 |
| [`docs/ram_map.md`](keyboards/ydkb/athena75_rgb_advanced/docs/ram_map.md) | 264 KiB SRAM 的分区，以及 app 那 80 KiB 窗口是怎么切出来的 |
| [`src/sim/README.md`](keyboards/ydkb/athena75_rgb_advanced/src/sim/README.md) | 仿真器用法：两个前端、控制 socket、调试、回归测试 |
| [`docs/simulator.md`](keyboards/ydkb/athena75_rgb_advanced/docs/simulator.md) | 仿真器实现：机器模型与解释 / 块 / 本地码三档执行 |

## 上游与许可

除本键盘目录外，仓库其余部分（`quantum/`、`tmk_core/`、`platforms/`、`drivers/` …）都是 [QMK](https://qmk.fm) 与 [Vial](https://get.vial.today) 的上游代码，上游的说明保留在 [`readme_qmk.md`](readme_qmk.md)。

与 QMK 一致，采用 GPL-2.0-or-later，见 [`LICENSE`](LICENSE)。
