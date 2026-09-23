#!/usr/bin/env python3
# ============================================================
# conv_boblbobl-all.py
#
# All contents needs to be verified as correct and not rely on comments in here.
#
# One-shot ROM converter for the Galagino Plus `boblbobl` machine.
# The `boblbobl` machine IS MAME `boblbobl` = "Bobble Bobble" (bootleg of
# Taito's "Bubble Bobble", 1986). Driver: src/mame/taito/bublbobl.cpp
# (ROM_START( boblbobl ), machine config bublbobl_state::boblbobl()).
#
# Three Z80s (main + sub + audio, all @ 24MHz/4 = 6MHz for main/sub,
# 24MHz/8 = 3MHz for audio), single YM2203 (no YM3526 on this bootleg -
# that's Tokio-only), NO protection MCU (the bootleg replaces the Bubble
# Bobble MC6801U4 with a simple PAL emulated by boblbobl_ic43_a/b in
# boblbobl.cpp - nothing to extract from ROMs for that).
#
# Self-contained (same style as romconv/conv_gng-all.py). Meant to be run
# from the romconv/ directory:
#
#     cd GalaginoPlus-main\romconv
#     python conv_boblbobl-all.py
#
# With no arguments it reads GalaginoPlus-main\romszip\boblbobl.zip and
# writes the headers into
# GalaginoPlus-main\source\src\machines\boblbobl\ (both paths resolved
# relative to this script, so the working directory does not matter). It
# verifies every size + CRC32 against the MAME `boblbobl` set first, so a
# wrong/renamed dump is caught early.
#
# USAGE
#     python conv_boblbobl-all.py [ROMSRC] [-o OUTDIR] [--no-verify] [--no-preview]
#
#     ROMSRC   boblbobl.zip, OR a folder holding the loose ROM files
#              (default: ../romszip/boblbobl.zip relative to this script)
#     -o       output folder for the .h files
#              (default: ../source/src/machines/boblbobl relative to this script)
#
# INPUT FILES  (MAME `boblbobl` set - `mame boblbobl -verifyroms` must pass)
#     bb3 bb5 bb4                       maincpu   0x08000 x3 (banked)
#     a78-08.37                         subcpu    0x08000
#     a78-07.46                         audiocpu  0x08000
#     a78-09.12 .. a78-14.17            gfx1      1st plane, 6 x 0x8000
#     a78-15.30 .. a78-20.35            gfx1      2nd plane, 6 x 0x8000
#     a71-25.41                         proms     0x100 (sprite composition
#                                                  lookup - USED by emulation,
#                                                  see build_gfx()'s callers
#                                                  in boblbobl.cpp)
#     pal16r4.u36 pal16l8.u38 pal16l8.u4  plds    (NOT USED by emulation)
#     a78-01.17                         (tokio MCU ROM, present in the dump
#                                        but NOT part of ROM_START(boblbobl) -
#                                        not read by this script)
#
# OUTPUT HEADERS  (written to OUTDIR)
#     boblbobl_maincpu.h   const unsigned char boblbobl_maincpu[0x30000]
#                            0x00000 bb3 (fixed, always mapped 0x0000-0x7fff)
#                            0x10000 bb5, 0x18000 bb4 (banked into
#                            0x8000-0xbfff, 4 x 0x4000 windows per bank per
#                            bublbobl_bankswitch_w - see boblbobl.cpp)
#                            0x08000-0x0ffff and 0x20000-0x2ffff are gaps
#                            (never loaded by MAME either - left 0xFF here).
#     boblbobl_subcpu.h    const unsigned char boblbobl_subcpu[0x8000] (a78-08.37)
#     boblbobl_audiocpu.h  const unsigned char boblbobl_audiocpu[0x8000] (a78-07.46)
#     boblbobl_gfx.h       const unsigned char boblbobl_gfx[16384][8][8] (pen 0-15)
#     boblbobl_proms.h     const unsigned char boblbobl_proms[0x100] (a71-25.41,
#                            sprite composition lookup - screen_update_bublbobl()
#                            reads this directly, NOT gfxdecode - see notes there)
#
# NOT generated (hand-authored / already present in this folder, kept as-is):
#     boblbobl_dipswitches.h  - DIP config, generated separately by dipgen.py
#                               from `mame -listxml boblbobl`.
#     boblbobl_logo.h         - custom menu artwork.
#     boblbobl_plds.h         - REMOVED. pal16r4.u36/pal16l8.u38/pal16l8.u4
#                               are PAL equation dumps, not read by emulation
#                               (boblbobl_ic43_a/b in boblbobl.cpp emulate the
#                               PAL's OBSERVED behavior directly in C, not by
#                               interpreting the fusemap).
#
# MAME reference: src/mame/taito/bublbobl.cpp (driver by Chris Moore, Nicola
# Salmoria), src/mame/taito/bublbobl_v.cpp (screen_update_bublbobl), src/mame/
# taito/bublbobl_m.cpp (boblbobl_ic43_a/b protection stubs).
#   Z80 main/sub @ 24MHz/4 = 6MHz, Z80 audio @ 24MHz/8 = 3MHz, YM2203 @ 3MHz
#   common_maincpu_map   0x0000-0x7fff rom, 0x8000-0xbfff bankr("bank1"),
#                        0xc000-0xdcff videoram, 0xdd00-0xdfff objectram,
#                        0xe000-0xf7ff share1 (shared w/ subcpu), 0xf800-0xf9ff
#                        palette (RGBx_444, big-endian, base+ext byte pairs)
#   bootleg_map          (boblbobl only) adds 0xfe00-0xfe03/0xfe80-0xfe83
#                        ic43 protection, 0xff00-0xff03 DSW0/DSW1/IN0/IN1
#   charlayout           8,8  RGN_FRAC(1,2)  4bpp planes {0,4,RGN_FRAC(1,2),
#                        RGN_FRAC(1,2)+4}  xoffs{3,2,1,0,8+3,8+2,8+1,8+0}
#                        (ONE single gfx layout used for EVERYTHING - there
#                        is no separate tile/sprite layout on this hardware;
#                        screen_update_bublbobl() composites both the
#                        background AND sprites out of the same videoram/
#                        objectram + gfx(0), driven by the proms lookup)
#   gfx1 region          0x80000 bytes, ROMREGION_INVERT (every byte
#                        bitwise-NOT'd after loading - replicated below)
# ============================================================

