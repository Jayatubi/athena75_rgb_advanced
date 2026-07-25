# athena75_rgb_advanced — 外部 SPI-NOR Flash 地址分布

> 完整的片外 flash 地址分布，从低到高（偏移从 `0x000000` 开始）。
> 说明每段的作用、来源常量，以及一次实测的物理内容快照。

## 0. 芯片与寻址

- **Flash 芯片**：Winbond **W25Q128**（JEDEC `EF 40 18`）→ **16 MiB = `0x0100_0000`**。
- **容量常量**：`PICO_FLASH_SIZE_BYTES = 16 MiB`（`config.h`）。RP2040 的
  `flash_range_erase/program` 用**从 0 开始的偏移量**（不是 XIP 地址），并内部
  `hard_assert(offset+count <= PICO_FLASH_SIZE_BYTES)`。
- **XIP 基址**：`XIP_BASE = 0x1000_0000`。即：`XIP 地址 = 0x1000_0000 + flash 偏移`。
- **四个 XIP 别名窗口**（同一片 16 MiB 的不同缓存语义，probe 用它们对比读）：
  | 窗口基址 | 含义 |
  |---|---|
  | `0x1000_0000` | cached（普通带缓存） |
  | `0x1100_0000` | no-alloc（命中用缓存，未命中不填充） |
  | `0x1200_0000` | no-cache |
  | `0x1300_0000` | no-cache + no-alloc（直接读芯片，绕过 XIP 缓存） |

---

## 1. 顶层地址分布（偏移从 0 开始，低 → 高）

| 起始偏移 | 结束偏移 | XIP 起始 | 大小 | 段 | 作用 |
|---|---|---|---|---|---|
| `0x00_0000` | `0x1F_0000` | `0x1000_0000` | ≈ 1.94 MiB | **固件镜像区** | boot2 + 固件代码/只读数据（LD 管理）。实际约 110 KiB，其余为余量。 |
| `0x1F_0000` | `0x20_0000` | `0x101F_0000` | 64 KiB | **Vial/VIA EEPROM** | wear-leveling 备份区（逻辑 32 KiB）。**禁写**（见写预算规则）。 |
| `0x20_0000` | `0x40_0000` | `0x1020_0000` | 2 MiB | **固件预留尾部** | "≤4 MiB 固件 LD 预留"的尾部，当前未使用。 |
| `0x40_0000` | `0x80_0000` | `0x1040_0000` | 4 MiB | **开机动画区 (boot)** | QGF 开机图 + 余量，由独立 UF2 烧录。`BOOT_QGF_ADDR`（splash 只用开头）。 |
| `0x80_0000` | `0x100_0000` | `0x1080_0000` | 8 MiB | **App 槽区 (slots)** | 32 × 256 KiB slot，relocatable slot app（SETTINGS/MATRIX/ACE）。`ATHENA_APP_AREA_*`。 |

> 注 1：固件区仍为 4 MiB（`0x1000_0000..0x1040_0000`）。EEPROM 固定在 `0x1F_0000`
> （= 2 MiB − 64 KiB，沿用旧布局），固件代码必须落在 `0x1F_0000` 之下；
> `0x20_0000..0x40_0000` 这 2 MiB 是该 4 MiB 预留里未用的尾部。
>
> 注 2：旧的"独立 10 MiB keyframe 动画区"已废弃 —— 开机动画区扩到 4 MiB，其后 8 MiB
> 全部规划为 app 槽。ACE（原 SLIDES）以 slot app + 连续数据槽的形式播放关键帧。
>
> 注 3：App 槽几何（`app_upload.h` / `proto.h` / `app_pkg.h` / `apps/sdk/app.ld`）：
> slot0 = `0x1080_0000` .. slot31 = `0x10FC_0000`，每 slot 256 KiB。首 slot 内固定为：
> `+0x00000..+0x3E800` = 代码（≤250 KiB），`+0x3E800..+0x3F000` = 32×32
> big-endian RGB565 icon（2048 B），`+0x3F000..+0x40000` = save/data sector（4 KiB）。
> `.app` v3 将代码、icon 与额外数据 blob 存放在同一个完整包中，并声明必须连续
> 保留的 slot 数。ACE 当前占
> 13 个连续 slot：首 slot 为程序，随后 12 个 slot 放独立 UF2 安装的 raw QGF 数据；
> 运行时通过 `host_api.app_base()` 定位到 `app_base + 0x40000`。
> `host_tool app install foo.app` 默认走 raw-HID PUT；`--method uf2` 则先通过 HID
> 请求固件检查并分配连续 slot，再按固件返回地址重定位、生成 UF2，并自动进入
> BOOTSEL 安装。两种方式都可省略 `--slot`；显式地址被占用时均拒绝覆盖。
> ACE 的 QGF 已内嵌在 `ace.app`，无需也不接受独立 `--data-uf2`。host_tool 从包内
> 同时取得程序、icon、数据和连续 slot 数；PUT 与 UF2 安装均一次处理完整包。
> 已安装 App 的纯代码升级使用 `host_tool app update foo.app`：host_tool 按包名
> 找到首 slot，固件核对名称与 slot_count 后仅擦写代码和 icon；后续数据 slots 以及
> 首 slot `+0x3F000` 的 4 KiB save sector 均保持不动。
>
> MATRIX/ACE 的菜单设置只通过 `host_api.save_read/save_write/save_busy` 访问该
> save sector；App 不直接调用 flash 擦写接口。

---

