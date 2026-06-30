#!/usr/bin/env python3
"""Identify game_state fields (credits, player_count, current_player, current_ball)
by stimulus->response signatures, not assumed values.

Why signatures: pricing is non-linear (Metallica = 3 coins/1 credit + bonus
credits), live state lives at fixed platform offsets, and artificial exerciser
runs over-fit. So we drive a KNOWN stimulus the exerciser can produce and match
each field's behavioural fingerprint, then INTERSECT across ROMs (a real platform
field sits at the same offset on every ROM) and anchor to real dumps.

Signatures
----------
credits        : non-decreasing as coins are inserted AND drops by exactly 1 when
                 a game starts (coin counters only ever rise -> unique).
player_count   : 0 in attract, then increments by 1 with each START press (1,2,3,4).
current_ball   : 1 during ball 1, increments to 2,3.. as balls drain.
current_player : in a 2-player game, cycles 1<->2 as players alternate.

Usage
  python3 identify_game_state.py --rom mtl_180h --field credits
  python3 identify_game_state.py --rom mtl_180h --rom trn_174 --field player_count
  python3 identify_game_state.py --rom mtl_180h --field credits --players 2
"""
from __future__ import annotations
import argparse, glob, os, re, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXERCISER = HERE / "bin" / "pinmame_exerciser"
BASE = 0x02100000  # Stern SAM nvram base; override with --base for other platforms
# Trough switches mined from SAM VBS recipes (so START actually begins a game).
SAM_TROUGH_HOLDS = [18, 19, 20, 21]


def setup_root(roms_dirs):
    root = Path("/tmp/idgs-root")
    if root.exists():
        shutil.rmtree(root)
    for sub in ("roms", "nvram", "cfg"):
        (root / sub).mkdir(parents=True, exist_ok=True)
    for d in roms_dirs:
        for zf in glob.glob(os.path.join(d, "*.zip")):
            link = root / "roms" / os.path.basename(zf)
            if not link.exists():
                link.symlink_to(zf)
    return root


def run(rom, root, *, coins, starts, holds, boot_ms, out_dir):
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [str(EXERCISER), "--rom", rom, "--vpm-path", str(root), "--out-dir", str(out_dir),
           "--coins", str(coins), "--starts", str(starts),
           "--boot-ms", str(boot_ms), "--start-gap-ms", "2500", "--quiet-logs"]
    for h in holds:
        cmd += ["--hold-switch", str(h)]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if '"found":1' not in res.stdout:
        return None
    return out_dir


def ordered(out_dir, kind):
    """Return ordered (label, bytes). kind='coin' or 'start' selects the *_after family."""
    files = {}
    if kind == "coin":
        files[(0, 0)] = out_dir / "boot.nv"
        for f in glob.glob(str(out_dir / "coin*_key5_after.nv")):
            k = int(re.match(r"coin(\d+)_", os.path.basename(f)).group(1))
            files[(1, k)] = Path(f)
    snaps = [(k, open(p, "rb").read()) for k, p in sorted(files.items()) if p.exists()]
    return snaps


def snap(out_dir, name):
    p = out_dir / name
    return open(p, "rb").read() if p.exists() else None


def find_credits(out_dir):
    coins = ordered(out_dir, "coin")
    starts = sorted(glob.glob(str(out_dir / "start*_after.nv")))
    if not coins or not starts:
        return []
    last_coin = coins[-1][1]
    after_start = open(starts[-1], "rb").read()
    ln = min(len(b) for _, b in coins) ; ln = min(ln, len(after_start))
    hits = []
    for off in range(ln):
        vals = [b[off] for _, b in coins]
        if vals != sorted(vals) or vals[-1] - vals[0] < 1:
            continue
        if last_coin[off] - after_start[off] == 1:  # one credit consumed on start
            hits.append((off, vals, last_coin[off], after_start[off]))
    return hits


def find_player_count(out_dir, players):
    """Byte that is 0 at boot and rises by 1 with each start press up to `players`."""
    boot = snap(out_dir, "boot.nv")
    starts = sorted(glob.glob(str(out_dir / "start*_after.nv")),
                    key=lambda p: int(re.search(r"start(\d+)_", os.path.basename(p)).group(1)))
    if boot is None or len(starts) < players:
        return []
    seq = [boot] + [open(s, "rb").read() for s in starts]
    ln = min(len(b) for b in seq)
    hits = []
    for off in range(ln):
        vals = [b[off] for b in seq]
        # boot 0, then 1,2,..,players across the start snapshots (allow lag-tolerant final==players)
        if vals[0] == 0 and vals[-1] == players and vals == sorted(vals) and vals[-1] <= players:
            hits.append((off, vals))
    return hits


def report(field, per_rom):
    sets = []
    for rom, hits in per_rom:
        offs = {h[0] for h in hits}
        sets.append(offs)
        print(f"  [{rom}] {len(hits)} candidate(s): " +
              ", ".join(f"0x{BASE+h[0]:08X}{h[1] if field!='credits' else ''}" for h in hits[:8]))
    common = set.intersection(*sets) if sets and all(sets) else set()
    print(f"\n  CROSS-ROM common offsets for '{field}': " +
          (", ".join(f"0x{BASE+o:08X}" for o in sorted(common)) if common else "(none)"))
    return common


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", action="append", required=True, help="ROM id; repeatable")
    ap.add_argument("--field", required=True, choices=["credits", "player_count"])
    ap.add_argument("--players", type=int, default=1)
    ap.add_argument("--coins", type=int, default=12)
    ap.add_argument("--boot-ms", type=int, default=14000)
    ap.add_argument("--roms-dir", action="append",
                    default=[os.path.expanduser("~/.pinmame/roms")])
    ap.add_argument("--extra-roms", default=os.path.expanduser("~/tables"),
                    help="glob parent for */pinmame/roms")
    ap.add_argument("--holds", default=",".join(map(str, SAM_TROUGH_HOLDS)))
    args = ap.parse_args()

    roms_dirs = list(args.roms_dir) + glob.glob(os.path.join(args.extra_roms, "*", "pinmame", "roms"))
    root = setup_root(roms_dirs)
    holds = [int(x) for x in args.holds.split(",") if x.strip()]
    starts = max(args.players, 1) if args.field == "player_count" else 1

    print(f"=== identify '{args.field}' (players={args.players}) ===")
    per_rom = []
    for rom in args.rom:
        out = Path(f"/tmp/idgs-{rom}-{args.field}")
        if run(rom, root, coins=args.coins, starts=starts, holds=holds,
               boot_ms=args.boot_ms, out_dir=out) is None:
            print(f"  [{rom}] ROM not found / no snapshots"); continue
        hits = find_credits(out) if args.field == "credits" else find_player_count(out, args.players)
        per_rom.append((rom, hits))
    report(args.field, per_rom)


if __name__ == "__main__":
    main()
