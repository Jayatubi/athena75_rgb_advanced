#!/usr/bin/env python3
"""开机动画制作: GIF / 视频 / PNG 序列 -> 128x128 QGF (+ 可选 UF2)。

产物有两种用法, 都在 docs/usage.md 里:

    host_tool boot install boot.qgf        # USB 直传, 键盘上确认
    host_tool upload boot.uf2              # 大文件走 BOOTSEL, 快一个数量级

输入可以是:

    x.gif                 逐帧连同它自己的帧间隔
    x.mp4 / .mov / ...    需要 ffmpeg (缺了会说清楚), 按 --fps 抽帧
    dir/                  按文件名排序的 PNG/JPG 序列, 按 --fps 定时
    x.png                 单张图, 配 --hold 就是一张静态开机图
                          再加 --fade 500 就是淡入 - 停留 - 淡出

开机动画区是 4 MiB。整帧 RLE 大约只够 130~2500 帧(取决于画面), 但 delta 帧只存
变化的矩形, 实拍视频通常能再省一大截。装不下时默认自动降帧率重试, 会把过程打出来。

需要 Pillow: python3 -m pip install pillow  (装了 numpy 会快很多)
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qgf_build
import uf2_common

SIZE = 128
FADE_STEP_MS = 33                       # 淡入淡出的默认步长, 约 30 fps
BOOT_ADDR = 0x10400000                  # 固件里的 BOOT_QGF_ADDR
BOOT_BYTES = 0x400000                   # 开机动画区 4 MiB
VIDEO_EXT = {".mp4", ".mov", ".mkv", ".avi", ".webm", ".m4v", ".gif"}
IMAGE_EXT = {".png", ".jpg", ".jpeg", ".bmp", ".webp"}


def die(msg):
    raise SystemExit(f"error: {msg}")


def fit(img, mode):
    """缩放到 128x128: cover = 填满后裁掉多余, contain = 整幅装进去, 周围补黑。"""
    from PIL import Image
    if mode == "stretch":
        return img.resize((SIZE, SIZE), Image.LANCZOS)
    w, h = img.size
    scale = max(SIZE / w, SIZE / h) if mode == "cover" else min(SIZE / w, SIZE / h)
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    img = img.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
    canvas.paste(img.convert("RGB"), ((SIZE - nw) // 2, (SIZE - nh) // 2))
    return canvas


# ---- 帧来源 ----------------------------------------------------------------
def frames_from_gif(path, fps, fit_mode):
    """GIF 自带每帧时长; 给了 --fps 就按固定间隔重新采样这条时间轴。"""
    from PIL import Image, ImageSequence
    im = Image.open(path)
    raw = []
    for fr in ImageSequence.Iterator(im):
        d = fr.info.get("duration", im.info.get("duration", 100)) or 100
        raw.append((fit(fr.convert("RGBA"), fit_mode), int(d)))
    if not fps:
        return raw
    total = sum(d for _, d in raw)
    step = 1000.0 / fps
    out, t = [], 0.0
    while t < total:
        acc = 0
        pick = raw[-1][0]
        for img, d in raw:
            if t < acc + d:
                pick = img
                break
            acc += d
        out.append((pick, int(round(step))))
        t += step
    return out


def frames_from_video(path, fps, fit_mode, start, duration):
    """ffmpeg 直接吐 128x128 的 rgb24 裸流, 省掉临时文件。"""
    ff = shutil.which("ffmpeg")
    if not ff:
        die("这是视频, 需要 ffmpeg 才能抽帧 (装一个, 或先自己转成 GIF / PNG 序列)")
    if fit_mode == "cover":
        vf = f"fps={fps},scale={SIZE}:{SIZE}:force_original_aspect_ratio=increase,crop={SIZE}:{SIZE}"
    elif fit_mode == "contain":
        vf = (f"fps={fps},scale={SIZE}:{SIZE}:force_original_aspect_ratio=decrease,"
              f"pad={SIZE}:{SIZE}:(ow-iw)/2:(oh-ih)/2:black")
    else:
        vf = f"fps={fps},scale={SIZE}:{SIZE}"
    cmd = [ff, "-v", "error", "-nostdin"]
    if start:
        cmd += ["-ss", str(start)]
    cmd += ["-i", path]
    if duration:
        cmd += ["-t", str(duration)]
    cmd += ["-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]

    from PIL import Image
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    need = SIZE * SIZE * 3
    delay = int(round(1000.0 / fps))
    out = []
    while True:
        buf = p.stdout.read(need)
        if not buf or len(buf) < need:
            break
        out.append((Image.frombytes("RGB", (SIZE, SIZE), buf), delay))
    err = p.stderr.read().decode("utf-8", "replace").strip()
    p.wait()
    if not out:
        die(f"ffmpeg 没有解出任何一帧{': ' + err if err else ''}")
    return out


def frames_from_images(paths, fps, fit_mode):
    from PIL import Image
    delay = int(round(1000.0 / fps))
    return [(fit(Image.open(p), fit_mode), delay) for p in paths]


def collect(args):
    """按输入类型取帧, 统一成 [(PIL 图, 每帧毫秒)]。"""
    src = args.input
    if os.path.isdir(src):
        paths = sorted(os.path.join(src, f) for f in os.listdir(src)
                       if os.path.splitext(f)[1].lower() in IMAGE_EXT)
        if not paths:
            die(f"{src} 里没有图片")
        return frames_from_images(paths, args.fps or 20, args.fit)
    ext = os.path.splitext(src)[1].lower()
    if ext == ".gif":
        return frames_from_gif(src, args.fps, args.fit)
    if ext in VIDEO_EXT:
        return frames_from_video(src, args.fps or 20, args.fit, args.start, args.duration)
    if ext in IMAGE_EXT:
        from PIL import Image
        return [(fit(Image.open(src), args.fit), args.hold)]
    die(f"不认识的输入类型: {src}")


def fade(frames, ms_in, ms_out, step):
    """头尾各接一段与黑色的混合帧: 从全黑淡入, 再淡回全黑。

    对单张 PNG 就是"淡入 - 停留 - 淡出"的开机图; 对视频/GIF 则是给它包一层开合。
    末帧收在全黑上, 面板接下来切到 launcher 时不会有一下跳变。
    """
    from PIL import Image
    black = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))

    def ramp(img, ms, rising):
        n = max(1, int(round(ms / step)))
        out = []
        for i in range(n):
            a = i / n if rising else 1.0 - (i + 1) / n
            out.append((Image.blend(black, img.convert("RGB"), a), step))
        return out

    head = ramp(frames[0][0], ms_in, True) if ms_in > 0 else []
    tail = ramp(frames[-1][0], ms_out, False) if ms_out > 0 else []
    return head + frames + tail


def trim(frames, start, duration):
    """GIF / PNG 序列的裁剪 (视频的裁剪交给 ffmpeg 自己做, 更快)。"""
    if not start and not duration:
        return frames
    out, t = [], 0.0
    for img, d in frames:
        end = t + d / 1000.0
        if end > start and (not duration or t < start + duration):
            out.append((img, d))
        t = end
    if not out:
        die("裁剪之后一帧都不剩")
    return out


def build(frames, args, verbose):
    """量化 + 编码, 返回 (qgf 字节, 统计)。"""
    px = [(qgf_build.to_rgb565(img), d) for img, d in frames]
    kinds = {}

    def note(i, kind, n):
        kinds[kind] = kinds.get(kind, 0) + 1
        if verbose:
            print(f"   frame {i:4d}  {kind:10s} {n:7d} B")

    qgf = qgf_build.build_qgf(px, SIZE, SIZE, delta=not args.no_delta,
                              rle=not args.no_rle, on_frame=note)
    return qgf, kinds


def main():
    ap = argparse.ArgumentParser(
        description="GIF / 视频 / PNG 序列 -> 开机动画 QGF",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("input", help="x.gif / x.mp4 / 一个目录 / 单张 PNG")
    ap.add_argument("-o", "--out", help="输出 QGF (默认与输入同名的 .qgf)")
    ap.add_argument("--uf2", nargs="?", const="", metavar="PATH",
                    help="同时写一个烧到 boot 区的 UF2 (不给路径就与 QGF 同名)")
    ap.add_argument("--fps", type=float, default=0,
                    help="重采样帧率; GIF 默认沿用自带时序, 视频/序列默认 20")
    ap.add_argument("--hold", type=int, default=1500,
                    help="单张图停留的毫秒数 (默认 1500)")
    ap.add_argument("--fade", type=int, default=0, metavar="MS",
                    help="头尾各加这么多毫秒的淡入淡出 (等于同时给 --fade-in/--fade-out)")
    ap.add_argument("--fade-in", type=int, default=-1, metavar="MS", help="淡入毫秒数")
    ap.add_argument("--fade-out", type=int, default=-1, metavar="MS", help="淡出毫秒数")
    ap.add_argument("--fit", choices=["cover", "contain", "stretch"], default="cover",
                    help="cover=填满裁边(默认) / contain=整幅补黑边 / stretch=拉伸")
    ap.add_argument("--start", type=float, default=0, help="从第几秒开始")
    ap.add_argument("--duration", type=float, default=0, help="取多少秒")
    ap.add_argument("--max-bytes", type=int, default=BOOT_BYTES,
                    help=f"体积上限 (默认 {BOOT_BYTES} = 整个 boot 区)")
    ap.add_argument("--no-fit-budget", action="store_true",
                    help="超预算就直接报错, 不自动降帧率")
    ap.add_argument("--no-delta", action="store_true", help="每帧都存整帧")
    ap.add_argument("--no-rle", action="store_true", help="不做 RLE 压缩")
    ap.add_argument("-v", "--verbose", action="store_true", help="逐帧打印编码结果")
    args = ap.parse_args()

    try:
        import PIL  # noqa: F401
    except ImportError:
        die("需要 Pillow: python3 -m pip install pillow")

    ext = os.path.splitext(args.input)[1].lower()
    by_ffmpeg = not os.path.isdir(args.input) and ext in VIDEO_EXT and ext != ".gif"
    frames = collect(args)
    if not by_ffmpeg:                       # 视频的裁剪已经由 ffmpeg 做过了
        frames = trim(frames, args.start, args.duration)

    ms_in = args.fade if args.fade_in < 0 else args.fade_in
    ms_out = args.fade if args.fade_out < 0 else args.fade_out
    if ms_in > 0 or ms_out > 0:
        step = int(round(1000.0 / args.fps)) if args.fps else FADE_STEP_MS
        frames = fade(frames, ms_in, ms_out, step)
        print(f">> 淡入 {ms_in} ms / 淡出 {ms_out} ms, 每 {step} ms 一帧")

    total_ms = sum(d for _, d in frames)
    print(f">> {len(frames)} 帧, {total_ms / 1000:.2f} s, {SIZE}x{SIZE} ({args.fit})")

    qgf, kinds = build(frames, args, args.verbose)
    # 装不下就抽帧: 每一轮丢掉一半, 并把时长补回留下的帧, 播放速度不变。
    while len(qgf) > args.max_bytes and not args.no_fit_budget and len(frames) > 1:
        print(f"!! {len(qgf)} B 超出 {args.max_bytes} B, 帧率减半后重试")
        merged = []
        for i in range(0, len(frames) - 1, 2):
            merged.append((frames[i][0], frames[i][1] + frames[i + 1][1]))
        if len(frames) % 2:
            merged.append(frames[-1])
        frames = merged
        qgf, kinds = build(frames, args, args.verbose)
    if len(qgf) > args.max_bytes:
        die(f"{len(qgf)} B 装不进 {args.max_bytes} B "
            f"(试试 --fps 更低 / --duration 更短)")

    out = args.out or os.path.splitext(args.input)[0].rstrip(os.sep) + ".qgf"
    with open(out, "wb") as f:
        f.write(qgf)

    raw = len(frames) * SIZE * SIZE * 2
    print(f">> {out}: {len(qgf)} B ({len(qgf) / raw * 100:.1f}% of raw, "
          f"{len(qgf) * 100 // BOOT_BYTES}% of the boot region)")
    print(">> " + ", ".join(f"{k} x{v}" for k, v in sorted(kinds.items())))
    print(f">> {len(frames)} 帧, {sum(d for _, d in frames) / 1000:.2f} s")

    if args.uf2 is not None:
        path = args.uf2 or os.path.splitext(out)[0] + ".uf2"
        with open(path, "wb") as f:
            f.write(uf2_common.to_uf2(qgf, BOOT_ADDR))
        print(f">> {path}: UF2 -> 0x{BOOT_ADDR:08X}")


if __name__ == "__main__":
    main()
