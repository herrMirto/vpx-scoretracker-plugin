#!/usr/bin/env python3
"""Regenerate fresh PinMAME .nv files by booting a ROM and grabbing its NVRAM.

Some ROMs ship with bad/uninitialized .nv dumps (factory-blank, wrong size,
or just garbage), which makes high-score mapping and PINemHi comparison
impossible. This helper boots each ROM in an isolated PinMAME root, lets it run
briefly (optionally inserting a coin + start so the ROM initializes its NVRAM /
default high-score table), and saves the resulting NVRAM snapshot.

It does NOT need libpinmame to flush to disk: pinmame_exerciser already snapshots
live NVRAM via PinmameGetNVRAM, so the last snapshot it writes IS the current
(game-initialized) NVRAM. We just copy that out.

By default it writes fresh files to --out-dir and leaves your real nvram alone.
Use --apply to overwrite ~/.pinmame/nvram/<rom>.nv (a .bak copy is kept).
Use --blank to start from an EMPTY nvram so the ROM writes factory defaults
(recommended when the existing dump is garbage).

Examples:
  python3 regen_nvram.py --rom agent777 --rom bushido --blank
  python3 regen_nvram.py --rom ngndshkm --apply           # overwrite (keeps .bak)
"""

from __future__ import annotations

import argparse
import glob
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXERCISER = HERE / "bin" / "pinmame_exerciser"


def regen_one(rom, roms_dir, exerciser, out_dir, blank, boot_ms, coins, starts):
    root = Path("/tmp/regen-nvram-roots") / rom
    if root.exists():
        shutil.rmtree(root)
    for sub in ("roms", "nvram", "cfg", "memcard", "ini"):
        (root / sub).mkdir(parents=True, exist_ok=True)
    # symlink every rom zip so parents/clones resolve
    for zf in glob.glob(os.path.join(roms_dir, "*.zip")):
        link = root / "roms" / os.path.basename(zf)
        if not link.exists():
            link.symlink_to(zf)
    src_nv = Path(roms_dir).parent / "nvram" / f"{rom}.nv"
    if not blank and src_nv.exists():
        shutil.copy(src_nv, root / "nvram" / f"{rom}.nv")

    run_out = Path("/tmp/regen-nvram-out") / rom
    if run_out.exists():
        shutil.rmtree(run_out)
    run_out.mkdir(parents=True, exist_ok=True)
    cmd = [str(exerciser), "--rom", rom, "--quiet-logs",
           "--vpm-path", str(root), "--out-dir", str(run_out),
           "--boot-ms", str(boot_ms), "--coins", str(coins), "--starts", str(starts)]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return rom, "timeout", None
    if '"found":1' not in res.stdout:
        return rom, "rom-not-found", None
    snaps = sorted(glob.glob(str(run_out / "*.nv")), key=os.path.getmtime)
    if not snaps:
        return rom, "no-snapshot", None
    fresh = Path(snaps[-1]).read_bytes()
    nz = sum(1 for b in fresh if b)
    dest = Path(out_dir) / f"{rom}.nv"
    dest.write_bytes(fresh)
    return rom, f"ok ({len(fresh)}b, {nz} nonzero)", fresh


def main():
    ap = argparse.ArgumentParser(description="Regenerate fresh PinMAME .nv files")
    ap.add_argument("--rom", action="append", default=[], help="ROM id; repeatable")
    ap.add_argument("--roms", help="comma-separated ROM ids")
    ap.add_argument("--roms-dir", default=os.path.expanduser("~/.pinmame/roms"))
    ap.add_argument("--out-dir", default="/tmp/regen-nvram")
    ap.add_argument("--exerciser", default=str(EXERCISER))
    ap.add_argument("--blank", action="store_true",
                    help="start from empty nvram so the ROM writes factory defaults")
    ap.add_argument("--boot-ms", type=int, default=8000)
    ap.add_argument("--coins", type=int, default=2)
    ap.add_argument("--starts", type=int, default=1)
    ap.add_argument("--apply", action="store_true",
                    help="overwrite ~/.pinmame/nvram/<rom>.nv (keeps a .bak)")
    args = ap.parse_args()

    roms = list(args.rom)
    if args.roms:
        roms += [r.strip() for r in args.roms.split(",") if r.strip()]
    if not roms:
        ap.error("give at least one --rom or --roms")
    if not os.path.exists(args.exerciser):
        ap.error(f"exerciser not found: {args.exerciser} (build it first)")
    os.makedirs(args.out_dir, exist_ok=True)
    nvram_dir = Path(args.roms_dir).parent / "nvram"

    for rom in roms:
        rom, status, fresh = regen_one(rom, args.roms_dir, args.exerciser,
                                       args.out_dir, args.blank, args.boot_ms,
                                       args.coins, args.starts)
        applied = ""
        if fresh is not None and args.apply:
            target = nvram_dir / f"{rom}.nv"
            if target.exists():
                shutil.copy(target, target.with_suffix(".nv.bak"))
            target.write_bytes(fresh)
            applied = f" -> applied to {target}"
        print(f"{rom:12} {status}{applied}")


if __name__ == "__main__":
    main()
