"""共享 UF2 工具: 打包字节为 UF2, 并把所有产物统一归档到 artifacts/firmware/。"""
import glob
import os
import shutil
import struct
from datetime import datetime

FAMILY_ID = 0xE48BFF56   # RP2040 UF2 family id
UF2_PAYLOAD = 256        # 每个 512B block 的有效载荷
KEEP = 10                # 每个 base 保留的历史版本数

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
KBD_DIR = os.path.dirname(TOOLS_DIR)
BUILDS_DIR = os.path.join(KBD_DIR, "artifacts", "firmware")
HISTORY_DIR = os.path.join(BUILDS_DIR, "history")


def to_uf2(data, start_addr):
    """把原始字节封装成 UF2 (目标 flash 地址 start_addr, RP2040 family)。"""
    num_blocks = (len(data) + UF2_PAYLOAD - 1) // UF2_PAYLOAD
    out = bytearray(num_blocks * 512)
    for i in range(num_blocks):
        bo = i * 512
        do = i * UF2_PAYLOAD
        struct.pack_into("<I", out, bo + 0, 0x0A324655)     # UF2 magic 0
        struct.pack_into("<I", out, bo + 4, 0x9E5D5157)     # UF2 magic 1
        struct.pack_into("<I", out, bo + 8, 0x00002000)     # flags: familyID present
        struct.pack_into("<I", out, bo + 12, start_addr + do)
        struct.pack_into("<I", out, bo + 16, UF2_PAYLOAD)
        struct.pack_into("<I", out, bo + 20, i)
        struct.pack_into("<I", out, bo + 24, num_blocks)
        struct.pack_into("<I", out, bo + 28, FAMILY_ID)
        chunk = data[do:do + UF2_PAYLOAD]
        out[bo + 32:bo + 32 + len(chunk)] = chunk
        struct.pack_into("<I", out, bo + 508, 0x0AB16F30)   # UF2 magic end
    return bytes(out)


def _prune(base, keep=KEEP):
    hist = sorted(
        glob.glob(os.path.join(HISTORY_DIR, f"{base}_*.uf2")),
        key=os.path.getmtime, reverse=True,
    )
    removed = []
    for old in hist[keep:]:
        try:
            os.remove(old)
            removed.append(old)
        except OSError:
            pass
    return removed


def archive_bytes(uf2_bytes, base, keep=KEEP):
    """写 builds/<base>.uf2 (latest) + builds/history/<base>_<stamp>.uf2 (history), 并裁剪历史。
    返回 (latest_path, stamped_path, pruned_list)。"""
    os.makedirs(HISTORY_DIR, exist_ok=True)
    latest = os.path.join(BUILDS_DIR, f"{base}.uf2")
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    stamped = os.path.join(HISTORY_DIR, f"{base}_{stamp}.uf2")
    with open(latest, "wb") as f:
        f.write(uf2_bytes)
    shutil.copy2(latest, stamped)
    pruned = _prune(base, keep)
    return latest, stamped, pruned


def archive_file(src_path, base, keep=KEEP):
    """归档一个已存在的 .uf2 文件 (例如固件构建产物)。"""
    with open(src_path, "rb") as f:
        return archive_bytes(f.read(), base, keep)
