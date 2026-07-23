# Athena75 RGB（Fork 固件说明）

YDKB / KBDFans Athena75 RGB：RP2040 + 128×128 GC9107 SPI LCD + Vial。
本文档是本 fork 固件的**产品说明**，描述相对原版固件新增的能力与特性，以及如何使用它们。

* MCU: RP2040（双核，显示独占一个核）
* LCD: 128×128 彩色屏
* 键位方案: `vial`（默认，支持 Vial 上位机改键）
* USB VID/PID: `0x9D5B` / `0x2514`

---

## 相对原版固件的主要变化

| 方面 | 原版固件 | 本 fork |
|------|----------|---------|
| 屏幕内容 | 固定播放内置 GIF（多个独立动画槽） | 单一 12MB 动画槽，播放用户自制关键帧动画，配 MCU 实时补间特效 |
| 屏幕特效 | 直接逐帧播放 | 5 种可切换的关键帧过渡特效（滑动 / 溶解 / 抖动 / 漩涡 / 随机），带速度、方向、补间等实时参数 |
| 屏上菜单 | 无 | 全屏 LCD 菜单模式，可调动画特效、RGB 灯效、屏幕校准，带平滑过渡动画 |
| 屏幕校准 | 无 | 虚拟屏幕：可标定有效显示窗口以贴合外壳遮挡（存 EEPROM） |
| 熄屏省电 | 依赖 USB suspend | 空闲自动熄屏 + 手动开关 + USB 挂起/恢复统一处理，唤醒可靠 |
| 屏幕文字 | Quantum Painter 字体 | 内嵌 Cozette 位图字库，白字黑描边 HUD，含 Unicode 符号 |
| 自制动画 | — | 提供 PNG 序列 → 屏幕动画的转换工具，真彩 RGB565，不受 GIF 256 色限制 |

原版的矩阵扫描、Vial 改键、RGB Matrix、SOCD / Snap Tap、boot 开机图等能力均保留。

---

## LCD 动画特效

屏幕循环播放动画关键帧，关键帧之间由固件实时生成过渡帧。可在多种特效间切换：

| 特效 | 效果 |
|------|------|
| **SLIDE** | 当前帧沿随机方向滑出、下一帧从对侧滑入；可叠加运动残影拖尾 |
| **DISSOLVE** | 以中心为锚点缩放淡入淡出 |
| **SHAKE** | 过渡期间抖动并交叉淡入下一帧 |
| **WHIRL** | 漩涡：旧帧旋出淡出、新帧旋入淡入 |
| **RANDOM** | 在其余特效中随机，隔若干关键帧自动换一个（并随机其次级参数） |

每次切换特效、速度或方向后，屏幕左上角会短暂显示当前状态名（约 2 秒）。

### 用 gif 键调节

gif 键（键位 `0x7e04`）用于实时控制动画。规则：首次点按或组合键会先弹出 HUD 显示**当前**状态而不改设置；HUD 仍在显示时再操作才真正生效。方向键与 `-`/`=` 支持短按一步、长按连发。

| 操作 | 功能 |
|------|------|
| 点按（无组合） | 切换到下一特效 |
| 按住 gif + ↑ / ↓ | 减慢 / 加快（关键帧停留间隔，HUD 显示 `GAP n`） |
| 按住 gif + ← / → | 切换方向 / 模式（含义随当前特效不同，见下） |
| 按住 gif + `-` / `=` | 减少 / 增加补间帧数（HUD 显示 `TWN n`） |
| 按住 gif + F | 开 / 关右上角帧耗时显示（如 `19.2ms`） |
| 按住 gif + Space | 进入 LCD 菜单模式 |

← / → 在不同特效下的含义：

| 当前特效 | ← / → 作用 |
|----------|-----------|
| SLIDE | 残影强度：`GHOST OFF` / `LOW` / `MID` / `HIGH` |
| DISSOLVE | 缩放方向：放大离开 ↔ 缩小离开 |
| WHIRL | 旋向：`CW → CCW → ALT`（ALT 按关键帧奇偶交替） |
| RANDOM | 换特效的间隔（`RND 10` … `RND 100`） |
| SHAKE | 无方向 |

---

## LCD 菜单模式

按住 **gif + Space** 进入全屏菜单。进入后无需再按住 gif，键盘输入被完全拦截（不外发到电脑），动画暂停。30 秒无操作自动退出。

| 操作 | 功能 |
|------|------|
| ↑ / ↓ | 移动焦点（长按连发，列表两端 wrap） |
| → / Enter | 进入下级 / 触发动作项 |
| ← / Esc | 返回上级（顶层：Esc 退出菜单） |
| Space | 就地选中 / 切换焦点项（单选或开关） |

菜单项前的圆圈有两类语义：**单选（radio）**为一组内互斥选择（各特效、RGB 模式、各档位）；**开关（checkbox）**为独立开/关（如 FT HUD、RGB 电源）。带参数的项本身也是文件夹，→/Enter 进入其参数子菜单。

**菜单结构**：

