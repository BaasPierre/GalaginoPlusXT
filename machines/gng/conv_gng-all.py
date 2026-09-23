#!/usr/bin/env python3
# ============================================================
# conv_gng-all.py
#
# One-shot ROM converter for the Galagino Plus `gng` machine.
# The `gng` machine IS MAME `gng` = "Ghosts'n Goblins (World? set 1)"
# (Capcom, 1985, unencrypted MC6809 main + Z80 audio + 2x YM2203, ROT0).
#
# Self-contained (same style as romconv/conv_pbaction-all.py). Meant to be
# run from the romconv/ directory:
#
#     cd GalaginoPlus-main\romconv
#     python conv_gng-all.py
#
# With no arguments it reads GalaginoPlus-main\romszip\gng.zip and writes the
# headers into GalaginoPlus-main\source\src\machines\gng\ (both paths resolved
# relative to this script, so the working directory does not matter). It
# verifies every size + CRC32 against the MAME `gng` (parent) set first, so a
# wrong/renamed dump is caught early.
#
# USAGE
#     python conv_gng-all.py [ROMSRC] [-o OUTDIR] [--no-verify] [--no-preview]
#
#     ROMSRC   gng.zip, OR a folder holding the loose ROM files
#              (default: ../romszip/gng.zip relative to this script)
#     -o       output folder for the .h files
#              (default: ../source/src/machines/gng relative to this script)
#
# INPUT FILES  (MAME `gng` parent set - `mame gng -verifyroms` must pass)
#     mm_c_04 mm_c_03 mm_c_05           maincpu   0x04000 / 0x08000 / 0x10000
#     gg2.bin                           audiocpu  0x00000 (0x8000)
#     gg1.bin                           chars     0x00000 (0x4000)  8x8x2
#     gg11 gg10 gg9 gg8 gg7 gg6         tiles     6 x 0x4000        16x16x3
#     gg17 gg16 gg15 / gg14 gg13 gg12   sprites   ERASEFF 0x20000   16x16x4
#     tbp24s10.14k 63s141.2e            proms     (NOT USED by gng - see below)
#     gg-pal10l8.bin                    plds      (NOT USED - PAL dump)
#
# OUTPUT HEADERS  (written to OUTDIR)
#     gng_maincpu.h    const unsigned char gng_maincpu[0x18000]
#                        0x00000-0x03fff = 0xFF gap (unmapped)
#                        0x04000 mm_c_04, 0x08000 mm_c_03, 0x10000 mm_c_05
#                        CPU map: 0x4000-0x5fff = 8K bank window into this
#                        image (bank 0..3 -> +0x10000+n*0x2000, bank 4 ->
#                        +0x4000); 0x6000-0xffff -> +0x6000..+0xffff.
#     gng_audiocpu.h   const unsigned char gng_audiocpu[0x8000]   (gg2.bin)
#     gng_chars.h      const unsigned char gng_chars[1024][8][8]     (pen 0-3)
#     gng_tiles.h      const unsigned char gng_tiles[1024][16][16]   (pen 0-7)
#     gng_sprites.h    const unsigned char gng_sprites[1024][16][16] (pen 0-15)
#
# MAME's gng driver is ROT0. Tiles/chars/sprites are emitted here exactly as
# MAME's gfxdecode would decode them - native orientation, no rotation of any
# kind applied at conversion time. Any screen rotation needed for the target
# panel is entirely the renderer's problem (gng.cpp), not this script's.
#
# NOT generated (hand-authored / not ROM-derived):
#     gng_dipswitches.h  - DIP config, generated separately by dipgen.py from
#                          `mame -listxml gng` (already present, keep it).
#     gng_logo.h         - custom menu artwork (already present).
#     gng_proms.h        - REMOVED. MAME marks tbp24s10.14k "video timing (not
#                          used)" and 63s141.2e "priority (not used)"; gng has
#                          a RAM palette (0x3800 ext / 0x3900 base,
#                          xxxxBBBBGGGGRRRR) decoded live by the port. Nothing
#                          to extract.
#     gng_plds.h         - REMOVED. gg-pal10l8 is a PAL equation dump, unused
#                          by emulation.
#
# MAME reference: src/mame/capcom/gng.cpp  (driver by Nicola Salmoria)
#   MC6809 @ 12MHz/2, Z80 audio @ 12MHz/4, 2x YM2203 @ 12MHz/8
#   gng_main_map        0x4000-0x5fff bankr, 0x6000-0xffff rom, 0x3e00 bank_w
#   charlayout          8,8  RGN_FRAC(1,1) 2bpp  planes {4,0}
#   tilelayout          16,16 RGN_FRAC(1,3) 3bpp planes {RF(2,3),RF(1,3),RF(0,3)}
#   spritelayout        16,16 RGN_FRAC(1,2) 4bpp planes {RF(1,2)+4,RF(1,2)+0,4,0}
#   get_fg_tile_info    code = fgvram[i] + ((attr&0xc0)<<2)  color attr&0x0f  base 0x80
#   get_bg_tile_info    code = bgvram[i] + ((attr&0xc0)<<2)  color attr&0x07  base 0x00
#                       priority group = (attr&0x08)>>3
#   draw_sprites        code = spr[o] + ((attr<<2)&0x300)    color (attr>>4)&3 base 0x40
#                       transpen 15; attr: b0 sx hi, b2 flipx, b3 flipy
# ============================================================

