#!/usr/bin/env python3
# ============================================================
# conv_arkanoid-all.py
#
# One-shot ROM converter for the Galagino Plus `arkanoid` machine.
# The `arkanoid` machine IS MAME `arkanoid` = "Arkanoid (World, older)"
# (Taito 1986). Driver: src/mame/taito/arkanoid.cpp (ROM_START( arkanoid ),
# machine config arkanoid_state::arkanoid()), video arkanoid_v.cpp,
# MCU interface src/mame/shared/taito68705.cpp (ARKANOID_68705P5).
#
# Everything below is taken from those MAME files - re-check them, not these
# comments, if anything looks wrong.
#
# USAGE
#     python conv_arkanoid-all.py [ROMSRC] [-o OUTDIR] [--no-verify] [--no-preview]
#
#     ROMSRC   arkanoid.zip, OR a folder holding the loose ROM files
#              (default: GalaginoPlus-main/romconv/arkanoid.zip, resolved
#              relative to this script - the working directory does not matter)
#     -o       output folder for the .h files (default: this script's folder)
#
# INPUT FILES  (MAME `arkanoid` set - `mame arkanoid -verifyroms` must pass)
#     a75__01-1.ic17   maincpu 0x0000-0x7fff
#     a75__11.ic16     maincpu 0x8000-0xbfff... (0x8000 bytes loaded at 0x8000;
#                      arkanoid_map maps 0x0000-0xbfff as ROM, so only the
#                      first 0x4000 of this chip are reachable by the Z80)
#     a75__06.ic14     "mcu:mcu" 0x800 - MC68705P5 program (ARKANOID_68705P5)
#     a75__03.ic64     gfx1 0x00000 (bitplane 0 = pen bit 0)
#     a75__04.ic63     gfx1 0x08000 (bitplane 1 = pen bit 1)
#     a75__05.ic62     gfx1 0x10000 (bitplane 2 = pen bit 2)
#     a75-07.ic24      proms 0x000 red   (512 x 4 bit)
#     a75-08.ic23      proms 0x200 green
#     a75-09.ic22      proms 0x400 blue
#   Not used (ROM_REGION "alt_mcus", not loaded into any device):
#     arkanoid_mcu.ic14, a75-06__bootleg_68705.ic14, arkanoid1_68705p3.ic14
#
# OUTPUT HEADERS  (written to OUTDIR)
#     arkanoid_rom.h      const unsigned char arkanoid_rom[0xC000]
#     arkanoid_mcu.h      const unsigned char arkanoid_mcu[0x800]
#     arkanoid_gfx.h      const unsigned char arkanoid_gfx[4096][8][8]  (pen 0-7, [y][x])
#     arkanoid_palette.h  const unsigned short arkanoid_palette[512]
#                         (RGB565, byte-swapped for the display DMA - same
#                          convention as the other machines' palettes)
#
# NOT generated (kept as-is): arkanoid_dipswitches.h (tools/dipswitches/dipgen.py),
# arkanoid_logo.h (menu artwork).
# ============================================================

from __future__ import annotations

import argparse
import sys
import zipfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
# source/src/machines/arkanoid -> GalaginoPlus-main
PROJECT = HERE.parents[3]

# --- MAME `arkanoid` ROM manifest: name -> (size, CRC32) --------------------
ROMS = {
    "a75__01-1.ic17": (0x8000, 0x5BCDA3B0),
    "a75__11.ic16":   (0x8000, 0xEAFD7191),
    "a75__06.ic14":   (0x0800, 0x0BE83647),
    "a75__03.ic64":   (0x8000, 0x038B74BA),
    "a75__04.ic63":   (0x8000, 0x71FAE199),
    "a75__05.ic62":   (0x8000, 0xC76374E2),
    "a75-07.ic24":    (0x0200, 0x0AF8B289),
    "a75-08.ic23":    (0x0200, 0xABB002FB),
    "a75-09.ic22":    (0x0200, 0xA7C6C277),
}


