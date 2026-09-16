"""Compose Latin-1 cells for the GUIFont atlas (GUIFont.cpp Load).

The atlas is 16 glyphs per row from U+0020, a red separator at (0, 0) and
on each glyph's scanline, colour-key at top-right. Width is the run to the
next red minus one. This tool keeps 0x20-0x7F cells byte-identical and
fills 0x80-0xFF, except the engine HUD-icon indexes listed below. Those
cells are restored from the wave tip and pinned; the generator refuses
to emit if any of them would change. Names use Menus/FontSmall through
GlyphFontFor (GUIFont.cpp:143), so FontLarge HUD cells are not the
letter atlas and Skins/FontSmall HUD cells are not name letters.
"""

from __future__ import annotations

import hashlib
import subprocess
import sys
from io import BytesIO
from pathlib import Path

from PIL import Image
from PIL.PngImagePlugin import PngInfo

ROOT = Path(__file__).resolve().parents[2]
FONT_PATHS = [
    ROOT / "Data/Base.rte/GUIs/Skins/Menus/FontSmall.png",
    ROOT / "Data/Base.rte/GUIs/Skins/Menus/FontLarge.png",
    ROOT / "Data/Base.rte/GUIs/Skins/FontSmall.png",
    ROOT / "Data/Base.rte/GUIs/Skins/FontLarge.png",
]
CONTACT_PATH = Path(__file__).resolve().parent / "latin1-contact-sheet.png"
ASCII_END = 128
ROW_COUNT = 16
FIRST = 32

# Printable cp1252 in 0x80-0x9F; the unused slots stay empty.
CP1252_UNUSED = frozenset({0x81, 0x8D, 0x8F, 0x90, 0x9D})

# Signed-char HUD indexes the engine draws (unsigned = signed + 256).
# FontSmall HUD lives only on Skins/FontSmall.png (GetSmallFont(), FrameMan.cpp:1268).
# Menus/FontSmall.png is the name atlas; 0xCF/0xD5/0xD6 stay Ï/Õ/Ö there.
#   0xCF  AHuman.cpp:3672  -49 pickup prefix
#   0xD5  DataModule.cpp:57,122  -43 via LoadingScreen.cpp:139
#   0xD6  Reader.cpp:550  -42 via LoadingScreen.cpp:139
FONT_SMALL_HUD = frozenset({0xCF, 0xD5, 0xD6})
WAVE_TIP = "6447c4c2e3"
# FontLarge: GetLargeFont() and TitleScreen's Menus/FontLarge.png.
#   0xC2  GameActivity.cpp:2824  -62 team-one (commented draw; cell is the icon)
#   0xC5  GameActivity.cpp:2824  -59 team-two
#   0xC6  Actor.cpp:2707 / GameActivity.cpp:2820 / Metagame / Scenario  -58 gold
#   0xC7  Actor.cpp:2707 / GameActivity.cpp:2820  -57 gold-picked
#   0xC8  AHuman.cpp:3551 / ACrab.cpp:1662  -56 ammo
#   0xCF  AHuman.cpp:3563  -49 hand trio
#   0xD0  MetagameGUI.cpp:536  -48 brain
#   0xD1  MetagameGUI.cpp:4576  -47
#   0xD2  MetagameGUI.cpp:4576  -46
#   0xD6  HeldDevice.cpp:546  -42 pickup arrow
#   0xD7  HeldDevice.cpp:549  -41
#   0xD8  HeldDevice.cpp:552  -40
#   0xD9  Actor.cpp:2695 / MetagameGUI.cpp:5508  -39 death
#   0xDB  AHuman.cpp:3563  -37 hand
#   0xDC  MetagameGUI.cpp:5170  -36
#   0xDD  TitleScreen.cpp:562  -35 copyright on Menus/FontLarge
#   0xE1..0xE7  AHuman.cpp:3633 / ACrab.cpp:1676  jet -31..-25
#   0xEA  MetagameGUI.cpp:458  -22 menu
FONT_LARGE_HUD = frozenset({
    0xC2, 0xC5, 0xC6, 0xC7, 0xC8, 0xCF,
    0xD0, 0xD1, 0xD2, 0xD6, 0xD7, 0xD8, 0xD9,
    0xDB, 0xDC, 0xDD,
    0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xEA,
})


