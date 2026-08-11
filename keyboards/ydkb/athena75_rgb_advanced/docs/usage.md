# athena75_rgb_advanced — 操作与参考手册

> 仓库根目录的 [`readme.md`](../../../../readme.md) 是面向读者的介绍：这个固件是什么、
> 架构长什么样、怎么写自己的 app。本文是另一半——**怎么按键、有哪些命令、哪些宏可以调**。
> 文中相对路径都相对 `keyboards/ydkb/athena75_rgb_advanced/`。

## 1. 两种输入模式

键位 **`0x7e04`**（gif 键）只负责在两种模式之间切换，不再承担旧版「点按切特效 /
gif+组合键调参 / gif+Space 开菜单」的职责。

| 模式 | 行为 |
|------|------|
| **键盘模式**（默认） | 按键正常发给电脑；Vial、SOCD / Snap Tap 等与 QMK 一致 |
| **OS 模式** | 除 gif 外所有键进入 core0 → core1 的事件队列，驱动启动器与当前 app；**不**发给 USB host |

菜单或对话框弹出时只在 OS 模式下吃键。担心误触可以先切回键盘模式再按 gif。
OS 模式闲置 30 秒（`APP_OS_IDLE_MS`）自动退回键盘模式。

## 2. 启动器

开机顺序：boot 动画（flash `0x1040_0000` 起的 QGF，无效或空白则跳过）→ 启动器。

