#!/usr/bin/env python3
"""Draw the WFC tilesets as 16×16 pixel art and emit wfc_tiles.c/h.

Nothing is downsampled from a big image any more: every tile is authored at its
final 16×16 size in the FC/SFC idiom — a small fixed palette, one-pixel bevels
lit from the top-left, and dithered surface texture.

Two rules keep the tiles joinable, and both are checked at build time:

  * The shape of a tile is a mask derived only from its adjacency bits, so any
    two tiles the solver may place side by side show the same material at the
    joint (`verify_seams`).
  * Surface texture uses periods that divide 16, so a pattern started in one
    tile continues correctly in the next.

    python3 make_tiles.py
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
import zlib
from pathlib import Path
from typing import NamedTuple

PNG_SIG = b"\x89PNG\r\n\x1a\n"
TILE = 16
TILE_BYTES = TILE * TILE * 2
LAST = TILE - 1

# Adjacency tables — must match wfc_app.c.
# The trailing single-port entries are terminals: somewhere for a net or a run
# to end. Without them every trace either loops or leaves the panel, which is
# most of why a board of these reads as wallpaper rather than as a circuit.
CIRCUIT_EXITS = [0, 10, 5, 3, 9, 6, 12, 11, 7, 14, 13, 15, 1, 2, 4, 8]  # N=1 E=2 S=4 W=8
WANG_COAST = [0, 15, 8, 4, 2, 1, 9, 6, 12, 3, 10, 5, 14, 13, 11, 7]  # N=8 E=4 S=2 W=1

DOOR_N, DOOR_S = 1, 2


class CornerTile(NamedTuple):
    """One tile of a corner-labelled set: shape, then what is built on it.

    `bits` is the corner mask (NW=8 NE=4 SE=2 SW=1) and decides the shape, so
    several tiles may share one and differ only in what they carry. `doors` is
    part of the matching rule rather than decoration: a wall always straddles a
    tile boundary, half in each tile, so a door has to be agreed on by both.
    """

    bits: int
    doors: int = 0
    decor: str = ""


# Doors are hung in horizontal walls only. Partly for room — the solver's option
# mask is 32 bits wide — but mostly because a door in a wall running away from
# the viewer would be drawn edge-on, and at 16 px that reads as a smudge.
# A door tile needs a partner on the far side of the wall for every shape the
# partner might be forced into, hence one per corner pattern that leaves the
# shared edge solid rock.
DUNGEON_TILES = [CornerTile(b) for b in range(16)] + [
    CornerTile(0, DOOR_S),
    CornerTile(4, DOOR_S),
    CornerTile(8, DOOR_S),
    CornerTile(12, DOOR_S),
    CornerTile(0, DOOR_N),
    CornerTile(1, DOOR_N),
    CornerTile(2, DOOR_N),
    CornerTile(3, DOOR_N),
    CornerTile(15, decor="fire"),
]

ISLAND_TILES = [CornerTile(b) for b in range(16)] + [
    CornerTile(15, decor="trees"),
    CornerTile(15, decor="hut"),
]

Mask = list[list[bool]]


class TileArt(NamedTuple):
    rgb: bytes
    mask: Mask


# ----------------------------------------------------------------- png / rgb


def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def read_png(path: Path) -> tuple[int, int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(PNG_SIG):
        raise ValueError(f"{path}: not a PNG")
    pos, width, height, channels, color_type = 8, 0, 0, 0, 0
    compressed = bytearray()
    palette, alpha = b"", b""
    while pos + 12 <= len(data):
        size = struct.unpack(">I", data[pos : pos + 4])[0]
        kind = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + size]
        pos += 12 + size
        if kind == b"IHDR":
            width, height, depth, color_type, comp, filt, interlace = struct.unpack(
                ">IIBBBBB", body
            )
            if depth != 8 or color_type not in (2, 3, 6) or comp or filt or interlace:
                raise ValueError(f"{path}: need 8-bit non-interlaced RGB/RGBA/indexed PNG")
            channels = 3 if color_type == 2 else 4 if color_type == 6 else 1
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            alpha = body
        elif kind == b"IDAT":
            compressed.extend(body)
        elif kind == b"IEND":
            break
    if not width or not compressed:
        raise ValueError(f"{path}: missing IHDR/IDAT")

    raw = zlib.decompress(compressed)
    stride = width * channels
    out = bytearray(height * stride)
    src = dst = 0
    previous = bytearray(stride)
    for _ in range(height):
        mode = raw[src]
        src += 1
        row = bytearray(raw[src : src + stride])
        src += stride
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            if mode == 1:
                row[x] = (row[x] + left) & 0xFF
            elif mode == 2:
                row[x] = (row[x] + up) & 0xFF
            elif mode == 3:
                row[x] = (row[x] + ((left + up) >> 1)) & 0xFF
            elif mode == 4:
                row[x] = (row[x] + paeth(left, up, upper_left)) & 0xFF
            elif mode != 0:
                raise ValueError(f"unsupported PNG filter {mode}")
        out[dst : dst + stride] = row
        dst += stride
        previous = row

    if color_type == 3:
        if not palette:
            raise ValueError(f"{path}: indexed PNG has no palette")
        rgba = bytearray(width * height * 4)
        for i, idx in enumerate(out):
            po = idx * 3
            rgba[i * 4 : i * 4 + 4] = bytes(
                (
                    palette[po],
                    palette[po + 1],
                    palette[po + 2],
                    alpha[idx] if idx < len(alpha) else 255,
                )
            )
        return width, height, 4, bytes(rgba)

    return width, height, channels, bytes(out)


def write_png(path: Path, w: int, h: int, rgb: bytes) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            o = (y * w + x) * 3
            row += rgb[o : o + 3]
        rows.append(bytes(row))
    png = (
        PNG_SIG
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
        + chunk(b"IEND", b"")
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def rgba_at(pixels: bytes, channels: int, w: int, x: int, y: int) -> tuple[int, int, int]:
    i = (y * w + x) * channels
    r, g, b = pixels[i], pixels[i + 1], pixels[i + 2]
    if channels == 4:
        a = pixels[i + 3]
        r, g, b = r * a // 255, g * a // 255, b * a // 255
    return r, g, b


def rgb565_be(r: int, g: int, b: int) -> bytes:
    return struct.pack(">H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))


def tile_to_565(tile_rgb: bytes) -> bytes:
    out = bytearray(TILE_BYTES)
    for i in range(TILE * TILE):
        o = i * 3
        out[i * 2 : i * 2 + 2] = rgb565_be(tile_rgb[o], tile_rgb[o + 1], tile_rgb[o + 2])
    return bytes(out)


def set_pixel(buf: bytearray, x: int, y: int, rgb: bytes) -> None:
    o = (y * TILE + x) * 3
    buf[o : o + 3] = rgb


def fill_rect(buf: bytearray, x0: int, y0: int, x1: int, y1: int, rgb: bytes) -> None:
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            set_pixel(buf, x, y, rgb)


# ------------------------------------------------------------ shape helpers


def mask_at(m: Mask, x: int, y: int) -> bool:
    """Neighbour lookup that clamps instead of falling off the tile.

    Reading outside as "empty" would draw an outline along every tile border and
    box each tile in; clamping makes a shape read as if it carried on next door.
    """
    return m[min(LAST, max(0, y))][min(LAST, max(0, x))]


def face_of(m: Mask, x: int, y: int) -> str:
    """Which side of the shape a filled pixel sits on (light comes from top-left)."""
    if not mask_at(m, x, y - 1):
        return "top"
    if not mask_at(m, x, y + 1):
        return "bottom"
    if not mask_at(m, x - 1, y):
        return "left"
    if not mask_at(m, x + 1, y):
        return "right"
    return "core"


def face_grid(m: Mask) -> list[list[str]]:
    return [
        [face_of(m, x, y) if m[y][x] else "" for x in range(TILE)] for y in range(TILE)
    ]


def ring_distance(m: Mask, x: int, y: int, want: bool, limit: int) -> int:
    """How many pixels away the nearest `want` pixel is, capped at limit + 1."""
    for r in range(1, limit + 1):
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if max(abs(dx), abs(dy)) == r and mask_at(m, x + dx, y + dy) == want:
                    return r
    return limit + 1


BAND = 3
"""Pixels each side of a joint that a neighbour's shading can reach.

