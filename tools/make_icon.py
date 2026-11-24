"""Draws the application icon: a round cut-paper kid's head in a blue beanie with a yellow
pompom, in front of a green mountain. Drawn here from scratch.

    python tools/make_icon.py

Writes icons/app.png (512x512, window icon and game thumbnail) and icons/app.ico
(16-256 px, the executable's icon). Colours match tools/make_launcher_art.py.
"""
import math
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
S = 1024  # drawing size; downscaled for every output size

SKY = (0x8F, 0xD8, 0xF8)
MOUNTAIN = (0x00, 0x70, 0x4A)
SNOW = (0xF6, 0xF7, 0xF7)
SKIN = (0xFC, 0xDC, 0xB4)
HAT = (0x5B, 0xC2, 0xDA)
HAT_TRIM = (0xF4, 0xE2, 0x2A)
INK = (0x11, 0x11, 0x11)


def draw_icon():
    art = Image.new("RGBA", (S, S), SKY + (255,))
    d = ImageDraw.Draw(art)

    # Mountain with a snow cap, and snow at the bottom.
    d.polygon([(-200, 1024), (512, 170), (1224, 1024)], fill=MOUNTAIN)
    d.polygon([(512, 170), (700, 395), (630, 430), (560, 400), (470, 440), (390, 405), (330, 388)], fill=SNOW)
    d.rectangle([0, 900, S, S], fill=SNOW)

    # Head: a wide oval.
    cx, cy = 512, 600
    d.ellipse([cx - 390, cy - 300, cx + 390, cy + 330], fill=SKIN, outline=INK, width=6)

    # Beanie: the top part of a slightly bigger oval, cut straight across.
    hat = Image.new("L", (S, S), 0)
    hd = ImageDraw.Draw(hat)
    hd.ellipse([cx - 400, cy - 330, cx + 400, cy + 330], fill=255)
    hd.rectangle([0, cy - 40, S, S], fill=0)
    art.paste(HAT + (255,), (0, 0), hat)
    # Trim band.
    d.rounded_rectangle([cx - 392, cy - 78, cx + 392, cy - 28], radius=22, fill=HAT_TRIM)
    # Pompom: a fuzzy cluster of blobs.
    for i in range(9):
        a = i / 9 * math.tau
        px, py = cx + math.cos(a) * 42, cy - 330 + math.sin(a) * 24
        d.ellipse([px - 46, py - 34, px + 46, py + 34], fill=HAT_TRIM)

    # Eyes: two big whites leaning on each other, small pupils looking inward.
    for ex in (cx - 92, cx + 92):
        d.ellipse([ex - 100, cy - 30, ex + 100, cy + 160], fill=(255, 255, 255), outline=INK, width=5)
    for px in (cx - 52, cx + 52):
        d.ellipse([px - 17, cy + 55, px + 17, cy + 89], fill=INK)
    # Mouth and the chin crease.
    d.arc([cx - 40, cy + 222, cx + 40, cy + 262], 200, 340, fill=INK, width=7)
    d.arc([cx - 300, cy - 40, cx + 300, cy + 300], 25, 155, fill=INK, width=5)

    # Rounded-square mask and a dark rim so it reads on any background.
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=190, fill=255)
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(art, (0, 0), mask)
    ImageDraw.Draw(out).rounded_rectangle([6, 6, S - 7, S - 7], radius=186, outline=(0x0E, 0x3B, 0x2A, 255), width=14)
    return out


def main():
    icon = draw_icon()
    (ROOT / "icons").mkdir(exist_ok=True)
    icon.resize((512, 512), Image.LANCZOS).save(ROOT / "icons" / "app.png")
    sizes = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)]
    icon.resize((256, 256), Image.LANCZOS).save(ROOT / "icons" / "app.ico", sizes=sizes)
    print("wrote icons/app.png and icons/app.ico")


if __name__ == "__main__":
    main()
