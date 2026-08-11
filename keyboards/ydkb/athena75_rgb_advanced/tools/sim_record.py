#!/usr/bin/env python3
"""Record a slot app running in athena_sim as an animated GIF.

    python3 tools/sim_record.py fish --seconds 10 --frame-ms 16
    python3 tools/sim_record.py fish --save-from-device 0x10C40000

Boots the emulator with a blank flash, installs <app>.app, optionally drops a
save sector into the app's slot (a file, or one pulled off a real keyboard with
host_tool), launches the app and shoots the panel through the control socket.

Frames are paced on the machine's *virtual* clock, not the wall clock, so the
GIF plays at a true 1x however fast or slow the host happens to emulate.

By default the capture interval matches each app's LCD tick (usually 16 ms).
Use --gif-ms for the delay baked into the GIF file — README viewers (GitHub
included) often clamp below ~20 fps, so packing 500×16 ms frames makes playback
feel sluggish even though wall time is 1× in a local viewer. 40 ms → 25 fps is
a good README default.

Output: build/sim-record/<app>/<app>.gif plus the raw frames beside it.
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

# How often the app calls present() under normal play (ms).
APP_FRAME_MS = {
    "brick": 16,
    "fish": 16,
    "life": 16,
    "matrix": 16,
    "maze": 16,
    "settings": 16,
    "wfc": 16,
}
DEFAULT_FRAME_MS = 16
README_GIF_MS = 40       # 25 fps — survives most web GIF viewers

SLOT_SIZE = 0x40000     # 256 KiB reserved per app slot
SAVE_OFF = 0x3F000      # the save sector is the slot's last 4 KiB
SAVE_SIZE = 0x1000
XIP_BASE = 0x10000000
FLASH_BYTES = 16 * 1024 * 1024
APP_MAGIC = b"A75APP\0\0"
HDR_NAME_OFF = 40       # app_header_t.name

# matrix_scan() only comes up around nine virtual seconds in, so keys pressed
# before that are simply not seen.
SETTLE_MS = 14000
GIF_KEY = (8, 2)        # toggles OS input mode
ENTER_KEY = (9, 0)      # launches the selected app


def repo_root() -> Path:
    here = Path(__file__).resolve()
    out = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=here.parent,
                         capture_output=True, text=True)
    if out.returncode == 0:
        return Path(out.stdout.strip())
    return here.parents[4]


def pick_sim(root: Path) -> Path:
    # macOS first because that is the case where this interpreter and the emulator
    # share a loopback -- the ctl client below runs in this process. Driving the
    # .exe from WSL means reaching the Windows loopback, which 127.0.0.1 is not.
    # There is one copy of the emulator and it lives inside the desktop package
    # (tools/sim_bin.sh says the same thing for the shell scripts).
    app = "Athena75 Simulator"
    for rel in (f"sim/macos/{app}.app/Contents/MacOS/{app}", f"sim/windows/{app}/{app}.exe"):
        p = root / "artifacts" / rel
        if p.exists():
            return p
    sys.exit("error: no athena_sim in artifacts/sim/ (tools/build_sim.sh)")


def run_sim(sim: Path, args: list, quiet=True) -> None:
    # Always --headless: this is a recording, and a window would only slow the
    # machine down to the speed of the screen it was being drawn on.
    out = subprocess.run([str(sim), "--headless"] + [str(a) for a in args],
                         capture_output=quiet, text=True)
    if out.returncode != 0:
        if quiet:
            sys.stderr.write(out.stdout or "")
            sys.stderr.write(out.stderr or "")
        sys.exit(f"error: simulator exited {out.returncode}")


def fresh_flash(path: Path) -> None:
    # A blank W25Q128 reads as 0xFF, and the firmware's first boot depends on it.
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"\xFF" * FLASH_BYTES)


def find_slot(flash: bytes, name: str) -> int:
    """Offset of the slot holding `name`, or -1."""
    for off in range(0, len(flash), SLOT_SIZE):
        if flash[off:off + 8] != APP_MAGIC:
            continue
        hdr = flash[off + HDR_NAME_OFF:off + HDR_NAME_OFF + 16]
        if hdr.split(b"\0")[0].decode("ascii", "replace").upper() == name.upper():
            return off
    return -1


def pull_save(root: Path, slot_addr: int) -> bytes:
    """Read an installed app's save sector off a real keyboard."""
    tool = root / "artifacts" / "host" / "windows" / "host_tool.exe"
    if not tool.exists():
        tool = root / "artifacts" / "host" / "macos" / "host_tool"
    addr = slot_addr + SAVE_OFF
    out = subprocess.run([str(tool), "probe", "read", hex(addr), str(SAVE_SIZE)],
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"error: host_tool probe read failed:\n{out.stdout}{out.stderr}")
    data = bytearray()
    for line in out.stdout.splitlines():
        m = re.match(r"^0x[0-9A-Fa-f]+:\s+((?:[0-9A-Fa-f]{2}\s+)+)$", line)
        if m:
            data += bytes(int(b, 16) for b in m.group(1).split())
    if len(data) < SAVE_SIZE:
        sys.exit(f"error: read {len(data)} of {SAVE_SIZE} save bytes")
    return bytes(data[:SAVE_SIZE])


