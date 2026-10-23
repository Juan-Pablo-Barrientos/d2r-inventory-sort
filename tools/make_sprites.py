"""Draws the Sort and Compact button sprites used by the plugin.

usage: py -I tools/make_sprites.py [preview.png]

Writes assets/<icon>_button.sprite (64x64 frames) and
assets/<icon>_button.lowend.sprite (32x32 frames) for each icon. D2R sprites are a 40-byte
SpA1 header followed by uncompressed RGBA. Button frames sit side by side:
normal, pressed, disabled, hovered.
"""
import math
import os
import random
import struct
import sys
import zlib

FRAME = 64
SS = 4  # supersampling factor
FRAMES = ("normal", "pressed", "disabled", "hovered")

GOLD = (201, 165, 87)
GOLD_HI = (240, 210, 130)
GOLD_DARK = (110, 82, 36)
IRON = (58, 56, 54)
IRON_DARK = (22, 21, 20)


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def inside_round_rect(x, y, x0, y0, x1, y1, r):
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def sort_icon(x, y):
    """Three bars of decreasing length and a down arrow, in 64-unit space."""
    for top, length in ((18, 30), (29, 22), (40, 14)):
        if 12 <= x <= 12 + length and top <= y <= top + 6:
            return True
    # Arrow shaft and head.
    if 45 <= x <= 49 and 14 <= y <= 38:
        return True
    if 38 <= y <= 48 and abs(x - 47) <= (48 - y) * 0.8:
        return True
    return False


def compact_icon(x, y):
    """Packed blocks on the left and a dashed empty block on the right."""
    for x0, y0, x1, y1 in ((12, 14, 22, 50), (25, 14, 34, 30), (25, 33, 34, 50)):
        if x0 <= x <= x1 and y0 <= y <= y1:
            return True
    # Dashed outline of the free space.
    x0, y0, x1, y1, stroke = 39, 14, 52, 50, 2.5
    on_vertical = (x0 <= x <= x0 + stroke or x1 - stroke <= x <= x1) and y0 <= y <= y1
    on_horizontal = (y0 <= y <= y0 + stroke or y1 - stroke <= y <= y1) and x0 <= x <= x1
    if on_vertical and (y - y0) % 7 < 4.5:
        return True
    if on_horizontal and (x - x0) % 7 < 4.5:
        return True
    return False


ICONS = {"sort": sort_icon, "compact": compact_icon}


def icon_coverage(icon, x, y, offset):
    return ICONS[icon](x - offset, y - offset)


def icon_outline(icon, x, y, offset):
    for dx, dy in ((-1.5, 0), (1.5, 0), (0, -1.5), (0, 1.5), (1.5, 1.5)):
        if icon_coverage(icon, x + dx, y + dy, offset):
            return True
    return False


def draw_frame(kind, icon, rng_seed=7):
    rng = random.Random(rng_seed)
    size = FRAME * SS
    noise = [[rng.uniform(-1, 1) for _ in range(FRAME)] for _ in range(FRAME)]
    pixels = []
    for py in range(size):
        for px in range(size):
            x = (px + 0.5) / SS
            y = (py + 0.5) / SS
            color = None
            alpha = 0.0
            if inside_round_rect(x, y, 1, 1, 63, 63, 6):
                alpha = 1.0
                # Iron frame with a bevel: light top-left, dark bottom-right.
                bevel = (x + y) / 128.0
                color = mix((92, 88, 84), IRON_DARK, bevel)
                if inside_round_rect(x, y, 5, 5, 59, 59, 4):
                    color = GOLD_DARK if kind != "disabled" else (70, 68, 66)
                if inside_round_rect(x, y, 7, 7, 57, 57, 3):
                    # Stone face with a little texture.
                    n = noise[int(y)][int(x)] * 6
                    shade = 34 + (1 - y / 64) * 12 + n
                    if kind == "pressed":
                        shade -= 8
                    if kind == "hovered":
                        shade += 10
                    color = (shade, shade * 0.97, shade * 0.93)
                    offset = 1 if kind == "pressed" else 0
                    if icon_coverage(icon, x, y, offset):
                        if kind == "disabled":
                            color = (88, 86, 84)
                        else:
                            t = (y - 14) / 34
                            top = GOLD_HI if kind == "hovered" else mix(GOLD_HI, GOLD, 0.4)
                            color = mix(top, GOLD_DARK if kind == "pressed" else GOLD, max(0, min(1, t)))
                    elif icon_outline(icon, x, y, offset):
                        color = (12, 10, 8)
            if color is None:
                pixels.append((0, 0, 0, 0))
            else:
                if kind == "disabled":
                    g = sum(color) / 3 * 0.75
                    color = (g, g, g)
                pixels.append((color[0], color[1], color[2], alpha))
    # Downsample.
    out = []
    for fy in range(FRAME):
        row = []
        for fx in range(FRAME):
            r = g = b = a = 0.0
            for sy in range(SS):
                base = (fy * SS + sy) * size + fx * SS
                for sx in range(SS):
                    pr, pg, pb, pa = pixels[base + sx]
                    r += pr * pa
                    g += pg * pa
                    b += pb * pa
                    a += pa
            n = SS * SS
            if a > 0:
                row.append((r / a, g / a, b / a, a / n))
            else:
                row.append((0, 0, 0, 0))
        out.append(row)
    return out


def halve(frame):
    out = []
    for y in range(0, FRAME, 2):
        row = []
        for x in range(0, FRAME, 2):
            quad = [frame[y][x], frame[y][x + 1], frame[y + 1][x], frame[y + 1][x + 1]]
            a = sum(p[3] for p in quad)
            if a == 0:
                row.append((0, 0, 0, 0))
                continue
            row.append(tuple(sum(p[i] * p[3] for p in quad) / a for i in range(3)) + (a / 4,))
        out.append(row)
    return out


def to_rgba(frames):
    height = len(frames[0])
    rows = bytearray()
    for y in range(height):
        for frame in frames:
            for r, g, b, a in frame[y]:
                rows += bytes((round(r), round(g), round(b), round(a * 255)))
    return bytes(rows)


def sprite(frames):
    frame_width = len(frames[0][0])
    height = len(frames[0])
    header = struct.pack("<4sHHIIII", b"SpA1", 31, frame_width, frame_width * len(frames), height, 0, len(frames))
    return header.ljust(40, b"\0") + to_rgba(frames)


def png(path, frames, scale=4):
    width = len(frames[0][0]) * len(frames)
    height = len(frames[0])
    rgba = to_rgba(frames)
    raw = bytearray()
    for y in range(height):
        line = b"".join(rgba[(y * width + x) * 4:(y * width + x) * 4 + 4] * scale for x in range(width))
        raw += (b"\0" + line) * scale

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width * scale, height * scale, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw))))
        f.write(chunk(b"IEND", b""))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    previews = []
    for icon in ICONS:
        frames = [draw_frame(kind, icon) for kind in FRAMES]
        low = [halve(frame) for frame in frames]
        with open(os.path.join(root, "assets", icon + "_button.sprite"), "wb") as f:
            f.write(sprite(frames))
        with open(os.path.join(root, "assets", icon + "_button.lowend.sprite"), "wb") as f:
            f.write(sprite(low))
        previews.append(frames)
    if len(sys.argv) > 1:
        png(sys.argv[1], [frame for frames in previews for frame in frames])


if __name__ == "__main__":
    main()