Beaches and bevels look `BAND - 1` pixels into a tile, so this whole strip has
to come out identical on both sides of a joint or the trim breaks at the seam.
"""


REACH = LAST - (BAND - 1)
"""How far a corner's influence carries.

Kept short enough that the far pair of corners contributes nothing within BAND
of an edge: there, only the two corners on that edge matter — and a legal
neighbour has the very same two.
"""


def corner_mask(bits: int) -> Mask:
    """Corner-labelled Wang shape (NW=8 NE=4 SE=2 SW=1).

    Each set corner casts a round falloff and the tile is filled where they add
    up past half. Because the falloff dies before it can cross the tile, the
    BAND-wide strip along every edge is a function of that edge's two corners
    alone, so two legal neighbours press together strips that match pixel for
    pixel — beaches and bevels included, not just the material.
    """
    # Low enough that neighbouring corners grow into one landmass rather than a
    # string of beads, high enough that two diagonal corners stay separate
    # instead of meeting in a 45° neck.
    level = 0.42
    corners = ((0, 0, bits & 8), (LAST, 0, bits & 4), (LAST, LAST, bits & 2), (0, LAST, bits & 1))
    out: Mask = []
    for y in range(TILE):
        row = []
        for x in range(TILE):
            here = 0.0
            for cx, cy, on in corners:
                if not on:
                    continue
                d = math.hypot(x - cx, y - cy)
                if d < REACH:
                    here += 1.0 - d / REACH
            row.append(here >= level)
        out.append(row)
    return out


ROOM = LAST - BAND
"""Half-width of a dungeon room block, and with it the thickness of a wall.