def hud_keep_set(path):
    path = Path(path)
    if path.name == "FontSmall.png" and "Menus" not in path.parts:
        return FONT_SMALL_HUD
    if path.name == "FontLarge.png":
        return FONT_LARGE_HUD
    return frozenset()

# Accented letters: compose from the font's own base + a diacritic.
ACCENTED = {
    0xC0: ("A", "grave"),
    0xC1: ("A", "acute"),
    0xC2: ("A", "circumflex"),
    0xC3: ("A", "tilde"),
    0xC4: ("A", "diaeresis"),
    0xC5: ("A", "ring"),
    0xC7: ("C", "cedilla"),
    0xC8: ("E", "grave"),
    0xC9: ("E", "acute"),
    0xCA: ("E", "circumflex"),
    0xCB: ("E", "diaeresis"),
    0xCC: ("I", "grave"),
    0xCD: ("I", "acute"),
    0xCE: ("I", "circumflex"),
    0xCF: ("I", "diaeresis"),
    0xD1: ("N", "tilde"),
    0xD2: ("O", "grave"),
    0xD3: ("O", "acute"),
    0xD4: ("O", "circumflex"),
    0xD5: ("O", "tilde"),
    0xD6: ("O", "diaeresis"),
    0xD9: ("U", "grave"),
    0xDA: ("U", "acute"),
    0xDB: ("U", "circumflex"),
    0xDC: ("U", "diaeresis"),
    0xDD: ("Y", "acute"),
    0xE0: ("a", "grave"),
    0xE1: ("a", "acute"),
    0xE2: ("a", "circumflex"),
    0xE3: ("a", "tilde"),
    0xE4: ("a", "diaeresis"),
    0xE5: ("a", "ring"),
    0xE7: ("c", "cedilla"),
    0xE8: ("e", "grave"),
    0xE9: ("e", "acute"),
    0xEA: ("e", "circumflex"),
    0xEB: ("e", "diaeresis"),
    0xEC: ("l", "grave"),
    0xED: ("l", "acute"),
    0xEE: ("l", "circumflex"),
    0xEF: ("l", "diaeresis"),
    0xF1: ("n", "tilde"),
    0xF2: ("o", "grave"),
    0xF3: ("o", "acute"),
    0xF4: ("o", "circumflex"),
    0xF5: ("o", "tilde"),
    0xF6: ("o", "diaeresis"),
    0xF9: ("u", "grave"),
    0xFA: ("u", "acute"),
    0xFB: ("u", "circumflex"),
    0xFC: ("u", "diaeresis"),
    0xFD: ("y", "acute"),
    0xFF: ("y", "diaeresis"),
}

# Spacing marks and caron letters.
MARKS = {
    0x88: (" ", "circumflex"),
    0x98: (" ", "tilde"),
    0xA8: (" ", "diaeresis"),
    0xAF: (" ", "macron"),
    0xB4: (" ", "acute"),
    0xB8: (" ", "cedilla"),
    0x8A: ("S", "caron"),
    0x8E: ("Z", "caron"),
    0x9A: ("s", "caron"),
    0x9E: ("z", "caron"),
    0x9F: ("Y", "diaeresis"),
}


class Cell:
    __slots__ = ("width", "rows")

    def __init__(self, width, height, bg):
        self.width = width
        self.rows = [[bg] * width for _ in range(height)]

    def copy(self):
        other = Cell(self.width, len(self.rows), self.rows[0][0] if self.width else (0, 0, 0, 0))
        other.rows = [row[:] for row in self.rows]
        return other


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def parse_font(path, source=None):
    original = source if source is not None else Image.open(path)
    rgba = original.convert("RGBA")
    width, height = rgba.size
    pix = rgba.load()
    red = pix[0, 0]
    bg = pix[width - 1, 0]
    font_h = 0
    for y in range(1, height):
        if pix[0, y] == red:
            font_h = y
            break
    if font_h <= 0:
        raise RuntimeError(f"{path}: no row separator on x=0")
    cells = {}
    x = 1
    y = 0
    on_line = 0
    for code in range(FIRST, 256):
        scan = 0
        n = x
        while n < width and pix[n, y] != red:
            n += 1
            scan += 1
        cell_w = max(0, scan - 1)
        cell = Cell(cell_w, font_h, bg)
        for j in range(font_h):
            for i in range(cell_w):
                if x + i < width and y + j < height:
                    cell.rows[j][i] = pix[x + i, y + j]
        cells[code] = cell
        x += scan + 1
        on_line += 1
        if on_line >= ROW_COUNT:
            on_line = 0
            x = 1
            y += font_h
            if y + font_h > height:
                break
    return {
        "path": Path(path),
        "original": original,
        "rgba": rgba,
        "red": red,
        "bg": bg,
        "font_h": font_h,
        "cells": cells,
    }


