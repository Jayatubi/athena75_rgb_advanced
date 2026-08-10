# MATRIX

<img src="../../../docs/apps/matrix.gif" width="256" alt="MATRIX">

数字雨，可以叠一层时钟水印。

时钟不是画在雨上面的另一层：键盘上没有 RTC，时间由 `host_tool synctime` 从电脑推过来
（`host_api.clock_sec()`，没对过时就是 0）；显示的时候，落在数字笔画上的那些字符只是
被给了一个更亮的下限和一种更亮的颜色，其余照常随雨淡出。所以水印会跟着雨一起呼吸，
雨扫过时浮现，扫过之后按 **CLOCK** 设定的那个下限停住。

同一份参数在菜单里和不离开雨的情况下都能改，改哪儿都一样。

## 菜单（Enter）

| 项 | 取值 |
|---|---|
| SPEED | FAST / MED / SLOW / V.SLOW |
| DENSITY | HIGH / MED / LOW / MIN |
| CLOCK | 50% / 62% / 75% / 88% / 100%——时钟笔画最暗能淡到多少 |

## 直接按键

| 键 | 功能 |
|---|---|
| ↑ / ↓ | 速度 |
| → / ← | 密度 |
| `=` / `-` | 时钟笔画的亮度下限 |
| Enter | 打开菜单 |
| Esc | 退出回启动器 |

方向键与 `-` `=` 长按连发，但只暂存，所以按住不放也只在退出 app 时写一次 flash。

## 构建与安装

以下命令在 `keyboards/ydkb/athena75_rgb_advanced/` 下执行：

```bash
bash src/app/tools/build_app.sh matrix        # -> artifacts/apps/matrix.app
host_tool app install artifacts/apps/matrix.app
host_tool synctime                            # 对时，时钟水印才有内容
```

---

通用操作与设置落盘规则见 [`docs/usage.md`](../../../docs/usage.md)；
app 一览见[仓库根 readme](../../../../../../readme.md#预置-app)。
