# artifacts —— 编译好的成品

这个目录里的东西拿来就能用：不用装 ARM 工具链，不用 Docker，不用编译。第一次上手
只需要用到 `firmware/` 里的那个 UF2 和 `host/` 里对应你系统的 `host_tool`。

> ## ⚠️ 动手之前：请自己准备好原厂固件
>
> 刷入本固件会覆盖键盘里原有的固件；之后装 app、换开机动画，还会覆盖原厂用来存 GIF
> 数据的那部分 flash；Vial / VIA 的键位配置也会在首次启动时被重新初始化。这些都**没有
> 一键还原**。
>
> 本项目不附带、也没有办法替你取得 YDKB 的原厂固件。**请在动手之前自行准备好一份**，
> 否则将来想退回原样会很麻烦。这块键盘不是官方支持的改装对象，风险由你自己承担。

## 目录里有什么

| 路径 | 内容 |
|------|------|
| [`firmware/`](firmware) | `ydkb_athena75_rgb_advanced_vial.uf2` —— 最终版固件，第一次刷的就是它 |
| [`apps/`](apps) | 打包好的 slot app（`.app`），装进键盘后在 OS 模式的启动器里出现 |
| [`boot/`](boot) | 开机动画（`.qgf`），从原厂固件里分离出来的两段，详见 [`boot/readme.txt`](boot/readme.txt) |
| [`host/`](host) | `host_tool`：macOS 原生二进制、Windows `.exe`。刷固件、装 app、截图、对时都靠它 |
| [`sim/`](sim) | 仿真器 `athena_sim`。没有键盘也能把整套固件跑起来，详见 [`sim/readme.txt`](sim/readme.txt) |

## 第一次：用 BOOTSEL 刷固件

第一次只能用这个办法。`host_tool` 是通过**本固件自己的 Raw HID 协议**跟键盘说话的，
原厂固件上没有这个接口，所以在刷进去之前它找不到你的键盘。

1. 拔掉键盘的 USB 线。
2. 按住键盘上的 **BOOTSEL** 键不放，插上 USB，然后松手。
3. 电脑上会出现一个名为 **RPI-RP2** 的 U 盘。
4. 把 `artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2` 拖进这个 U 盘。
5. 拷完键盘会自动重启，LCD 亮起来就成了。

首次启动会花几秒初始化 EEPROM，属于正常现象。

## 之后：用 host_tool

`host_tool` 就在 `host/` 下，按系统取用，不需要安装：

```bash
artifacts/host/macos/host_tool          # macOS（Apple Silicon 原生）
artifacts/host/windows/host_tool.exe    # Windows
```

macOS 上第一次访问 USB / HID，系统会要求给运行它的终端授予**输入监控**权限
（系统设置 → 隐私与安全性 → 输入监控），给完重开终端即可。

下面用 `host_tool` 指代它。先确认键盘认得到：

```bash
host_tool devices          # 列出插着的键盘和正在跑的仿真器，以及各自的固件版本
```

### 后续升级固件

不用再拆 BOOTSEL 了，固件自己会配合：

```bash
host_tool upload           # 不带参数就自动挑 artifacts/firmware/ 里的那个 UF2
host_tool upload path/to/other.uf2
```

键盘 LCD 上会弹出「Update firmware?」，**在键盘上按 Enter 确认**，它会自动重启进
BOOTSEL 并完成拷贝。

### 装 app

```bash
host_tool app install artifacts/apps/wfc.app        # 首次安装，同样需要在 LCD 上确认
host_tool app install artifacts/apps/settings.app
host_tool app update  artifacts/apps/matrix.app     # 升级：只换代码和图标，保留存档与设置
```

装完按键盘上的 **gif 键**进入 OS 模式，启动器里就能看到它们。带时钟的 app（比如
MATRIX）还需要对一次时间：

```bash
host_tool synctime         # 或 host_tool daemon 常驻对时
```

### 换开机动画

`boot/` 里放着从原厂固件分离出来的两段动画，装哪个都行；自己做的放
`boot/private/`（这个目录不进 git），也一样能按名字装：

```bash
host_tool boot list                # 有哪些可选，各自多少帧
host_tool boot install kbdfans     # 原厂 vial 键位实际播放的那段
host_tool boot install athena      # 原厂固件内置的 Athena 字样
host_tool boot info                # 键盘现在装的是什么
```

同样要在 LCD 上确认，写完下次开机生效。做新的见
[`docs/usage.md`](../keyboards/ydkb/athena75_rgb_advanced/docs/usage.md#开机动画)。

### 键位配置的备份与恢复

这一份是 Vial / VIA 的 EEPROM，和固件、app 都是分开的：

```bash
host_tool backup -o my-layout.bin
host_tool restore my-layout.bin
```

## 想退回原厂

按住 BOOTSEL 上电，把你**事先准备好的**原厂 UF2 拖进 RPI-RP2 盘即可。

---

更完整的说明：整机用法与 `host_tool` 全部子命令见
[`docs/usage.md`](../keyboards/ydkb/athena75_rgb_advanced/docs/usage.md)，flash 分区见
[`docs/flash_map.md`](../keyboards/ydkb/athena75_rgb_advanced/docs/flash_map.md)。
