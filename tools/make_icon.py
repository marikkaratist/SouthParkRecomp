"""Draws the application icon: an original snowman mascot in a striped knit hat, in a flat
cut-paper style, on a transparent background.

    python tools/make_icon.py

Writes icons/app.png (512x512, window icon and game thumbnail) and icons/app.ico
(16-256 px, the executable's icon).
"""
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
S = 1024  # drawing size; downscaled for every output size

SNOW = (0xF7, 0xF9, 0xFB, 255)
SNOW_SHADE = (0xD3, 0xE2, 0xEC, 255)
HAT = (0x7B, 0x3F, 0xB5, 255)
STRIPE = (0xFF, 0x8A, 0x1F, 255)
POM = (0xFF, 0xFF, 0xFF, 255)
CARROT = (0xF2, 0x7A, 0x1A, 255)
CARROT_DARK = (0xC9, 0x5A, 0x0E, 255)
COAL = (0x22, 0x22, 0x26, 255)
OUTLINE = (0x1E, 0x2A, 0x3A, 255)


def mask_ellipse(box):
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).ellipse(box, fill=255)
    return m


def draw_icon():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = S // 2

    # Head: a snowball with a dark outline and a soft shadow on the lower right.
    head_box = [cx - 380, 260, cx + 380, 1000]
    d.ellipse([head_box[0] - 10, head_box[1] - 10, head_box[2] + 10, head_box[3] + 10], fill=OUTLINE)
    head = mask_ellipse(head_box)
    img.paste(SNOW, (0, 0), head)
    shade = ImageChops.subtract(head, mask_ellipse([cx - 420, 210, cx + 300, 930]))
    img.paste(SNOW_SHADE, (0, 0), shade)

    # Knit hat: a dome over the top of the head with orange stripes and a folded brim.
    dome = Image.new("L", (S, S), 0)
    ImageDraw.Draw(dome).pieslice([cx - 360, 150, cx + 360, 820], 180, 360, fill=255)
    stripes = Image.new("L", (S, S), 0)
    sd = ImageDraw.Draw(stripes)
    for y in (250, 340, 430):
        sd.rectangle([0, y, S, y + 40], fill=255)
    d.pieslice([cx - 372, 138, cx + 372, 832], 180, 360, fill=OUTLINE)
    img.paste(HAT, (0, 0), dome)
    img.paste(STRIPE, (0, 0), ImageChops.multiply(dome, stripes))
    d.rounded_rectangle([cx - 400, 440, cx + 400, 545], radius=40, fill=OUTLINE)
    d.rounded_rectangle([cx - 390, 450, cx + 390, 535], radius=34, fill=STRIPE)
    for x in range(cx - 360, cx + 361, 60):  # ribbing on the brim
        d.line([(x, 462), (x, 523)], fill=(0xE0, 0x70, 0x10, 255), width=8)
    # Pompom.
    d.ellipse([cx - 108, 70, cx + 108, 270], fill=OUTLINE)
    d.ellipse([cx - 98, 80, cx + 98, 260], fill=POM)

    # Coal eyes and smile.
    for ex in (cx - 140, cx + 140):
        d.ellipse([ex - 46, 590, ex + 46, 682], fill=COAL)
        d.ellipse([ex - 22, 604, ex - 2, 624], fill=(255, 255, 255, 255))
    for i, x in enumerate(range(cx - 190, cx + 191, 76)):
        y = 865 - (60 if i in (0, 5) else 20 if i in (1, 4) else 0)
        d.ellipse([x - 22, y - 22, x + 22, y + 22], fill=COAL)

    # Carrot nose pointing to the right.
    d.polygon([(cx - 30, 700), (cx + 300, 760), (cx - 30, 800)], fill=OUTLINE)
    d.polygon([(cx - 18, 712), (cx + 270, 760), (cx - 18, 788)], fill=CARROT)
    for x in (cx + 60, cx + 140):
        d.line([(x, 732), (x + 8, 752)], fill=CARROT_DARK, width=7)
    return img


def main():
    icon = draw_icon()
    # Trim the empty margin so the head fills the icon.
    box = icon.getbbox()
    side = max(box[2] - box[0], box[3] - box[1]) + 24
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.paste(icon.crop(box), ((side - (box[2] - box[0])) // 2, (side - (box[3] - box[1])) // 2))
    (ROOT / "icons").mkdir(exist_ok=True)
    square.resize((512, 512), Image.LANCZOS).save(ROOT / "icons" / "app.png")
    sizes = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)]
    square.resize((256, 256), Image.LANCZOS).save(ROOT / "icons" / "app.ico", sizes=sizes)
    print("wrote icons/app.png and icons/app.ico")


if __name__ == "__main__":
    main()
