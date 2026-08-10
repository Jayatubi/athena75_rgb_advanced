# athena_sim — 仿真器结构与 JIT / 非 JIT 执行原理

> 面向开发者的实现文档。用户向的用法说明在 `src/sim/README.md`；
> 本文讲的是"里面是怎么做的"，尤其是客户机代码的三档执行路径。
> 代码位置全部相对 `keyboards/ydkb/athena75_rgb_advanced/`；命令则在仓库根执行
>（`KB=keyboards/ydkb/athena75_rgb_advanced`），产物目录 `artifacts/` 也在仓库根。

## 0. 它是什么，不是什么

`athena_sim` 是 **RP2040 全系统仿真器**（full-system emulator），跑的是**原封不动的
出厂固件**：`artifacts/firmware/*.uf2` 被写进一个 16 MiB 的 flash 镜像，然后从 reset
向量开始被一颗仿真的 ARMv6-M 执行——经过真正的 bootrom 入口、真正的 boot2、真正的
ChibiOS 调度器、真正的 USB 枚举，最后落到建模的 GC9107 屏和 WS2812 灯带上。固件里
**没有任何 `#ifdef SIMULATOR`**，也不需要为仿真单独编译。

它**不是**：

- 不是 `.app` 的字节码虚拟机。slot app 被装进 flash 槽位，由固件按硬件上的同一条路径
  加载执行（细节见 `docs/flash_map.md` 和第 5 节）。
- 不是把 app 的 C 源码重新编成 x86 在 PC 上跑。仿真器里执行的始终是 ARM Thumb 机器码。
- 不是周期精确的。指令计数是调度单位，不是真实周期数。

文中"JIT"指的是**把客户机 Thumb 指令翻译成宿主机（x86-64 / arm64）机器码**，
和 app 系统完全正交。

## 1. 全景图

```
   前端（二选一）
   ├── athena_sim      gui/main_gui.c      SDL2 窗口，4 ms 一片，对齐墙上时钟
   └── athena_sim_cli  headless/main_headless.c  无窗口，1 ms 一片，CI 用
                              │  sim_run_us()
                              ▼
   机器层  core/machine.c     sim_run_cycles()：双核轮转 + 外设 poll
                              │  cpu_run()
                              ▼
   CPU 层  core/cpu_armv6m.c  ┌ cpu_run_interp()   一条一条（非 JIT，参考实现）
                              └ cpu_run_blocked()  一块一块
                                   ├ b.code(c)      本地机器码（默认）
                                   └ exec_decoded() 解释执行块内指令
                              │
   翻译层  jit/               jit_frontend.c 切块 → jit_cache.c 存块
                              → jit_x64.c / jit_a64.c 发射宿主机码
                              → jit_code.c 可执行内存
                              │  bus_read / bus_write
                              ▼
   设备层  periph/ board/ image/ net/ dbg/
```

关键的分层约束：**指令语义只有一份**。三档路径最终都调用 `cpu_armv6m.c` 里同一个
`exec_decoded()`；本地后端不能发射的编码也回调它。所以三档不可能在语义上分叉。

## 2. 构建与两个前端

```bash
bash $KB/tools/build_sim.sh                     # -> artifacts/sim/<os>/athena_sim{,_cli}
bash $KB/tools/build_sim.sh --test              # 顺便跑像素回归
bash $KB/tools/build_sim.sh --windows           # 从 WSL 驱动 MSVC，产出 .exe
bash $KB/tools/build_sim.sh --windows --no-sdl  # 只要 CLI
bash $KB/tools/build_sim.sh --app               # 再打包成桌面能双击的形态
```

归档的只有 macOS 与 Windows 两个平台，没有 Linux 版；在 WSL 里要构建的是 `--windows`。

`--app` 把窗口版包成各自桌面认识的形状——macOS 是
`artifacts/sim/macos/Athena75 Simulator.app`，Windows 是
`artifacts/sim/windows/Athena75 Simulator/` 这个可以整个搬走的文件夹。两者都连同
它们自带的固件一起提交在仓库里，所以固件或 app 变了之后要重跑一次 `--app` 才会跟上。

双击时没有任何命令行参数，而 `athena_sim` 的固件、布局、flash 三样都没有默认值。
所以固件与布局装进一个 `Resources/` 目录（`.app` 里是 `Contents/Resources`，
Windows 上就在 `.exe` 旁边），由 `athena_sim` 自己循着可执行文件的位置去找
（`core/os.c` 的 `os_exe_dir()`），两边因此都不需要在二进制前面再垫一层启动脚本。
16 MiB 的 flash 不能放在可能只读的包里，改为首次运行时在用户自己的数据目录下创建
（macOS 的 Application Support、Windows 的 `%LOCALAPPDATA%`），`Resources/apps/`
里的 `.app` 也只在这个 flash 尚不存在、即第一次启动时装入。

图标两边同源：`src/sim/gui/appicon.png`——真机旋钮的照片裁成方形，按 macOS 的圆角
方形网格（1024 画布里 824 的方块、185 的圆角）连同 alpha 一起存成成品。
`tools/make_icons.py` 从它渲染出 `.icns` 与 `.ico`，前者进 bundle，后者在编译期作为
资源链进 `.exe`。容器都是这个脚本自己拼的，不经 `iconutil`，所以在 WSL 上也能给
Windows 版出图标。Windows 版同时链成 GUI 子系统，双击不弹控制台；从终端启动时
`os_attach_console()` 再把输出接回父控制台。