def ink_colors(cell, red, bg):
    colors = set()
    for row in cell.rows:
        for pixel in row:
            if pixel != red and pixel != bg:
                colors.add(pixel)
    return colors


def ink_count(cell, red, bg):
    return sum(1 for row in cell.rows for pixel in row if pixel != red and pixel != bg)


def style_of(font):
    """Stroke colours and vertical bands from the font's own A/a/g/`."""
    cells = font["cells"]
    red, bg = font["red"], font["bg"]
    a = cells[ord("A")]
    counts = {}
    for row in a.rows:
        for pixel in row:
            if pixel != red and pixel != bg:
                counts[pixel] = counts.get(pixel, 0) + 1
    ranked = sorted(counts, key=counts.get, reverse=True)
    hi = ranked[0] if ranked else (255, 255, 255, 255)
    lo = ranked[1] if len(ranked) > 1 else hi
    mid = ranked[2] if len(ranked) > 2 else lo

    def first_ink(code):
        cell = cells[ord(code)]
        for y, row in enumerate(cell.rows):
            if any(pixel != red and pixel != bg for pixel in row):
                return y
        return 2

    def last_ink(code):
        cell = cells[ord(code)]
        for y in range(len(cell.rows) - 1, -1, -1):
            if any(pixel != red and pixel != bg for pixel in cell.rows[y]):
                return y
        return font["font_h"] - 3

    return {
        "hi": hi,
        "lo": lo,
        "mid": mid,
        "cap_top": first_ink("A"),
        "lc_top": first_ink("a"),
        "base": last_ink("A"),
        "desc": last_ink("g"),
        "ascii_colors": frozenset().union(*(ink_colors(cells[c], red, bg) for c in range(33, 127) if c in cells)),
    }


def is_hud(code, font):
    return code in hud_keep_set(font["path"])


def empty_cell(font, width):
    return Cell(max(1, width), font["font_h"], font["bg"])


def copy_cell(font, ch):
    return font["cells"][ord(ch) if isinstance(ch, str) else ch].copy()


def put_hi(cell, x, y, style, font):
    if y < 0 or y >= len(cell.rows) or x < 0 or x >= cell.width:
        return
    if cell.rows[y][x] in (font["red"],):
        return
    cell.rows[y][x] = style["hi"]
    sx, sy = x + 1, y + 1
    if 0 <= sy < len(cell.rows) and 0 <= sx < cell.width and cell.rows[sy][sx] == font["bg"]:
        cell.rows[sy][sx] = style["lo"]


def clear_band(cell, y0, y1, font):
    for y in range(max(0, y0), min(len(cell.rows), y1)):
        for x in range(cell.width):
            if cell.rows[y][x] != font["red"]:
                cell.rows[y][x] = font["bg"]


def widen(cell, width, font):
    if width <= cell.width:
        return cell
    out = empty_cell(font, width)
    for y, row in enumerate(cell.rows):
        for x, pixel in enumerate(row):
            out.rows[y][x] = pixel
    return out


def blit(dst, src, dx, dy, font):
    for y, row in enumerate(src.rows):
        ty = y + dy
        if ty < 0 or ty >= len(dst.rows):
            continue
        for x, pixel in enumerate(row):
            tx = x + dx
            if tx < 0 or tx >= dst.width:
                continue
            if pixel != font["bg"] and pixel != font["red"]:
                dst.rows[ty][tx] = pixel


def hcombine(font, left_ch, right_ch, overlap):
    left = copy_cell(font, left_ch)
    right = copy_cell(font, right_ch)
    width = max(left.width, left.width + right.width - overlap)
    out = empty_cell(font, width)
    blit(out, left, 0, 0, font)
    blit(out, right, left.width - overlap, 0, font)
    return out


