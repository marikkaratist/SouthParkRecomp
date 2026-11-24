"""Generates the launcher artwork in assets/launcher/ in the show's flat cut-paper look:
flat sky, big green mountains with snow caps, snow-laden pines, a yellow road sign.

    python tools/make_launcher_art.py

Everything is drawn here from scratch. The pictures are plain paths, polygons and
flat fills so the launcher's SVG renderer (lunasvg) draws them exactly.
"""
import random
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "assets" / "launcher"

SKY = "#8FD8F8"          # must match sky_color in src/main/launcher_animation.cpp
MOUNTAIN = "#00704A"
MOUNTAIN_BACK = "#0B7F55"
SNOW = "#F6F7F7"
SNOW_SHADE = "#DDE6EA"
PINE = "#1F8A56"
PINE_DARK = "#14724A"
INK = "#111111"


def svg(w, h, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'
            f'{body}</svg>\n')


def pts(points):
    return " ".join(f"{x:.1f},{y:.1f}" for x, y in points)


def mountain(px, py, left, right, base, color, cap_depth):
    """A big triangle with a rounded snow cap whose lower edge droops in soft waves."""
    # The body's tip sits under the rounded cap so no green shows above the snow.
    out = [f'<polygon fill="{color}" points="{pts([(left, base), (px, py + 14), (right, base)])}"/>']
    # Where the cap meets each slope.
    def at(x0, depth):
        t = depth / (base - py)
        return px + (x0 - px) * t, py + depth
    lx, ly = at(left, cap_depth * 1.05)
    rx, ry = at(right, cap_depth * 0.95)
    # Rounded top: the peak is softened with a quadratic curve.
    d = (f"M{lx:.1f},{ly:.1f} L{px - 40:.1f},{py + 22:.1f} Q{px:.1f},{py - 6:.1f} {px + 40:.1f},{py + 22:.1f} "
         f"L{rx:.1f},{ry:.1f} ")
    # Wavy lower edge back to the left.
    n = 4
    for i in range(1, n + 1):
        x = rx + (lx - rx) * i / n
        y = ry + (ly - ry) * i / n
        cx = rx + (lx - rx) * (i - 0.5) / n
        cy = ry + (ly - ry) * (i - 0.5) / n + (34 if i % 2 else 10)
        d += f"Q{cx:.1f},{cy:.1f} {x:.1f},{y:.1f} "
    d += "Z"
    out.append(f'<path fill="{SNOW}" d="{d}"/>')
    return "".join(out)


def snowy_pine(rng, x, ground, h):
    """A pine in tiers; every tier is green with a ragged blanket of snow on top."""
    out = []
    tiers = 6
    w_max = h * 0.36
    tip = ground - h
    # Narrow tip.
    out.append(f'<polygon fill="{PINE}" points="{pts([(x, tip - h * 0.06), (x + w_max * 0.12, tip + h * 0.12), (x - w_max * 0.12, tip + h * 0.12)])}"/>')
    for i in range(tiers):
        t0 = (i) / tiers
        t1 = (i + 1.25) / tiers
        top = tip + h * 0.85 * t0
        bot = tip + h * 0.85 * t1
        w = w_max * (0.25 + 0.75 * (i + 1) / tiers)
        # Green tier with a jagged hem.
        hem = []
        teeth = 5 + i
        for k in range(teeth + 1):
            hx = x + w - 2 * w * k / teeth
            hy = bot + (0 if k % 2 else rng.uniform(10, 18))
            hem.append((hx, hy))
        color = PINE if i % 2 == 0 else PINE_DARK
        out.append(f'<polygon fill="{color}" points="{pts([(x, top)] + hem)}"/>')
        # Snow blanket over the upper part of the tier, with drips.
        sw = w * 0.82
        sb = top + (bot - top) * 0.55
        drips = []
        teeth = 4 + i
        for k in range(teeth + 1):
            dx = x + sw - 2 * sw * k / teeth
            dy = sb + (rng.uniform(8, 22) if k % 2 else -rng.uniform(0, 6))
            drips.append((dx, dy))
        out.append(f'<polygon fill="{SNOW}" points="{pts([(x, top + 6)] + drips)}"/>')
    # Trunk hidden in snow; a short dark stub.
    out.insert(0, f'<rect x="{x - h*0.03:.1f}" y="{ground - h*0.2:.1f}" width="{h*0.06:.1f}" height="{h*0.2:.1f}" fill="#5A3A22"/>')
    return "".join(out)