from __future__ import annotations

import argparse
import sys
import zipfile
import zlib
from pathlib import Path

# --- MAME `gng` (parent) ROM manifest: name -> (size, CRC32) --------------
ROMS = {
    # maincpu (ROM_REGION 0x18000; 0x0000-0x3fff unmapped)
    "mm_c_04": (0x4000, 0x4F94130F),
    "mm_c_03": (0x8000, 0x1DEF138A),
    "mm_c_05": (0x8000, 0xED28E86E),
    # audiocpu (ROM_REGION 0x10000; only 0x0000-0x7fff is ROM)
    "gg2.bin": (0x8000, 0x615F5B6F),
    # chars (ROM_REGION 0x4000) 8x8x2
    "gg1.bin": (0x4000, 0xECFCCF07),
    # tiles (ROM_REGION 0x18000) 16x16x3, three 0x8000 planes
    "gg11.bin": (0x4000, 0xDDD56FA9),  # 0x00000  plane 1 half a
    "gg10.bin": (0x4000, 0x7302529D),  # 0x04000  plane 1 half b
    "gg9.bin":  (0x4000, 0x20035BDA),  # 0x08000  plane 2 half a
    "gg8.bin":  (0x4000, 0xF12BA271),  # 0x0c000  plane 2 half b
    "gg7.bin":  (0x4000, 0xE525207D),  # 0x10000  plane 3 half a
    "gg6.bin":  (0x4000, 0x2D77E9B2),  # 0x14000  plane 3 half b
    # sprites (ROM_REGION 0x20000, ROMREGION_ERASEFF) 16x16x4, two 0x10000 halves
    "gg17.bin": (0x4000, 0x93E50A8F),  # 0x00000  planes 1-2 third a
    "gg16.bin": (0x4000, 0x06D7E5CA),  # 0x04000  planes 1-2 third b
    "gg15.bin": (0x4000, 0xBC1FE02D),  # 0x08000  planes 1-2 third c
    "gg14.bin": (0x4000, 0x6AAF12F9),  # 0x10000  planes 3-4 third a
    "gg13.bin": (0x4000, 0xE80C3FCA),  # 0x14000  planes 3-4 third b
    "gg12.bin": (0x4000, 0x7780A925),  # 0x18000  planes 3-4 third c
    # proms / plds (present in the zip, verified, but NOT used by emulation)
    "tbp24s10.14k": (0x100, 0x0EAF5158),
    "63s141.2e":    (0x100, 0x4A1285A4),
    "gg-pal10l8.bin": (0x2C, 0x87F1B7E0),
}


