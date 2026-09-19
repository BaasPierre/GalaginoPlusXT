#!/usr/bin/env python3
r"""dipgen.py -- generate a <game>_dipswitches.h from MAME's own dip-switch data.

WHAT IT DOES
------------
Runs  `mame.exe -listxml <game>`  and parses the <dipswitch> elements it
emits. That XML is MAME resolving the EXACT ioport the game's GAME/GAMEL line
references (after all PORT_INCLUDE / PORT_MODIFY), so it is immune to the
classic mistake of reading the wrong INPUT_PORTS block by hand (e.g. taking
`nonplus_poker`'s SW1 when the game actually uses `peplus_poker`'s).

For every dip PORT (tag, e.g. SW1 / DSW1 / DSW2 ...) it writes:
  - one #define per switch option (value masked into that switch's bits)
  - one #define per switch selecting the MAME default
  - a <GAME>_<TAG>_DEFAULT that ORs the per-switch defaults into the full
    power-on byte
and a header comment recording where each number came from.

The DEFAULT byte = OR of every dipvalue whose  default="yes"  (first one, if
MAME lists several for an overlapping multi-bit switch). Unused bits declared
via PORT_DIPUNUSED_DIPLOC show up in -listxml as name="Unused" with the bit
SET by default (MAME uses IP_ACTIVE_LOW == 0xFFFFFFFF as the field default),
so they are folded into DEFAULT automatically -- matching real hardware,
where an unwired DIP line floats/reads high.

WHAT YOU NEED
-------------
  - mame.exe  (this script auto-detects C:\mame\mame.exe and a few common
    spots; override with  --mame PATH  or the MAME env var)
  - the game's ROMs are NOT required for -listxml (it's static metadata)

USAGE
-----
  python dipgen.py <game>                 # -> ./<game>_dipswitches.h  (here)
  python dipgen.py <game> --src           # -> ../../src/machines/<game>/<game>_dipswitches.h
  python dipgen.py <game> --outdir DIR
  python dipgen.py <game> --stdout        # print, don't write
  python dipgen.py <game> --prefix FOO    # macro prefix (default: GAME upper)
  python dipgen.py <g1> <g2> ...          # several at once
  python dipgen.py <game> --xml FILE      # parse a saved -listxml file, skip mame

  # skip mame entirely and pipe XML in:
  mame -listxml <game> | python dipgen.py <game> --xml -

EXAMPLE
-------
  python dipgen.py peps0043 --src
    peps0043 uses `peplus_slots` -> PORT_INCLUDE(peplus), no PORT_MODIFY
    -> SW1 = { SW1:1 Line Frequency 60Hz(default)/50Hz, SW1:2..8 Unused/high }
    -> PEPS0043_SW1_DEFAULT == 0xFF
"""
import argparse
import os
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent


# ----------------------------------------------------------------------------
# locating mame.exe
# ----------------------------------------------------------------------------
def find_mame(explicit: str | None) -> str:
    cands = []
    if explicit:
        cands.append(Path(explicit))
    if os.environ.get("MAME"):
        cands.append(Path(os.environ["MAME"]))
    cands += [
        Path(r"C:\mame\mame.exe"),
        Path(r"C:\mame\mame-master\mame.exe"),
        HERE.parents[2] / "mame" / "mame.exe",   # source/mame/mame.exe
        Path("mame.exe"), Path("mame"),
    ]
    for c in cands:
        if c.exists() or c.name == c.as_posix():  # last two: rely on PATH
            if c.exists():
                return str(c)
    # fall back to PATH lookup
    from shutil import which
    for n in ("mame", "mame64", "mame.exe"):
        p = which(n)
        if p:
            return p
    return ""


