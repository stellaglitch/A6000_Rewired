"""Draws res/drawable-*/ic_launcher.png: a patch strip of twelve wires, two of them crossed.

    python make_icon.py RES_DIR
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96}


def icon(n):
    s = 8  # draw large, then shrink, for clean edges
    im = Image.new("RGBA", (n * s, n * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    w = n * s
    d.rounded_rectangle([0, 0, w - 1, w - 1], radius=w // 6, fill=(20, 20, 26, 255))
    pins = 6
    xs = [int(w * (0.18 + 0.64 * i / (pins - 1))) for i in range(pins)]
    top, bot = int(w * 0.2), int(w * 0.8)
    route = [0, 1, 4, 3, 2, 5]  # 2 and 4 swapped
    for i, j in enumerate(route):
        col = (255, 92, 138, 255) if i != j else (220, 220, 230, 255)
        d.line([xs[i], top, xs[j], bot], fill=col, width=max(s * 2, w // 22))
    for x in xs:
        r = w // 22
        d.ellipse([x - r, top - r, x + r, top + r], fill=(220, 220, 230, 255))
        d.ellipse([x - r, bot - r, x + r, bot + r], fill=(220, 220, 230, 255))
    return im.resize((n, n), Image.LANCZOS)


def main():
    res = Path(sys.argv[1])
    for dens, n in SIZES.items():
        out = res / f"drawable-{dens}"
        out.mkdir(parents=True, exist_ok=True)
        icon(n).save(out / "ic_launcher.png")


if __name__ == "__main__":
    main()
