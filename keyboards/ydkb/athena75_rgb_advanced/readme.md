# Athena75 RGB Advanced（Fork 固件说明）

YDKB / KBDFans **Athena75 RGB**：RP2040 双核 + 128×128 GC9107 SPI LCD + Vial 改键。  
本 fork 在保留矩阵扫描、RGB Matrix、SOCD / Snap Tap、Vial EEPROM 等原厂能力的基础上，把 LCD 做成可安装 **slot app** 的小系统：开机动画、桌面启动器、独立 `.app` 包，以及跨平台的 **`host_tool`**（刷固件、截图、装 app、备份 Vial 配置等）。

| 项目 | 值 |
|------|-----|
| MCU | RP2040（core0：QMK + USB；core1：显示 OS） |
| LCD | 128×128 RGB565 |
| 默认键位 | `vial`（Vial 上位机改键） |
| USB | VID/PID `0x9D5B` / `0x2514`，Raw HID `0xFF60` / `0x61` |

更细的 flash / RAM 分区见 [`docs/flash_map.md`](docs/flash_map.md)、[`docs/ram_map.md`](docs/ram_map.md)。源码与产物目录见 [`src/README.md`](src/README.md)。

---

## 相对原版 YDKB 固件

| 方面 | 原版 | 本 fork |
|------|------|---------|
| 屏幕内容 | 内置固定 GIF 槽 | **Flash app 槽**（8 MiB，32×256 KiB）+ 可更换的 slot app（ACE 关键帧、MATRIX 雨、LIFE 等） |
| 动画与特效 | 逐帧播放 | **ACE** app：关键帧 + MCU 实时补间（SLIDE / DISSOLVE / SHAKE / WHIRL / RANDOM） |
| 屏上设置 | 无 / 极少 | 共享 **菜单引擎** + 各 app 自带菜单树（SETTINGS、ACE、MATRIX、LIFE） |
| 输入 | 始终当键盘 | **gif 键**切换「键盘模式 / OS 模式」；OS 模式下按键驱动启动器与 app |
| 上位机 | Vial | Vial + 原生 **`host_tool`**（HID 刷 UF2、LCD 截图、装 app、EEPROM 备份、flash 探针等） |
| 开机动画 | 内置 | 独立 **boot 区**（4 MiB QGF，UF2 烧录） |
| 校准与省电 | 有限 | **虚拟屏幕**校准（EEPROM）；空闲熄屏 + 手动开关 + USB suspend 统一唤醒 |
| 字体 | Quantum Painter | Cozette 位图字库 + 菜单/UI 组件 |

---

## 运行模型（开机之后）

1. **Boot 动画**（可选）：Flash `0x1040_0000` 起的 QGF；无效/空白则跳过。  
2. **OS 启动器**：2×2 图标网格，列出 `app_scan` 发现的已安装 app；方向键移动，Enter/Space 启动，Esc 退出 OS 模式回到纯键盘。  
3. **Slot app**：在 core1 独立运行（代码链在各自 flash slot，RAM 窗口 `0x2002_C000` 起 80 KiB）。退出 app 回到启动器。  
4. **菜单叠加**：app 内 Enter（或 SETTINGS 常驻菜单）调用 `host_api.menu_run()`，固件菜单引擎在 core0 绘 UI，core1 app 暂停 tick。

固件根菜单（未装 SETTINGS 时仍可用）大致为：**RGB**、**APP**（扫描列表）、**LCD TEST**、**REBOOT**、**EXIT**。完整系统设置建议安装 **`settings.app`**（见下）。

---

## gif 键与两种输入模式

键位 **`0x7e04`**（gif 键）**只负责切换模式**，不再承担旧版「点按切特效 / gif+组合键调参 / gif+Space 开菜单」：

| 模式 | 行为 |
|------|------|
| **键盘模式**（默认） | 按键正常发给电脑；Vial、SOCD 等与 QMK 一致 |
| **OS 模式** | 除 gif 外所有键进入 core0→core1 事件队列，驱动启动器 / 当前 app；**不**发给 USB host |

菜单或对话框弹出时：仅在 **OS 模式** 下吃键；可先切回键盘模式再按 gif，避免误触。  
OS 模式长时间无操作会自动退回键盘模式（与菜单 idle 一致，约 30s，见 `config.h`）。

---

## 预置 slot app（源码在 `src/app/`）