`src/sim/CMakeLists.txt` 的要点：

| 目标 | 内容 |
|---|---|
| `athena_sim_core` | 静态库：`core/ jit/ periph/ board/ image/ net/ dbg/` 全部，外加 `host/common/png.c` 与 `app_pkg.c`（与 host_tool 共用，避免 PNG 写出和 `.app` 解析两边漂移） |
| `athena_sim_cli` | `headless/main_headless.c` |
| `athena_sim` | `gui/*.c` + SDL2；**找不到 SDL2 就跳过**，只出 CLI |

两件值得知道的构建事实：

- **全局开 LTO**（`check_ipo_supported` + `CMAKE_INTERPROCEDURAL_OPTIMIZATION`）。
  原因写在 CMake 注释里：解释器每条指令都要穿过 `bus.c` 取指，而两者在不同编译单元，
  没有 LTO 那就是最热路径上一次真实函数调用。
- **JIT 没有编译期开关**。`jit_x64.c` 和 `jit_a64.c` 永远参与编译，用哪个由宿主机架构
  的预处理宏决定，用不用由**运行期命令行**决定。

前端只是不同的驱动循环：GUI 每帧按 `dt` 预算调用 `sim_run_us()`（`Ctrl+Tab` turbo 时
不再对齐墙上时钟），CLI 按 `--slice-us`（默认 1000）步进到 `--run-ms` 为止。

## 3. 机器模型

### 3.1 地址空间

`core/sim.h` 的几何常量：

| 区域 | 地址 | 大小 | 说明 |
|---|---|---|---|
| bootrom | `0x0000_0000` | 16 KiB | **HLE**：入口地址被派发到宿主实现，不是真代码 |
| XIP flash | `0x1000_0000` | 16 MiB | W25Q128；四个别名窗口都可读 |
| SRAM | `0x2000_0000` | 264 KiB | SRAM0-3 条带 + SRAM4 + SRAM5 |
| MMIO | 其余 | — | `mmio_attach()` 注册，最多 40 段 |

主频记为 `SIM_CLK_MHZ = 125`，虚拟时间 = 指令数 / 125 MHz。

### 3.2 启动流程

`sim_reset()` 复刻 flash boot：把 flash 头 256 字节拷到 SRAM5 顶部，从那里开始执行
boot2——boot2 自己配 SSI、把 VTOR 指向 `0x1000_0100`、跳到 reset handler。
`--skip-boot2` 则直接从向量表取 SP/PC 起跑。

因为复位会往 RAM 里"凭空"放代码（没有经过任何 store），`sim_reset()` 里必须
`jit_flush_all()`，否则块缓存会拿着上一次的翻译结果。

### 3.3 调度器

`sim_run_cycles()`（`core/machine.c`）：

```
while (还有预算) {
    target = cycles + quantum            // quantum 默认 64 条指令
    core0 跑到 target；core1 跑到 target
    cycles = target
    sim_periph_poll()                    // USB、HID 桥、控制socket、gdb、timer…
    deadlock_check(); profile_sample()
}
```

两核在**同一个线程里交替**，所以调度是确定性的：同样的输入跑两次，日志能逐行 diff。
需要系统调用的 poller 用 `sim_add_poll_every()` 降频（`SIM_NET_POLL_CYCLES` = 1 ms 虚拟
时间），因为 64 周期一片意味着无条件 poll 每虚拟秒会调用约 200 万次。

### 3.4 自旋节流（spin throttling）

固件在一核操作 flash 时会让另一核在共享 SRAM 上转一个标志位循环
（`c1_before_flash_operation`）。那个核会花掉整个 quantum 重读三条指令，而且第一次
之后的每次读**必然**得到同一答案——对方这一片还没运行过。

`spin_update()` 的判据是两个条件同时成立：这一片结束时的寄存器状态**出现过**
（16 个 GPR + SP 对 + APSR/IPSR/PRIMASK/CONTROL 的 FNV 指纹，记最近 4 个），
并且这一片**没有任何 store 或 MMIO 访问**（`s->side_effects[]`）。两者合起来说明它
什么也没做、也没走到任何地方。CRC 循环同样没有 store，但累加器不会重复，所以指纹把它
挡在外面。

处理方式是**节流而不是停车**：下一片只给 4 条指令（`SPIN_POLL_INSTR`），剩下的用虚拟
时间补齐。误判的代价是一个慢片，而不是死锁——那个核永远还在跑。

### 3.5 建模的外设

| 区域 | 建模程度 |
|---|---|
| CPU | 两颗 ARMv6-M；异常、PendSV/SVC、`EXC_RETURN`、MSP/PSP 分组、每核独立 NVIC |
| flash | W25Q128 **命令级**（JEDEC `EF 40 18`、4 KiB 擦除、256 B 页写） |
| 时钟 | RESETS/XOSC/PLL/CLOCKS/WATCHDOG，所有 ready/lock 位立即置起 |
| 定时 | TIMER 四个 alarm 驱动 ChibiOS tick |
| SIO | 核间 FIFO（含 core1 启动握手）、spinlock、每核除法器与插值器 |
| USB | 设备控制器 + DPSRAM + 一个能走完标准枚举的虚拟主机 |
| LCD | PL022 SPI1 → GC9107：CASET/RASET/RAMWR、+2/+1 视口偏移、INVON、真的 128×128 GRAM |
| 矩阵 | 88 级 GP6/GP7 移位链、GP8/9/10 直连输入、GP7 兼职背光 |
| RGB | DMA 节流的 PIO0 TX FIFO 直接解码成 86 颗 WS2812 |

