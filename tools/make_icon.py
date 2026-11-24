"""Draws the application icon in the launcher's cut-paper style, from the show's setting:
green snow-capped mountains, a crooked wooden board with "SP", a yellow crossing sign.

    python tools/make_icon.py [valley|board|crossing] [--preview out.png]

Writes icons/app.png (512x512, window icon and game thumbnail) and icons/app.ico
(16-256 px, the executable's icon). Colours match tools/make_launcher_art.py.
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
S = 1024  # drawing size; downscaled for every output size

SKY = (0x8F, 0xD8, 0xF8, 255)
MOUNTAIN = (0x00, 0x70, 0x4A, 255)
MOUNTAIN_BACK = (0x0B, 0x7F, 0x55, 255)
SNOW = (0xF6, 0xF7, 0xF7, 255)
PINE = (0x1F, 0x8A, 0x56, 255)
WOOD = (0x9A, 0x6A, 0x54, 255)
POST = (0x6B, 0x3A, 0x1E, 255)
SIGN_YELLOW = (0xFF, 0xE0, 0x1B, 255)
INK = (0x11, 0x11, 0x11, 255)
FONT = ROOT / "assets" / "Suplexmentary Comic NC.ttf"


def mountain(d, px, py, left, right, base, color, cap):
    d.polygon([(left, base), (px, py), (right, base)], fill=color)
    t = cap / (base - py)
    lx, rx, y = px + (left - px) * t, px + (right - px) * t, py + cap
    d.polygon([(px, py), (rx, y), (rx - (rx - px) * 0.35, y + 34), (px, y - 4), (lx + (px - lx) * 0.4, y + 30), (lx, y)], fill=SNOW)


def scenery(d, peaks=True):
    d.rectangle([0, 0, S, S], fill=SKY)
    if peaks:
        mountain(d, 760, 240, 380, 1300, 860, MOUNTAIN_BACK, 150)
        mountain(d, 280, 170, -340, 900, 860, MOUNTAIN, 190)
    d.rectangle([0, 820, S, S], fill=SNOW)


def pine(d, x, ground, h):
    w = h * 0.38
    for i in range(4):
        top = ground - h + i * h * 0.2
        bot = top + h * 0.34
        ww = w * (0.45 + 0.18 * i)
        d.polygon([(x, top), (x + ww, bot), (x - ww, bot)], fill=PINE)
        d.polygon([(x, top + 4), (x + ww * 0.6, top + (bot - top) * 0.55), (x - ww * 0.6, top + (bot - top) * 0.55)], fill=SNOW)


def text_centered(d, text, cx, cy, size, color, angle_img=None):
    font = ImageFont.truetype(str(FONT), size)
    box = d.textbbox((0, 0), text, font=font)
    d.text((cx - (box[0] + box[2]) / 2, cy - (box[1] + box[3]) / 2), text, font=font, fill=color)


def board(img, cx, cy, w, h, angle, text, size):
    """A crooked plank board on one post, outlined in black, with black lettering; rotated."""
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.polygon([(cx - 26, cy + h * 0.4), (cx + 26, cy + h * 0.4), (cx + 34, S), (cx - 18, S)], fill=POST, outline=INK, width=10)
    quad = [(cx - w / 2, cy - h / 2 + 18), (cx + w / 2, cy - h / 2), (cx + w / 2 + 12, cy + h / 2), (cx - w / 2 + 10, cy + h / 2 + 10)]
    d.polygon(quad, fill=WOOD, outline=INK, width=12)
    for k in (1, 2):
        y = cy - h / 2 + h * k / 3
        d.line([(cx - w / 2 + 14, y + 8), (cx + w / 2 + 4, y - 6)], fill=INK, width=5)
    text_centered(d, text, cx, cy - h * 0.02, size, INK)
    img.alpha_composite(layer.rotate(angle, center=(cx, cy), resample=Image.BICUBIC))


def crossing(d, cx, cy, s):
    d.rectangle([cx - 16, cy, cx + 16, S], fill=POST)
    d.polygon([(cx, cy - s - 18), (cx + s + 18, cy), (cx, cy + s + 18), (cx - s - 18, cy)], fill=INK)
    d.polygon([(cx, cy - s), (cx + s, cy), (cx, cy + s), (cx - s, cy)], fill=SIGN_YELLOW)
    def figure(fx, fy, k):
        d.ellipse([fx - 26 * k, fy - 118 * k, fx + 26 * k, fy - 66 * k], fill=INK)
        w = int(22 * k)
        for a, b in [((fx, fy - 60 * k), (fx - 8 * k, fy + 16 * k)), ((fx - 8 * k, fy + 16 * k), (fx - 44 * k, fy + 84 * k)),
                     ((fx - 8 * k, fy + 16 * k), (fx + 32 * k, fy + 84 * k)), ((fx - 4 * k, fy - 40 * k), (fx + 44 * k, fy - 8 * k)),
                     ((fx - 4 * k, fy - 40 * k), (fx - 48 * k, fy - 4 * k))]:
            d.line([a, b], fill=INK, width=w)
            d.ellipse([b[0] - w / 2, b[1] - w / 2, b[0] + w / 2, b[1] + w / 2], fill=INK)
    figure(cx - 60, cy + 20, 1.0)
    figure(cx + 80, cy + 44, 0.75)


def draw(variant):
    img = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(img)
    if variant == "board":
        scenery(d)
        board(img, 512, 500, 820, 520, 6, "SP", 330)
    elif variant == "crossing":
        scenery(d)
        crossing(d, 512, 470, 330)
    elif variant == "valley":
        scenery(d)
        for (x, g, h) in [(110, 900, 380), (230, 930, 300), (900, 900, 380), (790, 930, 290)]:
            pine(d, x, g, h)
        board(img, 512, 560, 560, 400, 7, "SP", 250)
    else:
        raise SystemExit(f"unknown variant {variant}")
    # Rounded-square mask and a dark rim so it reads on any background.
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=190, fill=255)
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(img, (0, 0), mask)
    ImageDraw.Draw(out).rounded_rectangle([6, 6, S - 7, S - 7], radius=186, outline=(0x0E, 0x3B, 0x2A, 255), width=14)
    return out


def main():
    args = sys.argv[1:]
    if "--preview" in args:
        target = Path(args[args.index("--preview") + 1])
        sheet = Image.new("RGBA", (3 * 300 + 3 * 120, 330), (255, 255, 255, 255))
        d = ImageDraw.Draw(sheet)
        for i, v in enumerate(["board", "crossing", "valley"]):
            icon = draw(v)
            x = i * 420
            big = icon.resize((300, 300), Image.LANCZOS)
            sheet.paste(big, (x, 24), big)
            for j, s in enumerate((64, 32, 16)):
                small = icon.resize((s, s), Image.LANCZOS)
                sheet.paste(small, (x + 310, 24 + j * 80), small)
            d.text((x + 4, 4), f"{i + 1}. {v}", fill=(0, 0, 0, 255))
        sheet.save(target)
        print("preview", target)
        return
    variant = args[0] if args else "valley"
    icon = draw(variant)
    (ROOT / "icons").mkdir(exist_ok=True)
    icon.resize((512, 512), Image.LANCZOS).save(ROOT / "icons" / "app.png")
    sizes = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)]
    icon.resize((256, 256), Image.LANCZOS).save(ROOT / "icons" / "app.ico", sizes=sizes)
    print(f"wrote icons/app.png and icons/app.ico ({variant})")


if __name__ == "__main__":
    main()