| App | 作用 | 菜单 / 操作要点 |
|-----|------|-----------------|
| **SETTINGS** | 系统设置 app：RGB、**SLEEP**（LCD+RGB 空闲时间）、已装 app 列表（详情 / 卸载）、LCD TEST、REBOOT | 安装后相当于「主设置」；与固件根菜单能力重叠但更完整 |
| **ACE** | 关键帧动画播放器；QGF 数据占 **13 个连续 slot**（1 代码 + 12 数据）；补间特效与旧版固件 ANIMATION 菜单相同 | Enter 打开 **ANIMATION** 菜单树；参数保存在 slot 末 4 KiB save 区 |
| **MATRIX** | 数字雨 + 可选时钟水印；参数经 save 区持久化 | Enter/Space 打开 **MATRIX** 菜单（SPEED / DENSITY / CLOCK）；`host_tool synctime` 推时钟 |
| **LIFE** | Conway + **环面**；屏保向图案与自动扰动 | 默认 **SAVER**（大格：枪+滑翔机群；小格：群）；无聊时自动注入滑翔机；**PATTERN**：SAVER/GUN/SWARM/MIX |
| **MAZE** | 16×16 完美迷宫自动寻路演示（中心 32×32 为封闭区） | Enter 打开 **MAZE** 菜单（SPEED）；走完自动重开一局 |
| **BRICK** | Breakout 打砖块屏保（Arkanoid 式砖型）：10×10、AI 挡板、随机胶囊（约 1/3 碎砖掉落） | 每关随机：彩色/银砖分界（4–6 行）、配色旋转、4–6 块金砖（不同列）、0–10 个空洞；通关后进下一关布局再变。自动演示；**↑↓** 调速；Enter = SPEED；Space 重开。胶囊带字母：E 加长、N 缩短、D 多球、S 减速、F 加速、C 粘球、L 穿砖。AI 会先算「取完还赶不赶得回球的落点」，来得及就绕路去接，且只接有利的（N/F 不追） |
| **FISH** | 鱼群屏保：Q15.16 定点（`src/firmware/lib/fixed_math`）移植自 TS 参考实现——Reynolds 转向推力 + 线性阻力积分（无硬限速）、软化平方反比池壁力场（GLASS 法向朝内 + CURRENT 沿边顺时针）、限速转向与预测式避墙；鱼身为四节火车 + 行波摆尾，按参考的几何模式画成逐节收窄的三角 | Enter 打开 **FISH** 菜单，全是滑条（←→ 一步、`-`/`=` 五步、Shift 十步）：SPEED 8–100 px/s、SCHOOL 1–10 条、VISION 8–86 px、SEPARATE / ALIGN / COHERE 0–40、GLASS / CURRENT 0–150 %、TYPING 0–200 %、FEED 0–120 s；Space 重新放鱼，↑↓ 速度、←→ 逐条增减鱼、`-`/`=` 水流强度（步长与滑条一致）。**VISION 是唯一的邻居半径**（与参考实现一致）：三条规则都只作用于半径内的同伴，分离按 1/d² 加权，所以它同时决定鱼群的松紧。滑条范围照搬参考的 UI：权重 0–4.0（这里 ×10 存整数，默认 30 / 10 / 10 即参考的 3 / 1 / 1），视野 2–20 世界单位；参考的池塘边长 30 单位对应这里 128 px，即 1 单位 ≈ 4.27 px，它默认的 3 单位视野落在 13 px。GLASS 是池壁法向（朝池内推）强度、CURRENT 是池壁切向（沿边顺时针）强度，两者按巡航速度的百分比给出，并按 `S·cruise·R²/(d²+R²)` 随离墙距离衰减（这两个默认 35 % / 25 % 高于参考的 10 %，是这块小屏上刻意的偏离）。**TYPING** 让鱼群感知打字速度：app 在键盘输入模式下（gif 切回打字、或 OS 模式静置 30 s 自动切回）继续运行，此时读固件的 WPM（`host_api::wpm`，OS 模式下恒为 0，因为翻菜单不算打字），按 `1 + TYPING% × (0.15 + 0.25 × min(WPM,80)/80)` 抬高巡航速度——重点是「开始打字」而不是打得多快，所以一有 WPM 读数就先给一个台阶，默认 100 % 时落笔即 +15 %、80 WPM 满档 +40 %；平滑是非对称的，冲上去约 0.16 s、落回来约 1.2 s。固件侧配套开了 `WPM_LAUNCH_CONTROL` 并把 `WPM_SAMPLE_SECONDS` 收到 3 s，否则久坐后开打要在一整个 5 s 空窗里慢慢爬，读数远低于真实手速。池壁力场仍按基础速度算，所以打字只让鱼更快、不会让水池变硬；置 0 关闭。需要固件构建号 ≥ 260731，旧固件上该滑条无效。**体色即速度**（沿用参考实现的做法）：`hsl(hue, 78 %, 58 %→44 % 逐节变暗)`，hue 由速度从 220°（蓝，慢）线性走到 0°（红，快），并量化到 8° 使鱼群呈现有限的几档色带。参考的映射区间是 0–2× 巡航速度，那是假设鱼真能游到巡航速度；这个移植版有阻力和 0.2× 的最低速下限，实测只在 0.2×–0.55× 之间活动，所以两端标定为 0.15×–0.6× 巡航速度，蓝→红才用得满。冷暖跟的是**基础**巡航速度，所以打字时整群会明显由蓝转绿转黄。**FEED（投饵）**：池里同一时间最多一个饵料奇点，`FEED` 秒没有饵料就投一个，或者在此之前「打字量」够了就提前投（按 WPM 积分估算的字符数，80 字符 ≈ 16 个词，因为 app 在键盘模式下看不到按键本身，只能读 WPM）；饵料点用的正是参考实现的鼠标汇聚力场，强度也照搬：`3.0×cruise` 的引力（R = 64 px，即参考的 15 世界单位）叠加 `2.0×cruise` 的核心斥力（R = 13 px，即参考的 3），两者都是软化平方反比，合力处处朝内但在饵料处最弱，所以鱼群是围成一团而不是塌成一点。**引力必须和分离同量级**才有"引力奇点"的观感：Reynolds 转向是先归一化再乘权重，分离只要邻居在 VISION 内就以满力（3× 巡航）输出，所以引力一旦低于它（这里最早取的是 1×，怕 3× 峰值加速度在 128 px 小屏上把鱼群半秒甩过去），鱼群就只是"朝那边飘"而不会汇聚。饵料是**被啃完**的而不是数人头：一份饵料含 10 个「鱼·秒」，每条进到 14 px 内的鱼每秒吃掉 1 个，所以整群围上来约 2 s 吃完、单独一条要 10 s（实测一颗饵料从出现到消失大约 3.5 s，够看清鱼群聚拢和散开），吃到 2/3 时那颗点会缩成一小粒（看得出在变少），吃完即清除奇点并重新开始计时；30 s 没被吃到也会自行消散，免得一个到不了的点永久占住唯一的名额。**不能**用「同时有 N 条鱼进到 R 内」判定：分离权重（3）远大于饵料引力（1），鱼群会稳定保持约一个 VISION 的间距，于是把 VISION 调到 31 的池子里三条鱼永远挤不进一个「不靠碰巧就已经有三条」的小半径——真机上第一次试就是这样，饵料放了 10 s 没人吃，只能等 30 s 超时。落点在两个随机候选里取离鱼群质心更远的那个，否则饵料落在群里等于什么都没发生。饵料画成一个会呼吸的暖色小点（画在鱼下面，先到的鱼会盖住它）；因为体色即速度，扑食途中整群会明显转红。FEED 置 0 完全关闭投饵 |

