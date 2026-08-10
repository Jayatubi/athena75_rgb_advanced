# athena75_rgb_advanced — 片内 SRAM 地址分布

> RP2040 片内 SRAM 的完整地址分布，从低到高（`0x2000_0000` 起）。
> 说明每段作用、来源常量，以及一次实测的固件占用快照。
> 与 `flash_map.md`（片外 SPI-NOR flash）互为姊妹文档。

## 0. 芯片与寻址

- **SRAM 总量**：**264 KiB**（`0x2000_0000`..`0x2004_2000`），**两核统一编址、共享**，
  没有硬件上的"per-core 专属"分区。core0 写、core1 读的共享数据（`fbShow`、
  `menu_view`、dialog 状态等）就是靠这块共享内存。
- **物理分 6 个 bank**（地址连续）：
  | bank | 地址 | 大小 | 特点 |
  |---|---|---|---|
  | SRAM0–3 | `0x2000_0000`..`0x2004_0000` | **256 KiB** | **交错(striped)**：连续访问轮流命中不同 bank，降低两核竞争。做主 RAM。 |
  | SRAM4 | `0x2004_0000`..`0x2004_1000` | 4 KiB | 非交错。整块划给 **core0 栈**。 |
  | SRAM5 | `0x2004_1000`..`0x2004_2000` | 4 KiB | 非交错。整块划给 **core1 栈**。 |
- **别名窗口**：每个 bank 另有非交错别名映射在 `0x2100_0000+`（本固件未用）。

---

## 1. 顶层地址分布（低 → 高）

| 起始 | 结束 | 大小 | 段 | 作用 |
|---|---|---|---|---|
| `0x2000_0000` | `0x2002_C000` | 176 KiB | **firmware ram0** | 固件 `.data` + `.bss` + heap。 |
| `0x2002_C000` | `0x2004_0000` | 80 KiB | **slot-app arena** | 当前 app 的固定 `.data/.bss`；关键帧动画类 app 可用到约 64 KiB。 |
| `0x2004_0000` | `0x2004_1000` | 4 KiB | **SRAM4**（core0 栈） | core0 的异常栈(MSP) + 线程栈(PSP) + core-local 数据。 |
| `0x2004_1000` | `0x2004_2000` | 4 KiB | **SRAM5**（core1 栈） | core1 的异常栈 + 线程栈 + core-local 数据 + boot。 |

> **关键点**：slot app arena 是链接脚本硬切出的独占窗口，不属于 ChibiOS heap，
> 也不与 core0/core1 的独立栈 bank 重叠。

---

## 2. 共享 SRAM0–3 细分

固件链接器只可使用 `0x2000_0000..0x2002_C000`（176 KiB）；`.data`、`.bss`
之后的余量由 ChibiOS heap 吸收。独立 app 只能使用
`0x2002_C000..0x2004_0000`（80 KiB）。两段均为共享 SRAM，只是由链接器隔离。

当前构建 `size` 报告的 BSS/NOLOAD 合计为 182936 B（其中包含链接器扩展后的 heap）；
删除内置 ANIMATION/MATRIX 后，关键帧双缓冲改由运行中的 slot app 放在 app arena，
不会与固件静态 BSS 同时占据同一段 RAM。

---

## 3. SRAM4 / SRAM5（每核 4 KiB 栈 bank）细分

栈尺寸来自 `platforms/chibios/platform.mk`（`USE_EXCEPTIONS_STACKSIZE=0x400`
→ 异常/主栈 1 KiB；`USE_PROCESS_STACKSIZE=0x800` → 线程/进程栈 2 KiB）；
core1 复用同样尺寸（`rules_stacks_c1.ld`：`__c1_*_stack_size__ = __*_stack_size__`）。

**SRAM4 — core0（`0x2004_0000`..`0x2004_1000`）**