不建模：周期精度、USB 物理层、PIO 作为指令集（直接解码 FIFO 字，这就是 WS2812 程序做的
全部事情）。

所有 flash 修改都收口在 `flash_erase_range()` / `flash_program_range()`
（`image/flash_image.c`），无论来路是 bootrom HLE、SPI 命令模型还是离线 app 安装。
这个收口点也是块缓存失效的挂钩位置。

---

## 4. 客户码执行：三档

三档**退休完全相同的指令、顺序也相同**，区别只在"每条指令要问多少个问题"。

| 命令行 | `cfg.jit` | `cfg.jit_native` | 行为 |
|---|---|---|---|
| `--no-jit` | false | false | 纯解释器，参考实现 |
| `--jit` | true | false | 一块一块，块内解释执行 |
| `--jit-native` | true | true | 块编译成宿主机码 —— **默认** |
| `--jit-verify` | true | 不变 | 每条块内指令重读客户机字节校验 |

默认值在 `headless/main_headless.c` 的 `main()` 开头就写死（`cfg.jit = cfg.jit_native = true`）。
`sim_create()` 里只有 `cfg.jit` 为真才 `jit_attach(s)`；否则 `s->jit` 恒为 NULL，
`cpu_run()` 永远走解释器。

### 4.1 共享的语义内核

```1160:1176:keyboards/ydkb/athena75_rgb_advanced/src/sim/core/cpu_armv6m.c
static SIM_FORCEINLINE void exec_decoded(cpu_t *c, uint32_t pc, uint16_t op, uint16_t hw2) {
    if ((op & 0xF800u) >= 0xE800u) {
        // 32-bit encodings live in 0xE800..0xFFFF (11101/11110/11111).
        c->r[15] = pc + 4u;
        if ((op & 0xF800u) == 0xF000u || (op & 0xF800u) == 0xF800u) {
            exec_thumb32(c, pc, op, hw2);
        } else {
```

`exec_decoded` / `exec16` 都标了 `SIM_FORCEINLINE`，而且这**不是**留给优化器决定的。
源码注释记录了一个真实教训：块执行器给 `exec_decoded` 增加第二个调用者之后，LTO 开始
逐次构建地决定要不要把它内联进解释器循环，同一份源码在 **33 到 42 宿主周期/客户指令**
之间摇摆。强制内联把快的那一侧钉住，代价是 switch 有两份副本——而这正是块执行器想要的。

### 4.2 第一档：解释器（非 JIT）

`cpu_run_interp()`。循环体每条指令做的事，按顺序：

1. `take_pending()` —— 有没有优先级更高的挂起异常要进入
2. `sleeping` —— 停在 WFI/WFE 就烧掉整片
3. `ipc < SIM_ROM_SIZE` → `bootrom_hle_dispatch()`，bootrom 是宿主实现的桩
4. `fetch16()` 取指（穿过 bus）
5. **调试门**：`break_pc | bp_count | trace | prof_blocks` 四个合并成**一次**按位或的
   测试——注释明确说明这是故意用位运算而不是短路，因为四个都是廉价加载，短路只会多买
   几个分支
6. `stall_check()` —— 自旋计数
7. `exec_decoded()` + `after_instr()`（SP 越界哨兵）
8. `cycles++ / instr++`

这就是"非 JIT 原理"的全部：**没有任何缓存**，每条指令重新回答上面所有问题。它慢，
但它是唯一一份能定义"正确"的实现，也是所有回退路径的落点。

### 4.3 第二档：块缓存（块内解释）

块缓存买到的不是"更快的指令"，而是**每条指令更少的问题**：上面第 1、3、4、5、6 步
从"每条一次"变成"每块一次"。

#### 切块规则（`jit/jit_frontend.c`）

块是**一条直线**：控制流从顶部进、从底部出。`jit_translate()`：

- 起点必须是**偶地址**且**不在 bootrom 内**（bootrom 是宿主桩，块执行器无法调用）
- 最多 `JIT_BLOCK_MAX = 32` 条
- **不允许跨 granule 边界**（256 B，`JIT_GRAN_SHIFT = 8`），这样一个 generation 计数器
  就能单独判定整块的有效性
- 遇到 `ends_block(op)` 就收尾：所有 32 位编码（BL 是要紧的那个）、BX/BLX、
  高寄存器形式的 ADD/MOV 写 r15、`POP {..., PC}`、BKPT、hint（WFI 等）、B、B\<cond\>
- **少于 2 条不成块**：块的意义就是摊薄进入成本，而且把单指令自旋留给解释器，
  才能让它的 stall 检测器继续作用在它本来为之而写的循环上

安全性是**双重判定**的：解码期靠上面这张"能写 r15 的编码"清单，运行期再拿 r15 和解码器
预测的下一地址比对。所以清单漏掉一条编码只是**块被截短**（性能问题），不会算错。

