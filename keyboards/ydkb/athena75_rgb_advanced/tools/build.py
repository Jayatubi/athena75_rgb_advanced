#!/usr/bin/env python3
"""Core build for Athena75 RGB (vial). Platform-agnostic.

Owns the shared work: pinned QMK docker image, make, size, uf2 archive. Platform
entry points (build_wsl.sh / build_mac.sh) do host-specific prep (e.g. WSL mirror
sync) then invoke this script. To flash, use upload.py (BOOTSEL + copy a UF2);
building and uploading are deliberately separate tools.

Usage (direct, or via a platform wrapper):
  python3 build.py                     # docker build ydkb/athena75_rgb_advanced:vial
  python3 build.py -c                  # clean then build
  python3 build.py --keymap via
  python3 build.py --jobs 8
  python3 build.py --build-root PATH   # docker mount root (WSL mirror)
  python3 build.py --backend native    # host toolchain (often too strict)
  python3 build.py --check-env

The built firmware is archived to tools/builds/<base>.uf2; upload it with:
  python3 upload.py                    # BOOTSEL + upload that firmware

On Windows without host Docker, this forwards to build_wsl.sh inside WSL so the
mirror / rsync path stays in the platform wrapper.
"""
from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

# --- Configuration -------------------------------------------------------------
KEYBOARD = "ydkb/athena75_rgb_advanced"
DEFAULT_KEYMAP = "vial"
DEFAULT_JOBS = 16
DOCKER_IMAGE = (
    "ghcr.io/qmk/qmk_cli@sha256:"
    "16c4916e95b99bf88d27b15aec8db409ee17265d1710287fde248c6666508966"
)

IS_WINDOWS = platform.system() == "Windows"
WSL_DISTRO = os.environ.get("WSL_DISTRO", "")

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[3]  # …/tools -> repo root

sys.path.insert(0, str(SCRIPT_DIR))
import uf2_common  # noqa: E402  (local module in tools/, unified UF2 archiving)


# --- Small helpers -------------------------------------------------------------
def step(msg):
    print(f">> {msg}", flush=True)


def err(msg):
    print(f"error: {msg}", file=sys.stderr, flush=True)


def artifact_base(keymap):
    return f"{KEYBOARD}_{keymap}".replace("/", "_").replace("-", "_")


def on_path(exe):
    return shutil.which(exe) is not None


def in_wsl():
    if os.environ.get("WSL_DISTRO_NAME") or os.environ.get("WSL_INTEROP"):
        return True
    try:
        with open("/proc/version", encoding="utf-8", errors="ignore") as f:
            v = f.read().lower()
        return "microsoft" in v or "wsl" in v
    except OSError:
        return False


def submodules_ok(root: Path):
    return (root / "lib" / "chibios" / "os").is_dir() and \
           (root / "lib" / "pico-sdk" / "src").is_dir()


