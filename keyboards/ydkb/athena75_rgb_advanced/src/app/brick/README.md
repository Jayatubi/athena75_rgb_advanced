# BRICK

<img src="../../../docs/apps/brick.gif" width="256" alt="BRICK">

自己打给自己看的打砖块：7×7 一堵墙，AI 操作挡板，掉胶囊，清完换关。

屏保里的 AI 通常只做一件事——把挡板挪到球下面。那样球会一直被顶回正中附近，最后剩下
角落里几块砖谁也够不着，画面就僵在那儿了。这里的挡板**是瞄着砖打的**：先预测球会落在
哪（连侧壁反弹一起算），再挑一块目标砖，然后把挡板往旁边挪一点，让接触点的偏移正好
把球送过去。挡板不是接球的，是球拍。

几个能看出来的细节：

- **残局换打法**。剩不到 8 块砖时改用**贴边反弹**：把侧壁展开来算，那些超出挡板最大
  偏转角的砖就变成一次或两次撞墙的折线球，而不是一个打不到的方向。
- **金砖是打不碎的**，还会挡住正上方的砖。所以目标砖底下压着金砖时，AI 会绕到侧面去
  打它，免得球先啃到盾。
- **顺路捡胶囊**。只有算出来"捡完还来得及回到球下面"时才会去绕这一趟。
- 球快贴到底线时（差 28 px 以内）一切花活作废，直接钻到球正下方保命。

胶囊共七种：EXPAND / SHRINK / MULTI / SLOW / FAST / CATCH / FIRE。大约每两块砖掉一个——
128×128 的屏太小，掉少了根本看不见。

## 菜单（Enter）

| 项 | 取值 |
|---|---|
| SPEED | FAST / MED / SLOW / V.SLOW |

## 直接按键

| 键 | 功能 |
|---|---|
| ↑ / ↓ 或 `-` / `=` | 速度 |
| Space | 重开一局 |
| Enter | 打开菜单 |
| Esc | 退出回启动器 |

## 构建与测试

以下命令在 `keyboards/ydkb/athena75_rgb_advanced/` 下执行：

```bash
bash src/app/tools/build_app.sh brick        # -> artifacts/apps/brick.app
host_tool app install artifacts/apps/brick.app

bash src/app/tools/test_brick.sh             # 离线重放长局，卡死就报错（球冻住 / 飞出面板 / 一局不再推进）
python3 src/app/brick/make_icon.py           # 图标是脚本画的像素图（--zoom 8 另存放大预览）
```

---

通用操作与设置落盘规则见 [`docs/usage.md`](../../../docs/usage.md)；
app 一览见[仓库根 readme](../../../../../../readme.md#预置-app)。