解码器用的 `peek16()` 是独立写的纯读取（只认 flash 与 SRAM 两个窗口），**不走 bus**：
没有 watchpoint、没有 MMIO、没有日志。理由是解码器绝不能改变机器状态——它运行的时刻
和它所解码的指令执行的时刻并不相同。

#### 缓存组织（`jit/jit_cache.c` + `jit_internal.h`）

```
索引表  jit_block_t block[16384]     直接映射，键 = 起始 PC 的散列，16 B/块
指令池  jit_insn_t  pool[1<<18]      bump 分配，4 B/指令
代号表  uint16_t    gen[granule 总数]
```

为什么表和池要分开：把 32 条指令的定长数组塞进槽里，就要为平均 4 条的块付 32 条的空间，
表会涨到几 MB，于是**每次查表都掉出客户机本来住着的末级缓存**。拆开之后索引 16 B/块，
整个热工作集的表还能待在 L2，而一个块的指令是连续的、通常落在同一条 cache line 里。

- **冲突就覆盖**：没有分配器、没有淘汰策略。代价是一次重翻译，不会泄漏也不会碎片化。
- **池和代码区都不做整理**：槽被复用或 granule 失效的块，就把它分配到的东西漏掉；
  任一侧满了就整体丢弃重来（`jit_flush_all()`）。这才是"分配 = 挪一个指针"的前提。
- **`b->pc` 有值而 `b->n == 0`** 是"这里不值得成块"的缓存形式，让热 PC 每次是一次查表
  而不是一次解码尝试。

#### 失效

有效性 = **每 granule 一个 generation 计数器**。块记下自己被解码时的代号，任何写客户机
代码的动作把对应 granule 的代号 `++`，下次查表发现不匹配就重新解码。这让失效开销与
**被写范围**成正比，而不是与活块数量成正比——要紧，因为固件每次启动都要写几千次 flash，
而那些写没一次靠近代码。

两条写入路径：

- **flash**：只能经过 `flash_erase_range` / `flash_program_range` 这个收口点，按范围失效。
- **SRAM**：`jit_note_store()` 是内联的守卫。它只问 SRAM（块只可能住在 flash 或 SRAM），
  而且用**一次带索引的字节加载**问：`s->sram_code[granule]`，每 256 B 一个字节，
  有活块时非零。注释坦白说明这是故意不精确的——"问得更严谨的开销会超过失效省下的"。
  `sram_code[]` 被放在 `struct sim` 的**最后**，因为一千字节横在中间会把指令循环要读的
  字段推到不同 cache line 上，那比整套失效机制省下的还多。

#### 块执行循环（`cpu_run_blocked`，在 `cpu_armv6m.c` 里）

执行为什么写在 CPU 文件而不是 `jit/`：因为 `exec_decoded` 必须内联进这个循环，
而跨编译单元的内联没法可靠依赖。

内层循环里**缺席的东西才是重点**：没有取指、没有长度测试、没有调试门、没有 stall 计数、
没有每条指令的"有没有挂起异常"。剩下的是指令自身的语义，加上一条直线代码仍然可能答错的
两个问题：

```1394:1404:keyboards/ydkb/athena75_rgb_advanced/src/sim/core/cpu_armv6m.c
            at += ((op & 0xF800u) >= 0xE800u) ? 4u : 2u;
            done++;

            // r15 somewhere other than the instruction the decoder put next means
            // either the last instruction of the block -- a branch, which is why the
            // loop is about to end anyway -- or something the decoder did not
            // predict: a fault, an exception entry, a POP that took the PC. Either
            // way the rest of this block is no longer the right code to run. A
            // pending exception is the same story told by cpu_raise_hardfault, which
            // leaves r15 alone for the scheduler.
            if (c->r[15] != at || c->pend) break;
```

两个调度细节：

- **停车的核在这里也要烧掉整片**，和解释器一致。曾经把停车的核一周期一周期交给
  `cpu_run_interp`，结果它落在调度器后面，之后又靠超跑补回来——同一虚拟秒多退休了
  **38%** 的指令，而账记在了块缓存头上。
- **比剩余片长的块照跑，按预算截断**，而不是拒绝。拒绝会把"恰好跨片尾"的块全部推给
  解释器，实测约十五分之一。

查不到块（bootrom 桩、奇地址、单指令自旋）就退回**恰好一条**解释指令；若一条也没推进
（`c->cycles == was`）就直接返回，不在上面打转。

### 4.4 第三档：本地机器码 JIT

#### 后端选择

```15:21:keyboards/ydkb/athena75_rgb_advanced/src/sim/jit/jit_native.h
#if defined(__x86_64__) || defined(_M_X64)
#    define JIT_HAVE_BACKEND 1
#    define JIT_BACKEND_X64  1
#elif defined(__aarch64__) || defined(_M_ARM64)
#    define JIT_HAVE_BACKEND 1
#    define JIT_BACKEND_A64  1
#endif
```

其他架构上 `jit_backend_name()` 返回 NULL，`--jit-native` 就**静默等于 `--jit`**。
这不是错误：可移植执行器覆盖了后端会覆盖的一切。

#### 后端只翻译**前缀**

`jit_emit_block()` 的契约是"发射 `n` 条里本后端覆盖的那些，返回覆盖数"。遇到没有发射器的
编码，本地代码在那里结束，同一个块的余下部分由 `cpu_armv6m.c` 的可移植执行器接着跑。
这就是"部分覆盖是安全的而不仅仅是不完整的"——一条未实现指令最坏只让它自己慢一点。