## 2. 固件镜像区细分（`0x00_0000`–`0x1F_0000`）

| 起始偏移 | 大小 | 内容 |
|---|---|---|
| `0x00_0000` | 256 B | **boot2** 二级引导（配置 W25Q128 QSPI + XIP，随固件镜像一起）。 |
| `0x00_0000` | ≈ 109.5 KiB | **固件代码 + 只读数据**（中断向量表在 `0x1000_0000`；含 boot2）。当前 UF2 = 438×256 B ≈ 109.5 KiB。 |
| ≈ `0x01_B600` | 至 `0x1F_0000` | **固件区余量**（未被链接器占用；物理上是历史残留，非当前固件内容）。 |

---

## 3. 各段的来源（source of truth）

| 段 | 定义位置 |
|---|---|
| `PICO_FLASH_SIZE_BYTES` (16 MiB) | `config.h` |
| EEPROM base `0x001F_0000` / backing 64 KiB / logical 32 KiB | `keymaps/vial/config.h`（`WEAR_LEVELING_RP2040_FLASH_BASE`、`WEAR_LEVELING_BACKING_SIZE`） |
| 固件区 / boot 区 / app 槽区边界 | `app/c1_gfx.h`（`BOOT_QGF_ADDR`、注释里的分区图；`ANIM_QGF_ADDR` 为 legacy） |
| App 槽区 begin/end + slot 几何 | `app_upload.h`、`tools/host/common/proto.h`（`*_APP_AREA_*` / `*_APP_SLOT_*`）、`tools/host/common/app_pkg.h`（`APP_LINK_BASE`）、`apps/sdk/app.ld`（`LINK_BASE`/FLASH ORIGIN） |
| 打包/烧录地址（boot=`0x1040_0000`、boot 区 4 MiB） | `tools/png_to_uf2.py`（`BOOT_ADDR`/`BOOT_END`；`SLOT_*` 为 legacy） |
| JEDEC / diag 报告 | `probe_flash.c`、`user_rawhid.c`（`ath_handle_diag` / `ath_handle_probe`） |

---

## 4. 已废弃的历史布局：app 时代 "app slots"

旧的 app 实验平台（`put`/`ls`/`read`，已在提交 `6e7766571a` 移除）在同一片 flash 上
用过一套**不同且重叠**的分区，仅存在于旧固件/旧 host_tool binary 里：

```
app slots: 5 x 1 MiB from 0x10A0_0000 (reserve last 1 MiB from 0x10F0_0000)
```

该模型与当前的"10 MiB keyframe 槽（`0x1060_0000..0x1100_0000`）"**物理重叠**，属于历史遗留，
当前源码不再使用。相关调查见 `tools/HANDOVER_flash_put_investigation.md`。

---

## 5. 实测物理内容快照（2026-07-24, macOS host_tool，只读）

`host_tool probe` 的 1 MiB 步进可读性图（每格读 16 B）：

| XIP 地址 | 偏移 | 实测 | 说明 |
|---|---|---|---|
| `0x1000_0000` | 0 MiB | `00 B5 32 4B …` | 固件代码（向量表/boot2） |
| `0x1010_0000`–`0x1030_0000` | 1–3 MiB | `FF …` | 固件区/保留空隙未用（擦除态） |
| `0x101F_0000` | EEPROM 基址 | `FF …` | wear-leveling 备份区（当前基址处为擦除态） |
| `0x1040_0000` | 4 MiB | `… 51 47 46 …`（QGF） | boot 开机动画 QGF |
| `0x1060_0000` | 6 MiB | `… 51 47 46 …`（QGF） | anim 关键帧 QGF |
| `0x1050_0000` / `0x1070_0000`–`0x10F0_0000` | 5,7–15 MiB | `00 …` | 槽内 QGF 之后的尾部（见下方注意） |
| `0x10FF_0000` | 15.9 MiB | `FF …` | 槽顶部（擦除态） |

> **`0x00` vs `0xFF` 现象**：keyframe 槽尾部大片读到 `0x00`（既非文件、也非擦除态 `0xFF`）。
> 用 cached(`0x10A0…`) 与 no-cache(`0x13A0…`) 两窗口读结果一致 → 是芯片物理上的 `0x00`，
> 不是 XIP 缓存脏。这正是"数据写不进去"调查的现象，定性详见
> `tools/HANDOVER_flash_put_investigation.md`（写侧测试受 flash 写预算规则约束）。

---

## 6. 写入安全提醒

遵循 `.cursor/rules/flash-write-budget.mdc`：

- **禁写区**：固件镜像区 `0x00_0000`–`0x40_0000`（XIP `0x1000_0000`–`0x1040_0000`）；
  其中 EEPROM `0x1F_0000`–`0x20_0000`（XIP `0x101F_0000`–`0x1020_0000`）尤其不能碰。
- **App 槽区**：`0x80_0000`–`0x100_0000`（XIP `0x1080_0000`–`0x1100_0000`）由 slot app
  安装流程（`host_tool app ...` / BOOTSEL UF2）管理；手动 probe 刮擦会覆盖已装 app。
- **安全刮擦区**：boot 区/app 槽区 `0x40_0000`–`0x100_0000`（XIP `0x1040_0000`–`0x1100_0000`，
  如 `0x1080_0000`）。注意这会覆盖 boot 动画或已装 app——测试后需重烧才能恢复。
- `probe erase/prog` 只校验地址在 XIP 窗口内，**不保护固件/EEPROM**，地址由调用方负责。
