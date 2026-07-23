"""PNG 真彩帧 -> 补间 -> 直接 QGF/UF2, 全程不经 GIF 的 256 色量化。

复刻 make_montage.py 的补间效果 (上飞出 + 下缓入 + 递减残影),
但每帧保持 RGBA 真彩, 直接压成 RGB565 (65K 色) 写入 QGF,
避免 make_montage.py 里 to_p() 那步 256 色调色盘造成的失真。

输出目标: athena75_rgb 的 keyframe 大动画槽 (0x10600000, 10MB)。
需要 Pillow: python3 -m pip install pillow
"""
import argparse
import glob
import os
import struct

from PIL import Image

import uf2_common

SRC = "."                # PNG 源目录, 可用 --src 覆盖 (默认当前目录)
SIZE = 128

# --- 时序 (与 make_montage.py 对齐, 但补间帧默认抬到 100ms 抗撕裂) ---
HOLD_MS = 800            # 每个表情停留时长
TWEEN_MS = 100           # 过渡帧时长 (原 make_montage 是 80ms; 100ms 起面板才不串帧)
TWEEN = 1                # 每段补间帧数, 94*(1+TWEEN) 必须 <= 255

# --- 补间视觉参数 (与 make_montage.py 一致) ---
BG = (0, 0, 0)           # 不透明背景
TRAVEL = SIZE            # 飞入/飞出位移量
GHOSTS = 4               # 残影层数
GHOST_STEP = 16          # 残影层间距(px)
GHOST_ALPHA = 0.45       # 残影起始不透明度
B_EASE = 1.0             # 飞入缓出指数 (1.0=线性)

# --- QGF/UF2 ---
# flash 分区调整后, keyframe 区从 boot 区 (2MB) 之后开始:
#   0x10400000..0x10600000  boot 区 (2MB)
#   0x10600000..0x11000000  keyframe 区 (10MB)  <- 本脚本输出到这里
SLOT_ADDR = 0x10600000   # keyframe 区基址 (固件里的 ANIM_QGF_ADDR)
SLOT_BYTES = 0x11000000 - 0x10600000  # 10MB 槽容量 (压缩后按字节数限制)
MAX_FRAMES = 319         # 10MB / ~32.8KB 每帧 (未压缩时的帧数上限)
PER_FRAME_HEADER = bytes([0x02, 0xFD, 0x06, 0x00, 0x00, 0x08, 0x00, 0x00,
                          0xFF, 0x00, 0x00, 0x05, 0xFA, 0x00, 0x80, 0x00])


def text_shift_map(paths):
    """算各帧底部白色 "PENTA KILL!!!" 字样质心, 返回把它平移到全序列中位数
    目标位置所需的 (dx, dy) 整数位移 (源图坐标系)。用于 --align-text 对齐字样。"""
    import numpy as np
    imgs = [np.asarray(Image.open(p).convert("RGB")).astype(np.int32) for p in paths]
    H, W = imgs[0].shape[:2]
    y0 = int(H * 0.60)  # 只在下半部找字样, 避开上方角色
    cents = []
    for a in imgs:
        R, G, B = a[..., 0], a[..., 1], a[..., 2]
        mn = np.minimum(np.minimum(R, G), B)
        # 近白/米白、低饱和 (字体是白描边), 排除彩色角色像素
        white = (mn > 165) & (np.abs(R - G) < 40) & (np.abs(G - B) < 50)
        white[:y0, :] = False
        ys, xs = np.nonzero(white)
        cents.append((xs.mean(), ys.mean()) if len(xs) else (W / 2.0, H * 0.75))
    cents = np.array(cents)
    target = np.median(cents, axis=0)
    shifts = np.round(target - cents).astype(int)
    return {p: (int(dx), int(dy)) for p, (dx, dy) in zip(paths, shifts)}


def load_emojis(align_text=False):
    files = sorted(glob.glob(os.path.join(SRC, "Icon_*.png")))
    shift_map = text_shift_map(files) if align_text else {}
    out = []
    for f in files:
        im = Image.open(f).convert("RGBA")
        if align_text:
            dx, dy = shift_map[f]
            # 透明填充平移 (不能填黑, 否则破坏 alpha 使补间叠加失效)
            canvas = Image.new("RGBA", im.size, (0, 0, 0, 0))
            canvas.paste(im, (dx, dy))
            im = canvas
        out.append(im.resize((SIZE, SIZE), Image.LANCZOS))
    return out


def new_canvas():
    return Image.new("RGBA", (SIZE, SIZE), BG + (255,))


def paste_shifted(canvas, emoji, dy, opacity=1.0):
    layer = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    layer.alpha_composite(emoji, (0, int(round(dy))))
    if opacity < 1.0:
        a = layer.getchannel("A").point(lambda v: int(v * opacity))
        layer.putalpha(a)
    canvas.alpha_composite(layer)