发射前先**规划**（`plan_insn()`），因为序言写不出来之前必须知道这块要保存哪些寄存器，
事后重写变长编码得不偿失。规划顺带决定要不要放弃：

```949:949:keyboards/ydkb/athena75_rgb_advanced/src/sim/jit/jit_x64.c
    if (native < 2u || native * 2u < cover) return 0;
```

即：本地指令少于 2 条，或者不到全块的一半，就不值得——因为逃逸指令比可移植执行器的同一条
略贵，块必须"大部分是发射出来的"才划得来。

#### x86-64 后端的三个决定（`jit/jit_x64.c`）

**一、客户机寄存器留在 `cpu_t` 里，当内存操作数用。** 听起来是慢的选择，其实不是：
x86 指令白拿一个内存操作数，`cpu_t` 只有几百字节且常驻 L1；而反过来（把客户机寄存器
缓存进宿主机寄存器）必须在每次 helper 调用和每次块出口 spill。这里块平均 4 条指令，
养不起一个寄存器分配器。

常驻的宿主机寄存器只有四个半，都是两个 ABI 下的 callee-saved，所以能活过 helper 调用：

| 宿主机 | 用途 |
|---|---|
| `RBX` | `cpu_t *` |
| `R14` | 客户机 SRAM 的宿主基址（仅当块有内存访问时保存） |
| `R15` | 标志转换表 `s_nzc`（仅当块动标志时） |
| `R12` | 分组后的 SP，每块读一次（仅当块用 SP 时） |
| `R10`/`R11` | 地址与待存值的暂存；两个 ABI 里都不是参数寄存器，无需排序 |

序言只 push 这块**实际用到**的那几个（`push_r`），并按需 `sub rsp` 留出 Win64 的 32 字节
shadow space + 16 字节对齐。

**二、标志位在设置它的指令之后立刻"实体化"进 `apsr`。** 用 `lahf` + 一张 256 项表把
SF:ZF:CF 一次搬成 ARM 位置的 N/Z/C，OF 单独用 `setcc` 取（`lahf` 不带它）。逻辑运算
保留 C 和 V、移位保留 V，靠 `keep` 掩码表达；`apsr` 低 28 位永远保留，因为这里没资格
断定它们是零。

为什么不用惰性标志（只记操作数、需要时再算）：那样每一个后来的 `apsr` 读者——解释器的
`cond_pass`、ADC、MRS、异常入口——都得先问"标志解析了吗"。而这个移植的前提条件是
**`--no-jit` 保持参考实现**，那就排除了改变 `apsr` 的含义。

**三、加载内联 SRAM 快路径，存储一律调用 helper。** 这个不对称是故意的：

```469:476:keyboards/ydkb/athena75_rgb_advanced/src/sim/jit/jit_x64.c
static void emit_load(tr_t *t, unsigned rd, unsigned bytes, bool sign, unsigned len) {
    asm_t  *a = t->a;
    label_t slow = {{0}, 0}, done = {{0}, 0};

    mov_r_r(a, RAX, REG_ADDR);
    alu_r_i(a, ALU_SUB, RAX, SIM_SRAM_BASE);
    alu_r_i(a, ALU_CMP, RAX, SIM_SRAM_SIZE - bytes);
    jmp_to(a, CC_A, &slow);
```

- **加载**：`addr - SRAM_BASE` 一次无符号比较兼判下界与上界；再查对齐（未对齐是异常，
  由解释器报告）；命中就 `ld_idx` 直接从宿主 SRAM 取。落空跳 slow path 调
  `jit_ld32/16/8`——这些正是**解释器自己的访问函数**，于是 MMIO 派发、watchpoint、日志、
  异常全部保持解释器产生的**那个形式**。有符号加载在返回后补两次移位，和解释器的
  LDRSB/LDRSH 用的是同两条。
- **存储**：一律调 `jit_st32/16/8`。因为一次 store 必须给核标上"产生了副作用"
  （调度器的自旋节流要读它）、必须检查是否落在装着已翻译代码的 granule 上、
  而且**可能让正在运行的这个块本身失效**。加载没有这些问题，而加载是更常见的那一半。

所以"对 MMIO 的写、对存着翻译代码的 granule 的写、对 flash 的写"在本地码里的行为和解释
执行下**完全一致**。

#### 逃逸（escape）

没有发射器的编码由 `emit_escape()` 处理：把 `(cpu, pc, op|hw2<<16)` 交给
`jit_exec_one()`，也就是解释器的派发，从本地块内部执行一条指令。

```528:531:keyboards/ydkb/athena75_rgb_advanced/src/sim/jit/jit_x64.c
// One instruction handed to the interpreter's dispatch from inside the block. This
// is what keeps an encoding without an emitter from truncating everything after it:
// PUSH and POP alone were a third of the blocks this backend used to decline, and
// they are not worth emitting -- they write SP and touch up to nine words.
```

返回后检查 `c->pend`，再拿 r15 和解码器预测的下一地址比——这一次比对同时盖住了
"它把控制转走了"（`POP {PC}`、BX）和"它出错了"两种情况。如果这块缓存了 SP，
逃逸之后必须重读（PUSH/POP/ADD SP 都会动 SP）。