| 起始 | 大小 | 段 | 作用 |
|---|---|---|---|
| `0x2004_0000` | 1 KiB (0x400) | `.mstack` | 主/异常栈（MSP）。 |
| `0x2004_0400` | 2 KiB (0x800) | `.pstack` | 线程栈（PSP，`main()` 用）。 |
| `0x2004_0C00` | 288 B | `.ram4` | RP2040 core-local 数据段。 |
| `0x2004_0D20` | ~736 B | — | 空当（bank 内未用）。 |

**SRAM5 — core1（`0x2004_1000`..`0x2004_2000`）**

| 起始 | 大小 | 段 | 作用 |
|---|---|---|---|
| `0x2004_1000` | 1 KiB (0x400) | `.c1_mstack` | core1 主/异常栈。 |
| `0x2004_1400` | 2 KiB (0x800) | `.c1_pstack` | core1 线程栈。 |
| `0x2004_1C00` | 288 B | `.ram5` | RP2040 core-local 数据段。 |
| `0x2004_1F00` | 256 B | `ram7` | SRAM5 boot（`ld` 里的 `ram7`）。 |

> 每核 4 KiB bank ≈ 3 KiB 真·栈（1 KiB 异常 + 2 KiB 线程）+ ~1 KiB 空当，
> 且该 bank **只当栈用**，不放数据/堆。

---

## 4. 各段的来源（source of truth）

| 项 | 定义位置 |
|---|---|
| bank 边界（ram0 256K / SRAM4 / SRAM5 / ram7 boot） | `ld/RP2040_FLASH_TIMECRIT_16M.ld`（`MEMORY{}`） |
| 栈区域别名（MAIN/PROCESS/C1_* → ram4/ram5） | 同上（`REGION_ALIAS`） |
| 栈尺寸（主栈 0x400 / 线程栈 0x800） | `platforms/chibios/platform.mk`（`USE_EXCEPTIONS_STACKSIZE`/`USE_PROCESS_STACKSIZE`） |
| 栈布局（`.mstack`/`.pstack` 等 NOLOAD 段） | ChibiOS `.../ARMCMx/compilers/GCC/ld/rules_stacks{,_c1}.ld` |
| `.data`/`.bss`/`.heap` 落在 ram0 | `ld/...`（`DATA_RAM`/`BSS_RAM`/`HEAP_RAM` → `ram0`） |
| 实测各段大小/地址 | `arm-none-eabi-size -A` + `nm`（当前 `.build/*.elf`） |

---

## 5. Slot-app 的 RAM 窗口（已实施）

独立编译的 slot app 需要一小块固定地址的 RAM 放它的 `.data/.bss`（链接时写死，
上传时 RAM 指针不做重定位，见 `src/app/sdk/app.ld` + `src/host/common/app_pkg.*`）。

- **占位基址 `0x2004_0000` 不可用**：它正是 SRAM4（core0 主栈）起点，其后 8 KiB
  还会盖到 SRAM5（core1 栈）。直接加载会踩栈崩溃。
- **实测 app 需求**：MATRIX 988 B；关键帧动画 app 65568 B（两个 128×128 RGB565 帧）。
- **当前方案**：从 SRAM0–3 顶部切出 **80 KiB**
  (`0x2002_C000..0x2004_0000`)；固件 ram0 缩为 176 KiB。
- 三处 source of truth 必须同步：
  - `ld/RP2040_FLASH_TIMECRIT_16M.ld`：firmware `ram0` = 176K；
  - `src/app/sdk/app.ld`：app RAM `ORIGIN = 0x2002_C000`、`LENGTH = 80K`；
  - `src/host/common/app_pkg.h`：`APP_RAM_BASE = 0x2002_C000`、`APP_RAM_SPAN = 0x14000`。

---

## 6. 备注

- SRAM 是**共享**的：app 跑在 core1，但它的 `.data/.bss` 放共享 `ram0` 即可被正常访问。
- 与 flash 不同，SRAM **无擦写寿命问题**，读写不受 `flash-write-budget` 约束。