class Ctl:
    """One command per line, one reply per line."""

    def __init__(self, port: int, timeout=30.0):
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 2.0)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.sock.settimeout(20.0)
        self.buf = b""

    def cmd(self, line: str) -> str:
        self.sock.sendall(line.encode() + b"\n")
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("control socket closed")
            self.buf += chunk
        reply, self.buf = self.buf.split(b"\n", 1)
        return reply.decode(errors="replace").strip()

    def now_ms(self) -> float:
        m = re.search(r"t=([0-9.]+)ms", self.cmd("state"))
        return float(m.group(1)) if m else 0.0

    def wait_until(self, t_ms: float) -> None:
        while self.now_ms() < t_ms:
            time.sleep(0.004)

    def close(self):
        self.sock.close()


def resolve_frame_ms(app: str, fps, frame_ms):
    if frame_ms is not None:
        return max(1, frame_ms)
    if fps is not None:
        return max(1, round(1000 / fps))
    return APP_FRAME_MS.get(app.lower(), DEFAULT_FRAME_MS)


def gif_delay_ms(period_ms: int) -> int:
    """Quantise to GIF centiseconds (viewers ignore sub-10 ms delays)."""
    cs = max(2, round(period_ms / 10.0))
    return cs * 10


def make_gif(frames: list, out: Path, scale: int, period_ms: int) -> None:
    from PIL import Image
    imgs = []
    duration = gif_delay_ms(period_ms)
    for f in frames:
        im = Image.open(f).convert("RGB")
        if scale != 1:
            im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
        imgs.append(im)
    if not imgs:
        sys.exit("error: no frames captured")
    imgs[0].save(out, save_all=True, append_images=imgs[1:],
                 duration=duration, loop=0, optimize=False)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("app")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--fps", type=int, default=None,
                    help="capture rate override (default: 1000 / app frame ms)")
    ap.add_argument("--frame-ms", type=int, default=None,
                    help="sim capture interval in virtual ms (default: per-app, usually 16)")
    ap.add_argument("--gif-ms", type=int, default=None,
                    help=f"GIF frame period / capture step (default: {README_GIF_MS} for README)")
    ap.add_argument("--scale", type=int, default=2, help="nearest-neighbour zoom")
    ap.add_argument("--warmup-ms", type=int, default=1500,
                    help="virtual ms between launching the app and the first frame")
    ap.add_argument("--save-data", metavar="FILE",
                    help="bytes to place in the app's save sector")
    ap.add_argument("--save-from-device", metavar="SLOT_ADDR",
                    help="pull the save sector off a keyboard, e.g. 0x10C40000")
    ap.add_argument("--port", type=int, default=47811)
    ap.add_argument("--out", metavar="GIF")
    args = ap.parse_args()

    root = repo_root()
    kbd = root / "keyboards/ydkb/athena75_rgb_advanced"
    sim = pick_sim(root)
    fw = root / "artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2"
    pkg = root / "artifacts/apps" / f"{args.app}.app"
    for f in (fw, pkg):
        if not f.exists():
            sys.exit(f"error: missing {f}")

    out_dir = root / "build/sim-record" / args.app
    frame_dir = out_dir / "frames"
    shutil.rmtree(frame_dir, ignore_errors=True)
    frame_dir.mkdir(parents=True, exist_ok=True)
    flash = out_dir / "flash.bin"
    state = out_dir / "settled.state"
    gif = Path(args.out) if args.out else out_dir / f"{args.app}.gif"
    capture_ms = resolve_frame_ms(args.app, args.fps, args.frame_ms)
    if args.gif_ms is not None:
        gif_ms = max(1, args.gif_ms)
    elif args.out and str(args.out).startswith(str(kbd / "docs/apps")):
        gif_ms = README_GIF_MS
    else:
        gif_ms = capture_ms
    step_ms = gif_ms
    eff_fps = 1000.0 / step_ms
    gif_delay = gif_delay_ms(step_ms)

    save = None
    if args.save_from_device:
        print(">> pulling the save sector off the keyboard")
        save = pull_save(root, int(args.save_from_device, 0))
    elif args.save_data:
        save = Path(args.save_data).read_bytes().ljust(SAVE_SIZE, b"\xFF")[:SAVE_SIZE]

    print(f">> installing {pkg.name} into a blank flash")
    fresh_flash(flash)
    run_sim(sim, ["--uf2", fw, "--flash", flash, "--install-app", pkg,
                  "--log", "*=error", "--run-ms", 1000])

    if save is not None:
        blob = bytearray(flash.read_bytes())
        slot = find_slot(blob, args.app)
        if slot < 0:
            sys.exit(f"error: no installed app named {args.app} in the image")
        print(f">> save sector -> slot @ {hex(XIP_BASE + slot)}")
        blob[slot + SAVE_OFF:slot + SAVE_OFF + SAVE_SIZE] = save
        flash.write_bytes(bytes(blob))

    print(">> booting to interactive")
    run_sim(sim, ["--uf2", fw, "--flash", flash, "--log", "*=error",
                  "--run-ms", SETTLE_MS, "--save-state", state])

    total = SETTLE_MS + args.warmup_ms + int(args.seconds * 1000) + 2000
    print(f">> recording {args.seconds:g}s at {eff_fps:.1f} fps "
          f"({step_ms} ms capture, {gif_delay} ms GIF delay)")
    proc = subprocess.Popen(
        [str(sim), "--headless",
         "--uf2", str(fw), "--flash", str(flash), "--load-state", str(state),
         "--ctl-port", str(args.port), "--log", "*=error", "--run-ms", str(total)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    frames = []
    try:
        ctl = Ctl(args.port)
        t0 = ctl.now_ms()
        ctl.cmd(f"key {GIF_KEY[0]},{GIF_KEY[1]},60")     # OS input mode
        ctl.wait_until(t0 + 500)
        ctl.cmd(f"key {ENTER_KEY[0]},{ENTER_KEY[1]},60")  # launch the app
        start = t0 + 500 + args.warmup_ms
        ctl.wait_until(start)

        step = float(step_ms)
        n_frames = max(1, int(args.seconds * 1000 / step))
        for i in range(n_frames):
            ctl.wait_until(start + i * step)
            png = frame_dir / f"f{i:04d}.png"
            r = ctl.cmd(f"shot {png}")
            if not r.startswith("ok"):
                sys.exit(f"error: {r}")
            frames.append(png)
        ctl.cmd("quit")
        ctl.close()
    finally:
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()

    make_gif(frames, gif, args.scale, step_ms)
    size = gif.stat().st_size
    print(f">> {gif} ({len(frames)} frames @ {gif_delay} ms, {size // 1024} KiB)")


if __name__ == "__main__":
    main()