Rooms either side of a line of unset lattice corners leave `2 * (LAST - ROOM)`
pixels of rock between them, so a bigger block means a thinner wall: at the 12
here a partition is 6 px, against 14 px at 8. Half of that thickness lies in
each of the two tiles the wall runs between, which is why a door has to be
agreed between them. Anything past this stops a corner short of the BAND strip
on the far border, which is what the seam guarantee rests on.
"""


def room_mask(bits: int) -> Mask:
    """Corner-labelled rooms — the same labels as corner_mask, but square.

    A dungeon is built, not eroded, so each set corner claims a square block
    instead of a round falloff. Rooms then have straight walls and right-angled
    junctions, which is what stops the dungeon reading as the island with a
    different palette. The block stops short of the opposite border, so the
    seam guarantee is exactly the one corner_mask relies on.

    Read the other way round, what this draws is the wall: rock survives only
    where no nearby corner claimed the pixel, which leaves a thin partition
    between rooms and a cross at a junction.
    """
    corners = ((0, 0, bits & 8), (LAST, 0, bits & 4), (LAST, LAST, bits & 2), (0, LAST, bits & 1))
    out: Mask = []
    for y in range(TILE):
        row = []
        for x in range(TILE):
            row.append(
                any(
                    on and abs(x - cx) <= ROOM and abs(y - cy) <= ROOM
                    for cx, cy, on in corners
                )
            )
        out.append(row)
    return out


def roughen(m: Mask, salt: int) -> Mask:
    """Chew up the smooth boundary so coastlines look drawn, not computed.

    Only pixels BAND or more in from every border move, which leaves both the
    edge rule and the border strip above untouched.
    """
    out = [row[:] for row in m]
    for y in range(BAND, TILE - BAND):
        for x in range(BAND, TILE - BAND):
            here = m[y][x]
            if all(
                m[y + dy][x + dx] == here for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))
            ):
                continue
            if (((x * 61 + y * 97 + salt * 131) ^ (x * y * 29)) & 7) < 3:
                out[y][x] = not here
    return out


# ------------------------------------------------------------------ circuit

PCB_SUB = bytes((14, 44, 26))
PCB_SUB_HI = bytes((22, 60, 34))
PCB_SUB_LO = bytes((9, 31, 19))
PCB_SHADOW = bytes((72, 48, 12))
PCB_BASE = bytes((204, 156, 44))
PCB_LIGHT = bytes((250, 226, 132))
PCB_HOLE = bytes((10, 28, 16))

TRACE_LO, TRACE_HI = 6, 9  # the trace band; every port lives on these 4 pixels


def draw_substrate(buf: bytearray) -> None:
    for y in range(TILE):
        for x in range(TILE):
            if (x + y) % 8 == 0:
                col = PCB_SUB_HI
            elif x % 8 == 3 and y % 8 == 6:
                col = PCB_SUB_LO
            else:
                col = PCB_SUB
            set_pixel(buf, x, y, col)


def circuit_mask(exits: int) -> Mask:
    m = [[False] * TILE for _ in range(TILE)]

    def band(x0: int, x1: int, y0: int, y1: int) -> None:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                m[y][x] = True

    ports = bin(exits).count("1")
    if ports >= 3:
        band(TRACE_LO - 1, TRACE_HI + 1, TRACE_LO - 1, TRACE_HI + 1)  # solder pad
    elif ports == 1:
        band(TRACE_LO - 2, TRACE_HI + 2, TRACE_LO - 2, TRACE_HI + 2)  # terminal pad
    elif exits:
        band(TRACE_LO, TRACE_HI, TRACE_LO, TRACE_HI)
    if exits & 1:
        band(TRACE_LO, TRACE_HI, 0, TRACE_HI)
    if exits & 2:
        band(TRACE_LO, LAST, TRACE_LO, TRACE_HI)
    if exits & 4:
        band(TRACE_LO, TRACE_HI, TRACE_LO, LAST)
    if exits & 8:
        band(0, TRACE_HI, TRACE_LO, TRACE_HI)
    return m


def render_circuit(exits: int) -> TileArt:
    buf = bytearray(TILE * TILE * 3)
    draw_substrate(buf)
    m = circuit_mask(exits)
    shade = {
        "top": PCB_LIGHT,
        "left": PCB_LIGHT,
        "bottom": PCB_SHADOW,
        "right": PCB_SHADOW,
        "core": PCB_BASE,
    }
    for y in range(TILE):
        for x in range(TILE):
            if m[y][x]:
                set_pixel(buf, x, y, shade[face_of(m, x, y)])

    ports = bin(exits).count("1")
    if exits == 0:
        # Bare substrate. Scattering pads over the blank tile made the empty
        # parts of a board tile into perfect graph paper, and left a real pad
        # nothing to stand out against.
        pass
    elif ports == 1:
        # A trace has to stop *at* something, or it reads as artwork that broke
        # off rather than as the end of a net.
        fill_rect(buf, 6, 6, 9, 9, PCB_HOLE)
        set_pixel(buf, 6, 6, PCB_SHADOW)
        set_pixel(buf, 9, 9, PCB_LIGHT)
    elif ports >= 3:
        # Plated through-hole in the middle of the pad.
        fill_rect(buf, 7, 7, 8, 8, PCB_HOLE)
    return TileArt(bytes(buf), m)


# -------------------------------------------------------------------- pipes

PIPE_BG = bytes((12, 14, 26))
PIPE_BG_DOT = bytes((22, 26, 44))
PIPE_SHADOW = bytes((10, 44, 82))
PIPE_BASE = bytes((44, 138, 208))
PIPE_SHEEN = bytes((92, 182, 236))
PIPE_LIGHT = bytes((176, 232, 255))

PIPE_STEEL = bytes((150, 160, 178))
PIPE_STEEL_HI = bytes((212, 220, 232))
PIPE_STEEL_LO = bytes((76, 86, 106))
PIPE_BORE = bytes((16, 52, 92))

PIPE_LO, PIPE_HI = 5, 10
COLLAR_LO, COLLAR_HI = PIPE_LO - 1, PIPE_HI + 1
CAP_LO, CAP_HI = PIPE_LO - 2, PIPE_HI + 2
COLLAR_IN = 1
"""Where a coupling sits, measured in from the edge the tube leaves by. Only the
single row of pixels on the edge itself has to match a neighbour, so a collar
this far in is free — and since both tiles of a joint draw one, every crossing
of a tile boundary gets a pair of rings, the way real pipework is bolted up.
A run that stops inside a tile gets a blank flange instead, fatter than any
coupling, which is what makes a dead end read as the end of a pipe."""


Rect = tuple[int, int, int, int]

# Where the blanking plate goes for a run that stops inside the tile: across the
# far end of the tube, standing two pixels proud of it on either side.
PIPE_PLATE: dict[int, Rect] = {
    8: (CAP_LO, CAP_HI, PIPE_HI, PIPE_HI + 2),
    2: (CAP_LO, CAP_HI, PIPE_LO - 2, PIPE_LO),
    4: (PIPE_LO - 2, PIPE_LO, CAP_LO, CAP_HI),
    1: (PIPE_HI, PIPE_HI + 2, CAP_LO, CAP_HI),
}


def pipe_parts(bits: int) -> tuple[Mask, dict[tuple[int, int], str], Rect | None]:
    """The silhouette, plus which pixels are fittings rather than tube.

    Fittings are painted in steel afterwards instead of being shaded as part of
    the tube: a coupling one pixel proud of a six-pixel tube is far too small a
    step for bevelling alone to say "this is a separate part".
    """
    m = [[False] * TILE for _ in range(TILE)]
    collar: dict[tuple[int, int], str] = {}

    def band(x0: int, x1: int, y0: int, y1: int, axis: str = "") -> None:
        for y in range(max(0, y0), min(LAST, y1) + 1):
            for x in range(max(0, x0), min(LAST, x1) + 1):
                m[y][x] = True
                if axis:
                    collar[(x, y)] = axis

    plate = PIPE_PLATE.get(bits) if bin(bits).count("1") == 1 else None
    if plate is None and bits:
        band(PIPE_LO, PIPE_HI, PIPE_LO, PIPE_HI)
    if bits & 8:  # N
        band(PIPE_LO, PIPE_HI, 0, PIPE_HI)
        band(COLLAR_LO, COLLAR_HI, COLLAR_IN, COLLAR_IN + 1, "v")
    if bits & 4:  # E
        band(PIPE_LO, LAST, PIPE_LO, PIPE_HI)
        band(LAST - COLLAR_IN - 1, LAST - COLLAR_IN, COLLAR_LO, COLLAR_HI, "h")
    if bits & 2:  # S
        band(PIPE_LO, PIPE_HI, PIPE_LO, LAST)
        band(COLLAR_LO, COLLAR_HI, LAST - COLLAR_IN - 1, LAST - COLLAR_IN, "v")
    if bits & 1:  # W
        band(0, PIPE_HI, PIPE_LO, PIPE_HI)
        band(COLLAR_IN, COLLAR_IN + 1, COLLAR_LO, COLLAR_HI, "h")
    if plate:
        band(*plate)
    return m, collar, plate


def pipe_mask(bits: int) -> Mask:
    return pipe_parts(bits)[0]


def render_pipe(bits: int) -> TileArt:
    buf = bytearray(TILE * TILE * 3)
    for y in range(TILE):
        for x in range(TILE):
            dot = x % 8 == 2 and y % 8 == 5
            set_pixel(buf, x, y, PIPE_BG_DOT if dot else PIPE_BG)

    m, collar, plate = pipe_parts(bits)
    faces = face_grid(m)

    def face_at(x: int, y: int) -> str:
        return faces[min(LAST, max(0, y))][min(LAST, max(0, x))]

    for y in range(TILE):
        for x in range(TILE):
            face = faces[y][x]
            if not face:
                continue
            # The specular sits one pixel in from the rim rather than on it, so
            # the tube turns away at both edges and reads round.
            if face in ("top", "left"):
                col = PIPE_SHEEN
            elif face in ("bottom", "right"):
                col = PIPE_SHADOW
            elif face_at(x, y - 1) == "top" or face_at(x - 1, y) == "left":
                col = PIPE_LIGHT
            else:
                col = PIPE_BASE
            set_pixel(buf, x, y, col)

    for (x, y), axis in collar.items():
        across = y if axis == "h" else x
        if across == COLLAR_LO:
            col = PIPE_STEEL_HI
        elif across == COLLAR_HI:
            col = PIPE_STEEL_LO
        else:
            col = PIPE_STEEL
        set_pixel(buf, x, y, col)

    if plate:
        x0, x1, y0, y1 = plate
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if x == x0 or y == y0:
                    col = PIPE_STEEL_HI
                elif x == x1 or y == y1:
                    col = PIPE_STEEL_LO
                else:
                    col = PIPE_STEEL
                set_pixel(buf, x, y, col)
        # Two bolts, on the long axis of the plate, so it reads as bolted on.
        mid_x, mid_y = (x0 + x1) // 2, (y0 + y1) // 2
        span = x1 - x0 > y1 - y0
        for k in (-3, 3):
            bx, by = (mid_x + k, mid_y) if span else (mid_x, mid_y + k)
            set_pixel(buf, bx, by, PIPE_BORE)
    return TileArt(bytes(buf), m)


# ------------------------------------------------------------------ dungeon

# Light masonry standing in a dark room, rather than the other way round: the
# wall is the smaller share of the picture now that partitions are thin, and the
# eye reads the bright, bevelled material as the thing standing up.
DUN_WALL = bytes((166, 156, 138))
DUN_WALL_HI = bytes((214, 206, 188))
DUN_MORTAR = bytes((120, 111, 96))
DUN_FLOOR = bytes((54, 50, 62))
DUN_FLOOR_ALT = bytes((47, 44, 55))
DUN_FLOOR_LO = bytes((32, 30, 38))
DUN_JOINT = bytes((44, 41, 51))
DUN_DOOR = bytes((124, 74, 40))
DUN_DOOR_HI = bytes((166, 106, 60))
DUN_DOOR_LO = bytes((78, 44, 22))
DUN_IRON = bytes((222, 190, 96))

DOOR_X0, DOOR_X1 = 5, 10
"""Columns a door occupies. Well clear of BAND at either side, so a door never
disturbs the strips that have to match the tiles left and right of it."""

FIRE_LOG = bytes((92, 60, 34))
FIRE_STONE = bytes((104, 98, 92))
FIRE_LOW = bytes((214, 96, 30))
FIRE_MID = bytes((246, 158, 44))
FIRE_HOT = bytes((252, 226, 130))
FIRE_GLOW = bytes((78, 60, 58))


def draw_door(buf: bytearray, doors: int) -> None:
    """Paint this tile's half of a door into the rock along one edge.

    A wall is six pixels of rock straddling the tile boundary, three either
    side, so neither tile holds a whole door. Both halves are drawn from the
    same columns and the solver is told to pair them, which is what the door
    flags in the matching rule are for.
    """
    rows = range(TILE - 3, TILE) if doors & DOOR_S else range(0, 3)
    for y in rows:
        for x in range(DOOR_X0, DOOR_X1 + 1):
            if x in (DOOR_X0, DOOR_X1):
                col = DUN_DOOR_LO  # jamb
            elif y == (TILE - 3 if doors & DOOR_S else 2):
                col = DUN_DOOR_HI  # lit lintel on the room side
            elif x == (DOOR_X0 + DOOR_X1) // 2:
                col = DUN_DOOR_LO  # the seam between the two leaves
            else:
                col = DUN_DOOR
            set_pixel(buf, x, y, col)
    # One iron stud per half, so the pair reads as a door rather than a plank.
    set_pixel(buf, DOOR_X0 + 1, TILE - 2 if doors & DOOR_S else 1, DUN_IRON)


def draw_fire(buf: bytearray) -> None:
    """A campfire, kept inside the free interior so no seam can see it."""
    for x in range(5, 11):
        set_pixel(buf, x, 11, FIRE_STONE if x in (5, 10) else FIRE_LOG)
    set_pixel(buf, 6, 10, FIRE_LOG)
    set_pixel(buf, 9, 10, FIRE_LOG)
    flame = (
        (7, 10, FIRE_MID), (8, 10, FIRE_MID),
        (7, 9, FIRE_HOT), (8, 9, FIRE_HOT),
        (6, 9, FIRE_LOW), (9, 9, FIRE_LOW),
        (7, 8, FIRE_MID), (8, 8, FIRE_MID),
        (7, 7, FIRE_LOW), (8, 6, FIRE_LOW),
    )
    for x, y, col in flame:
        set_pixel(buf, x, y, col)


def render_dungeon(t: CornerTile) -> TileArt:
    m = room_mask(t.bits)  # no roughening: masonry should read as cut, not eroded
    buf = bytearray(TILE * TILE * 3)
    for y in range(TILE):
        for x in range(TILE):
            if m[y][x]:
                # Flagstones, deliberately flat. Bevelling each slab the way the
                # wall is bevelled makes the floor read as a second wall seen
                # face-on — which is the whole question a viewer has to answer at
                # a glance. Only a faint joint and a slight tint per slab, so the
                # floor stays a surface and depth belongs to the wall alone.
                if x % 8 == 0 or y % 8 == 0:
                    col = DUN_JOINT
                elif (x // 8 + y // 8) % 2:
                    col = DUN_FLOOR_ALT
                else:
                    col = DUN_FLOOR
                if not mask_at(m, x, y - 1):
                    col = DUN_FLOOR_LO  # in shadow of the wall above
            else:
                # Brick courses, every other course offset by half a brick.
                course = y // 4
                if y % 4 == 0:
                    col = DUN_MORTAR
                elif (x + (4 if course % 2 else 0)) % 8 == 0:
                    col = DUN_MORTAR
                elif y % 4 == 1:
                    col = DUN_WALL_HI
                else:
                    col = DUN_WALL
                if mask_at(m, x, y - 1):
                    col = DUN_WALL_HI  # lit cap where the wall meets open floor
            set_pixel(buf, x, y, col)
    if t.doors:
        draw_door(buf, t.doors)
    if t.decor == "fire":
        draw_fire(buf)
    return TileArt(bytes(buf), m)


# ------------------------------------------------------------------- island

SEA = bytes((32, 84, 168))
SEA_DEEP = bytes((20, 58, 128))
SEA_WAVE = bytes((72, 132, 208))
SEA_FOAM = bytes((178, 224, 244))
SAND = bytes((226, 202, 138))
SAND_LO = bytes((186, 158, 98))
GRASS = bytes((74, 146, 58))
GRASS_LO = bytes((46, 110, 42))
GRASS_HI = bytes((124, 186, 76))
TRUNK = bytes((88, 58, 32))
LEAF = bytes((26, 84, 38))
LEAF_LO = bytes((16, 56, 28))
LEAF_HI = bytes((70, 152, 62))
HUT_WALL = bytes((176, 138, 92))
HUT_WALL_LO = bytes((132, 100, 62))
HUT_ROOF = bytes((150, 62, 46))
HUT_ROOF_HI = bytes((196, 96, 70))
HUT_DOOR = bytes((66, 44, 30))


TREE = (
    " ### ",
    "#####",
    "#####",
    " ### ",
    "  |  ",
    "  |  ",
)


def draw_tree(buf: bytearray, x0: int, y0: int) -> None:
    """One tree, five wide, lit from the top left.

    The grass is already speckled with a darker green, so a canopy has to be
    both bigger than that speckle and darker than its darkest dot or it just
    reads as more texture.
    """
    for row, line in enumerate(TREE):
        for col, ch in enumerate(line):
            x, y = x0 + col, y0 + row
            if ch == "|":
                set_pixel(buf, x, y, TRUNK)
            elif ch == "#":
                if row + col <= 2:
                    col_rgb = LEAF_HI
                elif row >= 2 and col >= 3:
                    col_rgb = LEAF_LO
                else:
                    col_rgb = LEAF
                set_pixel(buf, x, y, col_rgb)


def draw_trees(buf: bytearray) -> None:
    # Two trees on a diagonal, kept inside the free interior. Three smaller ones
    # fitted, but at five pixels a canopy stops looking like a tree.
    draw_tree(buf, 3, 3)
    draw_tree(buf, 8, 6)


def draw_hut(buf: bytearray) -> None:
    """A gabled hut, as wide as the free interior allows.

    Drawn small it came out domed and read as a toadstool, so the roof is a
    straight-sided gable overhanging square walls, which is the silhouette that
    says building at this size.
    """
    for row, y in enumerate(range(4, 8)):  # gable, widening by one either side
        for x in range(7 - row, 9 + row):
            set_pixel(buf, x, y, HUT_ROOF_HI if x < 8 else HUT_ROOF)
    for y in range(8, 12):
        for x in range(5, 11):
            set_pixel(buf, x, y, HUT_WALL if x < 8 else HUT_WALL_LO)
    for y in range(9, 12):
        set_pixel(buf, 7, y, HUT_DOOR)
        set_pixel(buf, 8, y, HUT_DOOR)


def render_island(t: CornerTile) -> TileArt:
    m = roughen(corner_mask(t.bits), t.bits + 5)
    buf = bytearray(TILE * TILE * 3)
    for y in range(TILE):
        for x in range(TILE):
            if m[y][x]:
                d = ring_distance(m, x, y, False, 2)
                if d == 1:
                    col = SAND
                elif d == 2:
                    col = SAND_LO
                elif x % 4 == 1 and y % 4 == 2:
                    col = GRASS_LO
                elif x % 8 == 5 and y % 8 == 3:
                    col = GRASS_HI
                else:
                    col = GRASS
            else:
                if ring_distance(m, x, y, True, 1) == 1:
                    col = SEA_FOAM
                elif (x + 2 * (y // 2)) % 8 < 2 and y % 4 == 1:
                    col = SEA_WAVE
                elif x % 4 == 2 and y % 4 == 3:
                    col = SEA_DEEP
                else:
                    col = SEA
            set_pixel(buf, x, y, col)
    if t.decor == "trees":
        draw_trees(buf)
    elif t.decor == "hut":
        draw_hut(buf)
    return TileArt(bytes(buf), m)


# -------------------------------------------------------------- verification


def edge_cells(mask: Mask, side: str) -> list[bool]:
    """An edge minus its two corners — Wang corners are deliberately don't-care."""
    span = range(1, LAST)
    if side == "N":
        return [mask[0][x] for x in span]
    if side == "S":
        return [mask[LAST][x] for x in span]
    if side == "W":
        return [mask[y][0] for y in span]
    return [mask[y][LAST] for y in span]


