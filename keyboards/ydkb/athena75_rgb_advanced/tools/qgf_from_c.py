#!/usr/bin/env python3
"""把 QMK painter_convert_graphics 生成的 .qgf.c 还原成 .qgf 文件。

原厂固件是把开机动画编译进代码的: gfx/boot.qgf.c 里一个
`const uint8_t gfx_boot[N] = { 0x00, 0xFF, ... };` 数组就是完整的 QGF。本工具
把那串字节抠出来写成文件, 于是它可以直接送进

    host_tool boot install boot.qgf

artifacts/boot/ 下跟踪的两个原厂动画就是这么来的, 命令写在 artifacts/boot/readme.txt。

    qgf_from_c.py gfx/boot2.qgf.c -o artifacts/boot/kbdfans.qgf
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import uf2_common

BOOT_ADDR = 0x10400000
ARRAY_RE = re.compile(r"const\s+uint8_t\s+(\w+)\s*\[\s*\d*\s*\]\s*=\s*\{", re.S)


def die(msg):
    raise SystemExit(f"error: {msg}")


def extract(text, symbol):
    """取出 (符号名, 字节)。不指定符号时用文件里第一个 uint8_t 数组。"""
    for m in ARRAY_RE.finditer(text):
        if symbol and m.group(1) != symbol:
            continue
        end = text.index("}", m.end())
        body = text[m.end():end]
        return m.group(1), bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body))
    die(f"没找到 {symbol or 'uint8_t'} 数组; 这是 painter_convert_graphics 生成的 .c 吗?")


def describe(q):
    """按 src/firmware/app/qgf.c 的入口检查走一遍, 播放器会拒绝的这里也拒绝。"""
    if len(q) < 28 or q[0] != 0x00 or q[1] != 0xFF or q[5:8] != b"QGF" or q[8] != 0x01:
        die("不是 QGF 图像")
    total = int.from_bytes(q[9:13], "little")
    neg = int.from_bytes(q[13:17], "little")
    if total ^ 0xFFFFFFFF != neg:
        die("QGF 头里的长度和它的取反对不上")
    if total > len(q):
        die(f"头里写着 {total} 字节, 数组里只有 {len(q)} 字节")
    w, h, n = (int.from_bytes(q[i:i + 2], "little") for i in (17, 19, 21))
    return total, w, h, n


def main():
    ap = argparse.ArgumentParser(description="painter 生成的 .qgf.c -> .qgf",
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("input", help="gfx/xxx.qgf.c")
    ap.add_argument("-o", "--out", help="输出 .qgf (默认与输入同目录同名)")
    ap.add_argument("--symbol", help="数组名, 比如 gfx_boot2 (默认取第一个)")
    ap.add_argument("--uf2", nargs="?", const="", metavar="PATH",
                    help="同时写一个烧到 boot 区的 UF2 (不给路径就与 QGF 同名)")
    args = ap.parse_args()

    with open(args.input, "r", encoding="utf-8", errors="replace") as f:
        name, data = extract(f.read(), args.symbol)
    total, w, h, n = describe(data)
    if (w, h) != (128, 128):
        print(f"!! {w}x{h}, 面板是 128x128 —— 这张放上去播放器会跳过")

    out = args.out or os.path.splitext(os.path.splitext(args.input)[0])[0] + ".qgf"
    with open(out, "wb") as f:
        f.write(data[:total])
    print(f">> {name}: {n} 帧, {w}x{h}, {total} B -> {out}")

    if args.uf2 is not None:
        path = args.uf2 or os.path.splitext(out)[0] + ".uf2"
        with open(path, "wb") as f:
            f.write(uf2_common.to_uf2(data[:total], BOOT_ADDR))
        print(f">> {path}: UF2 -> 0x{BOOT_ADDR:08X}")


if __name__ == "__main__":
    main()
