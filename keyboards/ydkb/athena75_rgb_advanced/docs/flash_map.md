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
| `0x20_0000` | `0x40_0000` | `0x1020_0000` | 2 MiB | **保留空隙** | 属于"≤4 MiB 固件 LD 预留"的尾部，当前未使用。 |
| `0x40_0000` | `0x60_0000` | `0x1040_0000` | 2 MiB | **开机动画槽 (boot)** | QGF 开机图，由独立 UF2 烧录。`BOOT_QGF_ADDR`。 |
| `0x60_0000` | `0x100_0000` | `0x1060_0000` | 10 MiB | **关键帧动画槽 (anim)** | QGF 关键帧，由 anim UF2 烧录；固件 MCU 实时补间。`ANIM_QGF_ADDR`，`MAX_ANIM_FRAMES=319`。 |

> 注：`app/c1_gfx.h` 把 `0x1000_0000..0x1040_0000`（4 MiB）整体标为"固件区
> (code+rodata, ≤4MB, LD-managed)"。其中 EEPROM 被固定在 `0x1F_0000`（= 2 MiB − 64 KiB，
> 沿用 16M 之前的旧布局），所以固件实际代码必须落在 `0x1F_0000` 之下；
> `0x20_0000..0x40_0000` 这 2 MiB 是该 4 MiB 预留里未用的尾部。

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
| 固件区 / boot 槽 / keyframe 槽边界 | `app/c1_gfx.h`（`BOOT_QGF_ADDR`、`ANIM_QGF_ADDR`、注释里的分区图） |
| 打包/烧录地址（boot=`0x1040_0000`、anim=`0x1060_0000`、10 MiB 容量） | `tools/png_to_uf2.py`（`BOOT_ADDR`/`BOOT_END`/`SLOT_ADDR`/`SLOT_BYTES`） |
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
- **安全刮擦区**：数据区 `0x40_0000`–`0x100_0000`（XIP `0x1040_0000`–`0x1100_0000`，如 `0x1080_0000`）。
  注意这会覆盖 boot/anim 动画内容——测试后需重烧对应 UF2 才能恢复动画。
- `probe erase/prog` 只校验地址在 XIP 窗口内，**不保护固件/EEPROM**，地址由调用方负责。