留给解释器的典型集合：PUSH/POP、LDM/STM、高寄存器形式、按寄存器移位（计数不在编译期已知，
且 ARM 定义了 ≥32 的行为而 x86 会绕回 31）、SVC、扩展/REV/hint。README 记录：
**约 95% 的退休指令是本地执行的**，尽管被覆盖的编码集合小于这个比例。

#### 出口协议

本地块的签名是 `unsigned (*)(cpu_t *)`：返回**它退休了多少条客户机指令**，并把 r15 留在
客户机接下来该去的地方。它允许比它覆盖的少退休——出错会停，某个想逐条看指令的调试特性
也会停。调用方据此分流：

```1362:1368:keyboards/ydkb/athena75_rgb_advanced/src/sim/core/cpu_armv6m.c
        if (native && b.code && b.code_n <= limit) {
            done = b.code(c);
            st->native_entries++;
            st->native_retired += done;
            if (done != b.code_n || c->pend) {
                // It gave the block back early: a fault, or a debug feature it
                // declined to handle. Either way the rest is not ours to run.
```

**不能部分进入**：发射出来的代码没有"跑 k 条就停"的能力，所以跨片尾的块这一次走解释执行。
`done == 0`（连第一条都没退休）时不能循环，否则空转——交给解释器去推进。

块尾若不是分支，序言之外还要补一条 `mov [cpu+r15], at`，让可移植执行器知道从哪接手它
拒绝掉的那些指令。

#### arm64 后端的差异（`jit/jit_a64.c`）

形状和 x64 相同（寄存器留在 `cpu_t`、标志即时实体化、加载内联/存储调用），因为三者必须
逐条指令彼此一致、也和 `--no-jit` 一致。三处更容易：

1. **标志就是同一套标志**。ARM64 的 NZCV 就是客户机的 NZCV，位置相同、规则相同——
   `adds`/`subs` 对 carry 的定义完全一致，x64 那边的借位/进位修正在这里根本不存在。
   合并进 `apsr` 只是 `mrs` + 移位 + bitfield insert。
2. **合并不破坏标志**，所以紧跟在设标志指令后面的条件分支可以直接是原生 `b.cond`——
   不需要条件表、不需要位测试。这很值：五条客户机指令里有一条是条件分支，而多数紧跟比较。
   条件码编号 Thumb 和 arm64 也完全相同。
3. **定长一字编码**，没有 ModRM、没有变长、每个分支都够到一兆。

常驻寄存器：`x19` = cpu，`x20` = SRAM 基址，`x21` = SP，`x22`/`x23` = 范围检查用的
基址与上限。

#### 可执行内存（`jit/jit_code.c` + `core/os.c`）

- **4 MiB 的 bump 分配区**，`JIT_CODE_SIZE`。不逐块释放，理由和指令池一样。
  `jit_code_low()`（剩余 < 16 KiB）在**翻译前**询问，这样"缓存满"是一次 flush，
  而不是一个悄悄失去本地形式的块。
- 区域**一次性映射成它将保持的权限**，因为调用方按块提交、一秒可能有几十万块，
  每块一次 `mprotect` 比块本身省下的还贵。
- **Apple silicon 例外**：不允许同时可写可执行。那里用 `MAP_JIT`，写权限改成
  `pthread_jit_write_protect_np()` 逐线程切换——这是廉价的用户态操作而非系统调用。
  搞错的表现是第一次 store 直接 SIGBUS，而不是 mmap 报错。
- **提交**时按实际写入的字节范围做 icache 维护（`FlushInstructionCache` /
  `sys_icache_invalidate` / `__builtin___clear_cache`），**不**向上取整到页——
  为一个五十字节的块刷一整页不合算。
- **Win64 展开信息**：其他平台发射的代码不需要注册；Windows 上返回地址落在 loader 不认识
  的区域会让栈回溯**在那里终止**——对一个专门用来调试别的东西的程序，这不是可接受的失败
  模式。做法是 `RtlInstallFunctionTableCallback`（块是一个一个出现的，静态表得每块重建
  一次），回调拿 PC 二分查出所属块。每块的序言只有二十几种形状（保存了哪几个 callee-saved
  寄存器 + 有没有留调用帧），所以 `UNWIND_INFO` 在启动时建好共享；它们必须住在被注册的
  区域内部（`RUNTIME_FUNCTION` 用相对基址的偏移寻址它们），这就是**代码区第一页不是代码**
  的原因。

### 4.5 三档之间怎么切换

**每片决定一次**，在 `cpu_run()` 里：

```1427:1432:keyboards/ydkb/athena75_rgb_advanced/src/sim/core/cpu_armv6m.c
    if (s->jit && !(s->cfg.break_pc | s->bp_count | (unsigned)trace_enabled() |
                    (unsigned)prof_blocks_enabled())) {
        cpu_run_blocked(c, target);
    } else {
        cpu_run_interp(c, target);
    }
```

每片决定一次是安全的：这些标志不会在一片运行中间出现，而两个可能的
（断点命中、halt）都会先 return。

**回退矩阵**——凡是想逐条看指令的东西都会自己关掉上层档位，且只在它被 arm 的期间：