# --------------------------------------------------------------------------
class RomSource:
    """Reads the gng ROM files either from a .zip or a loose directory, with
    size + CRC32 verification against the MAME manifest above."""

    def __init__(self, path: Path, verify: bool):
        self.verify = verify
        if path.is_dir():
            self.zip = None
            self.dir = path
            self.label = str(path)
        elif zipfile.is_zipfile(path):
            self.zip = zipfile.ZipFile(path, "r")
            self.dir = None
            self.label = str(path)
            self._members = {Path(n).name.lower(): n for n in self.zip.namelist()}
        else:
            sys.exit(f"ERROR: {path} is neither a directory nor a zip archive")

    def _raw(self, name: str) -> bytes:
        if self.zip is not None:
            member = self._members.get(name.lower())
            if member is None:
                sys.exit(f"ERROR: {name} not found in {self.label}")
            return self.zip.read(member)
        p = self.dir / name
        if not p.exists():
            sys.exit(f"ERROR: missing {name} in {self.label}")
        return p.read_bytes()

    def load(self, name: str) -> bytes:
        """Read one ROM, checking size and (optionally) CRC32."""
        size, crc = ROMS[name]
        b = self._raw(name)
        if len(b) != size:
            sys.exit(f"ERROR: {name}: expected {size} bytes, got {len(b)}")
        if self.verify:
            got = zlib.crc32(b) & 0xFFFFFFFF
            if got != crc:
                sys.exit(
                    f"ERROR: {name}: CRC32 0x{got:08X}, expected 0x{crc:08X}. "
                    f"Wrong or bad dump (run `mame gng -verifyroms`), or pass "
                    f"--no-verify to override."
                )
        return b


# --- generic MAME planar gfx decoder --------------------------------------
# planes / xoffs / yoffs are ABSOLUTE BIT offsets into `data`, exactly like
# gfx_element::decode() in src/emu/drawgfx.cpp. planes[0] is the
# most-significant pen bit, planes[-1] the least. MAME reads bit b of byte B
# as MSB-first: (data[B] >> (7 - b)) & 1.
def mame_decode(data: bytes, width: int, height: int, planes: list[int],
                xoffs: list[int], yoffs: list[int], bits_per_tile: int,
                count: int, base_bit: int = 0) -> list[list[list[int]]]:
    tiles = []
    for t in range(count):
        base = base_bit + t * bits_per_tile
        tile = []
        for y in range(height):
            row = []
            for x in range(width):
                v = 0
                for p in planes:
                    off = base + yoffs[y] + xoffs[x] + p
                    bit = (data[off >> 3] >> (7 - (off & 7))) & 1
                    v = (v << 1) | bit
                row.append(v)
            tile.append(row)
        tiles.append(tile)
    return tiles


# --------------------------------------------------------------------------
def build_maincpu(rs: RomSource) -> bytes:
    """MAME ROM_REGION 0x18000 "maincpu":
        mm_c_04 @0x04000 (0x4000)
        mm_c_03 @0x08000 (0x8000)
        mm_c_05 @0x10000 (0x8000)
    0x00000-0x03fff is never loaded (MAME default-fills 0x00; the CPU map
    never touches it, so 0xFF is equally fine and reads more clearly as a
    "not present" gap). We use 0xFF to match how the other Galagino Plus
    main-ROM images pad."""
    cpu = bytearray(b"\xFF" * 0x18000)
    cpu[0x04000:0x08000] = rs.load("mm_c_04")
    cpu[0x08000:0x10000] = rs.load("mm_c_03")
    cpu[0x10000:0x18000] = rs.load("mm_c_05")
    return bytes(cpu)


def build_audiocpu(rs: RomSource) -> bytes:
    """MAME ROM_REGION 0x10000 "audiocpu": gg2.bin @0x0000 (0x8000).
    sound_map only uses 0x0000-0x7fff as ROM (0xc000-0xc7ff is RAM,
    0xe000-0xe003 the two YM2203s), so 0x8000 bytes are all that is needed."""
    return rs.load("gg2.bin")


def build_chars(rs: RomSource) -> list[list[list[int]]]:
    """charlayout, GFXDECODE gfx[0] ("chars", palette base 0x80):
        8,8  RGN_FRAC(1,1)  2 planes
        planeoffset { 4, 0 }                    <- plane0 (MSB) = bit 4
        xoffs { 0,1,2,3, 8+0,8+1,8+2,8+3 }
        yoffs { 0*16, 1*16, ... 7*16 }
        charincrement 16*8

    region = gg1.bin (0x4000). count = 0x4000*8 / (16*8) = 1024, which also
    matches the max fg code fgvram[i] + ((attr&0xc0)<<2) = 0..0x3ff.
    color = attr & 0x0f ; Capcom fg convention = pen 3 transparent (the port
    decides at blit time)."""
    region = rs.load("gg1.bin")
    planes = [4, 0]                                  # MSB, LSB
    xoffs = [0, 1, 2, 3, 8, 9, 10, 11]
    yoffs = [y * 16 for y in range(8)]
    bpt = 16 * 8
    count = len(region) * 8 // bpt
    assert count == 1024, count
    return mame_decode(region, 8, 8, planes, xoffs, yoffs, bpt, count)