class RomSource:
    def __init__(self, path: Path, verify: bool):
        self.path = path
        self.verify = verify
        self.zip = zipfile.ZipFile(path) if path.is_file() else None
        if self.zip is None and not path.is_dir():
            sys.exit(f"ROM source not found: {path}")

    def _raw(self, name: str) -> bytes:
        if self.zip is not None:
            # zip entries may sit in a sub folder
            for info in self.zip.infolist():
                if info.filename.split("/")[-1].lower() == name.lower():
                    return self.zip.read(info)
            sys.exit(f"{name} not found in {self.path}")
        p = self.path / name
        if not p.is_file():
            sys.exit(f"{name} not found in {self.path}")
        return p.read_bytes()

    def load(self, name: str) -> bytes:
        data = self._raw(name)
        size, crc = ROMS[name]
        if self.verify:
            got = zlib.crc32(data) & 0xFFFFFFFF
            if len(data) != size or got != crc:
                sys.exit(f"{name}: size {len(data):#x}/{size:#x} crc {got:08X}/{crc:08X} - wrong dump")
        return data


# ROM_REGION( 0x10000, "maincpu", 0 ); arkanoid_map: map(0x0000, 0xbfff).rom()
def build_rom(rs: RomSource) -> bytes:
    region = bytearray([0xFF] * 0x10000)
    region[0x0000:0x8000] = rs.load("a75__01-1.ic17")
    region[0x8000:0x10000] = rs.load("a75__11.ic16")
    return bytes(region[:0xC000])


# ROM_REGION( 0x0800, "mcu:mcu", 0 )
def build_mcu(rs: RomSource) -> bytes:
    return rs.load("a75__06.ic14")


# charlayout (arkanoid.cpp):
#   8,8, 4096, 3,
#   { 2*4096*8*8, 4096*8*8, 0 }    planes, first entry = most significant pen bit
#   { 0, 1, 2, 3, 4, 5, 6, 7 }     x: bit 0 = MSB of the row byte (MAME bit order)
#   { 0*8 .. 7*8 }                 y: one byte per row
#   8*8                            8 bytes per char
def build_gfx(rs: RomSource) -> list:
    region = rs.load("a75__03.ic64") + rs.load("a75__04.ic63") + rs.load("a75__05.ic62")
    planes = [2 * 4096 * 64, 4096 * 64, 0]      # bit offsets
    chars = []
    for c in range(4096):
        rows = []
        for y in range(8):
            row = []
            for x in range(8):
                pen = 0
                for p in planes:
                    bit = p + c * 64 + y * 8 + x
                    pen = (pen << 1) | ((region[bit >> 3] >> (7 - (bit & 7))) & 1)
                row.append(pen)
            rows.append(row)
        chars.append(rows)
    return chars


# PALETTE(config, m_palette, palette_device::RGB_444_PROMS, "proms", 512)
# emupal.cpp palette_init_rgb_444_proms(): weights 0x0e, 0x1f, 0x43, 0x8f
def build_palette(rs: RomSource) -> list:
    proms = rs.load("a75-07.ic24") + rs.load("a75-08.ic23") + rs.load("a75-09.ic22")

    def level(v: int) -> int:
        return (0x0E * (v & 1) + 0x1F * ((v >> 1) & 1) +
                0x43 * ((v >> 2) & 1) + 0x8F * ((v >> 3) & 1))

    rgb = []
    for i in range(512):
        rgb.append((level(proms[i]), level(proms[i + 512]), level(proms[i + 1024])))
    return rgb


def rgb565_swapped(r: int, g: int, b: int) -> int:
    c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return ((c >> 8) | (c << 8)) & 0xFFFF


def write_bytes_header(path: Path, guard: str, sym: str, data: bytes, note: str) -> None:
    with path.open("w", newline="\n") as f:
        f.write(f"#ifndef {guard}\n#define {guard}\n\n// {note}\n// Generated by conv_arkanoid-all.py - do not edit.\n\n")
        f.write(f"const unsigned char {sym}[{len(data):#x}] = {{\n")
        for i in range(0, len(data), 16):
            f.write("  " + ",".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",\n")
        f.write("};\n\n#endif\n")