def crossing_sign(x, ground):
    """A yellow diamond warning sign on a brown pole, with a simple walking-figure pictogram."""
    s = 120  # half diagonal
    cy = ground - 420
    out = [f'<rect x="{x - 8}" y="{cy}" width="16" height="{ground - cy}" fill="#6B3A1E"/>',
           f'<polygon fill="{INK}" points="{pts([(x, cy - s - 10), (x + s + 10, cy), (x, cy + s + 10), (x - s - 10, cy)])}"/>',
           f'<polygon fill="#FFE01B" points="{pts([(x, cy - s), (x + s, cy), (x, cy + s), (x - s, cy)])}"/>']
    # Pictogram: two walking figures (circles and thick strokes).
    def figure(fx, fy, k):
        return (f'<circle cx="{fx}" cy="{fy - 46*k:.1f}" r="{13*k:.1f}" fill="{INK}"/>'
                f'<path d="M{fx},{fy - 30*k:.1f} L{fx - 4*k:.1f},{fy + 8*k:.1f} M{fx - 4*k:.1f},{fy + 8*k:.1f} L{fx - 22*k:.1f},{fy + 42*k:.1f} '
                f'M{fx - 4*k:.1f},{fy + 8*k:.1f} L{fx + 16*k:.1f},{fy + 42*k:.1f} M{fx - 2*k:.1f},{fy - 20*k:.1f} L{fx + 22*k:.1f},{fy - 4*k:.1f} '
                f'M{fx - 2*k:.1f},{fy - 20*k:.1f} L{fx - 24*k:.1f},{fy - 2*k:.1f}" stroke="{INK}" stroke-width="{11*k:.1f}" stroke-linecap="round" fill="none"/>')
    out.append(figure(x - 26, cy + 10, 1.0))
    out.append(figure(x + 34, cy + 22, 0.78))
    return "".join(out)


def background():
    rng = random.Random(11)
    W, H = 1920, 1200
    b = [f'<rect width="{W}" height="{H}" fill="{SKY}"/>']

    # Mountains: a far one in the middle, two big ones in front.
    b.append(mountain(960, 420, 420, 1500, 1000, MOUNTAIN_BACK, 150))
    b.append(mountain(330, 170, -520, 1180, 1000, MOUNTAIN, 250))
    b.append(mountain(1580, 230, 760, 2500, 1000, MOUNTAIN, 230))

    # Snowfield.
    b.append(f'<path fill="{SNOW}" d="M0,930 C260,900 520,960 860,945 C1200,930 1500,965 1920,925 L1920,1200 L0,1200 Z"/>')

    # Pines: clusters at both sides and a few small ones far back.
    for (x, g, h) in [(70, 990, 430), (210, 1010, 520), (360, 990, 360), (-40, 1010, 380),
                      (560, 960, 230), (650, 955, 190),
                      (1270, 960, 200), (1360, 965, 240),
                      (1760, 1000, 380), (1880, 1015, 460)]:
        b.append(snowy_pine(rng, x, g, h))

    b.append(crossing_sign(1060, 1040))

    # Soft drifts in front, then the road along the bottom (the version label sits on it).
    b.append(f'<path fill="{SNOW_SHADE}" d="M0,1060 C300,1030 640,1075 980,1058 C1320,1042 1620,1075 1920,1050 L1920,1140 L0,1140 Z"/>')
    b.append(f'<path fill="{SNOW}" d="M0,1078 C340,1052 700,1094 1040,1078 C1380,1062 1660,1092 1920,1068 L1920,1140 L0,1140 Z"/>')
    b.append('<rect x="0" y="1136" width="1920" height="64" fill="#55524F"/>')
    for i in range(0, 1920, 140):
        b.append(f'<rect x="{i + 30}" y="1166" width="70" height="7" fill="#E9D46A"/>')
    return svg(W, H, "".join(b))


def title_sign():
    """A crooked plank board on one post, outlined in black like a paper cut-out. 900x760."""
    W, H = 900, 760
    board = [(30, 70), (860, 20), (880, 560), (60, 600)]
    b = [f'<polygon fill="#6B3A1E" stroke="{INK}" stroke-width="6" points="{pts([(420, 560), (480, 556), (500, 750), (440, 752)])}"/>',
         f'<polygon fill="#9A6A54" stroke="{INK}" stroke-width="7" stroke-linejoin="round" points="{pts(board)}"/>']

    # Plank seams run from the board's left edge to its right edge; both edges lean, so the
    # ends are worked out on them. The lowest seam passes under "RECOMPILED", not through it.
    def edge_x(top, bottom, y):
        return top[0] + (bottom[0] - top[0]) * (y - top[1]) / (bottom[1] - top[1])
    for (y0, y1) in [(190, 150), (330, 300), (548, 520)]:
        x0 = edge_x(board[0], board[3], y0) + 2
        x1 = edge_x(board[1], board[2], y1) - 2
        b.append(f'<path d="M{x0:.1f},{y0} L{x1:.1f},{y1}" stroke="{INK}" stroke-width="3" fill="none"/>')

    # The post goes into a snowdrift.
    b.append(f'<ellipse cx="470" cy="742" rx="120" ry="22" fill="{SNOW}"/>'
             f'<ellipse cx="470" cy="750" rx="96" ry="10" fill="{SNOW_SHADE}"/>')
    return svg(W, H, "".join(b))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for old in ("logo_sign.svg", "menu_sign.svg", "cloud.svg"):
        (OUT / old).unlink(missing_ok=True)
    for name, text in [("background.svg", background()), ("title_sign.svg", title_sign())]:
        (OUT / name).write_text(text, encoding="utf-8")
        print("wrote", OUT / name)


if __name__ == "__main__":
    main()