所有 app 的参数都存在自己 slot 末尾那 4 KiB save 区，并且**只在退出 app 时落盘一次**：菜单里改、直接按键改，都只是把结构体交给固件暂存（`cfg_save`），由固件在退出（或重载前，例如休眠唤醒）比较一次、需要才擦写。以前菜单是改一下就写一次 flash（长按连发时每一次重复都写一次），一个 sector 的擦写寿命经不起这么用。SETTINGS 改的 RGB、CapsLock 颜色、SLEEP 等属于 QMK 自己的 EEPROM，走的还是 eeconfig，不在此列。

构建并归档到 `artifacts/apps/`：

```bash
bash src/app/tools/build_app.sh settings   # 或 ace | matrix | life | maze | fish | brick
```

图标（`src/app/<app>/icon.png`，32×32）缺失时回落到 SDK 默认图标；FISH 的那张是脚本画的像素图，改完重跑即可：

```bash
python3 src/app/fish/make_icon.py --zoom 8   # --zoom 另存放大预览便于检查
```

安装到键盘（需已构建 `src/host` 的 `host_tool`，见下）：

```bash
host_tool app install path/to/life.app              # 默认 HID PUT，键盘 LCD 确认
host_tool app install path/to/ace.app --method uf2  # 大包装 / 多 slot 时可选 UF2 路径
host_tool app update path/to/matrix.app             # 仅更新代码+图标，保留 data/save
host_tool app info path/to/settings.app
```