def write_gfx_header(path: Path, chars: list) -> None:
    with path.open("w", newline="\n") as f:
        f.write("#ifndef ARKANOID_GFX_H\n#define ARKANOID_GFX_H\n\n")
        f.write("// gfx1 decoded with MAME charlayout: [code][y][x], pen 0-7\n")
        f.write("// Generated by conv_arkanoid-all.py - do not edit.\n\n")
        f.write(f"const unsigned char arkanoid_gfx[{len(chars)}][8][8] = {{\n")
        for c, ch in enumerate(chars):
            f.write(" {" + ",".join("{" + ",".join(str(p) for p in row) + "}" for row in ch) + f"}}, // {c:03x}\n")
        f.write("};\n\n#endif\n")


def write_palette_header(path: Path, rgb: list) -> None:
    with path.open("w", newline="\n") as f:
        f.write("#ifndef ARKANOID_PALETTE_H\n#define ARKANOID_PALETTE_H\n\n")
        f.write("// 512 colours from proms a75-07/08/09 (RGB_444_PROMS, emupal.cpp weights),\n")
        f.write("// RGB565 byte-swapped for the display DMA. Index = colour group * 8 + pen.\n")
        f.write("// Generated by conv_arkanoid-all.py - do not edit.\n\n")
        f.write("const unsigned short arkanoid_palette[512] = {\n")
        for i in range(0, 512, 8):
            f.write("  " + ",".join(f"0x{rgb565_swapped(*rgb[j]):04x}" for j in range(i, i + 8)) + ",\n")
        f.write("};\n\n#endif\n")


def preview(chars: list, rgb: list, outpng: Path) -> None:
    try:
        from PIL import Image
    except ImportError:
        print("PIL not installed - no preview written")
        return
    # 64 x 64 chars, colour group 0 of palette bank 0... use a fixed grey ramp
    # so every pen is visible regardless of palette.
    img = Image.new("RGB", (64 * 8, 64 * 8))
    px = img.load()
    for c, ch in enumerate(chars):
        ox, oy = (c % 64) * 8, (c // 64) * 8
        for y in range(8):
            for x in range(8):
                v = ch[y][x] * 36
                px[ox + x, oy + y] = (v, v, v)
    img.save(outpng)
    pal = Image.new("RGB", (32 * 8, 16 * 8))
    pp = pal.load()
    for i in range(512):
        ox, oy = (i % 32) * 8, (i // 32) * 8
        for y in range(8):
            for x in range(8):
                pp[ox + x, oy + y] = rgb[i]
    pal.save(outpng.with_name(outpng.stem + "_palette.png"))
    print(f"preview: {outpng}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("romsrc", nargs="?", default=str(PROJECT / "romconv" / "arkanoid.zip"))
    ap.add_argument("-o", "--outdir", default=str(HERE))
    ap.add_argument("--no-verify", action="store_true")
    ap.add_argument("--no-preview", action="store_true")
    args = ap.parse_args()

    rs = RomSource(Path(args.romsrc), not args.no_verify)
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)

    write_bytes_header(out / "arkanoid_rom.h", "ARKANOID_ROM_H", "arkanoid_rom", build_rom(rs),
                       "maincpu 0x0000-0xbfff: a75__01-1.ic17 + a75__11.ic16 (first 0x4000)")
    write_bytes_header(out / "arkanoid_mcu.h", "ARKANOID_MCU_H", "arkanoid_mcu", build_mcu(rs),
                       "MC68705P5 program a75__06.ic14 (ARKANOID_68705P5)")
    chars = build_gfx(rs)
    write_gfx_header(out / "arkanoid_gfx.h", chars)
    rgb = build_palette(rs)
    write_palette_header(out / "arkanoid_palette.h", rgb)
    print(f"headers written to {out}")

    if not args.no_preview:
        preview(chars, rgb, PROJECT / "source" / "debug" / "arkanoid_gfx_preview.png")


if __name__ == "__main__":
    main()
