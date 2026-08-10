# Athena75 RGB Advanced

YDKB / KBDFans Athena75 RGB（RP2040 + 128×128 GC9107 LCD + Vial）的第三方固件。

完整说明——定位、架构、预置 app、如何写自己的 app——在仓库根目录的
[`readme.md`](../../../readme.md)。本目录下的其他文档：

- [`docs/usage.md`](docs/usage.md) —— 操作与参考手册：按键、菜单、host_tool 子命令、可调宏
- [`src/README.md`](src/README.md) —— 目录职责与构建入口
- [`docs/flash_map.md`](docs/flash_map.md) —— flash 分区与 app 槽布局
- [`docs/ram_map.md`](docs/ram_map.md) —— SRAM 分区与 app RAM 窗口
- [`src/sim/README.md`](src/sim/README.md) —— `athena_sim` 仿真器

编译：

```bash
make ydkb/athena75_rgb_advanced:vial
# 或用固定 Docker 镜像并归档产物的脚本：
bash keyboards/ydkb/athena75_rgb_advanced/tools/build_wsl.sh   # Windows + WSL
bash keyboards/ydkb/athena75_rgb_advanced/tools/build_mac.sh   # macOS
python3 keyboards/ydkb/athena75_rgb_advanced/tools/build.py
```

刷入：`host_tool upload`，或按住 BOOTSEL 上电后把 UF2 拖进出现的 U 盘。