def get_listxml(game: str, mame: str, xml_arg: str | None) -> str:
    if xml_arg == "-":
        return sys.stdin.read()
    if xml_arg:
        return Path(xml_arg).read_text(errors="replace")
    if not mame:
        sys.exit("ERROR: mame.exe not found. Pass --mame PATH, set the MAME "
                 "env var, or use --xml FILE with a saved `-listxml` dump.")
    try:
        out = subprocess.run([mame, "-listxml", game],
                             capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as e:
        sys.exit(f"ERROR running mame -listxml {game}: {e}")
    if out.returncode != 0 or "<mame" not in out.stdout:
        sys.exit(f"ERROR: `mame -listxml {game}` failed:\n{out.stderr.strip()}")
    return out.stdout


# ----------------------------------------------------------------------------
# parsing
# ----------------------------------------------------------------------------
def c_ident(text: str) -> str:
    """Turn a MAME dip name / option label into a C identifier fragment."""
    out = []
    for ch in text.upper():
        if ch.isalnum():
            out.append(ch)
        elif ch in " -/.":
            out.append("_")
        # drop everything else (%, +, ', (), commas, ...)
    s = "".join(out)
    while "__" in s:
        s = s.replace("__", "_")
    return s.strip("_") or "OPT"


def parse_dips(xml_text: str, game: str):
    """Return {tag: {'mask': int, 'switches': [ ... ]}} for the requested game.

    Each switch: {name, mask, bits:[nums], options:[(label,value,is_default)],
                  default_value:int}
    """
    root = ET.fromstring(xml_text)
    machine = None
    for m in root.iter("machine"):
        if m.get("name") == game:
            machine = m
            break
    if machine is None:
        # -listxml <game> returns exactly one machine; take it if names differ
        machines = list(root.iter("machine"))
        if len(machines) == 1:
            machine = machines[0]
        else:
            sys.exit(f"ERROR: machine '{game}' not found in the XML "
                     f"(saw: {[m.get('name') for m in machines]}).")

    ports: dict[str, dict] = {}
    for ds in machine.iter("dipswitch"):
        tag = (ds.get("tag") or "DSW").lstrip(":")
        mask = int(ds.get("mask") or "0")
        name = ds.get("name") or "Unknown"
        bits = [int(dl.get("number")) for dl in ds.iter("diplocation")
                if dl.get("number")]
        opts = []
        default_value = None
        for dv in ds.iter("dipvalue"):
            val = int(dv.get("value") or "0")
            is_def = dv.get("default") == "yes"
            opts.append((dv.get("name") or "?", val, is_def))
            if is_def and default_value is None:
                default_value = val
        if default_value is None:
            default_value = 0
        ports.setdefault(tag, {"mask": 0, "switches": []})
        ports[tag]["mask"] |= mask
        ports[tag]["switches"].append({
            "name": name, "mask": mask, "bits": bits,
            "options": opts, "default_value": default_value,
        })

    # keep switches in physical order
    for p in ports.values():
        p["switches"].sort(key=lambda s: (min(s["bits"]) if s["bits"] else 99,
                                          s["mask"]))
    return ports, machine


# ----------------------------------------------------------------------------
# emit
# ----------------------------------------------------------------------------
def emit(game: str, prefix: str, ports: dict, mame_ver: str) -> str:
    P = prefix
    guard = f"{game.upper()}_DIPSWITCHES_H"
    L = []
    L.append(f"#ifndef {guard}")
    L.append(f"#define {guard}")
    L.append("")
    L.append("// ==========================================================================")
    L.append(f"// {game}_dipswitches.h  --  GENERATED by tools/dipswitches/dipgen.py")
    L.append(f"// Source: `mame -listxml {game}`" +
             (f"  ({mame_ver})" if mame_ver else ""))
    L.append("//")
    L.append("// -listxml is MAME resolving the exact ioport this game's GAME/GAMEL line")
    L.append("// uses, after every PORT_INCLUDE / PORT_MODIFY -- so these values are the")
    L.append("// real ones for THIS set, not a hand-read of the wrong INPUT_PORTS block.")
    L.append("//")
    L.append("// Each <TAG>_DEFAULT below = OR of every dipvalue marked default=\"yes\".")
    L.append("// 'Unused' switches (PORT_DIPUNUSED_DIPLOC) default to their bit SET, i.e.")
    L.append("// they read HIGH -- same as an unwired DIP line on real hardware.")
    L.append("//")
    L.append("// Re-run dipgen.py to regenerate. Hand-edit only the SELECTED #defines")
    L.append("// (below each switch) if you want a non-default machine configuration.")
    L.append("// ==========================================================================")
    L.append("")

    for tag, p in ports.items():
        default_byte = 0
        for s in p["switches"]:
            default_byte |= (s["default_value"] & s["mask"])

        L.append("// " + "-" * 74)
        L.append(f"// {tag}   (port mask {p['mask']:#04x})")
        L.append("// " + "-" * 74)
        # switch-name -> how many times it appears in this port, so we can
        # disambiguate duplicates (e.g. seven "Unused" bits) by location.
        name_counts: dict[str, int] = {}
        for s in p["switches"]:
            name_counts[s["name"]] = name_counts.get(s["name"], 0) + 1

        for s in p["switches"]:
            locs = "+".join(str(b) for b in s["bits"]) or "?"
            # base identifier for this switch; append the bit location when the
            # same name is used more than once in this port (keeps macros unique)
            sw_id = c_ident(s["name"])
            if name_counts[s["name"]] > 1:
                sw_id = f"{sw_id}_SW{'_'.join(str(b) for b in s['bits']) or hex(s['mask'])[2:].upper()}"
            L.append(f"//  {tag}:{locs}  {s['name']}  (bits {s['mask']:#04x})")
            # some MAME switches list the same option label twice with
            # different values (overlapping conditional setting groups) --
            # disambiguate those by the masked value.
            label_counts: dict[str, int] = {}
            for lab, _v, _d in s["options"]:
                label_counts[lab] = label_counts.get(lab, 0) + 1
            def_line = None
            for label, val, is_def in s["options"]:
                lab_id = c_ident(label)
                if label_counts[label] > 1:
                    lab_id = f"{lab_id}_{val & s['mask']:02X}"
                macro = f"{P}_{c_ident(tag)}_{sw_id}_{lab_id}"
                tag_str = "   // default" if is_def else ""
                L.append(f"//    {label:<28s} = {val & s['mask']:#04x}{tag_str}")
                line = f"#define {macro:<52s} {val & s['mask']:#04x}"
                if is_def and def_line is None:
                    def_line = (macro, line)
                L.append(line)
            # the SELECTED alias for this switch (points at the default)
            sel = f"{P}_{c_ident(tag)}_{sw_id}_SELECTED"
            if def_line:
                L.append(f"#define {sel:<52s} {def_line[0]}")
            else:
                L.append(f"#define {sel:<52s} {s['default_value'] & s['mask']:#04x}")
            L.append("")

        # the full power-on byte for this port (same disambiguated ids)
        parts = []
        for s in p["switches"]:
            sw_id = c_ident(s["name"])
            if name_counts[s["name"]] > 1:
                sw_id = f"{sw_id}_SW{'_'.join(str(b) for b in s['bits']) or hex(s['mask'])[2:].upper()}"
            parts.append(f"{P}_{c_ident(tag)}_{sw_id}_SELECTED")
        L.append(f"// {tag} power-on byte (all switches at MAME default = "
                 f"{default_byte:#04x})")
        L.append(f"#define {P}_{c_ident(tag)}_DEFAULT ( \\")
        for i, part in enumerate(parts):
            sep = " | \\" if i < len(parts) - 1 else "   \\"
            L.append(f"  {part}{sep}")
        L.append("  )")
        L.append(f"// == {default_byte:#04x}")
        L.append("")

    if not ports:
        L.append("// (mame -listxml reported NO <dipswitch> elements for this game.)")
        L.append("")

    L.append("#endif")
    L.append("")
    return "\n".join(L)


def mame_version(mame: str) -> str:
    if not mame:
        return ""
    try:
        r = subprocess.run([mame, "-version"], capture_output=True, text=True,
                           timeout=20)
        return r.stdout.strip().splitlines()[0] if r.stdout else ""
    except Exception:
        return ""


# ----------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", nargs="+", help="MAME short name(s)")
    ap.add_argument("--mame", help="path to mame.exe")
    ap.add_argument("--xml", help="parse this saved -listxml file instead of "
                                  "running mame ('-' = stdin)")
    ap.add_argument("--prefix", help="macro prefix (default: GAME in upper case)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--outdir", type=Path)
    g.add_argument("--src", action="store_true",
                   help="write into ../../src/machines/<game>/")
    g.add_argument("--stdout", action="store_true", help="print, do not write")
    args = ap.parse_args()

    if args.xml and len(args.game) != 1:
        sys.exit("--xml only works with a single game")

    mame = find_mame(args.mame) if not (args.xml and args.xml != "-") else ""
    if args.xml == "-":
        mame = ""
    ver = mame_version(mame) if mame and not args.xml else ""

    for game in args.game:
        xml_text = get_listxml(game, mame, args.xml)
        ports, _ = parse_dips(xml_text, game)
        prefix = (args.prefix or game).upper()
        text = emit(game, prefix, ports, ver)

        n_sw = sum(len(p["switches"]) for p in ports.values())
        summary = (f"{game}: {len(ports)} port(s), {n_sw} switch(es) -> "
                   + ", ".join(f"{P}_DEFAULT" for P in
                               (f"{prefix}_{c_ident(t)}" for t in ports)))

        if args.stdout:
            print(text)
            print(f"// {summary}", file=sys.stderr)
            continue

        if args.src:
            outdir = (HERE.parents[1] / "src" / "machines" / game).resolve()
        elif args.outdir:
            outdir = args.outdir
        else:
            outdir = HERE
        outdir.mkdir(parents=True, exist_ok=True)
        outfile = outdir / f"{game}_dipswitches.h"
        outfile.write_text(text, newline="\n")
        print(f"Wrote {outfile}")
        print(f"  {summary}")


if __name__ == "__main__":
    main()