ACE 的 emoji/QGF 数据打包需本地 `tools/emojis/`（**不入 git**）+ `src/app/tools/build_ace_data.py`；详见 ACE 构建脚本注释。

---

## ACE 动画特效（Enter 进入 ANIMATION 菜单）

关键帧之间由 MCU 实时补间，可在五种特效间切换（与旧 readme 行为一致，现由 **ACE app** 实现）：

| 特效 | 效果 |
|------|------|
| **SLIDE** | 滑入滑出，可配残影档位 |
| **DISSOLVE** | 缩放淡入淡出 |
| **SHAKE** | 抖动交叉淡入 |
| **WHIRL** | 漩涡旋转 |
| **RANDOM** | 随机特效与间隔 |

在 ACE 前台运行时，仍可用方向键与 `-`/`=` 等快捷键（见 `ace_app.c`）；Enter 打开完整 ANIMATION 菜单调 HOLD/TWN/FT HUD 等。

---

## 菜单操作（引擎通用）

SETTINGS / ACE / MATRIX / LIFE 共用同一套菜单引擎（`menu.c` + `ui_scene.c`）：

| 操作 | 功能 |
|------|------|
| ↑ / ↓ | 移动焦点（长按连发，列表 wrap） |
| → / Enter | 进入子级 / 触发动作 |
| ← / Esc | 返回；根级 Esc 关闭菜单 |
| Space | 单选切换 / 开关切换 |

带 **radio** / **toggle** 标记的项语义与旧版说明相同。菜单标题栏可显示固件构建号（`FW_BUILD_NUM`，构建日 UTC `YYMMDD`）。

---

## 虚拟屏幕（LCD TEST）

128×128 面板可能被外壳遮挡。虚拟屏幕定义有效显示窗口（原点 + 宽高），内容相对窗口绘制，窗外抹黑；存 EEPROM。

在 **LCD TEST**（固件菜单或 SETTINGS）中：方向键 / Shift+方向键移动边线，Enter 保存，Esc 放弃。

---

## LCD 与 RGB 电源

- **手动**：Command 模式 `O`，或原厂 LCD 开关键。  
- **空闲熄屏**：无操作超过配置时间（SETTINGS 里 **SLEEP** 可选 1/5/10/15 分钟或关闭）关闭 LCD；任意键唤醒。RGB 可与 LCD 共用超时策略。  
- **USB suspend / resume**：与电脑睡眠同步。  
- 手动关屏状态可持久；唤醒时会重新 init 面板，避免睡死。

---

## Flash 布局（摘要）

| 区域 | 大小 | 用途 |
|------|------|------|
| 固件 + boot2 | ≤ ~4 MiB（代码约 120 KiB 级） | QMK 镜像；**勿**用 probe 随意擦写 |
| Vial EEPROM | 64 KiB @ `0x001F_0000` | wear-leveling；**禁写**探针测试 |
| Boot QGF | 4 MiB @ `0x1040_0000` | 开机动画 UF2 |
| App slots | 8 MiB @ `0x1080_0000` | 32×256 KiB，slot app + ACE 连续数据 |

细节与 slot 内布局（代码 / icon / save / 多 slot 包）见 [`docs/flash_map.md`](docs/flash_map.md)。

---

## 工程目录

```
keyboards/ydkb/athena75_rgb_advanced/
├── src/firmware/     # QMK 固件（OS、菜单、上传、gfx）
├── src/host/         # host_tool（CMake）
├── src/app/          # slot app 源码 + SDK + build_app.sh
├── artifacts/        # 提交的 UF2、host 二进制、*.app
├── tools/            # build.py / build_mac.sh / build_wsl.sh、png_to_uf2 等
├── keymaps/ ld/      # QMK 惯例位置
└── config.h rules.mk …
```

---

## 编译

### 固件 UF2

```bash
# 仓库根目录等价：
make ydkb/athena75_rgb_advanced:vial

# 推荐脚本（固定 Docker QMK 镜像、体积统计、归档）：
bash keyboards/ydkb/athena75_rgb_advanced/tools/build_mac.sh    # macOS
bash keyboards/ydkb/athena75_rgb_advanced/tools/build_wsl.sh  # Windows + WSL
python3 keyboards/ydkb/athena75_rgb_advanced/tools/build.py
```