def verify_corner_seams(name: str, arts: list[TileArt], tiles: list[CornerTile]) -> int:
    """Assert legal neighbours press together strips that agree pixel for pixel.

    Matching only the two touching columns would still let a beach or a bevel
    stop dead at the joint, so the whole BAND-wide strip has to line up. Pairs
    are enumerated by the same rule the solver uses, doors included — a door
    tile whose partner were merely legal by shape would show half a door.
    """
    bad: list[str] = []
    checked = 0
    for a, ta in enumerate(tiles):
        ba = ta.bits
        ne_a, se_a, sw_a = (ba >> 2) & 1, (ba >> 1) & 1, ba & 1
        for b, tb in enumerate(tiles):
            bb = tb.bits
            nw_b, ne_b, sw_b = (bb >> 3) & 1, (bb >> 2) & 1, bb & 1
            ma, mb = arts[a].mask, arts[b].mask
            if ne_a == nw_b and se_a == sw_b:  # b sits to the east of a
                checked += 1
                for k in range(BAND):
                    if [r[LAST - k] for r in ma] != [r[k] for r in mb]:
                        bad.append(f"{a}E|{b}W@{k}")
                        break
            if (
                sw_a == nw_b
                and se_a == ne_b
                and bool(ta.doors & DOOR_S) == bool(tb.doors & DOOR_N)
            ):  # b sits below a
                checked += 1
                for k in range(BAND):
                    if ma[LAST - k] != mb[k]:
                        bad.append(f"{a}S|{b}N@{k}")
                        break
    if bad:
        print(f"  seam check {name}: {checked} joints, {len(bad)} BAD: {', '.join(bad[:8])}")
    else:
        print(f"  seam check {name}: {checked} legal joints, {BAND}px strips identical")
    return len(bad)


