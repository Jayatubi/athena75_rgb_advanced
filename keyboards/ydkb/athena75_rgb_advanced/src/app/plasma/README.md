# PLASMA

经典干涉 Plasma：在 128×128 面板上用整数正弦场 + 漂移波源生成多条干涉条纹。
热路径仅用整数/定点数；写入 `fb` 时按大端 RGB565（`fb_put` + `__builtin_bswap16`，与固件 `fbShow` 一致）。

参考：[Lode's Computer Graphics Tutorial — Plasma](https://lodev.org/cgtutor/plasma.html)

## 效果

- 空间波长介于旧版软带（/32）与过密 /16、/8 之间（`lode_phase` 宽度 18/12/16/14/12），128 面板上可见多条峰谷，并减轻 RGB565 条带造成的「卡顿感」。
- 两个距离项的中心按慢速 Lissajous **每帧**漂移（SPEED 只改步进/分频，不跳帧）；色板滚动更慢、也连续，让波源运动更抢眼。
- 色板仅 **SUN** / **SKY**（默认 SUN）；←→ 在两者间切换。

## 菜单（Enter）

| 项 | 取值 |
|---|---|
| SPEED | FAST / MED / SLOW / V.SLOW |
| PALETTE | SUN / SKY |
| MODE | NEAR / FULL |

### 色板

| 名 | 说明 |
|---|---|
| SUN | 柔和暖阳金（默认） |
| SKY | 明亮天空柔彩 |

### 渲染模式

| 名 | 说明 |
|---|---|
| NEAR | 64×64 + 2×2 最近邻（默认） |
| FULL | 原生 128×128 |

## 直接按键

| 键 | 功能 |
|---|---|
| ↑ / ↓ | 加快 / 减慢 |
| ← / → | 换色板（SUN ↔ SKY） |
| Space | 切换 MODE（NEAR ↔ FULL） |
| Enter | 打开菜单 |
| Esc | 退出回启动器 |

（`gif` 仍是固件的 OS 输入模式开关，不会进到 app。）

SPEED / PALETTE / MODE 写入 save（version 11）。

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
