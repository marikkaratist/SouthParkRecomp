"""Convert an N64 ROM dump to big-endian .z64 and verify it.

Usage: python tools/rom_prepare.py <input rom> <output .z64>

The input is never modified. Detects z64 / v64 (16-bit byteswapped) / n64
(32-bit little-endian) by the first four bytes, writes a .z64 copy, then
recomputes the header checksum to prove the dump is intact.
"""
from __future__ import annotations

import hashlib
import struct
import sys
import zlib
from pathlib import Path

MASK = 0xFFFFFFFF

# CRC32 of the boot code (0x40..0x1000) -> (CIC name, checksum seed)
CIC_TABLE = {
    0x6170A4A1: ("6101", 0xF8CA4DDC),
    0x90BB6CB5: ("6102", 0xF8CA4DDC),
    0x0B050EE0: ("6103", 0xA3886759),
    0x98BC2C86: ("6105", 0xDF26F436),
    0xACC8580A: ("6106", 0x1FEA617A),
}


def to_z64(data: bytes) -> tuple[bytes, str]:
    magic = data[:4]
    if magic == b"\x80\x37\x12\x40":
        return data, "z64 (big-endian)"
    if magic == b"\x37\x80\x40\x12":
        swapped = bytearray(data)
        swapped[0::2], swapped[1::2] = data[1::2], data[0::2]
        return bytes(swapped), "v64 (byteswapped)"
    if magic == b"\x40\x12\x37\x80":
        words = struct.unpack(f"<{len(data) // 4}I", data)
        return struct.pack(f">{len(words)}I", *words), "n64 (little-endian)"
    raise SystemExit(f"Not an N64 ROM: unknown magic {magic.hex()}")


def rol(value: int, bits: int) -> int:
    bits &= 31
    return ((value << bits) | (value >> (32 - bits))) & MASK if bits else value


def checksum(rom: bytes, cic: str, seed: int) -> tuple[int, int]:
    t1 = t2 = t3 = t4 = t5 = t6 = seed
    words = struct.unpack(">262144I", rom[0x1000:0x101000])
    for i, d in enumerate(words):
        if (t6 + d) & MASK < t6:
            t4 = (t4 + 1) & MASK
        t6 = (t6 + d) & MASK
        t3 ^= d
        r = rol(d, d & 0x1F)
        t5 = (t5 + r) & MASK
        t2 ^= r if t2 > d else t6 ^ d
        if cic == "6105":
            off = 0x40 + 0x0710 + ((i * 4) & 0xFF)
            t1 = (t1 + (struct.unpack(">I", rom[off:off + 4])[0] ^ d)) & MASK
        else:
            t1 = (t1 + (t5 ^ d)) & MASK
    if cic == "6103":
        return ((t6 ^ t4) + t3) & MASK, ((t5 ^ t2) + t1) & MASK
    if cic == "6106":
        return (t6 * t4 + t3) & MASK, (t5 * t2 + t1) & MASK
    return t6 ^ t4 ^ t3, t5 ^ t2 ^ t1


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    rom, fmt = to_z64(src.read_bytes())

    clock, entry, release = struct.unpack(">III", rom[0x04:0x10])
    crc1, crc2 = struct.unpack(">II", rom[0x10:0x18])
    name = rom[0x20:0x34].decode("ascii", "replace").rstrip()
    game_code = rom[0x3B:0x3F].decode("ascii", "replace")
    boot_crc = zlib.crc32(rom[0x40:0x1000])
    cic, seed = CIC_TABLE.get(boot_crc, (None, None))

    print(f"input format : {fmt}")
    print(f"size         : {len(rom)} bytes ({len(rom) / 2**20:.0f} MB)")
    print(f"name         : {name!r}")
    print(f"game code    : {game_code}  version {rom[0x3F]}")
    print(f"entrypoint   : 0x{entry:08X}")
    print(f"libultra rev : {chr(release & 0xFF)!r} (0x{release:08X})")
    print(f"clock rate   : 0x{clock:08X}")
    print(f"bootcode crc : 0x{boot_crc:08X} -> CIC {cic or 'UNKNOWN'}")
    print(f"header crc   : {crc1:08X} {crc2:08X}")
    if cic:
        c1, c2 = checksum(rom, cic, seed)
        ok = (c1, c2) == (crc1, crc2)
        print(f"computed crc : {c1:08X} {c2:08X} -> {'OK' if ok else 'MISMATCH (bad dump?)'}")
    print(f"sha1 (z64)   : {hashlib.sha1(rom).hexdigest()}")
    print(f"md5  (z64)   : {hashlib.md5(rom).hexdigest()}")

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(rom)
    print(f"written      : {dst}")


if __name__ == "__main__":
    main()