- **ANIMATION** — 五种特效（互斥选择，带参数者可进入调其参数）+ `HOLD`（关键帧停留，等同 GAP）/ `TWN`（补间帧数）/ `FT HUD`（右上角帧耗时开关，等同 gif+F）。
- **RGB** — 该项本身即 RGB 灯光电源开关；→ 进入子菜单：`EFFECT`（所有 RGB 灯效，按名字字母序，选中即开灯切换）、`BRIGHT` / `HUE` / `SAT` / `SPEED`（各档位对齐固件真实范围）。
- **LCD TEST** — 虚拟屏幕校准（见下）。
- **EXIT** — 退出菜单。

菜单带平滑过渡动画：逐项淡入 + 水平飞入、焦点框缓动、长列表滚动缓动，进入下级 / 返回上级有方向性的进出效果。

---

## 虚拟屏幕校准（LCD TEST）

面板为 128×128，但外壳边框会遮住边缘若干像素。虚拟屏幕定义一个**有效显示窗口**（原点 + 宽高），所有屏幕内容都相对该窗口显示，窗口外抹黑，让画面完整落在可见区域内。设置存 EEPROM，与 Vial 键位互不影响。

在菜单的 **LCD TEST** 屏内实时校准（显示棋盘格 + 红色边框标示当前窗口）：

| 操作 | 功能 |
|------|------|
| ↑ / ↓ | 移动上边 |
| Shift + ↑ / ↓ | 移动下边 |
| ← / → | 移动左边 |
| Shift + ← / → | 移动右边 |
| Enter | 保存并返回 |
| Esc | 放弃修改并返回 |

---

## LCD 电源管理

- **手动开关**：Command 模式下按 `O`，或原厂 LCD ON/OFF 键。
- **空闲熄屏**：无操作达到 `LCD_IDLE_TIMEOUT`（默认约 5 分钟）后关闭面板，按任意键唤醒。
- **USB 挂起 / 恢复**：随电脑睡眠 / 唤醒同步关屏 / 开屏。

三种路径统一处理，开屏时会重新初始化面板，解决“休眠后不亮”的问题。手动关屏状态会持久保存（重启后仍关），空闲熄屏为临时状态。

---

## 按键与命令

### Command 组合（默认 LShift + RShift 进入 Command）

| 键 | 功能 |
|----|------|
| `B` | 重启；再加 LCtrl → 进入 Bootloader |
| `O` | LCD 开 / 关 |
| `G` | 切换到下一动画特效（等同 gif 点按） |

此外保留 Vial 复位 bootloader 键码、SOCD / Snap Tap 等原厂逻辑。

---

## 自制屏幕动画

- 动画存放于 12MB 单一 Flash 槽，播放用户自制的**未压缩关键帧**，过渡帧由固件实时补间生成。
- 128×128 RGB565 每帧约 32KB，未压缩约可存 ≤383 帧。
- 制作流程：用 `tools/png_to_qgf.py` 把 PNG 序列转换为动画文件（UF2），保持真彩 RGB565（不经 GIF 的 256 色量化）。推荐用 `--keyframes` 只输出关键帧，配合固件补间。
- 刷入：把生成的 UF2 拖到 BOOTSEL 磁盘即可（与固件动画槽地址一致）。

---

## 编译与刷写

在仓库根目录：

```bash
make ydkb/athena75_rgb_advanced:vial
```

或使用随附的构建脚本（自带固定的 QMK 环境、体积统计、UF2 归档、可选刷写）：

```bash
python3 keyboards/ydkb/athena75_rgb_advanced/tools/build.py       # 通用
bash    keyboards/ydkb/athena75_rgb_advanced/tools/build_wsl.sh   # Windows + WSL
bash    keyboards/ydkb/athena75_rgb_advanced/tools/build_mac.sh   # macOS
```

常用参数：`-c` 先 clean、`-i` 编完等待 BOOTSEL 并拷贝 uf2、`--keymap via` 指定键位方案。

通用 QMK 环境说明见 [QMK Docs](https://docs.qmk.fm/#/getting_started_build_tools)。

---

## 可调配置（`config.h`）

面向动画/菜单表现的主要可调项：

| 宏 | 含义 |
|----|------|
| `FRAME_MS` | 一帧基准时间（ms） |
| `LCD_HOLD_FRAMES_LIST` | 可选的关键帧停留档位（GAP，gif+↑/↓） |
| `LCD_TWEEN_FRAMES_MIN` / `MAX` | 补间帧数可调范围（TWN，gif+`-`/`=`） |
| `LCD_HUD_MS` | 状态 HUD 显示时长（ms） |
| `LCD_GHOST_DECAY_LIST` | SLIDE 残影强度档位 |
| `LCD_SHAKE_AMP` | Shake 抖动幅度 |
| `LCD_DISSOLVE_ZOOM` | Dissolve 缩放量 |
| `LCD_WHIRL_STRENGTH_DEG` / `LCD_WHIRL_RADIUS` | Whirl 最大转角 / 影响半径 |
| `LCD_RAND_FRAMES_LIST` | RANDOM 换特效的间隔档位 |
| `LCD_IDLE_TIMEOUT` | 空闲熄屏时间（×0.5s，默认约 5 分钟） |
| `LCD_MENU_*` | 菜单布局与过渡动画参数、无输入自动退出时间等 |
