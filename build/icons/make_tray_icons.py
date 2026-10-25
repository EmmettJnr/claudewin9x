#!/usr/bin/env python3
"""Generates the remote client's tray icons: two dial-up style monitors.

Writes 16x16, 16-colour .ico files next to this script (no dependencies):

  tray_off.ico    both screens dark (not connected)
  tray_idle.ico   both screens on (connected, waiting)
  tray_left.ico   left screen lit (traffic)
  tray_right.ico  right screen lit (traffic)
  tray_both.ico   both screens lit

  python3 make_tray_icons.py [--preview preview.png]
"""

import os
import struct
import sys
import zlib

# Standard VGA palette, so the icons look right in 16-colour display modes
PALETTE = [
    (0x00, 0x00, 0x00), (0x80, 0x00, 0x00), (0x00, 0x80, 0x00), (0x80, 0x80, 0x00),
    (0x00, 0x00, 0x80), (0x80, 0x00, 0x80), (0x00, 0x80, 0x80), (0xC0, 0xC0, 0xC0),
    (0x80, 0x80, 0x80), (0xFF, 0x00, 0x00), (0x00, 0xFF, 0x00), (0xFF, 0xFF, 0x00),
    (0x00, 0x00, 0xFF), (0xFF, 0x00, 0xFF), (0x00, 0xFF, 0xFF), (0xFF, 0xFF, 0xFF),
]
BLACK, TEAL, SILVER, GREY, AQUA, WHITE = 0, 6, 7, 8, 14, 15

# Back monitor (A) top left, front monitor (B) bottom right.
# k outline, w highlight, g case, d shadow, A/B screens, . transparent
ART = [
    "kkkkkkkkkk......",
    "kwwwwwwwwk......",
    "kwAAAAAAdk......",
    "kwAAAAAAdk......",
    "kwAAAAAAdk......",
    "kwAAAAAAdk......",
    "kgggggkkkkkkkkkk",
    "kkkkkkkwwwwwwwwk",
    "..kddkkwBBBBBBdk",
    ".kddddkwBBBBBBdk",
    ".kkkkkkwBBBBBBdk",
    "......kwBBBBBBdk",
    "......kgggggggdk",
    "......kkkkkkkkkk",
    ".........kddk...",
    ".......kddddddk.",
]

FIXED = {"k": BLACK, "w": WHITE, "g": SILVER, "d": GREY}

# Screen states: dark, on (idle) or lit (traffic, with a glint)
OFF, ON, LIT = "off", "on", "lit"

ICONS = {
    "tray_off": (OFF, OFF),
    "tray_idle": (ON, ON),
    "tray_left": (LIT, ON),
    "tray_right": (ON, LIT),
    "tray_both": (LIT, LIT),
}


def render(screen_a, screen_b):
    """Returns 16 rows of palette indexes, None for transparent."""
    first = {"A": None, "B": None}
    rows = []
    for y, line in enumerate(ART):
        row = []
        for x, ch in enumerate(line):
            if ch == ".":
                row.append(None)
            elif ch in FIXED:
                row.append(FIXED[ch])
            else:
                state = screen_a if ch == "A" else screen_b
                if first[ch] is None:
                    first[ch] = (x, y)
                if state == OFF:
                    row.append(BLACK)
                elif state == ON:
                    row.append(TEAL)
                else:
                    row.append(WHITE if (x, y) == first[ch] else AQUA)
        rows.append(row)
    return rows


def ico_bytes(pixels):
    xor = bytearray()
    mask = bytearray()
    for row in reversed(pixels):  # DIBs are bottom-up
        for x in range(0, 16, 2):
            hi = pixels_or_black(row[x])
            lo = pixels_or_black(row[x + 1])
            xor.append((hi << 4) | lo)
        bits = 0
        for x, value in enumerate(row):
            if value is None:
                bits |= 0x8000 >> x
        mask += struct.pack(">H", bits) + b"\0\0"  # rows padded to 4 bytes

    header = struct.pack("<IiiHHIIiiII", 40, 16, 32, 1, 4, 0, len(xor) + len(mask), 0, 0, 16, 0)
    palette = b"".join(struct.pack("BBBB", b, g, r, 0) for r, g, b in PALETTE)
    image = header + palette + bytes(xor) + bytes(mask)
    directory = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", 16, 16, 16, 0, 1, 4, len(image), 22)
    return directory + image


def pixels_or_black(value):
    return BLACK if value is None else value


def write_preview(path, images, scale=8):
    """A strip of all icons, enlarged, on a taskbar-silver background."""
    gap = 4
    width = len(images) * (16 + gap) * scale
    height = 16 * scale
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for pixels in images:
            for x in range((16 + gap) * scale):
                px, py = x // scale, y // scale
                value = pixels[py][px] if px < 16 else None
                raw += bytes((0xC0, 0xC0, 0xC0) if value is None else PALETTE[value])

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    images = []
    for name, (a, b) in ICONS.items():
        pixels = render(a, b)
        images.append(pixels)
        with open(os.path.join(here, name + ".ico"), "wb") as f:
            f.write(ico_bytes(pixels))

    if len(sys.argv) == 3 and sys.argv[1] == "--preview":
        write_preview(sys.argv[2], images)


if __name__ == "__main__":
    main()