from __future__ import annotations

import argparse
import sys
import zipfile
import zlib
from pathlib import Path

# --- MAME `boblbobl` ROM manifest: name -> (size, CRC32) -------------------
ROMS = {
    # maincpu (ROM_REGION 0x30000; banked at 0x8000-0xbfff)
    "bb3": (0x8000, 0x01F81936),
    "bb5": (0x8000, 0x13118EB1),
    "bb4": (0x8000, 0xAFDA99D8),
    # subcpu (ROM_REGION 0x10000; only 0x0000-0x7fff is ROM)
    "a78-08.37": (0x8000, 0xAE11A07B),
    # audiocpu (ROM_REGION 0x10000; only 0x0000-0x7fff is ROM)
    "a78-07.46": (0x8000, 0x4F9A26E8),
    # gfx1 (ROM_REGION 0x80000, ROMREGION_INVERT) - 1st plane pair (4bpp lo)
    "a78-09.12": (0x8000, 0x20358C22),  # 0x00000
    "a78-10.13": (0x8000, 0x930168A9),  # 0x08000
    "a78-11.14": (0x8000, 0x9773E512),  # 0x10000
    "a78-12.15": (0x8000, 0xD045549B),  # 0x18000
    "a78-13.16": (0x8000, 0xD0AF35C5),  # 0x20000
    "a78-14.17": (0x8000, 0x7B5369A8),  # 0x28000
    # 0x30000-0x3ffff empty (ERASEFF gap - a78-14.17 lands at 0x28000, next
    # load a78-15.30 at 0x40000, so 0x30000-0x3ffff is never loaded)
    "a78-15.30": (0x8000, 0x6B61A413),  # 0x40000  2nd plane pair (4bpp hi)
    "a78-16.31": (0x8000, 0xB5492D97),  # 0x48000
    "a78-17.32": (0x8000, 0xD69762D5),  # 0x50000
    "a78-18.33": (0x8000, 0x9F243B68),  # 0x58000
    "a78-19.34": (0x8000, 0x66E9438C),  # 0x60000
    "a78-20.35": (0x8000, 0x9EF863AD),  # 0x68000
    # proms (ROM_REGION 0x100) - sprite composition lookup, USED by emulation
    "a71-25.41": (0x100, 0x2D0F8545),
    # plds (present in the zip, verified, but NOT used by emulation)
    "pal16r4.u36": (0x104, 0x22FE26AC),
    "pal16l8.u38": (0x104, 0xC02D9663),
    "pal16l8.u4":  (0x104, 0x077D20A8),
}



