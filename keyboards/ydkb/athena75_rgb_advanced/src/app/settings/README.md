# SETTINGS

<img src="../../../docs/apps/settings.gif" width="256" alt="SETTINGS">

系统设置面板：灯效与配色、熄屏时间、已装 app 的管理与存储占用、屏幕校准、重启与固件信息。

它本身也只是一个普通的 slot app——不装它，固件自带的根菜单照样能用。把设置界面做成
app 是有意为之：整套系统设置能完全用 SDK 写出来，本身就是这套接口够不够用的证明。
它用的是 `host_api` 里公开给所有 app 的同一批调用（`rgb_get/set`、`caps_color_*`、
`sleep_timeout_*`、`slot_query`、`app_area_erase`……），没有任何私货。

菜单里的 **APP → INSTALLED** 和 **LCD TEST** 是固件自己拥有的屏幕：app 只是在内容树里
放一个指向 `APP_MENU_CHILD_*` 的节点，固件接管绘制。**STORAGE** 则是这个 app 自己画的，
按 slot 逐格显示 32 个槽位的占用与图标。

## 菜单（Enter）

```
SETTINGS
├── RGB          （标题上的勾选框控制 SWITCH 键位的灯，rgb_matrix 总开关独立）
│   └── EFFECT / BRIGHT / HUE / SAT / SPEED / CAPS
├── SLEEP        1 / 5 / 10 / 15 分钟 / NEVER（LCD 与 RGB 共用）
├── APP
│   ├── INSTALLED   固件的已装 app 列表（可查看详情、卸载）
│   └── STORAGE     32 个 slot 的占用总览
├── LCD TEST     固件的屏幕校准页
├── REBOOT       NORMAL / BOOTSEL
└── SYSTEM       固件构建号与 ABI 版本
```

RGB、CapsLock 颜色、SLEEP 属于 QMK 自己的 EEPROM，走 eeconfig，不受 app 存档那套
「退出时统一落盘」的约束。

## 构建与安装

以下命令在 `keyboards/ydkb/athena75_rgb_advanced/` 下执行：

```bash
bash src/app/tools/build_app.sh settings          # -> artifacts/apps/settings.app
host_tool app install artifacts/apps/settings.app
```

---

通用操作与设置落盘规则见 [`docs/usage.md`](../../../docs/usage.md)；
app 一览见[仓库根 readme](../../../../../../readme.md#预置-app)。
