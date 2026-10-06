"""Read the installed mod's lettering from captured pictures and grade its scenes."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from PIL import Image


def lettering(package: Path, text: str) -> tuple[list[tuple[int, int]], int, int]:
    source = (package / "Scripts/Lib_Generic.lua").read_text(encoding="utf-8")
    chars = {}
    for char, index, width, dx, dy in re.findall(
        r'CF\["Chars"\]\["(.)"\] = \{ (\d+), (\d+), (?:nil|Vector\((-?\d+), (-?\d+)\)) \}', source
    ):
        chars[char] = (int(index), int(width) - 2, int(dx or 0), int(dy or 0))
    pixels, cursor = set(), 0
    for char in text:
        index, advance, dx, dy = chars[char]
        glyph = Image.open(package / f"UI/Letters/Letter{index - 1:03d}.png").convert("RGBA")
        for y in range(glyph.height):
            for x in range(glyph.width):
                r, g, b, alpha = glyph.getpixel((x, y))
                if alpha and r >= 220 and g >= 200:
                    pixels.add((cursor + dx + x - glyph.width // 2, dy + y - glyph.height // 2))
        cursor += advance
    left, top = min(x for x, y in pixels), min(y for x, y in pixels)
    pixels = sorted((x - left, y - top) for x, y in pixels)
    return pixels, max(x for x, y in pixels) + 1, max(y for x, y in pixels) + 1


def find_words(picture: Image.Image, package: Path, text: str) -> dict:
    pixels, width, height = lettering(package, text)
    ink = set(pixels)
    gaps = [(x, y) for y in range(height) for x in range(width)
            if all((x + dx, y + dy) not in ink for dx in range(-1, 2) for dy in range(-1, 2))]
    rgb = picture.convert("RGB")
    rows = []
    for y in range(rgb.height):
        bits = 0
        for x in range(rgb.width):
            r, g, b = rgb.getpixel((x, y))
            if r >= 170 and g >= 150 and r >= b + 30:
                bits |= 1 << x
        rows.append(bits)
    anchors = [pixels[i * (len(pixels) - 1) // 3] for i in range(4)]
    best = {"text": text, "confidence": 0.0, "background_confidence": 0.0, "rect": None, "pass": False}
    for y in range(max(0, rgb.height - height + 1)):
        possible = 0
        for ax, ay in anchors:
            possible |= rows[y + ay] >> ax
        possible &= (1 << max(0, rgb.width - width + 1)) - 1
        while possible:
            lowest = possible & -possible
            x = lowest.bit_length() - 1
            possible -= lowest
            confidence = sum(bool(rows[y + py] & (1 << (x + px))) for px, py in pixels) / len(pixels)
            if confidence > best["confidence"]:
                background = sum(not rows[y + py] & (1 << (x + px)) for px, py in gaps) / len(gaps)
                best.update(confidence=confidence, background_confidence=background, rect=[x, y, width, height],
                            **{"pass": confidence >= 0.95 and background >= 0.95})
    return best


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--picture", type=Path)
    parser.add_argument("--words", default="New game|Load game")
    options = parser.parse_args()
    package = options.repo / "Data/VoidWanderers.rte"
    if options.picture:
        checks = [find_words(Image.open(options.picture), package, word) for word in options.words.split("|")]
        print(json.dumps(checks, indent=2))
        return int(not all(check["pass"] for check in checks))
    parser.error("--picture is required")


if __name__ == "__main__":
    sys.exit(main())