# --------------------------------------------------------------------------
class RomSource:
    """Reads the boblbobl ROM files either from a .zip or a loose directory,
    with size + CRC32 verification against the MAME manifest above."""

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
        """Read one ROM, checking size and (optionally) CRC32. PLD dumps
        (pal16*) are commonly stored as 260-byte JEDEC files even though
        MAME's ROM_LOAD only cares about the first 0x104 bytes of equation
        data - accept either the exact size or a larger file, truncating to
        the manifest size, so a 260-byte dump of a 0x104-byte entry still
        verifies instead of hard-failing on a length mismatch."""
        size, crc = ROMS[name]
        b = self._raw(name)
        if len(b) != size:
            if len(b) > size:
                b = b[:size]
            else:
                sys.exit(f"ERROR: {name}: expected {size} bytes, got {len(b)}")
        if self.verify:
            got = zlib.crc32(b) & 0xFFFFFFFF
            if got != crc:
                sys.exit(
                    f"ERROR: {name}: CRC32 0x{got:08X}, expected 0x{crc:08X}. "
                    f"Wrong or bad dump (run `mame boblbobl -verifyroms`), or pass "
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
    """MAME ROM_REGION 0x30000 "maincpu":
        bb3 @0x00000 (0x8000)   -- fixed, always mapped 0x0000-0x7fff
        bb5 @0x10000 (0x8000)   -- banked window source
        bb4 @0x18000 (0x8000)   -- banked window source
    0x08000-0x0ffff and 0x20000-0x2ffff are never loaded by MAME (comments
    in ROM_START say "20000-2ffff empty"; 0x08000-0x0ffff is simply the gap
    between bb3's end and bb5's start). Filled with 0xFF here, matching how
    this project's other main-ROM images pad unmapped gaps (see
    conv_gng-all.py's build_maincpu())."""
    cpu = bytearray(b"\xFF" * 0x30000)
    cpu[0x00000:0x08000] = rs.load("bb3")
    cpu[0x10000:0x18000] = rs.load("bb5")
    cpu[0x18000:0x20000] = rs.load("bb4")
    return bytes(cpu)


def build_subcpu(rs: RomSource) -> bytes:
    """MAME ROM_REGION 0x10000 "subcpu": a78-08.37 @0x0000 (0x8000).
    subcpu_map only uses 0x0000-0x7fff as ROM, so 0x8000 bytes suffice."""
    return rs.load("a78-08.37")


def build_audiocpu(rs: RomSource) -> bytes:
    """MAME ROM_REGION 0x10000 "audiocpu": a78-07.46 @0x0000 (0x8000).
    sound_map only uses 0x0000-0x7fff as ROM (0x8000-0x8fff RAM,
    0x9000-0xa001 YM2203, 0xb000-0xb002 latches/semaphores), so 0x8000 bytes
    are all that is needed."""
    return rs.load("a78-07.46")


def build_proms(rs: RomSource) -> bytes:
    """MAME ROM_REGION 0x100 "proms": a71-25.41. Read directly by
    screen_update_bublbobl() (bublbobl_v.cpp) as a per-sprite-row lookup
    table driving the "next column" / "skip this row" / row-group-of-4
    composition logic - UNLIKE gng's video-timing PROM, this one genuinely
    feeds emulation and must be included verbatim, unmodified."""
    return rs.load("a71-25.41")


def build_gfx(rs: RomSource) -> list[list[list[int]]]:
    """charlayout, GFXDECODE gfx[0] ("gfx1", the ONLY gfx layout on this
    hardware - both background tiles and sprites are drawn from it, see
    screen_update_bublbobl() in bublbobl_v.cpp):
        8,8  RGN_FRAC(1,2)  4 planes
        planeoffset { 0, 4, RGN_FRAC(1,2), RGN_FRAC(1,2)+4 }
                       ^LSB  ^             ^              ^MSB
        xoffs { 3,2,1,0, 8+3,8+2,8+1,8+0 }
        yoffs { 0*16, 1*16, ... 7*16 }
        charincrement 16*8

    region = a78-09..a78-14 (1st half, 0x30000) + 0x30000-0x3ffff ERASEFF gap
    (0x10000) + a78-15..a78-20 (2nd half, 0x30000) = 0x80000 total, exactly
    matching ROM_REGION( 0x80000, "gfx1", ROMREGION_INVERT ).
    RGN_FRAC(1,2) plane stride = 0x40000*8 bits (half of 0x80000).
    count = (0x40000*8)/(16*8) = 16384, matching the maximum code value
    screen_update_bublbobl() can construct: for background/fg entries,
    `code = m_videoram[goffs] + 256*(m_videoram[goffs+1]&0x03) +
    1024*(gfx_attr&0x0f)` reaches 255 + 256*3 + 1024*15 = 16383; sprite
    entries (drawn through the SAME gfx(0)) are built identically from
    m_objectram, so both draw calls index into this same 16384-tile set.

    ROMREGION_INVERT: MAME bitwise-NOTs every byte of this region after
    loading (see src/emu/romload.cpp region_post_process, ROMREGION_INVERT
    flag) - replicated here by XORing the assembled region with 0xFF before
    decoding, since gfx_element::decode() runs on the ALREADY-inverted ROM
    image on real MAME."""
    half1 = b"".join(rs.load(n) for n in (
        "a78-09.12", "a78-10.13", "a78-11.14",
        "a78-12.15", "a78-13.16", "a78-14.17",
    ))
    half2 = b"".join(rs.load(n) for n in (
        "a78-15.30", "a78-16.31", "a78-17.32",
        "a78-18.33", "a78-19.34", "a78-20.35",
    ))
    assert len(half1) == 0x30000 and len(half2) == 0x30000

    region = bytearray(b"\x00" * 0x80000)
    region[0x00000:0x30000] = half1
    region[0x40000:0x70000] = half2
    # apply ROMREGION_INVERT
    region = bytes((b ^ 0xFF) for b in region)

    rf = 0x40000 * 8                                 # RGN_FRAC(1,2) in bits
    # Real MAME charlayout planeoffset = { 0, 4, RGN_FRAC(1,2), RGN_FRAC(1,2)+4 }.
    # gfx_element::decode() (source/mame/mame-master/src/emu/drawgfx.cpp)
    # assigns planeoffset[0] to the MOST significant pen bit (planebit
    # starts at 1<<(planes-1) and shifts right as the plane index
    # increases) - so this array must be written in the SAME order as the
    # real planeoffset{} initializer, MSB-first: [0, 4, rf, rf+4].
    # BUG FIX: an earlier version of this script had this reversed AND
    # transposed ([rf+4, rf, 4, 0]), which silently scrambled every tile's
    # pixel values while still looking superficially plausible in the PNG
    # preview (a consistent wrong permutation produces stable, structured
    # -looking noise, not random garbage) - found by re-deriving this
    # directly from gfx_element::decode() instead of trusting the prior
    # transcription.
    planes = [0, 4, rf, rf + 4]                      # MSB .. LSB
    xoffs = [3, 2, 1, 0, 8 + 3, 8 + 2, 8 + 1, 8 + 0]
    yoffs = [y * 16 for y in range(8)]
    bpt = 16 * 8
    count = rf // bpt
    assert count == 16384, count
    return mame_decode(region, 8, 8, planes, xoffs, yoffs, bpt, count)


# --- header writers ------------------------------------------------------
BANNER = "// Generated by conv_boblbobl-all.py from the MAME `boblbobl` ROM set. Do not edit.\n"


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
def preview(gfx, outpng: Path) -> None:
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

    img = block(gfx, 8, 64)
    img = img.resize((img.width * 2, img.height * 2), Image.NEAREST)
    img.save(outpng)
    print(f"  wrote {outpng.name} (preview)")


# --------------------------------------------------------------------------
def main() -> None:
    ap = argparse.ArgumentParser(description="Convert the MAME `boblbobl` ROM set to boblbobl headers.")
    ap.add_argument("romsrc", nargs="?", default=None,
                    help="boblbobl.zip or a folder of loose ROM files "
                         "(default: ../romszip/boblbobl.zip relative to this script)")
    ap.add_argument("-o", "--outdir", default=None,
                    help="output folder for the .h files "
                         "(default: ../source/src/machines/boblbobl relative to this script)")
    ap.add_argument("--no-verify", action="store_true", help="skip CRC32 checks")
    ap.add_argument("--no-preview", action="store_true", help="do not write the PNG preview")
    args = ap.parse_args()

    # this script lives in GalaginoPlus-main/romconv/ ; repo root is its parent
    here = Path(__file__).resolve().parent
    root = here.parent
    romsrc = (Path(args.romsrc).resolve() if args.romsrc
              else (root / "romszip" / "boblbobl.zip").resolve())
    outdir = (Path(args.outdir).resolve() if args.outdir
              else (root / "source" / "src" / "machines" / "boblbobl").resolve())

    if not romsrc.exists():
        sys.exit(f"ERROR: ROM source {romsrc} does not exist")
    outdir.mkdir(parents=True, exist_ok=True)

    verify = not args.no_verify
    print(f"ROMs : {romsrc}")
    print(f"out  : {outdir}")
    print(f"CRC verify: {'on' if verify else 'OFF'}\n")

    rs = RomSource(romsrc, verify)

    write_bytes_header(outdir / "boblbobl_maincpu.h", "_BOBLBOBL_MAINCPU_H_",
                       "boblbobl_maincpu", build_maincpu(rs),
                       "Main Z80 space image (0x30000). 0x00000 bb3 (fixed, "
                       "0x0000-0x7fff); 0x10000 bb5, 0x18000 bb4 (banked "
                       "0x8000-0xbfff windows, 0x4000 each, selected by "
                       "bublbobl_bankswitch_w bits 0-2 XORed with 4 - see "
                       "boblbobl.cpp). 0x08000-0x0ffff and 0x20000-0x2ffff "
                       "are unmapped gaps, filled 0xFF.")
    write_bytes_header(outdir / "boblbobl_subcpu.h", "_BOBLBOBL_SUBCPU_H_",
                       "boblbobl_subcpu", build_subcpu(rs),
                       "Sub Z80 CPU ROM a78-08.37 (0x0000-0x7fff). Shares "
                       "0xe000-0xf7ff RAM with the main CPU (share1/ "
                       "SUBCPU_SHARED_RAM); NMI'd by the main CPU via "
                       "bublbobl_nmitrigger_w.")
    write_bytes_header(outdir / "boblbobl_audiocpu.h", "_BOBLBOBL_AUDIOCPU_H_",
                       "boblbobl_audiocpu", build_audiocpu(rs),
                       "Audio Z80 CPU ROM a78-07.46 (0x0000-0x7fff). "
                       "0x8000-0x8fff RAM, 0x9000/0x9001 = YM2203 "
                       "(mirrored 0x9000-0x9ffe), 0xb000-0xb002 = "
                       "latches/semaphores/soundnmi ack (mirrored "
                       "0xb000-0xbffc).")
    write_bytes_header(outdir / "boblbobl_proms.h", "_BOBLBOBL_PROMS_H_",
                       "boblbobl_proms", build_proms(rs),
                       "Sprite composition lookup PROM a71-25.41 (0x100 "
                       "bytes) - read directly by screen_update_bublbobl() "
                       "to decide, per sprite metarow (32 of them per "
                       "sprite slot), whether to skip it, start a new "
                       "column, and which quadrant-pair of gfx tiles to "
                       "use. USED by emulation (unlike e.g. gng's unused "
                       "video-timing PROM) - see boblbobl.cpp's scan_sprites().")

    gfx = build_gfx(rs)
    write_tiles_header(outdir / "boblbobl_gfx.h", "_BOBLBOBL_GFX_H_",
                       "boblbobl_gfx", gfx, 8,
                       "The ONE gfx layout used for both background tiles "
                       "AND sprites on this hardware (charlayout, 4bpp, "
                       "pen 0-15), NATIVE (unrotated) MAME orientation: "
                       "boblbobl_gfx[code][y][x]. ROMREGION_INVERT already "
                       "applied (every source byte bitwise-NOT'd, matching "
                       "MAME's own post-load region processing) - do NOT "
                       "invert again in the emulator. code is a 14-bit "
                       "0-16383 index built by screen_update_bublbobl() from "
                       "videoram/objectram bytes plus the attribute byte's "
                       "high nibble (see this script's build_gfx() docstring "
                       "for the exact formula); transparent pen is 15.")

    if not args.no_preview:
        preview(gfx, here / "boblbobl_gfx_preview.png")

    print("\ndone.")


if __name__ == "__main__":
    main()