产物：`artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2`（及 `history/` 时间戳副本）。  
参数：`-c` clean；`KEYMAP=via` 换键位。

### host_tool

```bash
cmake -S keyboards/ydkb/athena75_rgb_advanced/src/host \
      -B keyboards/ydkb/athena75_rgb_advanced/src/host/build \
      -DCMAKE_BUILD_TYPE=Release
cmake --build keyboards/ydkb/athena75_rgb_advanced/src/host/build --config Release
```

预编译：`artifacts/host/macos/host_tool`、`artifacts/host/windows/host_tool.exe`。

### 开机动画 UF2

```bash
python3 tools/png_to_uf2.py boot path/to/splash.png   # -> boot UF2，烧到 boot 区
```

刷入：BOOTSEL 拖 UF2，或 `host_tool upload boot.uf2`（会走 LCD 确认，见下）。

---

## host_tool 子命令

| 命令 | 作用 |
|------|------|
| `devices` | 列出当前所有可操作对象：插着的键盘（`usb1`…）+ 在跑的 `athena_sim`（`sim:127.0.0.1:47801`），并显示各自固件 build |
| `upload [uf2] [--force] [--no-hid] [--timeout N]` | 默认 LCD 询问「Update firmware?」→ Enter 进 BOOTSEL → 拷贝 UF2；`--force` 跳过询问 |
| `snapshot [-o shot.png]` | Raw HID 抓 LCD RGB565 → PNG |
| `synctime [--utc] [--loop SEC]` | 推送 PC 时间（MATRIX 时钟） |
| `daemon …` | 常驻对时 + 重连 |
| `diag` / `fw` | 打印 flash/EEPROM 布局常量 / 固件 build 与 ABI |
| `backup [-o file.bin]` / `restore file.bin` | Vial/VIA EEPROM 备份与恢复 |
| `probe read ADDR [len]` / `erase` / `prog` | JEDEC + 探针读写（**遵守 flash 写预算**，禁写固件/EEPROM 区） |
| `app pack …` / `info` / `relocate` / `install` / `update` / `launch` | 打包、检查、安装、升级、直接启动 slot app |

默认 UF2 路径由 `src/host/common/paths.c` 解析（优先 `artifacts/firmware/`）。

全局选项 `-d, --device <#|id>`（可放在命令行任意位置）指定操作对象：`devices`
里的序号或 id、`usb`、`sim`、`sim:HOST:PORT`，或名字/路径的任意子串。不带
`--device` 时：`ATHENA_HID_SIM` 优先，否则用唯一插着的键盘；**同时插了两把键盘会
直接报错并列出候选**，不猜。`devices` 只探测默认仿真端口 47801–47804，其他端口用
`--device sim:HOST:PORT` 显式指定。

---

## Command 模式（LShift + RShift）

| 键 | 功能 |
|----|------|
| `B` | 重启；+ LCtrl → Bootloader |
| `O` | LCD 开/关 |

（Command `G` 仍映射旧接口 `next_gif_id()`，内置 ANIMATION 移除后为空操作；切特效请用 **ACE** app / 菜单。）

Vial bootloader 键码、SOCD / Snap Tap 等逻辑保留。

---

## 可调配置（`config.h` / `rules.mk`）

| 宏 / 项 | 含义 |
|---------|------|
| `FW_BUILD_NUM` | 菜单标题构建戳（`rules.mk` 默认 UTC 日期） |
| `FRAME_MS`、`LCD_HOLD_FRAMES_*`、`LCD_TWEEN_*` | ACE / 补间节奏（ACE 内亦有一套常量） |
| `LCD_HUD_MS`、`LCD_GHOST_*`、`LCD_SHAKE_*`、`LCD_DISSOLVE_*`、`LCD_WHIRL_*`、`LCD_RAND_*` | 特效参数档位 |
| `LCD_IDLE_TIMEOUT` | 固件侧 idle（与 SETTINGS SLEEP 配合） |
| `LCD_MENU_*` | 菜单动画与 auto-exit |
| `LCD_FLASH_PROMPT_MS` | `host_tool upload` 固件确认框超时 |

---

## 参考

- [QMK 构建环境](https://docs.qmk.fm/#/getting_started_build_tools)  
- 分区与 probe：`docs/flash_map.md`  
- Slot app RAM：`docs/ram_map.md`  
- Cursor skills（WSL/mac 刷机流程）：`.cursor/skills/build-athena75`、`host-tool-athena75`
