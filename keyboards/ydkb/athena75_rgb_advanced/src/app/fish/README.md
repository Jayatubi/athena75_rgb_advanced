# FISH

<img src="../../../docs/apps/fish.gif" width="256" alt="FISH">

一缸鱼。模型是 Reynolds 的 boids，但**因果链是真的**：

```
力 = 群体推力（朝巡航速度的 Reynolds steering）
   + 环境（池壁的切向流 + 向内的法向力）
   - 阻力 × 速度
```

力求和成加速度，加速度积成速度，速度积成位移——没有任何一处是靠直接改坐标让鱼动的。
也**没有速度上限**：最高速是推力和阻力自己找到的平衡点。积分之上只加了四条约束：一个
最低速度、一个最大转向率（所以鱼是划弧线的，不会原地掉头）、预判式的贴壁避让（沿着
玻璃滑走，而不是弹回来），以及一个兜底的位置钳制。

池壁的力场是柔化的平方反比 `|F| = S·R²/(d²+R²)`——没有硬边界可跨，贴着玻璃最强，往里
平滑衰减。切向分量在屏幕上顺时针走，两者合起来读着就像一股鱼群顺着游的环流。

一条鱼是四节关节串成的：0 号关节坐在位置上朝着航向，其余按固定间距被拖着走，再叠一条
行进的正弦波，越靠尾巴横向偏移越大。每节画成一个指向前一节的渐窄三角形。体色跟着速度
走，所以一群鱼加速的时候整片颜色都会变。

隔一阵子——或者你打字够快（QMK 的实时 WPM，见 `host_api.wpm()`）就提前——缸里会掉下
一粒饵，鱼群围过去，等聚够了它就没了。

全部算术是 Q15.16 定点，因为这颗核心没有 FPU。只有 hypot 和 atan2 是自己写的：
`fixed_sqrt` 走 exp/log 太慢也太糙，库里的 atan2 又会丢象限。

> `TYPING` 这项需要固件构建号 ≥ 260731（`FW_WPM_BUILD`），旧固件上取不到 WPM。

## 菜单（Enter）

十个滑条：

| 项 | 含义 |
|---|---|
| SPEED | 巡航速度（px/s） |
| SCHOOL | 鱼的条数 |
| VISION | 视野半径（px）。规则够不到视野外的邻居，所以它是粗调，下面三项是细调 |
| SEPARATE / ALIGN / COHERE | 分离 / 对齐 / 聚合三条 boids 规则的权重 |
| GLASS | 池壁力场强度（巡航速度的百分比） |
| CURRENT | 环流强度 |
| TYPING | 打字加速的响应比例 |
| FEED | 投饵间隔（秒），0 = 关闭 |

## 直接按键

| 键 | 功能 |
|---|---|
| ↑ / ↓ | 游速 |
| → / ← | 加一条 / 减一条鱼 |
| `=` / `-` | 环流强度 |
| Space | 重新投放鱼群 |
| Enter | 打开菜单 |
| Esc | 退出回启动器 |

## 构建与安装

以下命令在 `keyboards/ydkb/athena75_rgb_advanced/` 下执行：

```bash
bash src/app/tools/build_app.sh fish        # -> artifacts/apps/fish.app
host_tool app install artifacts/apps/fish.app

python3 src/app/fish/make_icon.py --zoom 8  # 图标是脚本画的像素图
```

---

通用操作与设置落盘规则见 [`docs/usage.md`](../../../docs/usage.md)；
app 一览见[仓库根 readme](../../../../../../readme.md#预置-app)。