def render_hold(emoji):
    c = new_canvas()
    c.alpha_composite(emoji, (0, 0))
    return c


def render_ghost(a, b, p):
    """p in (0,1): a 向上飞出, b 从下缓入, 两者带递减残影 (原效果)。"""
    c = new_canvas()
    a_y = -p * TRAVEL
    b_y = ((1 - p) ** B_EASE) * TRAVEL
    for m in range(GHOSTS, 0, -1):
        alpha = GHOST_ALPHA * (1 - m / (GHOSTS + 1))
        paste_shifted(c, a, a_y + m * GHOST_STEP, alpha)
        paste_shifted(c, b, b_y + m * GHOST_STEP, alpha)
    paste_shifted(c, a, a_y, 1.0)
    paste_shifted(c, b, b_y, 1.0)
    return c


def render_slide(a, b, p):
    """纯滑动: a 上移, b 下入, 无残影 (边缘干净, 撕裂少)。"""
    c = new_canvas()
    paste_shifted(c, a, -p * TRAVEL, 1.0)
    paste_shifted(c, b, ((1 - p) ** B_EASE) * TRAVEL, 1.0)
    return c


def render_dissolve(a, b, p):
    """交叉溶解: a/b 原位不动, 按 p 做 alpha 混合 (无位移, 无移动边缘, 最抗撕裂)。"""
    ca = new_canvas(); ca.alpha_composite(a, (0, 0))
    cb = new_canvas(); cb.alpha_composite(b, (0, 0))
    return Image.blend(ca, cb, p)


TWEEN_FUNCS = {"ghost": render_ghost, "slide": render_slide, "dissolve": render_dissolve}


def frame_to_rgb565(rgba):
    """RGBA -> 在黑底上合成 -> 大端 RGB565 字节 (无调色盘, 65K 色)。"""
    bg = Image.new("RGBA", rgba.size, BG + (255,))
    bg.alpha_composite(rgba)
    rgb = bg.convert("RGB")
    px = rgb.load()
    buf = bytearray(SIZE * SIZE * 2)
    k = 0
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            buf[k] = (v >> 8) & 0xFF
            buf[k + 1] = v & 0xFF
            k += 2
    return bytes(buf)


def rle_encode(data):
    """字节级 RLE, 与固件 qp_drawimage_byte_rle_decoder 完全对应:
      - 重复段: [计数 N(1..127)][值]     -> 值重复 N 次
      - 直出段: [标记 L+127(128..255)][L 个字面字节] -> L 个原样字节 (L 1..128)
    """
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 127:
            run += 1
        if run >= 2:
            out.append(run)                 # repeating count 1..127
            out.append(data[i])
            i += run
        else:
            lit = bytearray()
            while i < n and len(lit) < 128:
                if i + 1 < n and data[i + 1] == data[i]:
                    break                    # 下一处是重复段, 结束直出段
                lit.append(data[i]); i += 1
            out.append(127 + len(lit))       # 128..255
            out += lit
    return bytes(out)


def rle_decode(data, expected_len):
    """镜像固件解码逻辑, 仅用于往返自检 (压缩正确性验证)。"""
    out = bytearray(); i = 0
    while len(out) < expected_len:
        c = data[i]; i += 1
        if c >= 128:
            L = c - 127
            out += data[i:i + L]; i += L
        else:
            v = data[i]; i += 1
            out += bytes([v]) * c
    return bytes(out)


def _frame_block(pixels, delay, compress):
    payload = rle_encode(pixels) if compress else pixels
    if compress:  # 往返自检: 解错一位就会花屏, 必须验证
        assert rle_decode(payload, len(pixels)) == pixels, "RLE round-trip mismatch"
    comp = 0x01 if compress else 0x00
    blk = bytearray()
    # qgf_frame_v1_t (11B): header(5) + format + flags + compression + transp_idx + delay(2)
    blk += bytes([0x02, 0xFD]); blk += (6).to_bytes(3, "little")
    blk += bytes([0x08, 0x00, comp, 0xFF])
    blk += struct.pack("<H", delay & 0xFFFF)
    # qgf_data_v1_t header(5) + payload
    blk += bytes([0x05, 0xFA]); blk += len(payload).to_bytes(3, "little")
    blk += payload
    return bytes(blk)


def build_qgf(frames, compress=False):
    n = len(frames)
    blocks = [_frame_block(px, d, compress) for px, d in frames]
    master_header = n * 4 + 28          # graphics desc(23) + frame-offset header(5) + n*4
    total = master_header + sum(len(bl) for bl in blocks)

    b = bytearray()
    b += bytes([0x00, 0xFF, 0x12, 0x00, 0x00, 0x51, 0x47, 0x46, 0x01])  # graphics descriptor
    b += struct.pack("<I", total)
    b += struct.pack("<I", (~total) & 0xFFFFFFFF)
    b += struct.pack("<H", SIZE); b += struct.pack("<H", SIZE); b += struct.pack("<H", n)
    b += bytes([0x01, 0xFE]); b += (n * 4).to_bytes(3, "little")        # frame-offset descriptor
    off = master_header
    for bl in blocks:
        b += struct.pack("<I", off); off += len(bl)
    for bl in blocks:
        b += bl
    assert len(b) == total
    return bytes(b)