def verify_seams(name: str, arts: list[TileArt], specs: list, layout: str) -> int:
    """Assert every pair the solver may place together meets the same material."""
    if layout == "corner":
        return verify_corner_seams(name, arts, specs)
    bits_of = specs
    if layout == "wang":
        n_bit, e_bit, s_bit, w_bit = 8, 4, 2, 1
    else:
        n_bit, e_bit, s_bit, w_bit = 1, 2, 4, 8

    bad: list[str] = []
    checked = 0
    for a, ba in enumerate(bits_of):
        for b, bb in enumerate(bits_of):
            for side_a, side_b, mask_a, mask_b in (
                ("E", "W", e_bit, w_bit),
                ("S", "N", s_bit, n_bit),
            ):
                if bool(ba & mask_a) != bool(bb & mask_b):
                    continue
                checked += 1
                if edge_cells(arts[a].mask, side_a) != edge_cells(arts[b].mask, side_b):
                    bad.append(f"{a}{side_a}|{b}{side_b}")
    if bad:
        print(f"  seam check {name}: {checked} joints, {len(bad)} BAD: {', '.join(bad[:8])}")
    else:
        print(f"  seam check {name}: {checked} legal joints, OK")
    return len(bad)


# ------------------------------------------------------------------- output


def emit_c(out_dir: Path, sets: list[tuple[str, int, bytes]]) -> None:
    h = out_dir / "wfc_tiles.h"
    c = out_dir / "wfc_tiles.c"
    lines_h = [
        "// Auto-generated by make_tiles.py — do not edit.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define WFC_TILE_PX     {TILE}u",
        f"#define WFC_TILE_BYTES  {TILE_BYTES}u",
        f"#define WFC_TILESET_N   {len(sets)}u",
        "",
    ]
    for name, count, _ in sets:
        lines_h.append(f"#define WFC_{name.upper()}_TILES  {count}u")
    lines_h += [
        "",
        "typedef struct {",
        "    const char    *name;",
        "    uint8_t        n;",
        "    const uint8_t *rgb565;",
        "} wfc_tileset_blob_t;",
        "",
        "extern const wfc_tileset_blob_t wfc_tilesets[WFC_TILESET_N];",
        "",
    ]

    lines_c = [
        "// Auto-generated by make_tiles.py — do not edit.",
        '#include "wfc_tiles.h"',
        "",
    ]
    init = []
    for name, count, blob in sets:
        sym = f"wfc_tile_{name}"
        lines_c.append(f"static const uint8_t {sym}[{count * TILE_BYTES}u] = {{")
        for i in range(0, len(blob), 16):
            hexes = ", ".join(f"0x{b:02X}" for b in blob[i : i + 16])
            lines_c.append(f"    {hexes},")
        lines_c.append("};")
        lines_c.append("")
        init.append(f'    {{ "{name}", {count}u, {sym} }},')

    lines_c += [
        "const wfc_tileset_blob_t wfc_tilesets[WFC_TILESET_N] = {",
        *init,
        "};",
        "",
    ]
    h.write_text("\n".join(lines_h), encoding="utf-8")
    c.write_text("\n".join(lines_c), encoding="utf-8")
    print(f">> wrote {h.name}, {c.name}")