# --- Docker --------------------------------------------------------------------
def _docker_info_ok(docker_argv):
    try:
        return subprocess.run(
            [*docker_argv, "info"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode == 0
    except OSError:
        return False


def resolve_docker_cmd():
    """Return argv prefix: ['docker'] or ['sudo', '-n', 'docker']."""
    if on_path("docker") and _docker_info_ok(["docker"]):
        return ["docker"]
    if on_path("sudo") and _docker_info_ok(["sudo", "-n", "docker"]):
        return ["sudo", "-n", "docker"]
    return None


def docker_available():
    return resolve_docker_cmd() is not None


def docker_mount(path: Path):
    """Host path for `docker run -v <host>:/qmk_firmware` (forward slashes)."""
    return str(path.resolve()).replace("\\", "/")


def run_docker_make(build_root: Path, make_args, allow_fail=False):
    docker = resolve_docker_cmd()
    if not docker:
        err("docker daemon not reachable.")
        if in_wsl() or platform.system() == "Linux":
            print("  start it:  sudo service docker start", file=sys.stderr)
            print("  and/or:    sudo usermod -aG docker $USER  (re-open shell)",
                  file=sys.stderr)
        else:
            print("  start Docker Desktop / colima, or use build_wsl.sh / "
                  "--backend native", file=sys.stderr)
        sys.exit(1)

    cmd = [
        *docker, "run", "--rm",
        "-v", f"{docker_mount(build_root)}:/qmk_firmware",
        "-w", "/qmk_firmware",
        DOCKER_IMAGE,
        "make", *make_args,
    ]
    rc = subprocess.run(cmd).returncode
    if rc != 0 and not allow_fail:
        return rc
    return rc


def print_elf_size(build_root: Path, elf_rel: str, backend: str):
    if backend == "docker":
        docker = resolve_docker_cmd()
        if not docker:
            return
        subprocess.run(
            [*docker, "run", "--rm",
             "-v", f"{docker_mount(build_root)}:/qmk_firmware",
             "-w", "/qmk_firmware",
             DOCKER_IMAGE, "arm-none-eabi-size", elf_rel],
            check=False,
        )
        return
    if on_path("arm-none-eabi-size"):
        subprocess.run(
            ["arm-none-eabi-size", elf_rel], cwd=str(build_root), check=False,
        )


# --- Native (optional) ---------------------------------------------------------
def find_qmk_msys_bash():
    if not IS_WINDOWS:
        return None
    candidates = [
        r"C:\QMK_MSYS\usr\bin\bash.exe",
        os.path.join(os.environ.get("LOCALAPPDATA", ""), "QMK_MSYS", "usr", "bin", "bash.exe"),
        r"C:\msys64\usr\bin\bash.exe",
    ]
    for c in candidates:
        if c and Path(c).exists():
            return c
    return None


def to_msys_path(win_path):
    p = str(Path(win_path).resolve()).replace("\\", "/")
    if len(p) >= 2 and p[1] == ":":
        return "/" + p[0].lower() + p[2:]
    return p


def msys_env():
    env = os.environ.copy()
    env["MSYSTEM"] = "MINGW64"
    env["MSYS2_PATH_TYPE"] = "inherit"
    return env


def run_native_make(build_root: Path, make_args, allow_fail=False):
    if on_path("make") and on_path("arm-none-eabi-gcc"):
        rc = subprocess.run(["make", *make_args], cwd=str(build_root)).returncode
    else:
        bash = find_qmk_msys_bash()
        if not bash:
            err("no native toolchain (make + arm-none-eabi-gcc).")
            print("Use docker (default) or install QMK MSYS / brew qmk.",
                  file=sys.stderr)
            sys.exit(1)
        cmd = f"cd '{to_msys_path(build_root)}' && make " + " ".join(make_args)
        rc = subprocess.run([bash, "-lc", cmd], env=msys_env()).returncode
    return rc


# --- Windows → WSL wrapper -----------------------------------------------------
def wsl_prefix():
    base = ["wsl"]
    if WSL_DISTRO:
        base += ["-d", WSL_DISTRO]
    return base


def wsl_available():
    if not IS_WINDOWS or not on_path("wsl"):
        return False
    try:
        return subprocess.run(
            wsl_prefix() + ["--", "true"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        ).returncode == 0
    except OSError:
        return False


def to_wsl_path(win_path):
    p = str(Path(win_path).resolve()).replace("\\", "/")
    if len(p) >= 2 and p[1] == ":":
        return "/mnt/" + p[0].lower() + p[2:]
    return p


def forward_to_wsl_wrapper(argv_rest):
    """Hand off to build_wsl.sh so mirror / rsync stay platform-side."""
    wrapper = SCRIPT_DIR / "build_wsl.sh"
    if not wrapper.is_file():
        err(f"missing {wrapper}; cannot forward Windows build into WSL")
        sys.exit(1)
    step("no host docker — forwarding to build_wsl.sh inside WSL")
    cmd = wsl_prefix() + ["--", "bash", to_wsl_path(wrapper), *argv_rest]
    return subprocess.run(cmd).returncode


# --- Env report ----------------------------------------------------------------
def check_env():
    step("environment check")

    def ok(m):
        print(f"  [ok]   {m}")

    def miss(m):
        print(f"  [MISS] {m}")

    if docker_available():
        ok(f"docker ({' '.join(resolve_docker_cmd())})")
    elif IS_WINDOWS and wsl_available():
        ok("WSL available → will forward to build_wsl.sh")
    else:
        miss("docker (default backend)")

    if on_path("make") and on_path("arm-none-eabi-gcc"):
        ok("[native] make + arm-none-eabi-gcc")
    elif find_qmk_msys_bash():
        ok("[native] QMK MSYS toolchain")
    else:
        miss("[native] toolchain (optional; --backend native only)")

    ok(f"python {platform.python_version()}")
    if submodules_ok(REPO_ROOT):
        ok("git submodules checked out (repo root)")
    else:
        miss("git submodules → git submodule update --init --recursive")
    if in_wsl():
        ok("running inside WSL")
    print()


# --- Main ----------------------------------------------------------------------
def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv

    # Windows without host docker: platform work (mirror) lives in build_wsl.sh.
    # Keep --help / --check-env local so they don't hop into WSL.
    if (
        IS_WINDOWS
        and not docker_available()
        and wsl_available()
        and not any(a in ("-h", "--help", "--check-env") for a in argv)
    ):
        return forward_to_wsl_wrapper(argv)

    ap = argparse.ArgumentParser(
        description="Core Athena75 RGB firmware build (docker by default).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("-c", "--clean", action="store_true",
                    help="run make <target>:clean before building")
    ap.add_argument("--keymap", default=os.environ.get("KEYMAP", DEFAULT_KEYMAP),
                    help=f"keymap (default: {DEFAULT_KEYMAP})")
    ap.add_argument("--jobs", type=int,
                    default=int(os.environ.get("JOBS", DEFAULT_JOBS)),
                    help=f"parallel make jobs (default: {DEFAULT_JOBS})")
    ap.add_argument("--backend", choices=["docker", "native"], default="docker",
                    help="docker (default, pinned QMK image) or native toolchain")
    ap.add_argument("--build-root", type=Path, default=None,
                    help="directory docker mounts as /qmk_firmware "
                         "(default: repo root; WSL mirror passes this)")
    ap.add_argument("--check-env", action="store_true",
                    help="probe the environment and exit")
    args = ap.parse_args(argv)

    if args.check_env:
        check_env()
        return 0

    build_root = (args.build_root or REPO_ROOT).resolve()
    target = f"{KEYBOARD}:{args.keymap}"
    base = artifact_base(args.keymap)
    built_uf2 = build_root / f"{base}.uf2"
    elf_rel = f".build/{base}.elf"

    if not submodules_ok(build_root):
        err(f"git submodules missing under {build_root} "
            "(lib/chibios or lib/pico-sdk).")
        if build_root != REPO_ROOT.resolve():
            print("Mirror incomplete? Run: bash build_wsl.sh --sync all",
                  file=sys.stderr)
        else:
            print("Run: git submodule update --init --recursive", file=sys.stderr)
        return 1

    backend = args.backend
    if backend == "docker":
        def run_make(make_args, allow_fail=False):
            return run_docker_make(build_root, make_args, allow_fail=allow_fail)
    else:
        def run_make(make_args, allow_fail=False):
            return run_native_make(build_root, make_args, allow_fail=allow_fail)

    step(f"repo       : {REPO_ROOT}")
    if build_root != REPO_ROOT.resolve():
        step(f"build-root : {build_root}  (docker mount; uf2 copied back to repo)")
    step(f"target     : {target}")
    step(f"backend    : {backend}")
    if backend == "docker":
        d = resolve_docker_cmd()
        step(f"docker     : {' '.join(d) if d else '(unavailable)'}")
        step(f"image      : {DOCKER_IMAGE}")

    if args.clean:
        step(f"clean {target}")
        run_make([f"{target}:clean"], allow_fail=True)

    step(f"build {target} (-j {args.jobs})")
    rc = run_make([target, "-j", str(args.jobs)])
    if rc != 0:
        err(f"build failed (make exit code {rc})")
        return rc

    print_elf_size(build_root, elf_rel, backend)

    if not built_uf2.is_file():
        err(f"expected firmware {built_uf2} not found")
        return 1

    # Unified output: all UF2s (firmware / boot / keyframe) land in tools/builds/
    # as a stable latest <base>.uf2 plus a timestamped history (see uf2_common).
    latest, stamped, pruned = uf2_common.archive_file(str(built_uf2), base)
    out_uf2 = Path(latest)
    step(f"firmware   : {out_uf2}")
    step(f"archived   : {stamped}")
    for old in pruned:
        step(f"pruned old build: {old}")

    step("to flash:   python3 upload.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