| 条件 | 结果 | 判定点 |
|---|---|---|
| `--break` / gdb 断点 / `--trace` / `--prof-blocks` | 整片退到解释器 | `cpu_run()` |
| `--watch`（内存 watchpoint） | 保留块缓存，**关掉本地码**（内联加载路径不走 bus） | `cpu_run_blocked()` 的 `native` + 块序言里再查一次 `watch_len` |
| gdb 单步 | 解释器 | `bp_count` 路径 |
| 查不到块 / bootrom / 奇 PC / 单指令自旋 | 恰好一条解释指令 | `jit_lookup()` 返回 false |
| 后端拒绝发射（覆盖不足、空间不够、跳转超距） | 该块块内解释执行 | `jit_emit_block()` 返回 0 |
| 本后端没有的编码 | 该条 `jit_exec_one()` | `emit_escape()` |
| 块跨片尾 | 该次块内解释执行 | `b.code_n <= limit` 不成立 |
| 池满 / 代码区将满 | `jit_flush_all()`，重新开始 | `jit_lookup()` |
| 无可执行内存 / 无本后端 | `cfg.jit_native` 被清掉 | `jit_attach()` |
| 块缓存内存分配失败 | `s->jit` 留空，全程解释 | `jit_attach()` |

块序言里那次 watchpoint 检查为什么要留着（明明 `cpu_run_blocked` 已经查过）：
watchpoint 可能在两片之间才被装上。反过来，`cpu_run_blocked` 那次也要留着，
否则每条指令都得进本地码一次才被告知同一件事。

顺带一个不需要检查的：**SP 哨兵**（`sp_watch`）。这个后端发射的东西没有一样能写 r13——
所有被覆盖的编码都用三位表示寄存器，能碰到 SP 的形式全部留给解释器——所以本地块内 SP
不可能移动，哨兵没有新东西可看。

### 4.6 正确性怎么保证

1. **语义单一来源**：三档共用 `exec_decoded`；条件码用同一张 `cpu_cond_table()`
   （后端在发射期读它，值变成立即数）；内存的"难办情况"全部回到解释器的
   `jit_ld*` / `jit_st*`。
2. **控制流双重判定**：解码期的 `ends_block()` 清单 + 运行期 r15 与预测地址的比对。
   前者错只损失性能。
3. **`--jit-verify`**：块运行时重读每条指令下面的客户机字节，不符就报
   "stale decode"。故意不内联，因为在乎速度的运行里它是关掉的。
4. **像素回归两档都跑**：
   ```bash
   tools/sim_regress.py --extra=--no-jit
   tools/sim_regress.py --extra=--jit-native
   ```
   `tools/sim_regress.py` 从空白 flash 启动每个用例、按键、把面板 dump 与
   `tests/golden/` **逐字节**比对。RGB565→RGB888 的转换与 `src/host/common/` 完全相同，
   所以 golden 也可以用 `host_tool snapshot` 从真机录。
5. **存档一致性**：`--save-state` 存整机（含 flash），恢复后继续跑，落在和不间断运行
   同一条指令计数上。

### 4.7 性能

README 记录的实测（Apple M4，同一段 14 秒固件运行）：

| 档位 | 相对实时 | 宿主周期/客户指令 |
|---|---|---|
| `--no-jit` | 0.79× | 32 |
| `--jit` | 0.93× | — |
| `--jit-native` | 1.46× | 17 |

自测工具：`--host-mhz F` 报告每条客户指令花了多少宿主周期；`--prof-blocks` 报告客户机
基本块的长度分布和最热块头（这回答的是"块执行值不值得"，而 PC 采样器答不了——它采的是
时间去哪了，不是代码是什么形状）；运行结束时块缓存会打印
entries/retired/translations/collisions/invalidations/flushes/fallbacks 和本地码占比。

注意 `--realtime`：仿真器大约跑到实时的四分之三，不加它的话 `host_tool` 的墙钟超时会
相应变紧。

---

## 5. `.app` 与仿真器的关系

slot app 是**可重定位的原生 ARM Thumb 模块**，不是脚本、不是字节码：

- 链接到参考基址 `0x1080_0000`（`src/app/sdk/app.ld`），装进 `A75APKG` 容器（`.app`）；
- 安装时按 `slot_base - link_base` 给重定位表里记录的每个 flash 指针打补丁；
- 固件 `app_loader.c` 校验头/ABI/CRC，把 `.data` 从 flash 拷进 RAM、清 `.bss`，
  调用 `app_init(&g_api)` 拿到 `app_desc_t{enter,exit,tick}`，代码**从 flash 里 XIP 执行**；
- `host_api_t` 是固件侧的函数指针表，是 app 唯一的系统接口。

详见 `docs/flash_map.md`（槽位几何、`.app` v3 打包）与 `docs/ram_map.md`（80 KiB app arena）。

仿真器这边：

| | 硬件 | 仿真器 |
|---|---|---|
| app 二进制 | flash 里的 ARM Thumb | **同样的字节**，在仿真的 16 MiB flash 里 |
| 执行方式 | Cortex-M0+ 原生 XIP | 仿真的 ARMv6-M（解释 / 块 / 本地码） |
| 安装 | `host_tool app install`（HID）或 UF2 | `--install-app x.app` → `app_pkg_relocate` + `flash_program_range` |