def write_preview(path: Path, sets: list[tuple[str, int, bytes]], scale: int = 6) -> None:
    max_n = max(c for _, c, _ in sets)
    pw, ph = max_n * TILE * scale, len(sets) * TILE * scale
    canvas = bytearray(pw * ph * 3)
    for row, (_, count, blob) in enumerate(sets):
        for t in range(count):
            for y in range(TILE):
                for x in range(TILE):
                    so = (t * TILE_BYTES) + (y * TILE + x) * 2
                    v = (blob[so] << 8) | blob[so + 1]
                    r = ((v >> 11) & 0x1F) * 255 // 31
                    g = ((v >> 5) & 0x3F) * 255 // 63
                    b = (v & 0x1F) * 255 // 31
                    for dy in range(scale):
                        for dx in range(scale):
                            px = (t * TILE + x) * scale + dx
                            py = (row * TILE + y) * scale + dy
                            o = (py * pw + px) * 3
                            canvas[o : o + 3] = bytes((r, g, b))
    write_png(path, pw, ph, bytes(canvas))
    print(f">> preview: {path}")


def resize_icon(pixels: bytes, sw: int, sh: int, channels: int, size: int) -> bytes:
    out = bytearray(size * size * 3)
    for y in range(size):
        sy = y * sh // size
        for x in range(size):
            sx = x * sw // size
            r, g, b = rgba_at(pixels, channels, sw, sx, sy)
            o = (y * size + x) * 3
            out[o : o + 3] = bytes((r, g, b))
    return bytes(out)


