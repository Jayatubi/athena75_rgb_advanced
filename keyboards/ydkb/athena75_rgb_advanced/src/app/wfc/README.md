# WFC

<img src="../../../docs/apps/wfc.gif" width="256" alt="WFC">

波函数坍缩（mxgmn 的 simple tiled model），8×8 格 × 16 px 铺满 128×128，四套主题：
**Circuit / Pipes / Dungeon / Island**。

有意思的地方在于它正面处理了这类算法的天花板。邻接规则只看相邻一格，权重只看一张 tile
——两者都是**局部**的，光靠它们只能得到处处合法、整体无意义的图案：围不住任何东西的墙，
无缘无故绕圈或断头的走线。调权重也救不回来，因为权重是单个格子的边缘分布，压根没地方
安放"房间"这个概念。

所以比一张 tile 大的结构必须**在坍缩开始之前**就定好。每次重开先画一张**方案**：

- **Dungeon** —— 在角点格阵上切出若干房间，房间之间隔一行岩石。这个厚度不是随便定的：
  一条墙线正好是美术切出来的 6 px 隔断，而横墙两侧的那对格子正好是门的两半能拼上的
  形状。所以横墙上开门，竖墙上留过道（且不贴到两端，否则那面墙看着像根断桩）。最大的
  厅里放篝火。
- **Island** —— 中心一两团圆形陆地，格阵最外一圈恒定为海，海岸线才会自己闭合而不是碎成
  群岛。木屋放在陆水交界，树林放在内陆。
- **Circuit / Pipes** —— 若干条网络，每条是几个端子之间的直角走线。电路为此专门加了
  单端口 tile：没有地方可以收尾的话，一条走线就只能绕圈或者跑出面板。

方案是用**求解器自己的语言**写的——角点掩码或出口掩码——所以它只是在加权抽签里替某一张
tile 加注，永远不会和约束冲突。邻接已经排除掉的东西，方案说了也不算，票数还是归赔率。

`PLAN` 控制方案在有意见的地方能占多少票：OFF（退化成纯赔率，就是没有方案的年代）→
HINT → SOME → FIRM → EXACT（照着方案渲染，也是想看清方案到底画了什么时该用的档位）。

tile 美术是 `make_tiles.py` 直接画出来的 16×16 像素画，每次构建重新生成，所以它和求解器
的规则不可能走散。为什么是画而不是从插画缩放、接缝规则是怎么回事、权重怎么调——
都在 [`tiles/README.md`](tiles/README.md)。

## 菜单（Enter）

| 项 | 取值 |
|---|---|
| SPEED | TURBO / FAST / MED / SLOW / V.SLOW |
| PLAN | OFF / HINT / SOME / FIRM / EXACT |

未坍缩的格子按剩余候选 tile 的平均色填充，所以能看出一片区域"还没想好"。

## 直接按键

| 键 | 功能 |
|---|---|
| ← / → | 方案强度 |
| ↑ / ↓ 或 `-` / `=` | 坍缩速度 |
| Space | 换主题 |
| Enter | 打开菜单 |
| Esc | 退出回启动器 |

## 构建与验证

以下命令在仓库根目录下执行（`KB=keyboards/ydkb/athena75_rgb_advanced`）：

```bash
bash $KB/src/app/tools/build_app.sh wfc          # -> artifacts/apps/wfc.app（顺带重画 tile）
host_tool app install artifacts/apps/wfc.app

python3 $KB/src/app/wfc/make_tiles.py            # 每套主题的接缝检查
bash $KB/src/app/tools/test_wfc.sh 300 400       # 五档 PLAN 都能收敛，并报告与方案的吻合度
bash $KB/src/app/tools/preview_wfc.sh 12 $KB/build/grids 4   # 用真实 C 求解器离线画整盘，两秒出图
```

---

通用操作与设置落盘规则见 [`docs/usage.md`](../../../docs/usage.md)；
app 一览见[仓库根 readme](../../../../../../readme.md#预置-app)。