def build_tiles(rs: RomSource) -> list[list[list[int]]]:
    """tilelayout, GFXDECODE gfx[1] ("tiles", palette base 0x00):
        16,16  RGN_FRAC(1,3)  3 planes
        planeoffset { RGN_FRAC(2,3), RGN_FRAC(1,3), RGN_FRAC(0,3) }
                       ^MSB (=plane at bit-2 third)      ^LSB (first third)
        xoffs { 0,1,2,3,4,5,6,7, 16*8+0..7 }
        yoffs { 0*8, 1*8, ... 15*8 }
        charincrement 32*8

    region = gg11+gg10 (plane1, 0x8000) + gg9+gg8 (plane2) + gg7+gg6 (plane3).
    RGN_FRAC(1,3) plane stride = 0x8000*8 bits. count = (0x8000*8)/(32*8)
    = 1024, matching the max bg code bgvram[i] + ((attr&0xc0)<<2) = 0..0x3ff.
    color = attr & 0x07 ; priority group = (attr & 0x08) >> 3 (port handles
    the two-layer bg priority split)."""
    region = b"".join(rs.load(n) for n in (
        "gg11.bin", "gg10.bin",     # third 0  -> planeoffset RGN_FRAC(0,3) = LSB
        "gg9.bin",  "gg8.bin",      # third 1  -> RGN_FRAC(1,3)
        "gg7.bin",  "gg6.bin",      # third 2  -> RGN_FRAC(2,3) = MSB
    ))
    rf = len(region) * 8 // 3                        # RGN_FRAC(1,3) in bits
    planes = [2 * rf, 1 * rf, 0 * rf]                # MSB .. LSB
    xoffs = [i for i in range(8)] + [16 * 8 + i for i in range(8)]
    yoffs = [y * 8 for y in range(16)]
    bpt = 32 * 8
    count = rf // bpt
    assert count == 1024, count
    return mame_decode(region, 16, 16, planes, xoffs, yoffs, bpt, count)


def build_sprites(rs: RomSource) -> list[list[list[int]]]:
    """spritelayout, GFXDECODE gfx[2] ("sprites", palette base 0x40):
        16,16  RGN_FRAC(1,2)  4 planes
        planeoffset { RGN_FRAC(1,2)+4, RGN_FRAC(1,2)+0, 4, 0 }
                      ^MSB pair (2nd half)             ^LSB pair (1st half)
        xoffs { 0,1,2,3, 8+0,8+1,8+2,8+3, 32*8+0..3, 33*8+0..3 }
        yoffs { 0*16, 1*16, ... 15*16 }
        charincrement 64*8

    region = gg17+gg16+gg15 (1st half, 0x10000) + gg14+gg13+gg12 (2nd half).
    NOTE the zip loads the 2nd half at 0x10000 (there is a 0x0c000..0x0ffff
    ERASEFF gap after the first three 0x4000 ROMs) - RGN_FRAC(1,2) = exactly
    0x10000, so the halves line up. count = (0x10000*8)/(64*8) = 2048 by the
    layout, but only the low 1024 are addressable: sprite code
    spr[o] + ((attr<<2)&0x300) = 0..0x3ff. We decode 1024.
    color = (attr>>4) & 3 ; transparent pen 15."""
    half1 = b"".join(rs.load(n) for n in ("gg17.bin", "gg16.bin", "gg15.bin"))
    half2 = b"".join(rs.load(n) for n in ("gg14.bin", "gg13.bin", "gg12.bin"))
    assert len(half1) == 0xC000 and len(half2) == 0xC000
    # rebuild the region exactly as MAME's ROM_REGION(0x20000, ERASEFF):
    #   half1 at 0x00000 (fills 0x00000..0x0bfff, 0x0c000..0x0ffff = 0xFF)
    #   half2 at 0x10000 (fills 0x10000..0x1bfff, 0x1c000..0x1ffff = 0xFF)
    region = bytearray(b"\xFF" * 0x20000)
    region[0x00000:0x0C000] = half1
    region[0x10000:0x1C000] = half2
    region = bytes(region)

    rf = 0x10000 * 8                                 # RGN_FRAC(1,2) in bits
    planes = [rf + 4, rf + 0, 4, 0]                  # MSB .. LSB
    xoffs = [0, 1, 2, 3, 8, 9, 10, 11] + [32 * 8 + i for i in range(4)] + [33 * 8 + i for i in range(4)]
    yoffs = [y * 16 for y in range(16)]
    bpt = 64 * 8
    count = 1024                                     # only 0..0x3ff addressable
    return mame_decode(region, 16, 16, planes, xoffs, yoffs, bpt, count)