def icon_to_rgb565(rgb: bytes, size: int) -> bytes:
    out = bytearray(size * size * 2)
    for i in range(size * size):
        o = i * 3
        out[i * 2 : i * 2 + 2] = rgb565_be(rgb[o], rgb[o + 1], rgb[o + 2])
    return bytes(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", type=Path, default=Path(__file__).parent)
    ap.add_argument("--preview", type=Path, default=None)
    ap.add_argument("--icon-src", type=Path, help="source PNG for the 32x32 launcher icon")
    ap.add_argument("--icon-out", type=Path, help="write 32x32 icon.png")
    ap.add_argument("--icon-rgb565", type=Path, help="write launcher rgb565 blob")
    args = ap.parse_args()

    themes = [
        ("circuit", CIRCUIT_EXITS, "circuit", render_circuit),
        ("pipes", WANG_COAST, "wang", render_pipe),
        ("dungeon", DUNGEON_TILES, "corner", render_dungeon),
        ("island", ISLAND_TILES, "corner", render_island),
    ]

    sets: list[tuple[str, int, bytes]] = []
    seams = 0
    for name, specs, layout, render in themes:
        arts = [render(s) for s in specs]
        seams += verify_seams(name, arts, specs, layout)
        blob = b"".join(tile_to_565(a.rgb) for a in arts)
        sets.append((name, len(arts), blob))
        print(f">> {name}: {len(arts)} hand-drawn {TILE}x{TILE} tiles ({len(blob)} bytes)")

    if seams:
        sys.exit(f"error: {seams} seam mismatches — tiles would not join cleanly")

    emit_c(args.out_dir, sets)
    write_preview(args.preview or (args.out_dir / "build" / "tiles_preview.png"), sets)

    if args.icon_src:
        w, h, ch, px = read_png(args.icon_src)
        rgb = resize_icon(px, w, h, ch, 32)
        if args.icon_out:
            write_png(args.icon_out, 32, 32, rgb)
            print(f">> icon png: {args.icon_out}")
        if args.icon_rgb565:
            args.icon_rgb565.parent.mkdir(parents=True, exist_ok=True)
            args.icon_rgb565.write_bytes(icon_to_rgb565(rgb, 32))
            print(f">> icon rgb565: {args.icon_rgb565} ({32 * 32 * 2} bytes)")


if __name__ == "__main__":
    main()
