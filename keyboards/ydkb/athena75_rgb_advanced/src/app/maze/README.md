# MAZE

<img src="../../../docs/apps/maze.gif" width="256" alt="MAZE">

16×16 的完美迷宫（每格 8 px，任意两点之间恰有一条通路），生成完自己走通，停一下，
再换一张重来。

屏幕正中 32×32 px——4×4 格——是封死的，路必须绕开它。这不是装饰：一整块不可通行的
区域会让最朴素的深度优先挖洞算法容易在角落里憋出一大片死胡同，所以生成和寻路都得
把它当成真正的障碍来处理。

迷宫的形状由**种子**决定，起点和终点却取自开机以来的毫秒计时——两条随机数流是分开的，
所以同一个种子每次出现，走的都是不同的两个角。种子号画在屏幕上，←→ 可以翻。

## 菜单（Enter）

| 项 | 取值 |
|---|---|
| SPEED | FAST / MED / SLOW / V.SLOW |

## 直接按键

| 键 | 功能 |
|---|---|
| ← / → | 换种子（立刻重画一张迷宫） |
| ↑ / ↓ | 速度 |
| Enter | 打开菜单 |
| Esc | 退出回启动器 |

## 构建与安装

以下命令在 `keyboards/ydkb/athena75_rgb_advanced/` 下执行：

```bash
bash src/app/tools/build_app.sh maze        # -> artifacts/apps/maze.app
host_tool app install artifacts/apps/maze.app
```

---

通用操作与设置落盘规则见 [`docs/usage.md`](../../../docs/usage.md)；
app 一览见[仓库根 readme](../../../../../../readme.md#预置-app)。
