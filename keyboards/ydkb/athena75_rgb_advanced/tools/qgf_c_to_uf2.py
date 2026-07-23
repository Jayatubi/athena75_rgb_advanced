"""把 QMK Painter 生成的 .qgf.c 里的字节数组抽出来, 封装成 UF2,
烧到外部 flash 的指定地址 (默认 boot 区 0x10400000)。

用途: 通用工具, 把任意 .qgf.c 里的字节数组转成可单独 UF2 上传的文件。
注意: boot 动画不再编译进固件, 常规做法是用 boot_png_to_uf2.py 直接从 PNG 生成
      builds/boot.uf2; 本脚本仅在你手头有现成 .qgf.c 时备用。

QGF 本身与地址无关 (纯图像数据), 固件从 BOOT_QGF_ADDR 就地 XIP 读取。
产物统一归档到 tools/builds/ (见 uf2_common): builds/<base>.uf2 + 时间戳历史。

示例:
  python3 qgf_c_to_uf2.py some.qgf.c                        # -> builds/some.uf2 @ 0x10400000
  python3 qgf_c_to_uf2.py some.qgf.c --base boot --addr 0x10400000
"""
import argparse
import os
import re

import uf2_common

BOOT_ADDR = 0x10400000          # boot 区基址 (固件里的 BOOT_QGF_ADDR)


def parse_qgf_c(path):
    """从 .qgf.c 中抽取第一个大括号数组里的所有 0xNN 字节, 返回原始字节。"""
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        text = f.read()
    m = re.search(r"\{(.*)\}", text, re.DOTALL)   # 数组体 { ... }
    if not m:
        raise SystemExit(f"{path}: 未找到字节数组")
    vals = re.findall(r"0x([0-9A-Fa-f]{1,2})", m.group(1))
    if not vals:
        raise SystemExit(f"{path}: 数组里没有 0xNN 字节")
    data = bytes(int(v, 16) for v in vals)
    if not (len(data) >= 8 and data[5:8] == b"QGF"):
        raise SystemExit(f"{path}: 开头不是合法 QGF (缺少 'QGF' 签名)")
    return data


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=".qgf.c -> UF2 (统一归档到 builds/)")
    ap.add_argument("qgf_c", help="输入的 .qgf.c 文件 (含 gfx_xxx[] 字节数组)")
    ap.add_argument("--addr", default=hex(BOOT_ADDR),
                    help=f"目标 flash 地址 (默认 {hex(BOOT_ADDR)} = boot 区)")
    ap.add_argument("--base", default="boot",
                    help="产物基名 (builds/<base>.uf2, 默认 boot)")
    args = ap.parse_args()

    addr = int(args.addr, 0)
    data = parse_qgf_c(args.qgf_c)
    uf2 = uf2_common.to_uf2(data, addr)
    latest, stamped, _ = uf2_common.archive_bytes(uf2, args.base)
    print(f"qgf={len(data)}B  addr={hex(addr)}  uf2={len(uf2)}B")
    print(f"  latest  -> {latest}")
    print(f"  history -> {os.path.basename(stamped)}")
