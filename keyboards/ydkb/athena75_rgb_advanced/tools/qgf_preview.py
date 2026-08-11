#!/usr/bin/env python3
"""把 QGF 解成 GIF, 用来在电脑上预览开机动画。

播放规则跟固件里的 src/firmware/app/qgf.c 一致: RGB565 大端像素, 整帧或者
只重画一个矩形的 delta 帧, 数据可以是原始的也可以是 RLE 压缩的。所以这里
看到的效果就是键盘上会看到的效果。

    qgf_preview.py ../../../../artifacts/boot/athena.qgf -o athena.gif

输入也可以直接给 painter 生成的 .qgf.c。docs/boot/ 下跟踪的两张预览图就是
这么来的, 命令写在 artifacts/boot/readme.txt。
"""
import argparse
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qgf_from_c

RGB565 = [(((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
          for c in range(1 << 16)]


def rd(b, off, n):
    return int.from_bytes(b[off:off + n], "little")


def rle_decode(data, need):
    """qgf.c 里的那套: 高位置 1 是照抄 n 个像素, 否则是同一个像素重复 n 次。"""
    out, i = bytearray(), 0
    while i < len(data) and len(out) < need * 2:
        c = data[i]
        i += 1
        if c >= 128:
            n = c - 127
            out += data[i:i + n]
            i += n
        else:
            out += bytes([data[i]]) * c
            i += 1
    return bytes(out)


def decode(q):
    """返回 [(Image, 该帧停留的毫秒数)]。delta 帧叠在上一帧的画布上。"""
    w, h, n = (rd(q, i, 2) for i in (17, 19, 21))
    fb = [0] * (w * h)
    frames = []
    for i in range(n):
        off = rd(q, 28 + i * 4, 4)
        flags, comp = q[off + 6], q[off + 7]
        delay = rd(q, off + 9, 2)
        cur = off + 11
        rect = (0, 0, w - 1, h - 1)
        if q[cur] == 0x03:  # 调色板块, 整块跳过
            cur += 5 + rd(q, cur + 2, 3)
        if flags & 0x02:  # delta 帧: 只重画 rect 这一块
            r = cur + 5
            rect = tuple(rd(q, r + k * 2, 2) for k in range(4))
            cur += 5 + rd(q, cur + 2, 3)
        blen = rd(q, cur + 2, 3)
        data = q[cur + 5:cur + 5 + blen]
        rw, rh = rect[2] - rect[0] + 1, rect[3] - rect[1] + 1
        px = rle_decode(data, rw * rh) if comp else data
        for y in range(rh):
            row = y * rw * 2
            for x in range(rw):
                fb[(rect[1] + y) * w + rect[0] + x] = int.from_bytes(px[row + x * 2:row + x * 2 + 2], "big")
        img = Image.new("RGB", (w, h))
        img.putdata([RGB565[c] for c in fb])
        frames.append((img, delay))
    return frames


def main():
    ap = argparse.ArgumentParser(description="QGF -> GIF 预览",
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("input", help=".qgf, 或者 painter 生成的 .qgf.c")
    ap.add_argument("-o", "--out", help="输出 .gif (默认与输入同名)")
    ap.add_argument("--scale", type=int, default=1, help="整数倍放大, 邻近采样 (默认 1)")
    args = ap.parse_args()

    if args.input.endswith(".c"):
        with open(args.input, "r", encoding="utf-8", errors="replace") as f:
            data = qgf_from_c.extract(f.read(), None)[1]
    else:
        with open(args.input, "rb") as f:
            data = f.read()
    total, w, h, n = qgf_from_c.describe(data)

    frames = decode(data[:total])
    if args.scale > 1:
        frames = [(f.resize((w * args.scale, h * args.scale), Image.NEAREST), d) for f, d in frames]

    out = args.out or os.path.splitext(args.input)[0] + ".gif"
    frames[0][0].save(out, save_all=True, append_images=[f for f, _ in frames[1:]],
                      duration=[d for _, d in frames], loop=0, disposal=1)
    print(f">> {out}: {n} 帧, {w}x{h}, 一轮 {sum(d for _, d in frames)} ms")


if __name__ == "__main__":
    main()