def mark_points(kind, width, band, thick):
    """Logical stroke points for a diacritic inside an accent band."""
    top = max(0, band - (2 if thick == 1 else 3))
    mid = min(width // 2, max(0, width - 2))
    right = max(0, width - 2)
    if kind == "grave":
        return [(0, top), (0, top + 1)] if thick == 1 else [(0, top), (1, top + 1), (1, top + 2)]
    if kind == "acute":
        return [(right, top), (max(0, right - 1), top + 1)] if thick == 1 else [(right, top), (right - 1, top + 1), (max(0, right - 2), top + 2)]
    if kind == "circumflex":
        return [(mid, top), (max(0, mid - 1), top + 1), (min(width - 1, mid + 1), top + 1)]
    if kind == "caron":
        return [(max(0, mid - 1), top), (min(width - 1, mid + 1), top), (mid, top + 1)]
    if kind == "tilde":
        return [(0, top + 1), (1, top), (min(width - 1, 2), top + 1)] if width <= 4 else [(1, top + 1), (2, top), (3, top), (4, top + 1)]
    if kind == "diaeresis":
        gap = 2 if width >= 4 else 1
        return [(0, top), (min(width - 1, gap + 1), top)]
    if kind == "ring":
        # Closed 2x2 loop in the accent band, not the circumflex chevron.
        left = max(0, mid - 1)
        right = min(width - 1, left + 1)
        if right == left and left + 1 < width:
            right = left + 1
        return [(left, top), (right, top), (left, top + 1), (right, top + 1)]
    if kind == "macron":
        return [(x, top) for x in range(min(width, 3 if width < 5 else width - 1))]
    if kind == "cedilla":
        return []
    return []


def add_diacritic(cell, kind, style, font, lower):
    band = style["lc_top"] if lower else style["cap_top"]
    thick = 1 if font["font_h"] <= 12 else 2
    if kind == "cedilla":
        y = min(len(cell.rows) - 2, style["base"] + 1)
        put_hi(cell, max(0, cell.width // 2 - 1), y, style, font)
        put_hi(cell, max(0, cell.width // 2), min(len(cell.rows) - 1, y + 1), style, font)
        return cell
    if kind == "slash":
        h = len(cell.rows)
        for i in range(min(cell.width, h - style["cap_top"])):
            put_hi(cell, i, min(h - 2, style["base"] - i), style, font)
        return cell
    for x, y in mark_points(kind, cell.width, band, thick):
        put_hi(cell, x, y, style, font)
    return cell


def compose_letter(font, style, base, kind):
    cell = copy_cell(font, base)
    if base == " ":
        cell = empty_cell(font, max(4, font["cells"][ord("A")].width))
    lower = base.islower() or base == "l"
    return add_diacritic(cell, kind, style, font, lower)


def invert_vert(font, ch):
    src = copy_cell(font, ch)
    red, bg = font["red"], font["bg"]
    ys = [y for y, row in enumerate(src.rows) if any(pixel != red and pixel != bg for pixel in row)]
    if not ys:
        return src
    out = empty_cell(font, src.width)
    y0, y1 = ys[0], ys[-1]
    for y in range(y0, y1 + 1):
        out.rows[y0 + (y1 - y)] = src.rows[y][:]
    return out


def add_bar(cell, style, font, y):
    for x in range(cell.width):
        put_hi(cell, x, y, style, font)
    return cell


def compose_circled(font, letter):
    """O's ring around a C or R, so ©/® are not a bare letter."""
    ring = copy_cell(font, "O")
    mark = copy_cell(font, letter)
    width = max(ring.width, mark.width)
    out = empty_cell(font, width)
    blit(out, ring, max(0, (width - ring.width) // 2), 0, font)
    mx = max(1, (width - max(1, mark.width - 2)) // 2)
    for y in range(1, len(mark.rows) - 1):
        for x in range(1, mark.width - 1):
            pixel = mark.rows[y][x]
            if pixel == font["bg"] or pixel == font["red"]:
                continue
            tx = mx + (x - 1)
            if 0 <= tx < out.width:
                out.rows[y][tx] = pixel
    return out


def special(font, style, code):
    """Hand-defined or composed non-letter cp1252 / Latin-1 symbols."""
    a_w = font["cells"][ord("A")].width
    space_w = max(2, font["cells"][32].width)

    if code in CP1252_UNUSED or code == 0xA0:
        return empty_cell(font, space_w)
    if code == 0xAD:
        return copy_cell(font, "-")

    if code == 0xA1:
        return invert_vert(font, "!")
    if code == 0xBF:
        return invert_vert(font, "?")
    if code == 0xAB:
        return hcombine(font, "<", "<", 1)
    if code == 0xBB:
        return hcombine(font, ">", ">", 1)
    if code == 0x8B:
        return copy_cell(font, "<")
    if code == 0x9B:
        return copy_cell(font, ">")

    if code == 0xD7:
        return copy_cell(font, "x")
    if code == 0xF7:
        cell = copy_cell(font, "-")
        put_hi(cell, cell.width // 2, style["lc_top"], style, font)
        put_hi(cell, cell.width // 2, min(len(cell.rows) - 2, style["base"]), style, font)
        return cell
    if code == 0xB1:
        cell = copy_cell(font, "+")
        add_bar(cell, style, font, min(len(cell.rows) - 2, style["base"]))
        return cell
    if code == 0xB7:
        cell = empty_cell(font, 2)
        put_hi(cell, 0, style["lc_top"] + 1, style, font)
        return cell
    if code == 0xB0:
        cell = empty_cell(font, 3)
        add_diacritic(cell, "ring", style, font, False)
        return cell
    if code == 0xB2:
        cell = copy_cell(font, "2")
        out = empty_cell(font, cell.width)
        blit(out, cell, 0, -2, font)
        return out
    if code == 0xB3:
        cell = copy_cell(font, "3")
        out = empty_cell(font, cell.width)
        blit(out, cell, 0, -2, font)
        return out
    if code == 0xB9:
        cell = copy_cell(font, "1")
        out = empty_cell(font, cell.width)
        blit(out, cell, 0, -2, font)
        return out

    if code == 0xA3:
        cell = copy_cell(font, "L")
        add_bar(cell, style, font, style["cap_top"] + 1)
        return cell
    if code == 0xA5:
        cell = copy_cell(font, "Y")
        add_bar(cell, style, font, style["cap_top"] + 2)
        return cell
    if code == 0xA2:
        cell = copy_cell(font, "c")
        for y in range(style["lc_top"], style["base"] + 1):
            put_hi(cell, cell.width // 2, y, style, font)
        return cell
    if code == 0x80:
        cell = copy_cell(font, "C")
        add_bar(cell, style, font, style["cap_top"] + 1)
        add_bar(cell, style, font, style["cap_top"] + 2)
        return cell
    if code == 0xA4:
        cell = copy_cell(font, "o")
        put_hi(cell, 0, style["lc_top"], style, font)
        put_hi(cell, cell.width - 1, style["lc_top"], style, font)
        put_hi(cell, 0, style["base"], style, font)
        put_hi(cell, cell.width - 1, style["base"], style, font)
        return cell
    if code == 0xA6:
        cell = copy_cell(font, "|")
        mid = len(cell.rows) // 2
        clear_band(cell, mid, mid + 1, font)
        return cell
    if code == 0xAC:
        cell = copy_cell(font, "-")
        put_hi(cell, cell.width - 1, style["lc_top"] + 2, style, font)
        return cell
    if code == 0xA7:
        return hcombine(font, "S", "s", 2)
    if code == 0xB6:
        cell = copy_cell(font, "P")
        put_hi(cell, 0, style["base"], style, font)
        return cell
    if code == 0xA9:
        return compose_circled(font, "C")
    if code == 0xAE:
        return compose_circled(font, "R")
    if code == 0x99:
        return hcombine(font, "T", "M", 2)
    if code == 0xAA:
        cell = copy_cell(font, "a")
        add_bar(cell, style, font, min(len(cell.rows) - 1, style["base"] + 1))
        return cell
    if code == 0xBA:
        cell = copy_cell(font, "o")
        add_bar(cell, style, font, min(len(cell.rows) - 1, style["base"] + 1))
        return cell
    if code == 0xB5:
        cell = copy_cell(font, "u")
        put_hi(cell, 0, min(len(cell.rows) - 1, style["desc"]), style, font)
        return cell

    if code == 0x82:
        return copy_cell(font, ",")
    if code == 0x84:
        return hcombine(font, ",", ",", 0)
    if code == 0x91:
        return copy_cell(font, "`")
    if code == 0x92:
        return copy_cell(font, "'")
    if code == 0x93:
        return copy_cell(font, '"')
    if code == 0x94:
        return copy_cell(font, '"')
    if code == 0x85:
        cell = empty_cell(font, 6)
        for x in (0, 2, 4):
            put_hi(cell, x, style["base"], style, font)
        return cell
    if code == 0x95:
        cell = empty_cell(font, 3)
        put_hi(cell, 0, style["lc_top"] + 1, style, font)
        put_hi(cell, 1, style["lc_top"] + 1, style, font)
        return cell
    if code == 0x96:
        return copy_cell(font, "-")
    if code == 0x97:
        return hcombine(font, "-", "-", 1)
    if code == 0x86:
        cell = copy_cell(font, "+")
        for y in range(style["cap_top"], style["base"] + 1):
            put_hi(cell, cell.width // 2, y, style, font)
        return cell
    if code == 0x87:
        cell = special(font, style, 0x86)
        add_bar(cell, style, font, style["cap_top"] + 2)
        return cell
    if code == 0x83:
        return copy_cell(font, "f")
    if code == 0x89:
        return hcombine(font, "0", "/", 1)

    if code in (0xBC, 0xBD, 0xBE):
        num = {0xBC: "1", 0xBD: "1", 0xBE: "3"}[code]
        den = {0xBC: "4", 0xBD: "2", 0xBE: "4"}[code]
        out = empty_cell(font, max(5, a_w + 1))
        blit(out, copy_cell(font, num), 0, -2, font)
        put_hi(out, out.width // 2, style["lc_top"] + 1, style, font)
        blit(out, copy_cell(font, den), 1, 2, font)
        return out

    # Ligatures and crossed letters: documented pixel constructions.
    if code == 0xC6:
        return hcombine(font, "A", "E", 2)
    if code == 0xE6:
        return hcombine(font, "a", "e", 2)
    if code == 0x8C:
        return hcombine(font, "O", "E", 2)
    if code == 0x9C:
        return hcombine(font, "o", "e", 2)
    if code == 0xD8:
        return add_diacritic(copy_cell(font, "O"), "slash", style, font, False)
    if code == 0xF8:
        return add_diacritic(copy_cell(font, "o"), "slash", style, font, True)
    if code == 0xD0:
        cell = copy_cell(font, "D")
        add_bar(cell, style, font, style["cap_top"] + 2)
        return cell
    if code == 0xF0:
        cell = copy_cell(font, "d")
        add_bar(cell, style, font, style["lc_top"])
        return cell
    if code == 0xDE:
        return copy_cell(font, "P")
    if code == 0xFE:
        return hcombine(font, "b", "p", 3) if font["cells"][ord("b")].width >= 4 else copy_cell(font, "p")
    if code == 0xDF:
        cell = copy_cell(font, "B")
        put_hi(cell, cell.width - 1, min(len(cell.rows) - 1, style["base"] + 1), style, font)
        return cell

    return empty_cell(font, space_w)


def build_glyph(font, style, code):
    if code in ACCENTED:
        base, kind = ACCENTED[code]
        return compose_letter(font, style, base, kind), base
    if code in MARKS:
        base, kind = MARKS[code]
        return compose_letter(font, style, base, kind), base if base != " " else kind
    return special(font, style, code), "?"


def load_wave_tip_cells(path):
    """HUD pin cells from the wave tip, not from an already-emitted atlas."""
    rel = Path(path).resolve().relative_to(ROOT).as_posix()
    raw = subprocess.check_output(["git", "-C", str(ROOT), "show", f"{WAVE_TIP}:{rel}"])
    return parse_font(path, Image.open(BytesIO(raw)))["cells"]


def plan_high(font, style, pin_cells):
    planned = {}
    bases = {}
    kept = []
    uncovered = []
    for code in range(ASCII_END, 256):
        if code not in font["cells"]:
            uncovered.append(code)
            continue
        if is_hud(code, font):
            if code not in pin_cells:
                raise RuntimeError(f"{font['path']}: HUD cell U+{code:02X} missing at {WAVE_TIP}")
            planned[code] = pin_cells[code].copy()
            bases[code] = "HUD"
            kept.append(code)
            continue
        planned[code], bases[code] = build_glyph(font, style, code)
    return planned, bases, kept, uncovered


def row_width(cells, start):
    total = 1
    for code in range(start, start + ROW_COUNT):
        total += cells[code].width + 2
    return total + 1


def paint_row(canvas, y, cells, start, font):
    pix = canvas.load()
    width, height = canvas.size
    red, bg = font["red"], font["bg"]
    font_h = font["font_h"]
    for j in range(font_h):
        if y + j >= height:
            break
        for x in range(width):
            pix[x, y + j] = bg
        pix[0, y + j] = red if j == 0 else bg
    x = 1
    for code in range(start, start + ROW_COUNT):
        cell = cells[code]
        for j, row in enumerate(cell.rows):
            if y + j >= height:
                continue
            for i, pixel in enumerate(row):
                if x + i < width:
                    pix[x + i, y + j] = pixel
        pad = x + cell.width
        sep = pad + 1
        for j in range(font_h):
            if y + j >= height:
                continue
            if pad < width:
                pix[pad, y + j] = bg
            if sep < width:
                pix[sep, y + j] = red
        x = sep + 1


def emit_atlas(font, planned):
    cells = {code: font["cells"][code] for code in range(FIRST, ASCII_END) if code in font["cells"]}
    cells.update(planned)
    ascii_rows = 6
    font_h = font["font_h"]
    src = font["rgba"]
    src_w, src_h = src.size
    high_w = max(row_width(cells, start) for start in range(ASCII_END, 256, ROW_COUNT) if start in cells)
    out_w = max(src_w, high_w)
    out_h = max(src_h, 14 * font_h)
    canvas = Image.new("RGBA", (out_w, out_h), font["bg"])
    canvas.paste(src, (0, 0))
    if out_w > src_w:
        pix = canvas.load()
        for y in range(ascii_rows * font_h):
            for x in range(src_w, out_w):
                pix[x, y] = font["bg"]
        pix[out_w - 1, 0] = font["bg"]
    for start in range(ASCII_END, 256, ROW_COUNT):
        if start not in cells:
            continue
        paint_row(canvas, ((start - FIRST) // ROW_COUNT) * font_h, cells, start, font)
    canvas.putpixel((canvas.size[0] - 1, 0), font["bg"])
    return canvas, cells


def cell_pixels(cell):
    return tuple(tuple(row) for row in cell.rows)


def refuse_if_hud_changed(original_cells, rebuilt_cells, keep, path):
    for code in sorted(keep):
        if code not in original_cells:
            raise RuntimeError(f"{path}: HUD cell U+{code:02X} missing in source")
        if code not in rebuilt_cells:
            raise RuntimeError(f"{path}: HUD cell U+{code:02X} missing after emit")
        if cell_pixels(original_cells[code]) != cell_pixels(rebuilt_cells[code]):
            raise RuntimeError(f"refusing emit: HUD cell U+{code:02X} would change")


def quantize_like(original, rgba):
    if original.mode != "P":
        return rgba
    palette = original.getpalette() or []
    index_of = {}
    for i in range(len(palette) // 3):
        rgb = (palette[i * 3], palette[i * 3 + 1], palette[i * 3 + 2])
        index_of.setdefault(rgb, i)
    red_idx = original.getpixel((0, 0))
    bg_idx = original.getpixel((original.size[0] - 1, 0))
    red_rgb = (palette[red_idx * 3], palette[red_idx * 3 + 1], palette[red_idx * 3 + 2])
    bg_rgb = (palette[bg_idx * 3], palette[bg_idx * 3 + 1], palette[bg_idx * 3 + 2])
    index_of[red_rgb] = red_idx
    index_of[bg_rgb] = bg_idx
    out = Image.new("P", rgba.size, bg_idx)
    out.putpalette(palette)
    src = rgba.load()
    dst = out.load()
    for y in range(rgba.size[1]):
        for x in range(rgba.size[0]):
            r, g, b, a = src[x, y]
            dst[x, y] = index_of.get((r, g, b), bg_idx)
    dst[0, 0] = red_idx
    dst[rgba.size[0] - 1, 0] = bg_idx
    return out


def save_png(image, path):
    image = image.copy()
    image.info.clear()
    image.save(path, format="PNG", compress_level=9, optimize=False, pnginfo=PngInfo())


def parse_rebuilt(path, expected_ascii, orig_rgba, font_h):
    again = parse_font(path)
    for code in range(FIRST, ASCII_END):
        if cell_pixels(expected_ascii[code]) != cell_pixels(again["cells"][code]):
            raise RuntimeError(f"refusing emit: re-read ASCII cell U+{code:02X} changed")
    band = 6 * font_h
    width = orig_rgba.size[0]
    before = orig_rgba.crop((0, 0, width, band))
    after = again["rgba"].crop((0, 0, width, band))
    if before.tobytes() != after.tobytes():
        raise RuntimeError("refusing emit: ASCII rows of the atlas changed")
    return again


def extend_one(path):
    font = parse_font(path)
    style = style_of(font)
    keep = hud_keep_set(path)
    pin_cells = load_wave_tip_cells(path) if keep else {}
    planned, bases, kept, uncovered = plan_high(font, style, pin_cells)
    refuse_if_hud_changed(pin_cells, planned, keep, path)
    canvas, cells = emit_atlas(font, planned)
    written = quantize_like(font["original"], canvas)
    save_png(written, path)
    again = parse_rebuilt(path, font["cells"], font["rgba"], font["font_h"])
    refuse_if_hud_changed(pin_cells, again["cells"], keep, path)
    return {
        "path": path,
        "kept_hud": kept,
        "uncovered": uncovered,
        "bases": bases,
        "font": again,
        "style": style,
        "planned": {code: cells[code] for code in planned},
    }


def draw_contact(results, dest):
    # Menus FontSmall is the name/fallback atlas the lobby actually draws.
    primary = next(item for item in results if item["path"].as_posix().endswith("Menus/FontSmall.png"))
    font = primary["font"]
    style = primary["style"]
    red, bg = font["red"], font["bg"]
    font_h = font["font_h"]
    pairs = []
    for code in range(ASCII_END, 256):
        if code in CP1252_UNUSED:
            continue
        new = font["cells"][code]
        if is_hud(code, font):
            continue
        if ink_count(new, red, bg) == 0 and code != 0xA0:
            continue
        base_name = primary["bases"].get(code, "?")
        if isinstance(base_name, str) and len(base_name) == 1 and base_name.isalpha():
            base = copy_cell(font, base_name)
        elif base_name in ("grave", "acute", "circumflex", "tilde", "diaeresis", "ring", "cedilla", "macron", "caron"):
            base = empty_cell(font, 4)
        else:
            base = empty_cell(font, new.width)
        pairs.append((code, base, new))
    cols = 16
    pad = 2
    cell_w = max(8, max(max(a.width, b.width) for _, a, b in pairs) + 1)
    pair_w = cell_w * 2 + pad + 4
    pair_h = font_h + 8
    rows = (len(pairs) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * pair_w + 4, rows * pair_h + 4), (12, 20, 39, 255))
    pix = sheet.load()

    def stamp(cell, x0, y0):
        for y, row in enumerate(cell.rows):
            for x, pixel in enumerate(row):
                if pixel != bg and pixel != red:
                    px, py = x0 + x, y0 + y
                    if 0 <= px < sheet.size[0] and 0 <= py < sheet.size[1]:
                        pix[px, py] = pixel

    for i, (code, base, new) in enumerate(pairs):
        col, row = i % cols, i // cols
        x0 = 2 + col * pair_w
        y0 = 2 + row * pair_h
        stamp(base, x0, y0)
        stamp(new, x0 + cell_w + pad, y0)
    save_png(sheet, dest)
    return dest, len(pairs)


def main(argv):
    if argv[1:]:
        paths = [Path(p) for p in argv[1:]]
    else:
        paths = FONT_PATHS
    results = []
    for path in paths:
        print(f"extend {path}")
        results.append(extend_one(path))
        print(f"  hud kept {len(results[-1]['kept_hud'])} uncovered {results[-1]['uncovered']}")
    contact, count = draw_contact(results, CONTACT_PATH)
    print(f"contact {contact} pairs={count}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
