#!/usr/bin/env python3
"""Find volatile-RAM game-state flags (game_over, ball-in-play, etc.) from
exerciser CPU-RAM snapshots.

Many platforms keep live game state in volatile RAM that is NOT in the .nv
(Capcom DRAM, SAM/WPC work RAM, Gottlieb System 1, ...). Static dumps can't
reveal it; the exerciser must capture CPU memory during a real game with
`--cpu-window ADDR:LEN` (or `--ram-window`). This tool analyzes those snapshots.

Method (the one that found Capcom game_over at 0x000012F2 bit 0x40):
1. Order snapshots by time; phase them as attract / play / game-over using the
   exerciser's labels (boot+coins+start_before = attract; start_after.. = play;
   trailing snapshots = game-over, since a finished game returns toward attract).
2. Churn filter: keep only bytes that are CONSTANT across every attract snapshot
   (drops animation/timer noise in volatile RAM).
3. game_over signature: of those, keep bytes that DIFFER from the attract value
   for >= --threshold of the play phase AND RETURN to the attract value in every
   game-over snapshot. These behave like a game-in-progress / game_over flag.
4. With several exercise dirs (different ROMs), INTERSECT candidates by CPU
   address to surface the platform-wide flag (game-specific mode flags drop out).

Each candidate is reported with a compressed value timeline so you can eyeball
the clean rectangular pulse (off in attract -> on for the whole game -> off at
game over) and pick the right bit/mask.

Usage:
  python3 find_game_state.py --exercise-dir /tmp/bsb-batch/bsv103 \
                             --exercise-dir /tmp/bbb-batch/bbb109
  python3 find_game_state.py --exercise-dir DIR --window cpu_0x0 --over-snaps 3
"""

from __future__ import annotations

import argparse
import glob
import os
from pathlib import Path


def load_snapshots(exercise_dir: Path, window: str | None):
    """Return (labels, base, list-of-bytes) ordered by mtime.

    `window` selects the snapshot family by suffix, e.g. 'cpu_0x0' ->
    `<label>.cpu_0x0.bin`. If None, auto-pick the first cpu_/region_ window found,
    else fall back to '.nv'.
    """
    if window is None:
        for cand in sorted(glob.glob(os.path.join(exercise_dir, "*.cpu_0x*.bin"))):
            window = os.path.basename(cand).split(".", 1)[1].rsplit(".bin", 1)[0]
            break
    if window:
        pat = os.path.join(exercise_dir, f"*.{window}.bin")
        suffix = f".{window}.bin"
        base = int(window.split("0x")[1], 16) if "0x" in window else 0
    else:
        pat = os.path.join(exercise_dir, "*.nv")
        suffix = ".nv"
        base = 0
    files = sorted(glob.glob(pat), key=lambda p: os.path.getmtime(p))
    labels = [os.path.basename(f)[: -len(suffix)] for f in files]
    data = [open(f, "rb").read() for f in files]
    return labels, base, data


def phase_indices(labels, over_snaps):
    """attract = up to first 'start*_after'; over = last `over_snaps`; play between."""
    si = next(
        (i for i, l in enumerate(labels) if l.startswith("start") and l.endswith("after")),
        1,
    )
    n = len(labels)
    over_start = max(si + 1, n - over_snaps)
    return list(range(0, si)), list(range(si, over_start)), list(range(over_start, n))


def candidates(labels, data, threshold, over_snaps):
    attract, play, over = phase_indices(labels, over_snaps)
    if not attract or not play or not over:
        return {}, (attract, play, over)
    ln = min(len(d) for d in data)
    out = {}
    for i in range(ln):
        av = {data[a][i] for a in attract}
        if len(av) != 1:
            continue  # churn (not stable in attract)
        a = next(iter(av))
        if not all(data[o][i] == a for o in over):
            continue  # didn't return to attract value at game-over
        flipped = sum(1 for p in play if data[p][i] != a)
        if flipped >= threshold * len(play):
            out[i] = a
    return out, (attract, play, over)


def timeline(data, off):
    out = []
    i = 0
    n = len(data)
    s = [data[k][off] for k in range(n)]
    while i < n:
        j = i
        while j + 1 < n and s[j + 1] == s[i]:
            j += 1
        out.append(f"{s[i]:#04x}x{j - i + 1}")
        i = j + 1
    return " ".join(out)


def main():
    ap = argparse.ArgumentParser(description="Find volatile-RAM game-state flags")
    ap.add_argument("--exercise-dir", action="append", required=True,
                    help="exerciser output dir with cpu-window/.nv snapshots; repeatable")
    ap.add_argument("--window", help="snapshot family suffix, e.g. cpu_0x0 (default: auto)")
    ap.add_argument("--threshold", type=float, default=0.7,
                    help="min fraction of play phase the byte must differ from attract")
    ap.add_argument("--over-snaps", type=int, default=3,
                    help="number of trailing snapshots treated as game-over")
    ap.add_argument("--limit", type=int, default=25)
    args = ap.parse_args()

    per_dir = []
    for d in args.exercise_dir:
        labels, base, data = load_snapshots(Path(d), args.window)
        if not data:
            print(f"[{d}] no snapshots found"); continue
        cands, (att, play, over) = candidates(labels, data, args.threshold, args.over_snaps)
        # report addresses as CPU addresses (file offset + window base)
        cands = {base + off: (off, a) for off, a in cands.items()}
        per_dir.append((d, base, data, cands))
        print(f"[{os.path.basename(d.rstrip('/'))}] {len(labels)} snaps "
              f"(attract {len(att)} / play {len(play)} / over {len(over)}) "
              f"-> {len(cands)} game_over candidates")

    if not per_dir:
        return

    if len(per_dir) == 1:
        d, base, data, cands = per_dir[0]
        print("\nCandidates (clean rectangular pulse = game_over/in-progress flag):")
        for cpu in sorted(cands)[: args.limit]:
            off, a = cands[cpu]
            print(f"  CPU 0x{cpu:08X}: attract/over={a:#04x}  {timeline(data, off)}")
        return

    common = set.intersection(*[set(c[3]) for c in per_dir])
    print(f"\nCOMMON across {len(per_dir)} runs: {len(common)} CPU address(es) "
          "(platform-wide flag candidates):")
    for cpu in sorted(common):
        print(f"\n  CPU 0x{cpu:08X}:")
        for d, base, data, cands in per_dir:
            off = cands[cpu][0]
            print(f"    {os.path.basename(d.rstrip('/')):14} {timeline(data, off)}")


if __name__ == "__main__":
    main()
