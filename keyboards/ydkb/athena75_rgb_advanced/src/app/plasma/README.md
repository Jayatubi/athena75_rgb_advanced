# PLASMA

经典干涉 Plasma：在 128×128 面板上用整数正弦场 + 可选公式变体生成干涉条纹。
热路径仅用整数/定点数；写入 `fb` 时按大端 RGB565（`fb_put` + `__builtin_bswap16`，与固件 `fbShow` 一致）。

参考：[Lode's Computer Graphics Tutorial — Plasma](https://lodev.org/cgtutor/plasma.html)

## 效果

- **PATTERN** 在几种 Lode 风格场公式间切换（不是换色板）：XY / DIAG / DIST / RINGS / FULL。
- 时间相位只用 `uint8` 整数倍步进（禁止 `t>>1` 一类半周期陷阱），波源按慢速 Lissajous **每帧**漂移；色板滚动更慢、也连续。
- 色板共 9 种（默认 **GOLD**），支持 **AUTO PALETTE** 自动过渡：当前色板 A 会按 `lerp(A,B,t)` 平滑混到目标色板 B，结束后自动切到下一组（循环）。
- 混色在 8-bit RGB 里完成后再打包 RGB565，降低可见台阶；并保持 `pal_lut[255]=pal_lut[0]` 的无缝滚动语义。
- 出于帧预算考虑，混合 LUT 只在过渡步进变化时重建，不在像素热路径内逐点算插值。

## 菜单（Enter）

| 项 | 取值 |
|---|---|
| SPEED | FAST / MED / SLOW / V.SLOW |
| PATTERN | XY / DIAG / DIST / RINGS / FULL |
| PALETTE | SUN / SKY / SOFT / GOLD / PEACH / TEAL / DUSK / FIRE / ICE |
| AUTO PAL | OFF / ON |
| XFADE SPD | FAST / MID / SLOW（约 2s / 4s / 8s） |
| MODE | NEAR / FULL |

### 场公式（PATTERN）

相位按 `φ(p, w) = 41·p / w`（`sin_lut` 一周 256 ≡ 2π）。`t` 为场相位；双漂移中心由独立 Lissajous 相位 `s` **每帧**更新：

`C0 = (64 + 44 sin(s)/128, 64 + 40 cos(2s)/128)`  
`C1 = (64 + 50 sin(s+90)/128, 64 + 46 cos(3s+50)/128)`

`d0 = √((x−C0x)²+(y−C0y)²)`，`d1` 同理对 `C1`；角点距离 `dc = √(x²+y²)`。各项 `sin` 相加后除以项数，再经 6/5 增益与 soft knee 映到色板。

| 名 | 公式 |
|---|---|
| XY | 经典正交双正弦（Lode 早期）：`[sin(φ(x,20)+t) + sin(φ(y,20)+2t)] / 2` |
| DIAG | 再加斜向：`[sin(φ(x,20)+t) + sin(φ(y,16)+2t) + sin(φ(x+y,20)+t)] / 3` |
| DIST | 四正弦（x / y / 漂移中心 / 角点）：`[sin(φ(x,20)+t) + sin(φ(y,40)+2t) + sin(φ(d0,18)+3t) + sin(φ(dc,16)+2t)] / 4` |
| RINGS | 双漂移距离中心（圆环干涉）：`[sin(φ(d0,18)+t) + sin(φ(d1,16)+2t)] / 2` |
| FULL | 多正弦全项（默认）：`[sin(φ(x,22)+t) + sin(φ(y,16)+2t) + sin(φ(x+y,20)+t) + sin(φ(d0,18)+3t) + sin(φ(d1,16)+2t)] / 5` |

### 色板

| 名 | 说明 |
|---|---|
| SUN | 近黑 → 金 / 琥珀 / 珊瑚 / 玫桃（深底高饱和） |
| SKY | 深蓝 → 青 → 蔚蓝 → 淡紫 |
| SOFT | 深玫 / 桃 / 金（柔于 FIRE，不是灰雾） |
| GOLD | 近黑 → 暖金 / 琥珀（**默认**） |
| PEACH | 深褐 → 桃 → 杏 → 腮红 / 奶油 |
| TEAL | 深青绿 → 青 → 海沫 / 薄荷 |
| DUSK | 近黑紫 → 品红 → 橙夕照 |
| FIRE | 黑 → 红 → 橙 → 黄 → 白 → 黑（无缝） |
| ICE | 深蓝 → 青 → 白 → 深蓝 |

索引映射约 1.2× + soft knee；丰富度主要靠色板，不做陡峭拉伸。

### 渲染模式

| 名 | 说明 |
|---|---|
| NEAR | 64×64 + 2×2 最近邻（默认） |
| FULL | 原生 128×128 |

## 直接按键

| 键 | 功能 |
|---|---|
| ↑ / ↓ | 加快 / 减慢 |
| ← / → | 循环换色板（AUTO=ON 时会从该色板重新开始自动序列） |
| `-` / `=` | 上一个 / 下一个 PATTERN |
| Space | 切换 MODE（NEAR ↔ FULL） |
| Enter | 打开菜单 |
| Esc | 退出回启动器 |

（`gif` 仍是固件的 OS 输入模式开关，不会进到 app。）

SPEED / PATTERN / PALETTE / MODE / AUTO PAL / XFADE SPD 写入 save（version 13）。

## 构建与更新

以下命令在仓库根目录下执行（`KB=keyboards/ydkb/athena75_rgb_advanced`）：

```bash
bash $KB/src/app/tools/build_app.sh plasma
# Windows（WSL）：
wsl artifacts/host/windows/host_tool.exe app update artifacts/apps/plasma.app --slot 0x10D00000
```

键盘 LCD 确认 **INSTALL** 后完成覆盖更新（勿反复 `install`）。


---

通用操作与设置落盘规则见 [`docs/usage.md`](../../../docs/usage.md)；
app 一览见[仓库根 readme](../../../../../../readme.md#预置-app)。
