"""QGF 编码器 (Quantum Painter 的图像容器), 固件侧解析在 src/firmware/app/qgf.c。

一个 QGF 是: 图形描述符 -> 帧偏移表 -> 每帧一个块组。每帧可以是

  整帧          128x128 全部像素
  delta 帧      只重画一个矩形, 其余沿用上一帧 (flags bit1 + 0x04 块)

两者都能再套一层字节级 RLE。编码时对每帧把这几种组合都算一遍, 取最小的那个,
所以画面越静, 出来的文件越小 —— 这正是让实拍视频塞进 4 MiB 开机动画区的关键。

像素一律是大端 RGB565 (面板/固件 fbShow 的字节序), 不做调色盘, 不量化到 256 色。
RGB888→RGB565 时可选用有序 Bayer / Floyd–Steinberg 抖动, 减轻色带。
"""
import struct

try:
    import numpy as _np           # 有就用: 逐像素的活儿在纯 Python 里太慢
except ImportError:
    _np = None

RGB565_BPP = 8   # qp_image_format_t: RGB565_16BPP
FLAG_DELTA = 0x02

# 标准 Bayer 8×8 (0..63)。截断到 5/6/5 位前按通道量化步长做有序抖动。
_BAYER8 = (
    ( 0, 48, 12, 60,  3, 51, 15, 63),
    (32, 16, 44, 28, 35, 19, 47, 31),
    ( 8, 56,  4, 52, 11, 59,  7, 55),
    (40, 24, 36, 20, 43, 27, 39, 23),
    ( 2, 50, 14, 62,  1, 49, 13, 61),
    (34, 18, 46, 30, 33, 17, 45, 29),
    (10, 58,  6, 54,  9, 57,  5, 53),
    (42, 26, 38, 22, 41, 25, 37, 21),
)


def _as_rgb(img):
    """PIL 任意模式 -> RGB (RGBA 先在黑底上合成)。"""
    if img.mode == "RGB":
        return img
    from PIL import Image
    bg = Image.new("RGBA", img.size, (0, 0, 0, 255))
    bg.alpha_composite(img.convert("RGBA"))
    return bg.convert("RGB")


def _bayer_threshold(x, y):
    """有序抖动阈值, 居中到约 [-0.5, 0.5)。"""
    return (_BAYER8[y & 7][x & 7] + 0.5) / 64.0 - 0.5


def _to_rgb565_none(img):
    w, h = img.size
    if _np is not None:
        a = _np.asarray(img, dtype=_np.uint16)
        v = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
        return v.astype(">u2").tobytes()
    px = img.load()
    buf = bytearray(w * h * 2)
    k = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            buf[k] = (v >> 8) & 0xFF
            buf[k + 1] = v & 0xFF
            k += 2
    return bytes(buf)


def _to_rgb565_bayer(img):
    """有序 Bayer 8×8: 截断前按 R/G/B 量化步长加阈值, 动画友好、无误差扩散闪烁。"""
    w, h = img.size
    if _np is not None:
        a = _np.asarray(img, dtype=_np.float32)
        yy, xx = _np.indices((h, w))
        thr = (_np.asarray(_BAYER8, dtype=_np.float32)[yy & 7, xx & 7] + 0.5) / 64.0 - 0.5
        r = _np.clip(a[..., 0] + thr * 8.0, 0, 255)
        g = _np.clip(a[..., 1] + thr * 4.0, 0, 255)
        b = _np.clip(a[..., 2] + thr * 8.0, 0, 255)
        v = ((r.astype(_np.uint16) >> 3) << 11) | \
            ((g.astype(_np.uint16) >> 2) << 5) | \
            (b.astype(_np.uint16) >> 3)
        return v.astype(">u2").tobytes()
    px = img.load()
    buf = bytearray(w * h * 2)
    k = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            t = _bayer_threshold(x, y)
            r = min(255, max(0, int(r + t * 8.0)))
            g = min(255, max(0, int(g + t * 4.0)))
            b = min(255, max(0, int(b + t * 8.0)))
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            buf[k] = (v >> 8) & 0xFF
            buf[k + 1] = v & 0xFF
            k += 2
    return bytes(buf)


