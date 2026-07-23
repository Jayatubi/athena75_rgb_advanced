"""把一张 PNG 做成 boot 动画帧序列 (alpha 淡入 -> 淡出), 打包成 boot 区 UF2。

生成 N 帧 (默认 60 帧 x 16ms ≈ 1s): alpha 从 0 线性升到 1 (淡入),
再从 1 降回 0 (淡出), 每帧在黑底上合成为 RGB565, 组成一个 QGF 动画,
由固件的 qp_animate 播放。产物统一归档到 builds/ (见 uf2_common)。

复用 png_to_qgf 的 frame_to_rgb565 / build_qgf, 复用 uf2_common 的 to_uf2 / 归档。

示例:
  python3 boot_png_to_uf2.py boot.png                 # 60帧/16ms, 压缩, -> builds/boot.uf2
  python3 boot_png_to_uf2.py boot.png --frames 60 --ms 16 --raw
"""
import argparse
import os

from PIL import Image

import png_to_qgf as pq
import uf2_common

BOOT_ADDR = 0x10400000   # 固件里的 BOOT_QGF_ADDR
BOOT_END = 0x10600000    # boot 区结束 = keyframe 区起点 (ANIM_QGF_ADDR)
SIZE = pq.SIZE           # 128


def make_frames(img, n, frame_ms):
    """三角形 alpha 曲线: 第 0 帧 alpha≈0, 中间帧 alpha=1, 末帧 alpha≈0。"""
    base = img.convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)
    src_a = base.getchannel("A")
    peak = (n - 1) / 2.0
    frames = []
    for i in range(n):
        a = 1.0 - abs(i - peak) / peak          # 0 -> 1 -> 0
        layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        im = base.copy()
        im.putalpha(src_a.point(lambda v: int(v * a)))   # 缩放整体不透明度
        layer.alpha_composite(im)
        frames.append((pq.frame_to_rgb565(layer), frame_ms))  # 黑底合成 -> RGB565
    return frames


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="PNG -> boot 淡入淡出动画 UF2")
    ap.add_argument("png", help="输入 PNG (任意尺寸, 会缩放到 128x128)")
    ap.add_argument("--frames", type=int, default=60, help="总帧数 (默认 60)")
    ap.add_argument("--ms", type=int, default=16, help="每帧时长 ms (默认 16)")
    ap.add_argument("--addr", default=hex(BOOT_ADDR),
                    help=f"目标 flash 地址 (默认 {hex(BOOT_ADDR)} = boot 区)")
    ap.add_argument("--base", default="boot", help="产物基名 (builds/<base>.uf2)")
    ap.add_argument("--raw", action="store_true", help="不压缩 (默认 RLE 压缩)")
    args = ap.parse_args()

    img = Image.open(args.png)
    print(f"输入: {args.png}  size={img.size} mode={img.mode}")
    compress = not args.raw
    frames = make_frames(img, args.frames, args.ms)

    qgf = pq.build_qgf(frames, compress)
    slot = BOOT_END - int(args.addr, 0)   # boot 槽容量 (默认 2MB), 防止溢出到 keyframe 区
    if len(qgf) > slot:
        raise SystemExit(f"QGF {len(qgf)}B 超出 boot 槽 ({slot}B)")
    uf2 = uf2_common.to_uf2(qgf, int(args.addr, 0))
    latest, stamped, _ = uf2_common.archive_bytes(uf2, args.base)

    total_s = args.frames * args.ms / 1000
    ratio = f"  压缩率={len(qgf)/(args.frames*SIZE*SIZE*2)*100:.0f}%" if compress else ""
    print(f"frames={args.frames} x {args.ms}ms = {total_s:.2f}s  "
          f"qgf={len(qgf)}B  uf2={len(uf2)}B{ratio}  addr={args.addr}")
    print(f"  latest  -> {latest}")
    print(f"  history -> {os.path.basename(stamped)}")
