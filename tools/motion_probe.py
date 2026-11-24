"""Reads the strips saved by motion_probe.ps1 and prints the horizontal shift of
the picture from each strip to the next.

    python tools/motion_probe.py rom/probe

Smooth motion gives shifts of one sign and similar size. Shifts that flip sign
mean the picture is shaking; runs of zeros followed by a large step mean frames
are being repeated instead of interpolated.
"""
import struct
import sys
from pathlib import Path


def read_profile(path):
    """Column brightness of a 24-bit BMP, summed over its rows."""
    data = path.read_bytes()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    height = abs(height)
    stride = (width * 3 + 3) & ~3
    profile = [0] * width
    for row in range(height):
        base = offset + row * stride
        line = data[base:base + width * 3]
        blue, green, red = line[0::3], line[1::3], line[2::3]
        for x in range(width):
            profile[x] += blue[x] + green[x] + red[x]
    return profile


def shift_between(a, b, limit):
    """The shift s (pixels) for which b[x + s] best matches a[x]."""
    step = 2
    best, best_cost = 0, None
    for shift in range(-limit, limit + 1):
        cost = 0
        count = 0
        for x in range(limit, len(a) - limit, step):
            cost += abs(a[x] - b[x + shift])
            count += 1
        cost /= count
        if best_cost is None or cost < best_cost:
            best, best_cost = shift, cost
    still = sum(abs(a[x] - b[x]) for x in range(limit, len(a) - limit, step))
    return best, best_cost, still


def main():
    folder = Path(sys.argv[1])
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    times = [float(t) for t in (folder / "times.txt").read_text().replace(",", ".").split()]
    files = sorted(folder.glob("*.bmp"))
    profiles = [read_profile(f) for f in files]
    previous = profiles[0]
    shifts = []
    for index in range(1, len(profiles)):
        current = profiles[index]
        if current == previous:
            continue    # the screen has not changed since the last capture
        shift, cost, still = shift_between(previous, current, limit)
        shifts.append((times[index], shift))
        previous = current
    print("time_ms shift_px")
    for time, shift in shifts:
        print(f"{time:8.1f} {shift:+4d}")
    values = [s for _, s in shifts]
    flips = sum(1 for a, b in zip(values, values[1:]) if a * b < 0)
    zeros = sum(1 for v in values if v == 0)
    print(f"{len(values)} distinct pictures, {flips} sign flips, {zeros} zero shifts")


main()