固件不内置任何开机动画，播的完全是 boot 区里现有的那份数据：整帧、只重画一个矩形的
delta 帧、字节 RLE 压缩都认，按每帧自带的 delay 播一遍就交给启动器。原厂那两段动画
`host_tool boot list` 里就有（`artifacts/boot/`），换成自己的怎么做、怎么装，见
[§9 构建与安装](#9-构建与安装)。

| 操作 | 功能 |
|------|------|
| 方向键 | 在 2×2 图标网格里移动 |
| Enter / Space | 启动选中的 app |
| Esc | 退出 OS 模式，回到纯键盘 |

网格列出 `app_scan` 在 flash 槽区里发现的全部已安装 app。

## 3. 菜单引擎

固件提供菜单引擎（`src/firmware/menu.c` + `ui_scene.c`），app 只提供内容树，所以
所有 app 的菜单操作完全一致：

| 操作 | 功能 |
|------|------|
| ↑ / ↓ | 移动焦点（长按连发，列表 wrap） |
| → / Enter | 进入子级 / 触发动作 |
| ← / Esc | 返回；根级 Esc 关闭菜单 |
| Space | 单选（radio）切换 / 开关（toggle）翻转 |

标题栏可显示固件构建号（`FW_BUILD_NUM`，构建日 UTC `YYMMDD`）。菜单闲置
`LCD_MENU_IDLE_MS`（30 秒）自动关闭。

未安装 SETTINGS 时，固件自带的根菜单仍然可用：**RGB**、**APP**（已装列表）、
**LCD TEST**、**REBOOT**、**EXIT**。

## 4. 各 app 的按键

所有 app 一律 **Esc 退出回启动器**、**Enter 打开菜单**。菜单里能改的参数，多数还做了
直接按键的快捷方式（长按连发）——具体是哪几个键，见各 app 自己的说明：

| App | 说明 |
|-----|------|
| SETTINGS | [`src/app/settings/README.md`](../src/app/settings/README.md) |
| MATRIX | [`src/app/matrix/README.md`](../src/app/matrix/README.md) |
| LIFE | [`src/app/life/README.md`](../src/app/life/README.md) |
| MAZE | [`src/app/maze/README.md`](../src/app/maze/README.md) |
| BRICK | [`src/app/brick/README.md`](../src/app/brick/README.md) |
| FISH | [`src/app/fish/README.md`](../src/app/fish/README.md) |
| WFC | [`src/app/wfc/README.md`](../src/app/wfc/README.md) |

参数只在**退出 app 时**落盘一次：菜单里改也好、直接按键改也好，都只是把结构体交给
固件暂存（`cfg_save`），由固件在退出（或重载前，例如休眠唤醒）比较一次、需要才擦写。
早期版本是改一下写一次 flash，长按连发时每一次重复都写一次——一个扇区的擦写寿命
经不起这么用。SETTINGS 改的 RGB、CapsLock 颜色、SLEEP 等属于 QMK 自己的 EEPROM，
走 eeconfig，不在此列。

## 5. 虚拟屏幕（LCD TEST）

128×128 面板可能被外壳挡掉一圈。虚拟屏幕定义有效显示窗口（原点 + 宽高），内容相对
窗口绘制，窗外抹黑；结果存 EEPROM。

进入固件菜单或 SETTINGS 的 **LCD TEST**：

| 操作 | 功能 |
|------|------|
| ←→ / ↑↓ | 拖动左边线 / 上边线，对面那条不动 |
| Shift + ←→ / ↑↓ | 改拖右边线 / 下边线 |
| Enter | 保存 |
| Esc | 放弃 |

## 6. LCD 与 RGB 电源

- **手动**：Command 模式 `O`，或原厂的 LCD 开关键。手动关屏的状态可持久。
- **空闲熄屏**：超过 SETTINGS 里 **SLEEP** 设定的时间（1 / 5 / 10 / 15 分钟或关闭）
  关闭 LCD，任意键唤醒。RGB 与 LCD 共用这套超时策略。
- **USB suspend / resume**：与电脑睡眠同步。
- 唤醒时重新 init 面板，避免睡死。

## 7. Command 模式（LShift + RShift）

| 键 | 功能 |
|----|------|
| `B` | 重启；配合 LCtrl 进 Bootloader |
| `O` | LCD 开 / 关 |

Command `G` 仍映射旧接口 `next_gif_id()`，内置 ANIMATION 移除后是空操作；换特效请在
对应 slot app 的菜单里做。Vial bootloader 键码、SOCD / Snap Tap 等逻辑保留。

## 8. host_tool 子命令

| 命令 | 作用 |
|------|------|
| `devices` | 列出当前所有可操作对象：插着的键盘（`usb1`…）+ 在跑的 `athena_sim`（`sim:127.0.0.1:47801`），并显示各自固件 build |
| `upload [uf2] [--force] [--no-hid] [--timeout N]` | 默认先在 LCD 上问「Update firmware?」→ Enter 进 BOOTSEL → 拷贝 UF2；`--force` 跳过询问 |
| `snapshot [-o shot.png]` | Raw HID 抓 LCD 的 RGB565 帧存成 PNG |
| `synctime [--utc] [--loop SEC]` | 推送 PC 时间（MATRIX 的时钟水印用） |
| `daemon …` | 常驻对时 + 断线重连 |
| `diag` / `fw` | 打印 flash / EEPROM 布局常量 / 固件 build 与 ABI |
| `backup [-o file.bin]` / `restore file.bin` | Vial、VIA 的 EEPROM 备份与恢复 |
| `probe read ADDR [len]` / `erase` / `prog` | JEDEC 识别 + 探针读写 |
| `app pack / info / relocate / install / update / launch` | 打包、检查、重定位、安装、升级、直接启动 slot app |
| `boot list` | 列出 `artifacts/boot/` 与 `artifacts/boot/private/` 里的开机动画 |
| `boot install <名字\|file.qgf> [--method put\|uf2]` / `boot info` / `boot erase` | 写、查、删开机动画；`put` 走 USB 并在 LCD 上确认，`uf2` 改走 BOOTSEL |

不带参数时默认的 UF2 路径由 `src/host/common/paths.c` 解析，优先
`artifacts/firmware/`。

**选设备**：全局选项 `-d, --device <#|id>` 可以放在命令行任意位置，取值是 `devices`
列出的序号或 id、`usb`、`sim`、`sim:HOST:PORT`，或者名字 / 路径的任意子串。不带
`--device` 时：环境变量 `ATHENA_HID_SIM` 优先，否则用唯一插着的那把键盘；**同时插了
两把会直接报错并列出候选**，绝不替你猜。`devices` 只探测默认仿真端口 47801–47804，
其他端口要用 `--device sim:HOST:PORT` 显式指定。

**关于 `probe` 的写操作**：`erase` / `prog` 只校验地址落在 XIP 窗口内，**不保护固件
和 EEPROM 区**，地址由调用方负责。禁写区与安全刮擦区见
[`flash_map.md`](flash_map.md#6-写入安全提醒)。

## 9. 构建与安装

```bash
# 固件 UF2 —— 从仓库根目录
make ydkb/athena75_rgb_advanced:vial

# 或用固定 Docker 镜像、带体积统计与归档的脚本
bash keyboards/ydkb/athena75_rgb_advanced/tools/build_wsl.sh   # Windows + WSL
bash keyboards/ydkb/athena75_rgb_advanced/tools/build_mac.sh   # macOS
python3 keyboards/ydkb/athena75_rgb_advanced/tools/build.py    # 上面两个都只是它的外壳
```

产物：`artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2`（另有 `history/`
时间戳副本）。参数：`-c` clean，`KEYMAP=via` 换键位，`JOBS=N` 并行度。

```bash
# host_tool
cmake -S keyboards/ydkb/athena75_rgb_advanced/src/host \
      -B keyboards/ydkb/athena75_rgb_advanced/src/host/build -DCMAKE_BUILD_TYPE=Release
cmake --build keyboards/ydkb/athena75_rgb_advanced/src/host/build --config Release

# slot app
bash src/app/tools/build_app.sh settings   # 或 matrix | life | maze | fish | brick | wfc
host_tool app install  path/to/life.app              # 默认走 HID PUT，键盘 LCD 上确认
host_tool app install  path/to/wfc.app --method uf2  # 大包 / 多 slot 时可选的 UF2 路径
host_tool app update   path/to/matrix.app            # 只换代码和图标，保留 data 与 save
host_tool app info     path/to/settings.app

# 开机动画：现成的两段（从原厂固件分离出来，见 artifacts/boot/readme.txt）
host_tool boot list                          # 可选的名字，各自多少帧
host_tool boot install kbdfans               # 原厂 vial 键位实际播放的那段
host_tool boot install athena                # 原厂固件内置的 Athena 字样

# 自己做：GIF / 视频 / PNG 序列 / 单张图 -> 128x128 QGF
python3 tools/make_boot_anim.py demo.gif -o boot.qgf          # GIF 自带时序
python3 tools/make_boot_anim.py clip.mp4 --fps 20 --duration 4 -o boot.qgf
python3 tools/make_boot_anim.py frames/ --fps 24 -o boot.qgf  # PNG 序列
python3 tools/make_boot_anim.py logo.png --hold 2000 -o boot.qgf         # 静态开机图
python3 tools/make_boot_anim.py logo.png --hold 900 --fade 500 \
        -o ../../../artifacts/boot/private/ace.qgf                       # 淡入-停留-淡出

python3 tools/qgf_preview.py boot.qgf -o boot.gif   # 装之前先在电脑上看一眼

host_tool boot install boot.qgf              # USB 直传，键盘上按 WRITE 确认
host_tool boot install ace                   # 名字也行：artifacts/boot[/private]/<名字>.qgf
host_tool boot install boot.qgf --method uf2 # 大文件走 BOOTSEL，快一个数量级
host_tool boot info                          # 现在装的是什么
host_tool boot erase                         # 删掉，之后开机直接进启动器
```

`artifacts/boot/` 里跟踪着两段原厂动画（预览图见 [`docs/boot/`](boot)），
`artifacts/boot/private/` 不进 git——自己做的放那儿，`boot list` 一样列得出来，
`boot install <名字>` 一样装得上。`qgf_preview.py` 按固件同一套规则解码，
所以 GIF 里是什么样，屏上就是什么样。`--fade MS` 会在
首尾各接一段与黑色的混合帧（`--fade-in` / `--fade-out` 可分开给），末帧收在全黑上，
切到启动器时不会跳一下。

开机动画区有 4 MiB。转换器对每一帧都会在「整帧 / 只存变化矩形」和「原样 / RLE」之间
挑最小的那种，画面越静省得越多——实拍视频通常能压到原始体积的几个百分点。超出预算
时默认自动折半降帧率并把时长补回留下的帧（播放速度不变），过程会打印出来；
`--no-fit-budget` 则改成直接报错。`--fit cover|contain|stretch` 决定非正方形素材怎么
装进 128×128。视频需要 `ffmpeg`（没有会明确提示，可以先自己转成 GIF 或 PNG 序列）；
脚本本身需要 Pillow，装了 numpy 会快很多。

传输速度：raw HID 大约 20 KB/s，所以 2 MB 的动画走 `put` 要几分钟，`--method uf2`
重启进 BOOTSEL 拷贝只要几秒——host_tool 在文件偏大时会主动提醒。

每次安装都要把动画覆盖的每个 4 KiB 扇区擦一遍（4 MiB 就是 1024 次），扇区寿命有限，
所以先在仿真器上把效果调满意再写进键盘：`bash tools/sim_boot_check.sh demo.gif` 会把
转换、烧进仿真 flash、开机播放跑一遍，产出面板截图。
单张 PNG 的 alpha 淡入淡出开机图是另一条老路径：`python3 tools/png_to_uf2.py boot x.png`。

图标（`src/app/<app>/icon.png`，32×32）缺失时回落到 SDK 默认图标。有几个 app 的图标是
脚本画出来的，重画方式见各自的说明。

## 10. 可调配置（`config.h` / `rules.mk`）

| 宏 / 项 | 含义 |
|---------|------|
| `FW_BUILD_NUM` | 菜单标题上的构建戳（`rules.mk` 里默认取 UTC 日期） |
| `FRAME_MS`、`LCD_HOLD_FRAMES_*`、`LCD_TWEEN_*` | 关键帧动画与补间的节奏（动画类 app 内另有一套常量） |
| `LCD_HUD_MS`、`LCD_GHOST_*`、`LCD_SHAKE_*`、`LCD_DISSOLVE_*`、`LCD_WHIRL_*`、`LCD_RAND_*` | 各种特效的参数档位 |
| `LCD_IDLE_TIMEOUT` | 固件侧的 idle 计时（与 SETTINGS 的 SLEEP 配合） |
| `LCD_MENU_IDLE_MS` | 菜单无操作自动关闭 |
| `APP_OS_IDLE_MS` | OS 模式无操作自动退回键盘模式 |
| `LCD_FLASH_PROMPT_MS` | `host_tool upload` 那个确认框的超时 |

## 11. 相关文档

- [`flash_map.md`](flash_map.md) —— flash 完整分区、slot 内部布局、多 slot 包、写入安全区
- [`ram_map.md`](ram_map.md) —— SRAM 分区与 slot app 的 80 KiB RAM 窗口
- [`../src/sim/README.md`](../src/sim/README.md) —— 仿真器用法
- [`simulator.md`](simulator.md) —— 仿真器实现
- [`../src/app/sdk/host_api.h`](../src/app/sdk/host_api.h) —— app 能调用的全部系统服务
- [QMK 构建环境](https://docs.qmk.fm/#/getting_started_build_tools)