# --- header writers ------------------------------------------------------
BANNER = "// Generated by conv_gng-all.py from the MAME `gng` (parent) ROM set. Do not edit.\n"


def write_bytes_header(path: Path, guard: str, sym: str, data: bytes, note: str) -> None:
    with path.open("w") as f:
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        f.write(BANNER)
        f.write(f"// {note}\n\n")
        f.write(f"const unsigned char {sym}[{len(data)}] = {{\n")
        for i in range(0, len(data), 16):
            f.write("  " + ",".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",\n")
        f.write("};\n\n#endif\n")
    print(f"  wrote {path.name:22s} {len(data):#9x} bytes")


def write_tiles_header(path: Path, guard: str, sym: str, tiles: list, dim: int, note: str) -> None:
    with path.open("w") as f:
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        f.write(BANNER)
        f.write(f"// {note}\n")
        f.write(f"// {len(tiles)} tiles, {dim}x{dim} pixels, ROT0 (natural orientation).\n\n")
        f.write(f"const unsigned char {sym}[{len(tiles)}][{dim}][{dim}] = {{\n")
        for i, t in enumerate(tiles):
            rows = ["{" + ",".join(str(v) for v in t[y]) + "}" for y in range(dim)]
            f.write("  {" + ",".join(rows) + "}")
            f.write(",\n" if i < len(tiles) - 1 else "\n")
        f.write("};\n\n#endif\n")
    print(f"  wrote {path.name:22s} {len(tiles)} tiles ({dim}x{dim})")