`image/app_install.c` 复用 host_tool 的 `app_pkg` 重定位，所以这里写进去的槽位和
`host_tool app install` 从 raw HID 写进去的**逐字节相同**。当被仿真的固件走到
`loader_load` 并调用 `app_init` 时，那次调用仍然是**客户机 ARM 代码**——只是按当时的档位
被解释或被 JIT 翻译而已。

## 6. 调试设施

| 设施 | 用法 | 备注 |
|---|---|---|
| 分域日志 | `--log 'usb=debug,lcd=trace,*=info'`、`ATHENA_SIM_LOG`、GUI 勾选框 | `--log-file` 出 JSONL；调度确定性 ⇒ 两次运行可逐行 diff |
| 符号 | `--elf .build/*.elf` | PC 在日志/trace/断点/profiler 里显示成 `matrix_scan+0x1a` |
| GDB | `--gdb 3333`（`--gdb-wait` 复位即停） | 两核 = thread 1/2 |
| 指令 trace | `--trace` / `--trace-file` | 环形缓冲 4096；会强制解释器 |
| watchpoint | `--watch ADDR[,LEN]`、`--watch-after MS` | 会关掉本地码 |
| 采样 profiler | 每次运行结束自动打印；`--prof-top N` | 一片采一次 PC |
| 块普查 | `--prof-blocks` | 长度直方图 + 最热块头 |
| 存档 | `--save-state` / `--load-state`、GUI 的 F6/F7 | 含 flash，位精确 |
| 控制 socket | `--ctl-port N`：`key 9,0` / `down` / `up` / `shot f.png` / `leds` / `state` / `log` / `save` / `load` / `quit` | Enter 是 `9,0`，用来确认安装对话框 |
| Raw HID 桥 | `--hid-port N` + `export ATHENA_HID_SIM=127.0.0.1:N` | 真 `host_tool` 的每条命令都能对仿真器原样跑 |
| 死锁提示 | 自动 | 两核 stall 计数都过 10 万时报双核疑似死锁 |

## 7. 关键文件索引

| 文件 | 职责 |
|---|---|
| `src/sim/core/sim.h` | 几何常量、`sim_t` / `cpu_t` / `sim_config_t`、全部对外 API |
| `src/sim/core/cpu_armv6m.c` | ARMv6-M 语义、`exec_decoded`、`cpu_run_interp`、`cpu_run_blocked`、`jit_ld*/st*`、`jit_exec_one` |
| `src/sim/core/machine.c` | 生命周期、启动、双核调度、自旋节流、采样 profiler、块普查 |
| `src/sim/core/bus.c` | 地址派发（ROM/XIP/SRAM/MMIO）、watchpoint |
| `src/sim/core/os.c` | 平台垫片：socket、单调时钟、可执行内存（MAP_JIT / W^X） |
| `src/sim/core/state.c` | 整机存档（flash + SRAM + CPU） |
| `src/sim/jit/jit.h` | 块缓存对外契约、`jit_insn_t`、`jit_note_store()` |
| `src/sim/jit/jit_frontend.c` | 切块：`ends_block()`、`jit_translate()`、`peek16()` |
| `src/sim/jit/jit_cache.c` | 直接映射表、generation 失效、`jit_lookup()`、`jit_verify_insn()` |
| `src/sim/jit/jit_native.h` | 后端契约、helper 声明、代码区 API |
| `src/sim/jit/jit_x64.c` | Thumb → x86-64 |
| `src/sim/jit/jit_a64.c` | Thumb → arm64 |
| `src/sim/jit/jit_code.c` | 可执行内存 arena、Win64 unwind 注册 |
| `src/sim/periph/bootrom_hle.c` | 合成 bootrom（flash 例程 HLE） |
| `src/sim/image/flash_image.c` | 16 MiB 背书文件、擦/写收口点、写入即失效 |
| `src/sim/image/app_install.c` | 离线 `.app` 装槽（复用 host_tool 的重定位） |
| `src/sim/board/athena75_board.c` | 88 级矩阵移位链、按键注入、背光 |
| `src/sim/README.md` | 用户向使用说明 |
| `tools/build_sim.sh` / `tools/run_sim.sh` / `tools/sim_regress.py` | 构建 / 启动 / 像素回归 |

## 8. 常用命令

```bash
# 构建
bash $KB/tools/build_sim.sh
bash $KB/tools/build_sim.sh --windows

# GUI
artifacts/sim/<os>/athena_sim --uf2 artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2 \
                              --flash flash.bin

# 无头 + 装 app + 出图
artifacts/sim/<os>/athena_sim_cli --uf2 <fw.uf2> --flash flash.bin \
    --install-app artifacts/apps/maze.app --run-ms 4000 --png screen.png

# 让真 host_tool 对着仿真器说话
artifacts/sim/<os>/athena_sim_cli --uf2 <fw.uf2> --flash flash.bin --realtime --hid-port 4711
export ATHENA_HID_SIM=127.0.0.1:4711

# 三档对比 / 排查 JIT 差异
... --no-jit            # 参考实现
... --jit               # 只要块缓存
... --jit-native        # 默认
... --jit-verify        # 校验块里的解码是否失效
... --host-mhz 4400 --prof-blocks

# 回归
$KB/tools/sim_regress.py
$KB/tools/sim_regress.py --extra=--no-jit
$KB/tools/sim_regress.py --case launcher
$KB/tools/sim_regress.py --bless
```