def build(tween_ms, effect, base, tween_frames=TWEEN, align_text=False,
          compress=False, keyframes=False):
    emojis = load_emojis(align_text)
    n = len(emojis)
    if n == 0:
        raise SystemExit("未找到 Icon_*.png")

    if keyframes:
        # 只输出关键帧 (每个表情一帧停留), 补间交给固件 MCU 实时生成
        rgba_frames = [render_hold(e) for e in emojis]
        durations = [HOLD_MS] * n
        tween_fn = "keyframes"
    else:
        tween_fn = TWEEN_FUNCS.get(effect)  # None => cut (无补间帧)
        rgba_frames, durations = [], []
        for i in range(n):
            a = emojis[i]
            b = emojis[(i + 1) % n]
            rgba_frames.append(render_hold(a)); durations.append(HOLD_MS)
            if tween_fn is not None:
                for j in range(1, tween_frames + 1):
                    p = j / (tween_frames + 1)
                    rgba_frames.append(tween_fn(a, b, p)); durations.append(tween_ms)

    if len(rgba_frames) > MAX_FRAMES:
        raise SystemExit(f"帧数 {len(rgba_frames)} > {MAX_FRAMES}")

    frames = [(frame_to_rgb565(f), d) for f, d in zip(rgba_frames, durations)]
    raw_bytes = sum(len(px) for px, _ in frames)
    qgf = build_qgf(frames, compress)
    if len(qgf) > SLOT_BYTES:
        raise SystemExit(f"QGF {len(qgf)}B 超出 keyframe 槽 ({SLOT_BYTES}B)")
    uf2 = uf2_common.to_uf2(qgf, SLOT_ADDR)
    latest, stamped, _ = uf2_common.archive_bytes(uf2, base)

    total_s = sum(durations) / 1000
    ratio = f"  压缩率={len(qgf) / raw_bytes * 100:.0f}%" if compress else ""
    mode = "keyframes(MCU补间)" if keyframes else f"effect={effect} tween={tween_ms}ms"
    print(f"{mode}  frames={len(frames)}  loop={total_s:.1f}s  "
          f"qgf={len(qgf)}B  uf2={len(uf2)}B{ratio}  addr={hex(SLOT_ADDR)}")
    print(f"  latest  -> {latest}")
    print(f"  history -> {os.path.basename(stamped)}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="PNG 真彩直出 QGF/UF2 (big 槽)")
    ap.add_argument("--tween", type=int, default=TWEEN_MS, help="补间帧时长(ms)")
    ap.add_argument("--frames", type=int, default=TWEEN, help="每段补间帧数")
    ap.add_argument("--effect", default="dissolve",
                    choices=["ghost", "slide", "dissolve", "cut"],
                    help="补间效果: ghost(残影) / slide(纯滑动) / dissolve(溶解) / cut(硬切无补间)")
    ap.add_argument("--src", default=SRC, help="PNG 源目录 (含 Icon_*.png), 默认当前目录")
    ap.add_argument("--align-text", action="store_true",
                    help="对齐各帧底部 PENTA KILL 字样 (按白字质心平移, 需要 numpy)")
    ap.add_argument("--compress", action="store_true",
                    help="启用 QGF RLE 压缩 (固件软件解码, 显著缩小体积/加快烧录)")
    ap.add_argument("--keyframes", action="store_true",
                    help="只输出关键帧(每表情一帧), 补间交给固件 MCU 实时生成 "
                         "(默认不压缩, 固件直接 XIP 读取; 加 --compress 才用 RLE)")
    ap.add_argument("--base", default=None,
                    help="产物基名 (统一归档到 builds/<base>.uf2; 默认按模式自动命名)")
    args = ap.parse_args()
    SRC = args.src
    compress = args.compress  # 关键帧模式默认不压缩 (固件从 flash 直接寻址关键帧)
    if args.keyframes:
        suffix = ("_aligned" if args.align_text else "") + ("_rle" if compress else "_raw")
        default_base = f"penta_kill_keyframes{suffix}"
    else:
        suffix = ("_aligned" if args.align_text else "") + ("_rle" if compress else "")
        default_base = f"penta_kill_montage_12m_{args.effect}{args.frames}_t{args.tween}{suffix}"
    base = args.base or default_base
    build(args.tween, args.effect, base, args.frames, args.align_text, compress, args.keyframes)