# --- optional PNG preview ----------------------------------------------
def preview(chars, tiles, sprites, outpng: Path) -> None:
    try:
        from PIL import Image
    except ImportError:
        print("  (PIL not installed - skipping preview)")
        return

    PAL = [(20, 20, 20), (200, 60, 60), (60, 200, 60), (60, 60, 200),
           (200, 200, 60), (200, 60, 200), (60, 200, 200), (230, 230, 230),
           (120, 40, 40), (40, 120, 40), (40, 40, 120), (120, 120, 40),
           (120, 40, 120), (40, 120, 120), (150, 150, 150), (255, 255, 255)]

    def block(ts, dim, cols):
        rows = (len(ts) + cols - 1) // cols
        img = Image.new("RGB", (cols * (dim + 1), rows * (dim + 1)), (32, 32, 96))
        px = img.load()
        for i, t in enumerate(ts):
            ox, oy = (i % cols) * (dim + 1), (i // cols) * (dim + 1)
            for y in range(dim):
                for x in range(dim):
                    px[ox + x, oy + y] = PAL[t[y][x] & 15]
        return img

    parts = [block(chars, 8, 48), block(tiles, 16, 32), block(sprites, 16, 32)]
    W = max(p.width for p in parts)
    H = sum(p.height + 6 for p in parts)
    img = Image.new("RGB", (W, H), (0, 0, 0))
    y = 0
    for p in parts:
        img.paste(p, (0, y))
        y += p.height + 6
    img = img.resize((img.width * 2, img.height * 2), Image.NEAREST)
    img.save(outpng)
    print(f"  wrote {outpng.name} (preview)")


# --------------------------------------------------------------------------
def main() -> None:
    ap = argparse.ArgumentParser(description="Convert the MAME `gng` (parent) ROM set to gng headers.")
    ap.add_argument("romsrc", nargs="?", default=None,
                    help="gng.zip or a folder of loose ROM files "
                         "(default: ../romszip/gng.zip relative to this script)")
    ap.add_argument("-o", "--outdir", default=None,
                    help="output folder for the .h files "
                         "(default: ../source/src/machines/gng relative to this script)")
    ap.add_argument("--no-verify", action="store_true", help="skip CRC32 checks")
    ap.add_argument("--no-preview", action="store_true", help="do not write the PNG preview")
    args = ap.parse_args()

    # this script lives in GalaginoPlus-main/romconv/ ; repo root is its parent
    here = Path(__file__).resolve().parent
    root = here.parent
    romsrc = (Path(args.romsrc).resolve() if args.romsrc
              else (root / "romszip" / "gng.zip").resolve())
    outdir = (Path(args.outdir).resolve() if args.outdir
              else (root / "source" / "src" / "machines" / "gng").resolve())

    if not romsrc.exists():
        sys.exit(f"ERROR: ROM source {romsrc} does not exist")
    outdir.mkdir(parents=True, exist_ok=True)

    verify = not args.no_verify
    print(f"ROMs : {romsrc}")
    print(f"out  : {outdir}")
    print(f"CRC verify: {'on' if verify else 'OFF'}\n")

    rs = RomSource(romsrc, verify)

    write_bytes_header(outdir / "gng_maincpu.h", "_GNG_MAINCPU_H_",
                       "gng_maincpu", build_maincpu(rs),
                       "MC6809 space image (0x18000). 0x00000-0x03fff = 0xFF gap; "
                       "0x04000 mm_c_04, 0x08000 mm_c_03, 0x10000 mm_c_05. "
                       "CPU map: 0x4000-0x5fff = 8K bank (0x3e00 selects: 0..3 -> "
                       "0x10000+n*0x2000, 4 -> 0x4000); 0x6000-0xffff -> same offset here.")
    write_bytes_header(outdir / "gng_audiocpu.h", "_GNG_AUDIOCPU_H_",
                       "gng_audiocpu", build_audiocpu(rs),
                       "Z80 audio CPU ROM gg2.bin (0x0000-0x7fff). 0xc000-0xc7ff RAM, "
                       "0xe000/0xe002 = YM2203 #1/#2.")

    chars = build_chars(rs)
    tiles = build_tiles(rs)
    sprites = build_sprites(rs)

    write_tiles_header(outdir / "gng_chars.h", "_GNG_CHARS_H_",
                       "gng_chars", chars, 8,
                       "Foreground chars (gg1.bin), charlayout, 2bpp (pen 0-3), NATIVE "
                       "(unrotated) MAME orientation: gng_chars[code][y][x]. "
                       "gfx[0]: code = fgvram[i] + ((attr&0xc0)<<2); color = attr&0x0f; "
                       "flipyx = (attr&0x30)>>4; palette base 0x80; Capcom fg pen 3 transparent.")
    write_tiles_header(outdir / "gng_tiles.h", "_GNG_TILES_H_",
                       "gng_tiles", tiles, 16,
                       "Background tiles (gg6-gg11), tilelayout, 3bpp (pen 0-7), NATIVE "
                       "(unrotated) MAME orientation: gng_tiles[code][y][x]. "
                       "gfx[1]: code = bgvram[i] + ((attr&0xc0)<<2); color = attr&0x07; "
                       "flipyx = (attr&0x30)>>4; priority group = (attr&0x08)>>3; "
                       "palette base 0x00; opaque (pen 0 drawn).")
    write_tiles_header(outdir / "gng_sprites.h", "_GNG_SPRITES_H_",
                       "gng_sprites", sprites, 16,
                       "Sprites (gg12-gg17), spritelayout, 4bpp (pen 0-15), NATIVE "
                       "(unrotated) MAME orientation: gng_sprites[code][y][x]. "
                       "gfx[2]: code = spr[o] + ((attr<<2)&0x300); color = (attr>>4)&3; "
                       "attr b0 = sx bit8, b2 = flipx, b3 = flipy; palette base 0x40; "
                       "transparent pen 15. spriteram 0x1e00-0x1fff, buffered via 0x3c00.")

    if not args.no_preview:
        preview(chars, tiles, sprites, here / "gng_gfx_preview.png")

    print("\ndone.")


if __name__ == "__main__":
    main()
