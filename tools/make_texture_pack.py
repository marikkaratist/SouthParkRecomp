"""Builds an upscaled texture pack (.rtz) from textures dumped by the game.

    1. Dump: play with  SouthParkRecompiled.exe --dump-textures [dump folder]
       (without a folder: %LOCALAPPDATA%\SouthParkRecompiled\texture_dump)
       Every texture the game uses is written once (raw TMEM and tile parameters, named by
       its RT64 hash). Play through the parts of the game you want covered.
    2. Build:
       python tools/make_texture_pack.py all <dump folder> <work folder> [--scale 4] [--model realesrgan-x4plus-anime]
       or the steps one by one: decode, upscale, pack.
    3. Put <work folder>/southpark_hd.rtz into the game's mods/ folder and enable it in Mods.

Steps:
  decode   turns each dump into a PNG at the original size (work/native), decoding TMEM exactly as
           RT64's sampler does (shaders/TextureDecoder.hlsli): odd-row word swap, RGBA32 split
           across the two TMEM halves, palettes in the upper half.
  upscale  runs Real-ESRGAN (ncnn-vulkan) over the colour and the alpha separately (work/hd):
           the texture is padded the way the game samples it (wrap, mirror or clamp) so tiling
           textures stay seamless, colour under transparent pixels is bled in from the
           neighbours so edges get no halo, and one-bit alpha stays one-bit.
  pack     writes rt64.json (textures matched by their RT64 hash) and mod.json, then runs
           RT64's texture_packer to make the .rtz.

The pack is made from your own copy of the game and is meant for your own use.
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ESRGAN = ROOT.parent / "Tools" / "realesrgan" / "realesrgan-ncnn-vulkan.exe"
DEFAULT_PACKER = ROOT / "build" / "texture_packer.exe"

G_IM_FMT_RGBA, G_IM_FMT_YUV, G_IM_FMT_CI, G_IM_FMT_IA, G_IM_FMT_I = 0, 1, 2, 3, 4
G_IM_SIZ_4b, G_IM_SIZ_8b, G_IM_SIZ_16b, G_IM_SIZ_32b = 0, 1, 2, 3
G_TX_MIRROR, G_TX_CLAMP = 1, 2
TMEM_PALETTE = 0x800
PAD = 8  # texels of padding around each texture before upscaling


# --- decode -------------------------------------------------------------------------------------

def expand5(v):
    return ((v << 3) | (v >> 2)).astype(np.uint8)


def rgba16(v):
    v = v.astype(np.uint32)
    r, g, b = expand5((v >> 11) & 31), expand5((v >> 6) & 31), expand5((v >> 1) & 31)
    a = np.where(v & 1, 255, 0).astype(np.uint8)
    return np.stack([r, g, b, a], -1)


def ia16(v):
    v = v.astype(np.uint32)
    i, a = (v >> 8).astype(np.uint8), (v & 0xFF).astype(np.uint8)
    return np.stack([i, i, i, a], -1)


def decode_texture(tmem, tile, width, height, tlut):
    """RGBA uint8 array (height, width, 4), sampling TMEM like RT64's sampleTMEM."""
    fmt, siz = tile["fmt"], tile["siz"]
    address, stride = tile["tmem"] * 8, tile["line"] * 8
    if stride == 0 or width == 0 or height == 0:
        return None
    is_rgba32 = fmt == G_IM_FMT_RGBA and siz == G_IM_SIZ_32b
    uses_tlut = tlut in ("RGBA16", "IA16", 1, 2)
    shift = 2 if is_rgba32 else siz
    mask = 0x7FF if (is_rgba32 or uses_tlut) else 0xFFF

    y, x = np.mgrid[0:height, 0:width].astype(np.int64)
    odd_row = (y & 1).astype(bool)

    def load(rel, or_address=0):
        row_start = (rel // stride) * stride
        word = (rel - row_start) // 4
        swapped = address + row_start + ((word ^ 1) * 4) + (rel & 3)
        final = np.where(odd_row, swapped, address + rel)
        return tmem[((final & mask) | or_address) & 0xFFF].astype(np.uint32)

    pixel = y * stride + ((x << shift) >> 1)
    p0, p1 = load(pixel), load(pixel + 1)
    nibble = np.where((x & 1).astype(bool), p0 & 0xF, (p0 >> 4) & 0xF)

    if uses_tlut:
        if siz == G_IM_SIZ_4b:
            pal = TMEM_PALETTE + (tile["palette"] << 7) + (nibble << 3)
        else:
            pal = TMEM_PALETTE + (p0 << 3)
        value = (tmem[pal & 0xFFF].astype(np.uint32) << 8) | tmem[(pal + 1) & 0xFFF]
        return rgba16(value) if tlut in ("RGBA16", 1) else ia16(value)

    if siz == G_IM_SIZ_4b:
        if fmt == G_IM_FMT_IA:
            i = (((nibble >> 1) & 7) * 255 // 7).astype(np.uint8)
            return np.stack([i, i, i, np.where(nibble & 1, 255, 0).astype(np.uint8)], -1)
        if fmt == G_IM_FMT_CI:
            i = ((tile["palette"] << 4) | nibble).astype(np.uint8)
            return np.stack([i, i, i, i], -1)
        i = (nibble * 17).astype(np.uint8)
        return np.stack([i, i, i, i], -1)
    if siz == G_IM_SIZ_8b:
        if fmt == G_IM_FMT_IA:
            i, a = ((p0 >> 4) * 17).astype(np.uint8), ((p0 & 0xF) * 17).astype(np.uint8)
            return np.stack([i, i, i, a], -1)
        i = p0.astype(np.uint8)
        return np.stack([i, i, i, i], -1)
    if siz == G_IM_SIZ_16b:
        v = (p0 << 8) | p1
        if fmt == G_IM_FMT_RGBA:
            return rgba16(v)
        if fmt == G_IM_FMT_IA:
            return ia16(v)
        a, b = p0.astype(np.uint8), p1.astype(np.uint8)
        return np.stack([a, b, a, b], -1)
    if siz == G_IM_SIZ_32b and is_rgba32:
        p2, p3 = load(pixel, 0x800), load(pixel + 1, 0x800)
        return np.stack([p0, p1, p2, p3], -1).astype(np.uint8)
    return None


def cmd_decode(dump, work):
    native = work / "native"
    native.mkdir(parents=True, exist_ok=True)
    index = {}
    skipped = 0
    for tile_path in sorted(dump.glob("*.tile.json")):
        name = tile_path.name[: -len(".tile.json")]      # "<hash>.v<version>"
        rt64_hash, version = name.split(".v")
        tmem_path = dump / f"{name}.tmem"
        if not tmem_path.exists():
            continue
        info = json.loads(tile_path.read_text())
        tmem = np.frombuffer(tmem_path.read_bytes(), dtype=np.uint8)
        rgba = decode_texture(tmem, info["tile"], info["width"], info["height"], info["tlut"])
        if rgba is None:
            skipped += 1
            continue
        Image.fromarray(rgba, "RGBA").save(native / f"{rt64_hash}.png")
        tile = info["tile"]
        index[rt64_hash] = {
            "version": int(version),
            "width": info["width"], "height": info["height"],
            "clamp_s": bool(tile["cms"] & G_TX_CLAMP) or tile["masks"] == 0,
            "clamp_t": bool(tile["cmt"] & G_TX_CLAMP) or tile["maskt"] == 0,
            "mirror_s": bool(tile["cms"] & G_TX_MIRROR), "mirror_t": bool(tile["cmt"] & G_TX_MIRROR),
        }
    (work / "textures.json").write_text(json.dumps(index, indent=1))
    print(f"decoded {len(index)} textures into {native} ({skipped} in unsupported formats skipped)")


# --- upscale ------------------------------------------------------------------------------------

def pad_axis(a, axis, clamp, mirror):
    mode = "edge" if clamp else ("symmetric" if mirror else "wrap")
    widths = [(0, 0)] * a.ndim
    widths[axis] = (PAD, PAD)
    return np.pad(a, widths, mode=mode)


def bleed_colour(rgba):
    """Fills the colour of fully transparent pixels from opaque neighbours (no dark halos)."""
    rgb = rgba[..., :3].astype(np.float32)
    known = rgba[..., 3] > 0
    if known.all() or not known.any():
        return rgba[..., :3]
    for _ in range(16):
        if known.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            k = np.roll(known, (dy, dx), (0, 1))
            acc += np.roll(rgb, (dy, dx), (0, 1)) * k[..., None]
            cnt += k
        fill = (~known) & (cnt > 0)
        rgb[fill] = acc[fill] / cnt[fill][:, None]
        known = known | fill
    return rgb.clip(0, 255).astype(np.uint8)


def run_esrgan(esrgan, model, scale, src, dst):
    dst.mkdir(parents=True, exist_ok=True)
    cmd = [str(esrgan), "-i", str(src), "-o", str(dst), "-n", model, "-s", str(scale),
           "-m", str(esrgan.parent / "models"), "-f", "png"]
    print("running", " ".join(cmd))
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def cmd_upscale(work, esrgan, model, scale, min_size):
    index = json.loads((work / "textures.json").read_text())
    stage_rgb, stage_a = work / "stage_rgb", work / "stage_alpha"
    out_rgb, out_a, hd = work / "up_rgb", work / "up_alpha", work / "hd"
    for d in (stage_rgb, stage_a, out_rgb, out_a):
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)
    hd.mkdir(exist_ok=True)

    alpha_kind = {}
    for h, meta in index.items():
        rgba = np.array(Image.open(work / "native" / f"{h}.png").convert("RGBA"))
        if min(rgba.shape[:2]) < min_size:
            continue
        a = rgba[..., 3]
        alpha_kind[h] = "opaque" if (a == 255).all() else ("binary" if np.isin(a, (0, 255)).all() else "soft")
        rgb = bleed_colour(rgba)
        for axis, clamp, mirror in ((1, meta["clamp_s"], meta["mirror_s"]), (0, meta["clamp_t"], meta["mirror_t"])):
            rgb = pad_axis(rgb, axis, clamp, mirror)
            a = pad_axis(a, axis, clamp, mirror)
        Image.fromarray(rgb, "RGB").save(stage_rgb / f"{h}.png")
        if alpha_kind[h] != "opaque":
            Image.fromarray(np.stack([a, a, a], -1), "RGB").save(stage_a / f"{h}.png")

    run_esrgan(esrgan, model, scale, stage_rgb, out_rgb)
    if any(k != "opaque" for k in alpha_kind.values()):
        run_esrgan(esrgan, model, scale, stage_a, out_a)

    crop = PAD * scale
    for h, kind in alpha_kind.items():
        rgb = np.array(Image.open(out_rgb / f"{h}.png").convert("RGB"))[crop:-crop, crop:-crop]
        if kind == "opaque":
            a = np.full(rgb.shape[:2], 255, np.uint8)
        else:
            a = np.array(Image.open(out_a / f"{h}.png").convert("L"))[crop:-crop, crop:-crop]
            if kind == "binary":
                a = np.where(a >= 128, 255, 0).astype(np.uint8)
        Image.fromarray(np.dstack([rgb, a]), "RGBA").save(hd / f"{h}.png")
    print(f"upscaled {len(alpha_kind)} textures x{scale} into {hd}")


# --- pack ---------------------------------------------------------------------------------------

def cmd_pack(work, packer, name):
    index = json.loads((work / "textures.json").read_text())
    pack = work / "pack"
    shutil.rmtree(pack, ignore_errors=True)
    (pack / "textures").mkdir(parents=True)
    entries = []
    versions = set()
    for h, meta in sorted(index.items()):
        src = work / "hd" / f"{h}.png"
        if not src.exists():
            continue
        shutil.copy2(src, pack / "textures" / f"{h}.png")
        entries.append({"hashes": {"rt64": h, "rice": ""}, "path": f"textures/{h}.png"})
        versions.add(meta["version"])
    if len(versions) > 1:
        sys.exit(f"dumps with different hash versions {sorted(versions)}; dump again with one build")
    database = {
        "configuration": {"autoPath": "rt64", "configurationVersion": 3, "hashVersion": versions.pop()},
        "extraFiles": ["mod.json"],
        "operationFilters": [],
        "textures": entries,
    }
    (pack / "rt64.json").write_text(json.dumps(database, indent=4))
    (pack / "mod.json").write_text(json.dumps({
        "id": "southpark_hd_textures",
        "display_name": "South Park HD Textures",
        "short_description": "Upscaled textures",
        "description": "Textures dumped from your own copy of the game and upscaled with Real-ESRGAN "
                       "(tools/make_texture_pack.py).",
        "authors": ["you"],
        "game_id": "southpark",
        "version": "1.0.0",
        "minimum_recomp_version": "0.9.0",
        "dependencies": [],
    }, indent=4))
    # The packer insists on the low mip cache (it only holds DDS mips, so for PNG it stays small)
    # and writes "<folder name>.rtz" inside the folder it packs.
    subprocess.run([str(packer), str(pack), "--create-low-mip-cache"], check=True)
    subprocess.run([str(packer), str(pack), "--create-pack"], check=True)
    made = pack / f"{pack.name}.rtz"
    target = work / f"{name}.rtz"
    if not made.exists():
        sys.exit("texture_packer made no .rtz; see its output above")
    made.replace(target)
    print(f"{len(entries)} textures in {target}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", choices=["decode", "upscale", "pack", "all"])
    ap.add_argument("dump", type=Path, nargs="?", help="folder written by --dump-textures (decode, all)")
    ap.add_argument("work", type=Path, help="work folder for the intermediate files and the pack")
    ap.add_argument("--scale", type=int, default=4, choices=[2, 3, 4])
    ap.add_argument("--model", default="realesrgan-x4plus-anime")
    ap.add_argument("--min-size", type=int, default=8, help="leave textures smaller than this alone")
    ap.add_argument("--esrgan", type=Path, default=DEFAULT_ESRGAN)
    ap.add_argument("--packer", type=Path, default=DEFAULT_PACKER)
    ap.add_argument("--name", default="southpark_hd")
    a = ap.parse_args()
    a.work.mkdir(parents=True, exist_ok=True)
    if a.step in ("decode", "all"):
        if a.dump is None:
            sys.exit("decode needs the dump folder")
        cmd_decode(a.dump, a.work)
    if a.step in ("upscale", "all"):
        cmd_upscale(a.work, a.esrgan, a.model, a.scale, a.min_size)
    if a.step in ("pack", "all"):
        cmd_pack(a.work, a.packer, a.name)


if __name__ == "__main__":
    main()