def _to_rgb565_fs(img):
    """Floyd–Steinberg 误差扩散。静止图更匀, 动画里可能闪, 开机动画一般用 bayer。"""
    w, h = img.size
    if _np is not None:
        a = _np.asarray(img, dtype=_np.float32).copy()
        out = _np.empty((h, w), dtype=_np.uint16)
        for y in range(h):
            for x in range(w):
                old = a[y, x].copy()
                r5 = int(min(31, max(0, round(old[0] / 8.0))))
                g6 = int(min(63, max(0, round(old[1] / 4.0))))
                b5 = int(min(31, max(0, round(old[2] / 8.0))))
                # 用通道中点还原, 误差更接近真显示值
                new = _np.array((r5 * 255.0 / 31.0, g6 * 255.0 / 63.0, b5 * 255.0 / 31.0),
                                dtype=_np.float32)
                out[y, x] = (r5 << 11) | (g6 << 5) | b5
                err = old - new
                if x + 1 < w:
                    a[y, x + 1] += err * (7.0 / 16.0)
                if y + 1 < h:
                    if x > 0:
                        a[y + 1, x - 1] += err * (3.0 / 16.0)
                    a[y + 1, x] += err * (5.0 / 16.0)
                    if x + 1 < w:
                        a[y + 1, x + 1] += err * (1.0 / 16.0)
        return out.astype(">u2").tobytes()
    px = img.load()
    # 纯 Python: 工作缓冲
    buf_r = [[0.0] * w for _ in range(h)]
    buf_g = [[0.0] * w for _ in range(h)]
    buf_b = [[0.0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            buf_r[y][x] = float(r)
            buf_g[y][x] = float(g)
            buf_b[y][x] = float(b)
    out = bytearray(w * h * 2)
    k = 0
    for y in range(h):
        for x in range(w):
            r = min(255.0, max(0.0, buf_r[y][x]))
            g = min(255.0, max(0.0, buf_g[y][x]))
            b = min(255.0, max(0.0, buf_b[y][x]))
            r5 = min(31, max(0, int(round(r / 8.0))))
            g6 = min(63, max(0, int(round(g / 4.0))))
            b5 = min(31, max(0, int(round(b / 8.0))))
            nr, ng, nb = r5 * 255.0 / 31.0, g6 * 255.0 / 63.0, b5 * 255.0 / 31.0
            er, eg, eb = r - nr, g - ng, b - nb
            v = (r5 << 11) | (g6 << 5) | b5
            out[k] = (v >> 8) & 0xFF
            out[k + 1] = v & 0xFF
            k += 2

            def add(yy, xx, fr):
                if 0 <= yy < h and 0 <= xx < w:
                    buf_r[yy][xx] += er * fr
                    buf_g[yy][xx] += eg * fr
                    buf_b[yy][xx] += eb * fr

            add(y, x + 1, 7.0 / 16.0)
            add(y + 1, x - 1, 3.0 / 16.0)
            add(y + 1, x, 5.0 / 16.0)
            add(y + 1, x + 1, 1.0 / 16.0)
    return bytes(out)


def to_rgb565(img, dither="none"):
    """PIL RGB/RGBA 图 -> 大端 RGB565 字节。

    dither:
      none   直接截断 (旧行为)
      bayer  有序 Bayer 8×8 (动画默认推荐)
      fs     Floyd–Steinberg 误差扩散
    """
    img = _as_rgb(img)
    mode = (dither or "none").lower()
    if mode in ("none", "off", "0"):
        return _to_rgb565_none(img)
    if mode in ("bayer", "ordered", "bayer8"):
        return _to_rgb565_bayer(img)
    if mode in ("fs", "floyd", "floyd-steinberg", "floyd_steinberg"):
        return _to_rgb565_fs(img)
    raise ValueError(f"unknown dither mode: {dither!r} (use none|bayer|fs)")


def rle_encode(data):
    """字节级 RLE, 与固件 qgf.c 的 rle_into / QP 的 qp_drawimage_byte_rle_decoder 对应:
      - 重复段: [计数 N(1..127)][值]                 -> 值重复 N 次
      - 直出段: [标记 L+127(128..255)][L 个字面字节] -> L 个原样字节 (L 1..128)
    """
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 127:
            run += 1
        if run >= 2:
            out.append(run)
            out.append(data[i])
            i += run
        else:
            lit = bytearray()
            while i < n and len(lit) < 128:
                if i + 1 < n and data[i + 1] == data[i]:
                    break            # 下一处是重复段, 结束直出段
                lit.append(data[i]); i += 1
            out.append(127 + len(lit))
            out += lit
    return bytes(out)


def rle_decode(data, expected_len):
    """镜像固件解码逻辑, 仅用于往返自检。"""
    out = bytearray(); i = 0
    while len(out) < expected_len and i < len(data):
        c = data[i]; i += 1
        if c >= 128:
            L = c - 127
            out += data[i:i + L]; i += L
        else:
            v = data[i]; i += 1
            out += bytes([v]) * c
    return bytes(out)


def diff_rect(prev, cur, w, h):
    """两帧 RGB565 之间发生变化的最小包围盒 (l, t, r, b), 完全相同则返回 None。"""
    stride = w * 2
    if _np is not None:
        a = _np.frombuffer(prev, dtype=">u2").reshape(h, w)
        b = _np.frombuffer(cur, dtype=">u2").reshape(h, w)
        d = a != b
        rows = _np.flatnonzero(d.any(axis=1))
        if rows.size == 0:
            return None
        cols = _np.flatnonzero(d.any(axis=0))
        return int(cols[0]), int(rows[0]), int(cols[-1]), int(rows[-1])
    top = None
    bottom = 0
    for y in range(h):
        row = slice(y * stride, (y + 1) * stride)
        if prev[row] != cur[row]:
            if top is None:
                top = y
            bottom = y
    if top is None:
        return None
    # 行确定之后再收窄左右边界, 只在变化的行里找。
    left, right = w, -1
    for y in range(top, bottom + 1):
        base = y * stride
        for x in range(w):
            if left <= x:
                break
            if prev[base + x * 2:base + x * 2 + 2] != cur[base + x * 2:base + x * 2 + 2]:
                left = x
                break
        for x in range(w - 1, right, -1):
            if prev[base + x * 2:base + x * 2 + 2] != cur[base + x * 2:base + x * 2 + 2]:
                right = x
                break
    return left, top, right, bottom


def crop_rect(px, w, rect):
    """从整帧像素里取出矩形区域, 按矩形内的行优先顺序。"""
    l, t, r, b = rect
    stride = w * 2
    out = bytearray()
    for y in range(t, b + 1):
        out += px[y * stride + l * 2:y * stride + (r + 1) * 2]
    return bytes(out)


def _block(type_id, body):
    return bytes([type_id, (~type_id) & 0xFF]) + len(body).to_bytes(3, "little") + body


def _frame_block(payload, delay, compressed, rect):
    flags = FLAG_DELTA if rect else 0
    desc = bytes([RGB565_BPP, flags, 0x01 if compressed else 0x00, 0xFF]) + \
        struct.pack("<H", delay & 0xFFFF)
    blk = _block(0x02, desc)
    if rect:
        blk += _block(0x04, struct.pack("<HHHH", *rect))
    blk += _block(0x05, payload)
    return blk


def encode_frame(px, prev, w, h, delta=True, rle=True):
    """把一帧编成最小的那种表示, 返回 (block_bytes, 说明用的 kind)。

    候选: 整帧 raw / 整帧 RLE / delta raw / delta RLE。delta 是相对**上一帧解码结果**
    算的, 而固件正是把上一帧留在 fbShow 里再画增量, 所以两边看到的是同一张底图。
    """
    cands = [(px, None)]
    if delta and prev is not None:
        rect = diff_rect(prev, px, w, h)
        if rect is None:
            rect = (0, 0, 0, 0)              # 完全没变: 用 1x1 的最小增量占位
        cands.append((crop_rect(px, w, rect), rect))

    best = None
    for payload, rect in cands:
        for compressed in ((False, True) if rle else (False,)):
            body = rle_encode(payload) if compressed else payload
            if compressed and rle_decode(body, len(payload)) != payload:
                continue                      # 往返自检: 解错一位就会花屏
            cand = (len(body), body, compressed, rect)
            if best is None or cand[0] < best[0]:
                best = cand
    _, body, compressed, rect = best
    kind = ("delta" if rect else "full") + ("+rle" if compressed else "")
    return kind, body, compressed, rect


def build_qgf(frames, w, h, delta=True, rle=True, on_frame=None):
    """frames = [(rgb565_bytes, delay_ms), ...] -> QGF 字节。

    on_frame(i, kind, nbytes) 可用来观察每帧最后选了哪种编码。
    """
    blocks = []
    prev = None
    for i, (px, delay) in enumerate(frames):
        kind, body, compressed, rect = encode_frame(px, prev, w, h, delta, rle)
        blocks.append(_frame_block(body, delay, compressed, rect))
        if on_frame:
            on_frame(i, kind, len(blocks[-1]))
        prev = px
    n = len(blocks)
    master = 28 + n * 4                       # 图形描述符(23) + 偏移表头(5) + n*4
    total = master + sum(len(b) for b in blocks)

    out = bytearray()
    out += bytes([0x00, 0xFF]) + (18).to_bytes(3, "little")
    out += b"QGF" + bytes([0x01])
    out += struct.pack("<I", total)
    out += struct.pack("<I", (~total) & 0xFFFFFFFF)
    out += struct.pack("<HHH", w, h, n)
    out += bytes([0x01, 0xFE]) + (n * 4).to_bytes(3, "little")
    off = master
    for b in blocks:
        out += struct.pack("<I", off)
        off += len(b)
    for b in blocks:
        out += b
    assert len(out) == total
    return bytes(out)
