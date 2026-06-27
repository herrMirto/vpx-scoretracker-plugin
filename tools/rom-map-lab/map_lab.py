#!/usr/bin/env python3
"""Standalone ROM/NVRAM map exploration helper.

This lab tool is intentionally separate from the ScoreTracker plugin runtime.
It helps triage the remaining maps by:

* listing maps that still lack game_state;
* ranking solved sibling maps that may donate a compatible game_state layout;
* scanning NVRAM/capture files for known scores in common encodings.

The tool is conservative by design: it prints candidate reports and never edits
map JSON files.
"""

from __future__ import annotations

import argparse
import contextlib
import copy
import difflib
import io
import json
import math
import re
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


HEX_RE = re.compile(r"^0x[0-9a-fA-F]+$")


@dataclass(frozen=True)
class MapRecord:
    path: str
    full_path: Path
    data: dict[str, Any]
    platform: str
    roms: tuple[str, ...]
    has_game_state: bool


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text())


def parse_int(value: Any) -> int:
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        value = value.strip()
        if HEX_RE.match(value):
            return int(value, 16)
        return int(value)
    raise TypeError(f"cannot parse integer from {value!r}")


def fmt_addr(value: int) -> str:
    return f"0x{value:08X}" if value > 0xFFFF else f"0x{value:04X}"


def map_path_for_index_entry(entry: Any) -> str | None:
    if isinstance(entry, str):
        return entry
    if isinstance(entry, dict):
        for key in ("map", "path", "file"):
            value = entry.get(key)
            if isinstance(value, str):
                return value
    return None


def load_records(maps_root: Path) -> tuple[dict[str, Any], list[MapRecord]]:
    index = load_json(maps_root / "index.json")
    paths = sorted({p for p in (map_path_for_index_entry(v) for v in index.values()) if p})
    records: list[MapRecord] = []
    for rel in paths:
        full = maps_root / rel
        if not full.exists():
            continue
        data = load_json(full)
        meta = data.get("_metadata", {})
        records.append(
            MapRecord(
                path=rel,
                full_path=full,
                data=data,
                platform=str(meta.get("platform", "")),
                roms=tuple(str(r) for r in meta.get("roms", [])),
                has_game_state=bool(data.get("game_state")),
            )
        )
    return index, records


def resolve_rom(index: dict[str, Any], records: list[MapRecord], rom: str) -> MapRecord:
    entry = index.get(rom)
    if entry is None:
        raise SystemExit(f"ROM {rom!r} is not present in index.json")
    rel = map_path_for_index_entry(entry)
    for record in records:
        if record.path == rel:
            return record
    raise SystemExit(f"ROM {rom!r} points to missing map {rel!r}")


def platform_nvram_base(maps_root: Path, platform: str) -> int:
    platform_path = maps_root / "platforms" / f"{platform}.json"
    if not platform_path.exists():
        return 0
    data = load_json(platform_path)
    for region in data.get("memory_layout", []):
        if region.get("type") == "nvram":
            return parse_int(region.get("address", 0))
    return 0


def normalized_addr(addr: Any, base: int) -> int:
    value = parse_int(addr)
    return value - base if base and value >= base else value


def high_score_score_offsets(record: MapRecord, base: int) -> list[int]:
    offsets: list[int] = []
    for item in record.data.get("high_scores", []):
        score = item.get("score") if isinstance(item, dict) else None
        if isinstance(score, dict) and "start" in score:
            offsets.append(normalized_addr(score["start"], base))
    return offsets


def game_score_offsets(record: MapRecord, base: int) -> list[int]:
    game_state = record.data.get("game_state") or {}
    scores = game_state.get("scores") or []
    offsets: list[int] = []
    for item in scores:
        if isinstance(item, dict) and "start" in item:
            offsets.append(normalized_addr(item["start"], base))
    return offsets


def game_state_summary(record: MapRecord, base: int) -> dict[str, Any]:
    game_state = record.data.get("game_state") or {}
    return {
        "scores": [fmt_addr(o + base if base else o) for o in game_score_offsets(record, base)],
        "player_count": game_state.get("player_count", {}).get("start")
        if isinstance(game_state.get("player_count"), dict)
        else None,
        "credits": game_state.get("credits", {}).get("start")
        if isinstance(game_state.get("credits"), dict)
        else None,
        "game_over": game_state.get("game_over", {}).get("start")
        if isinstance(game_state.get("game_over"), dict)
        else None,
        "game_over_invert": game_state.get("game_over", {}).get("invert")
        if isinstance(game_state.get("game_over"), dict)
        else None,
    }


def decode_bcd(raw: bytes) -> int:
    digits: list[str] = []
    for byte in raw:
        hi = (byte >> 4) & 0x0F
        lo = byte & 0x0F
        if hi <= 9:
            digits.append(str(hi))
        if lo <= 9:
            digits.append(str(lo))
    text = "".join(digits).lstrip("0")
    return int(text) if text else 0


def decode_field(data: bytes, spec: dict[str, Any], base: int) -> Any:
    start = normalized_addr(spec["start"], base)
    length = int(spec.get("length", 1))
    encoding = str(spec.get("encoding", "")).lower()
    if start < 0 or start >= len(data):
        return None
    raw = data[start : min(start + length, len(data))]
    if not raw:
        return None
    if encoding == "int":
        if length == 1:
            return raw[0]
        if length == 2:
            return int.from_bytes(raw[:2], "little")
        if length == 4:
            return int.from_bytes(raw[:4], "little")
        return int.from_bytes(raw, "little")
    if encoding == "bcd":
        return decode_bcd(raw)
    if encoding == "bool":
        value = raw[0] != 0
        if bool(spec.get("invert", False)):
            value = not value
        return value
    if encoding == "ch":
        return raw.decode("ascii", errors="replace")
    return raw.hex()


def decode_game_state_snapshot(data: bytes, record: MapRecord, base: int) -> dict[str, Any]:
    game_state = record.data.get("game_state") or {}
    decoded: dict[str, Any] = {}
    scores = game_state.get("scores") or []
    if scores:
        decoded["scores"] = [
            decode_field(data, item, base)
            for item in scores
            if isinstance(item, dict) and "start" in item
        ]
    for key in (
        "highest_scores",
        "player_count",
        "current_player",
        "current_ball",
        "ball_count",
        "credits",
        "max_credits",
        "free_play",
        "game_over",
    ):
        spec = game_state.get(key)
        if isinstance(spec, dict) and "start" in spec:
            decoded[key] = decode_field(data, spec, base)
    return decoded


def common_prefix_len(a: str, b: str) -> int:
    total = 0
    for ca, cb in zip(a, b):
        if ca != cb:
            break
        total += 1
    return total


def score_record(target: MapRecord, candidate: MapRecord, maps_root: Path) -> float:
    base = platform_nvram_base(maps_root, target.platform)
    target_hs = high_score_score_offsets(target, base)
    candidate_hs = high_score_score_offsets(candidate, base)
    name_score = max(
        difflib.SequenceMatcher(None, tr, cr).ratio()
        for tr in target.roms or (Path(target.path).stem,)
        for cr in candidate.roms or (Path(candidate.path).stem,)
    )
    prefix_score = max(
        common_prefix_len(tr, cr) / max(len(tr), len(cr), 1)
        for tr in target.roms or (Path(target.path).stem,)
        for cr in candidate.roms or (Path(candidate.path).stem,)
    )
    path_score = difflib.SequenceMatcher(None, target.path, candidate.path).ratio()
    hs_score = 0.0
    if target_hs and candidate_hs:
        diffs = [abs(a - b) for a, b in zip(target_hs, candidate_hs)]
        avg = sum(diffs) / max(len(diffs), 1)
        hs_score = 1.0 / (1.0 + avg / 128.0)
    layout_score = 0.0
    game_offsets = game_score_offsets(candidate, base)
    if len(game_offsets) >= 4:
        deltas = [b - a for a, b in zip(game_offsets, game_offsets[1:])]
        if len(set(deltas)) == 1:
            layout_score = 1.0
        elif max(deltas, default=0) <= 16:
            layout_score = 0.75
    return (
        name_score * 0.35
        + prefix_score * 0.20
        + path_score * 0.15
        + hs_score * 0.20
        + layout_score * 0.10
    )


def cmd_missing(args: argparse.Namespace) -> None:
    maps_root = Path(args.maps_root)
    index, records = load_records(maps_root)
    missing = [r for r in records if not r.has_game_state]
    roms_missing = [
        rom
        for rom, entry in index.items()
        if (rel := map_path_for_index_entry(entry)) in {r.path for r in missing}
    ]
    by_platform: dict[str, int] = {}
    for record in missing:
        by_platform[record.platform] = by_platform.get(record.platform, 0) + 1

    print(f"Indexed ROM IDs: {len(index)}")
    print(f"Map files: {len(records)}")
    print(f"Maps with game_state: {len(records) - len(missing)}")
    print(f"Maps missing game_state: {len(missing)}")
    print(f"ROM IDs missing game_state: {len(roms_missing)}")
    print()
    print("Missing map files by platform:")
    for platform, count in sorted(by_platform.items(), key=lambda item: (-item[1], item[0])):
        print(f"  {platform or '(unknown)'}: {count}")

    if args.show:
        print()
        print("Missing map files:")
        for record in missing:
            roms = ", ".join(record.roms)
            print(f"  {record.path} [{record.platform}] {roms}")


def cmd_suggest(args: argparse.Namespace) -> None:
    maps_root = Path(args.maps_root)
    index, records = load_records(maps_root)
    target = resolve_rom(index, records, args.rom)
    base = platform_nvram_base(maps_root, target.platform)
    solved = [
        r
        for r in records
        if r.platform == target.platform and r.has_game_state and r.path != target.path
    ]
    ranked = sorted(
        ((score_record(target, candidate, maps_root), candidate) for candidate in solved),
        key=lambda item: item[0],
        reverse=True,
    )[: args.limit]

    print(f"ROM: {args.rom}")
    print(f"Target map: {target.path}")
    print(f"Platform: {target.platform}")
    print(f"Target has game_state: {target.has_game_state}")
    print(f"NVRAM base: {fmt_addr(base) if base else '0x0000'}")
    print()
    print("Best solved sibling candidates:")
    for rank, (score, candidate) in enumerate(ranked, start=1):
        summary = game_state_summary(candidate, base)
        print(f"{rank}. confidence-ish={score:.3f} {candidate.path}")
        print(f"   roms: {', '.join(candidate.roms)}")
        print(f"   scores: {', '.join(summary['scores'])}")
        if summary["game_over"]:
            inv = f" invert={summary['game_over_invert']}" if summary["game_over_invert"] is not None else ""
            print(f"   game_over: {summary['game_over']}{inv}")
        if summary["player_count"]:
            print(f"   player_count: {summary['player_count']}")
        if summary["credits"]:
            print(f"   credits: {summary['credits']}")
        print()


def ranked_suggestions(
    target: MapRecord, records: list[MapRecord], maps_root: Path, limit: int
) -> list[tuple[float, MapRecord]]:
    solved = [
        r
        for r in records
        if r.platform == target.platform and r.has_game_state and r.path != target.path
    ]
    return sorted(
        ((score_record(target, candidate, maps_root), candidate) for candidate in solved),
        key=lambda item: item[0],
        reverse=True,
    )[:limit]


def cmd_suggest_missing(args: argparse.Namespace) -> None:
    maps_root = Path(args.maps_root)
    _, records = load_records(maps_root)
    missing = [r for r in records if not r.has_game_state]
    for target in sorted(missing, key=lambda r: (r.platform, r.path)):
        ranked = ranked_suggestions(target, records, maps_root, args.limit)
        roms = ", ".join(target.roms)
        print(f"{target.path} [{target.platform}] {roms}")
        if not ranked:
            print("  no solved sibling maps on this platform")
            print()
            continue
        for score, candidate in ranked:
            base = platform_nvram_base(maps_root, target.platform)
            summary = game_state_summary(candidate, base)
            score_block = ", ".join(summary["scores"])
            print(f"  {score:.3f} <- {candidate.path} ({', '.join(candidate.roms)})")
            print(f"        scores: {score_block}")
            if summary["game_over"]:
                inv = (
                    f" invert={summary['game_over_invert']}"
                    if summary["game_over_invert"] is not None
                    else ""
                )
                print(f"        game_over: {summary['game_over']}{inv}")
        print()


def cmd_exercise_plan(args: argparse.Namespace) -> None:
    maps_root = Path(args.maps_root)
    index, records = load_records(maps_root)
    target = resolve_rom(index, records, args.rom)
    base = platform_nvram_base(maps_root, target.platform)
    suggestions = ranked_suggestions(target, records, maps_root, 5)
    plan = {
        "rom": args.rom,
        "target_map": target.path,
        "platform": target.platform,
        "nvram_base": fmt_addr(base) if base else "0x0000",
        "purpose": "Future headless libpinmame exercise plan; not executed by this script.",
        "sequence": [
            {"step": "boot", "wait_ms": 5000, "snapshot": "boot_attract"},
            {"step": "insert_coin", "switch_candidates": "platform/driver coin switches", "snapshot": "coin_inserted"},
            {"step": "press_start", "players": 1, "snapshot": "game_started_1p"},
            {"step": "fuzz_scoring_switches", "max_switches": 128, "pulse_ms": 80, "snapshot_each_score_change": True},
            {"step": "drain_or_outhole_until_game_over", "max_attempts": 20, "snapshot": "game_over_or_attract"},
            {"step": "repeat", "players": [2, 3, 4], "if_start_sequence_is_known": True},
        ],
        "expected_from_sibling_maps": [
            {
                "confidence": round(score, 3),
                "map": candidate.path,
                "roms": list(candidate.roms),
                "game_state": game_state_summary(candidate, base),
            }
            for score, candidate in suggestions
        ],
        "output_contract": {
            "snapshots": ["*.nv", "ram-*.bin"],
            "detected_scores": "list of score/address/encoding hits",
            "candidate_game_state": "JSON block, review-only",
        },
    }
    print(json.dumps(plan, indent=2))


def bcd_variants(score: int) -> list[bytes]:
    digits = str(score)
    variants: list[bytes] = []
    for pad_even in (False, True):
        d = digits
        if pad_even and len(d) % 2:
            d = "0" + d
        if len(d) % 2:
            continue
        raw = bytes((int(d[i]) << 4) | int(d[i + 1]) for i in range(0, len(d), 2))
        variants.append(raw)
        variants.append(raw[::-1])
    # score displays often drop trailing zeroes in BCD-like storage.
    if score % 10 == 0:
        variants.extend(bcd_variants(score // 10))
    unique: list[bytes] = []
    seen: set[bytes] = set()
    for item in variants:
        if item and item not in seen:
            seen.add(item)
            unique.append(item)
    return unique


def score_encodings(score: int) -> list[tuple[str, bytes]]:
    encodings: list[tuple[str, bytes]] = []
    for fmt, label in [
        ("<I", "uint32le"),
        (">I", "uint32be"),
        ("<i", "int32le"),
        (">i", "int32be"),
        ("<H", "uint16le"),
        (">H", "uint16be"),
    ]:
        try:
            encodings.append((label, struct.pack(fmt, score)))
        except struct.error:
            pass
    for raw in bcd_variants(score):
        encodings.append((f"bcd:{raw.hex()}", raw))
    return encodings


def find_all(data: bytes, needle: bytes) -> list[int]:
    positions: list[int] = []
    start = 0
    while True:
        pos = data.find(needle, start)
        if pos < 0:
            return positions
        positions.append(pos)
        start = pos + 1


def cmd_scan_scores(args: argparse.Namespace) -> None:
    maps_root = Path(args.maps_root)
    index, records = load_records(maps_root)
    target = resolve_rom(index, records, args.rom)
    base = platform_nvram_base(maps_root, target.platform)
    scores = [int(s.replace("_", "").replace(",", "")) for s in args.scores.split(",") if s.strip()]
    files = [Path(p) for p in args.nvram]
    if args.captures:
        files.extend(sorted(Path(args.captures).glob("*.nv")))
    files = [p for p in files if p.exists()]
    if not files:
        raise SystemExit("No NVRAM/capture files found")

    print(f"ROM: {args.rom}")
    print(f"Target map: {target.path}")
    print(f"Platform: {target.platform}")
    print(f"NVRAM base: {fmt_addr(base) if base else '0x0000'}")
    print(f"Scores: {', '.join(str(s) for s in scores)}")
    print()

    aggregate: dict[tuple[int, str], list[int]] = {}
    for file in files:
        data = file.read_bytes()
        print(f"File: {file}")
        for score in scores:
            hits: list[tuple[str, int]] = []
            for label, raw in score_encodings(score):
                for offset in find_all(data, raw):
                    hits.append((label, offset))
                    aggregate.setdefault((score, label), []).append(offset)
            print(f"  {score}:")
            if hits:
                for label, offset in sorted(hits, key=lambda item: (item[1], item[0]))[: args.max_hits]:
                    print(f"    {label:12s} offset={fmt_addr(offset)} addr={fmt_addr(offset + base)}")
                if len(hits) > args.max_hits:
                    print(f"    ... {len(hits) - args.max_hits} more")
            else:
                print("    no exact hit")
        print()

    # Candidate contiguous int32 score blocks are the most common modern case.
    by_score_uint32le: list[list[int]] = []
    for score in scores:
        positions = sorted(set(aggregate.get((score, "uint32le"), [])))
        by_score_uint32le.append(positions)
    if len(by_score_uint32le) >= 2:
        print("Contiguous uint32le block candidates:")
        found = False
        for first in by_score_uint32le[0]:
            for stride in (4, 5, 6, 8):
                if all((first + i * stride) in by_score_uint32le[i] for i in range(len(by_score_uint32le))):
                    print(
                        f"  stride={stride} offset={fmt_addr(first)} "
                        f"addr={fmt_addr(first + base)}..{fmt_addr(first + (len(scores)-1)*stride + base)}"
                    )
                    found = True
        if not found:
            print("  none")


def read_bytes(path: Path) -> bytes:
    return path.read_bytes()


def changed_offsets(before: bytes, after: bytes) -> list[tuple[int, int, int]]:
    n = min(len(before), len(after))
    out: list[tuple[int, int, int]] = []
    for i in range(n):
        if before[i] != after[i]:
            out.append((i, before[i], after[i]))
    if len(before) != len(after):
        longer = after if len(after) > len(before) else before
        for i in range(n, len(longer)):
            out.append((i, before[i] if i < len(before) else -1, after[i] if i < len(after) else -1))
    return out


def score_like_u32_changes(before: bytes, after: bytes, diffs: list[tuple[int, int, int]]) -> list[tuple[int, int, int]]:
    """Return int32 little-endian windows near changed bytes that increased."""
    candidates: list[tuple[int, int, int]] = []
    seen: set[int] = set()
    for off, _, _ in diffs:
        for start in range(max(0, off - 3), off + 1):
            if start in seen or start + 4 > len(before) or start + 4 > len(after):
                continue
            seen.add(start)
            b = struct.unpack_from("<I", before, start)[0]
            a = struct.unpack_from("<I", after, start)[0]
            if 0 <= b < a <= 10_000_000_000 and (a - b) <= 10_000_000:
                candidates.append((start, b, a))
    return sorted(candidates, key=lambda item: (item[0], item[2] - item[1]))


def cmd_analyze_exercise(args: argparse.Namespace) -> None:
    exercise_dir = Path(args.exercise_dir)
    if not exercise_dir.exists():
        raise SystemExit(f"Exercise directory does not exist: {exercise_dir}")

    pairs: list[tuple[int, Path, Path]] = []
    for before in sorted(exercise_dir.glob("*_sw*_before.nv")):
        after = before.with_name(before.name.replace("_before.nv", "_after.nv"))
        if not after.exists():
            continue
        m = re.search(r"_sw(\d+)_before\.nv$", before.name)
        sw = int(m.group(1)) if m else -1
        pairs.append((sw, before, after))

    if not pairs:
        raise SystemExit(f"No *_swN_before.nv / *_swN_after.nv pairs found in {exercise_dir}")

    rows = []
    for sw, before_path, after_path in pairs:
        before = read_bytes(before_path)
        after = read_bytes(after_path)
        diffs = changed_offsets(before, after)
        score_like = score_like_u32_changes(before, after, diffs)
        rows.append((sw, len(diffs), diffs[: args.max_diffs], score_like[: args.max_score_candidates]))

    rows.sort(key=lambda row: (-(len(row[3]) > 0), -row[1], row[0]))
    print(f"Exercise dir: {exercise_dir}")
    print(f"Switch pulses analyzed: {len(rows)}")
    print()
    for sw, count, first, score_like in rows[: args.limit]:
        print(f"switch {sw}: changed_bytes={count}")
        if first:
            rendered = ", ".join(f"0x{off:04X}:{old}->{new}" for off, old, new in first)
            print(f"  first diffs: {rendered}")
        if score_like:
            rendered = ", ".join(f"0x{off:04X}:{old}->{new}" for off, old, new in score_like)
            print(f"  uint32le increases: {rendered}")
        print()


def cmd_inspect_exercise(args: argparse.Namespace) -> None:
    maps_root = Path(args.maps_root)
    index, records = load_records(maps_root)
    target = resolve_rom(index, records, args.rom)
    base = platform_nvram_base(maps_root, target.platform)
    exercise_dir = Path(args.exercise_dir)
    if not exercise_dir.exists():
        raise SystemExit(f"Exercise directory does not exist: {exercise_dir}")

    files = sorted(exercise_dir.glob("*.nv"), key=lambda path: (path.stat().st_mtime_ns, path.name))
    if not files:
        raise SystemExit(f"No *.nv files found in {exercise_dir}")

    print(f"ROM: {args.rom}")
    print(f"Map: {target.path}")
    print(f"Platform: {target.platform}")
    print(f"NVRAM base: {fmt_addr(base) if base else '0x0000'}")
    print()
    previous: dict[str, Any] | None = None
    for file in files:
        decoded = decode_game_state_snapshot(file.read_bytes(), target, base)
        if args.changed and previous is not None and decoded == previous:
            continue
        print(f"{file.name}: {json.dumps(decoded, sort_keys=True)}")
        previous = decoded


def cmd_vbs_switches(args: argparse.Namespace) -> None:
    script = Path(args.script)
    if not script.exists():
        raise SystemExit(f"Script does not exist: {script}")
    text = script.read_text(errors="replace").splitlines()
    switch_re = re.compile(r"Controller\.Switch\((\d+)\)\s*=\s*([01])", re.IGNORECASE)
    pulse_re = re.compile(r"(?:vpmTimer\.)?PulseSw\s*\(?\s*(\d+)", re.IGNORECASE)
    sub_hit_re = re.compile(r"^\s*Sub\s+([A-Za-z_]*sw(\d+)[A-Za-z0-9_]*)_?Hit\b", re.IGNORECASE)

    direct: dict[int, dict[str, Any]] = {}
    pulses: dict[int, list[int]] = {}
    hit_subs: dict[int, list[tuple[int, str]]] = {}
    for lineno, line in enumerate(text, start=1):
        stripped = line.strip()
        for m in switch_re.finditer(line):
            sw = int(m.group(1))
            value = int(m.group(2))
            row = direct.setdefault(sw, {"set_0": [], "set_1": [], "contexts": []})
            row[f"set_{value}"].append(lineno)
            lower = stripped.lower()
            if any(word in lower for word in ("trough", "drain", "outhole", "plunger", "shooter")):
                row["contexts"].append((lineno, stripped[:160]))
        for m in pulse_re.finditer(line):
            pulses.setdefault(int(m.group(1)), []).append(lineno)
        m = sub_hit_re.search(line)
        if m:
            hit_subs.setdefault(int(m.group(2)), []).append((lineno, m.group(1)))

    likely_holds: list[tuple[int, str]] = []
    for sw, row in sorted(direct.items()):
        if row["set_1"] and not row["set_0"]:
            likely_holds.append((sw, "set to 1 without a matching set to 0"))
        elif row["contexts"]:
            likely_holds.append((sw, "physical-ball context: " + "; ".join(c for _, c in row["contexts"][:2])))

    print(f"Script: {script}")
    print(f"Direct Controller.Switch assignments: {len(direct)} switches")
    print(f"PulseSw calls: {len(pulses)} switches")
    print(f"Hit subs with switch-looking names: {len(hit_subs)} switches")
    print()
    if likely_holds:
        print("Likely hold/context switches:")
        for sw, reason in likely_holds[: args.limit]:
            print(f"  {sw}: {reason}")
        print()
    print("Likely pulse/scoring switches:")
    candidates = sorted(set(pulses) | {sw for sw, row in direct.items() if row["set_1"] and row["set_0"]} | set(hit_subs))
    for sw in candidates[: args.limit]:
        tags: list[str] = []
        if sw in pulses:
            tags.append(f"PulseSw lines {','.join(str(n) for n in pulses[sw][:4])}")
        if sw in direct:
            row = direct[sw]
            if row["set_1"]:
                tags.append(f"set1 lines {','.join(str(n) for n in row['set_1'][:4])}")
            if row["set_0"]:
                tags.append(f"set0 lines {','.join(str(n) for n in row['set_0'][:4])}")
        if sw in hit_subs:
            tags.append(f"Hit subs {','.join(name for _, name in hit_subs[sw][:4])}")
        print(f"  {sw}: {'; '.join(tags)}")


def cmd_candidate_map(args: argparse.Namespace) -> None:
    maps_root = Path(args.maps_root)
    index, records = load_records(maps_root)
    target = resolve_rom(index, records, args.rom)
    donor = resolve_rom(index, records, args.donor_rom)
    if not donor.has_game_state:
        raise SystemExit(f"Donor ROM {args.donor_rom!r} map has no game_state")

    candidate = copy.deepcopy(target.data)
    notes = candidate.setdefault("_notes", [])
    if not isinstance(notes, list):
        notes = [str(notes)]
        candidate["_notes"] = notes
    notes.append(
        "AUTO-CANDIDATE: game_state copied from "
        f"{donor.path} ({', '.join(donor.roms)}); review and verify before committing."
    )
    candidate["game_state"] = copy.deepcopy(donor.data["game_state"])
    candidate["_candidate"] = {
        "source": "tools/rom-map-lab/map_lab.py candidate-map",
        "target_rom": args.rom,
        "target_map": target.path,
        "donor_rom": args.donor_rom,
        "donor_map": donor.path,
        "status": "unverified",
    }

    out_root = Path(args.out_root)
    out_path = out_root / target.path
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(candidate, indent=2) + "\n")

    base = platform_nvram_base(maps_root, target.platform)
    print(f"Wrote candidate: {out_path}")
    print(f"Target: {target.path} ({', '.join(target.roms)})")
    print(f"Donor:  {donor.path} ({', '.join(donor.roms)})")
    print(f"Platform: {target.platform}")
    print(f"NVRAM base: {fmt_addr(base) if base else '0x0000'}")
    print("Candidate game_state:")
    print(json.dumps(game_state_summary(MapRecord(target.path, out_path, candidate, target.platform, target.roms, True), base), indent=2))


def cmd_local_inventory(args: argparse.Namespace) -> None:
    maps_root = Path(args.maps_root)
    tables_root = Path(args.tables_root)
    roms_dir = Path(args.roms_dir)
    index, records = load_records(maps_root)
    by_path = {record.path: record for record in records}
    rows: dict[str, dict[str, Any]] = {}

    for nvram in tables_root.glob("*/pinmame/nvram/*.nv"):
        rom = nvram.stem
        entry = index.get(rom)
        if entry is None:
            continue
        map_path = map_path_for_index_entry(entry)
        record = by_path.get(map_path)
        if record is None or record.has_game_state:
            continue
        table_dir = nvram.parent.parent.parent
        pinmame_dir = nvram.parent.parent
        scripts = sorted(table_dir.glob("*.vbs"))
        local_rom = pinmame_dir / "roms" / f"{rom}.zip"
        global_rom = roms_dir / f"{rom}.zip"
        row = {
            "rom": rom,
            "map": map_path,
            "platform": record.platform,
            "table": str(table_dir),
            "nvram": str(nvram),
            "script": str(scripts[0]) if scripts else None,
            "script_count": len(scripts),
            "rom_zip": str(local_rom if local_rom.exists() else global_rom)
            if local_rom.exists() or global_rom.exists()
            else None,
        }
        row["status"] = (
            "ready" if row["script"] and row["rom_zip"] else
            "needs_vbs" if not row["script"] else
            "needs_rom"
        )
        rows[rom] = row

    ordered = sorted(rows.values(), key=lambda row: (row["status"] != "ready", row["platform"], row["rom"]))
    if args.json:
        print(json.dumps(ordered, indent=2))
        return
    counts: dict[str, int] = {}
    for row in ordered:
        counts[row["status"]] = counts.get(row["status"], 0) + 1
    print(f"Local unresolved ROMs with NVRAM: {len(ordered)}")
    print(" ".join(f"{key}={value}" for key, value in sorted(counts.items())))
    print()
    for row in ordered:
        print(f"{row['status']:10s} {row['rom']:16s} [{row['platform']}] {row['map']}")
        print(f"  table:  {row['table']}")
        if row["script"]:
            print(f"  script: {row['script']}")


def cmd_prioritize_missing(args: argparse.Namespace) -> None:
    maps_root = Path(args.maps_root)
    tables_root = Path(args.tables_root)
    roms_dir = Path(args.roms_dir)
    index, records = load_records(maps_root)
    romnames_path = maps_root / "romnames.json"
    romnames = load_json(romnames_path) if romnames_path.exists() else {}
    evidence_path = Path(args.evidence_file)
    evidence_data = load_json(evidence_path) if evidence_path.exists() else {}
    rom_evidence = evidence_data.get("roms", {}) if isinstance(evidence_data, dict) else {}

    local_by_map: dict[str, list[tuple[str, str]]] = {}
    for nvram in tables_root.glob("*/pinmame/nvram/*.nv"):
        rom = nvram.stem
        entry = index.get(rom)
        if entry is None:
            continue
        map_path = map_path_for_index_entry(entry)
        table_dir = nvram.parent.parent.parent
        scripts = list(table_dir.glob("*.vbs"))
        has_rom = (
            (roms_dir / f"{rom}.zip").exists()
            or (nvram.parent.parent / "roms" / f"{rom}.zip").exists()
        )
        status = "ready" if scripts and has_rom else "needs_vbs" if not scripts else "needs_rom"
        local_by_map.setdefault(map_path, []).append((rom, status))

    tier_names = {
        "A": "near-direct donor",
        "B": "strong donor",
        "C": "layout-assisted",
        "D": "weak donor",
        "E": "from scratch",
    }
    rows = []
    excluded = []
    for target in (record for record in records if not record.has_game_state):
        indexed_roms = sorted(
            rom
            for rom, entry in index.items()
            if map_path_for_index_entry(entry) == target.path
        )
        if not indexed_roms:
            indexed_roms = list(target.roms)
        descriptions = [str(romnames.get(rom, "")) for rom in indexed_roms]
        experimental = bool(descriptions) and all(
            "prototype" in description.lower() or "freewpc" in description.lower()
            for description in descriptions
        )
        if experimental and not args.include_experimental:
            excluded.append(target.path)
            continue

        ranked = ranked_suggestions(target, records, maps_root, 3)
        confidence = ranked[0][0] if ranked else 0.0
        donor = ranked[0][1] if ranked else None
        base = platform_nvram_base(maps_root, target.platform)
        layouts = [tuple(game_score_offsets(record, base)) for _, record in ranked]
        consensus = bool(
            len(layouts) >= 2
            and layouts[0]
            and all(layout == layouts[0] for layout in layouts[1:])
        )
        effective = min(0.99, confidence + (0.08 if consensus else 0.0))
        if effective >= 0.80:
            tier = "A"
            probability = 0.90 + min(0.08, (effective - 0.80) * 0.4)
        elif effective >= 0.65:
            tier = "B"
            probability = 0.72 + (effective - 0.65) * 0.8
        elif effective >= 0.45:
            tier = "C"
            probability = 0.48 + (effective - 0.45) * 0.9
        elif donor:
            tier = "D"
            probability = 0.30 + effective * 0.3
        else:
            tier = "E"
            probability = 0.12

        evidence_rows = [rom_evidence[rom] for rom in indexed_roms if rom in rom_evidence]
        tier_penalty = max(
            (int(item.get("tier_penalty", 0)) for item in evidence_rows if isinstance(item, dict)),
            default=0,
        )
        if tier_penalty:
            tier_order = ["A", "B", "C", "D", "E"]
            tier = tier_order[min(tier_order.index(tier) + tier_penalty, len(tier_order) - 1)]
            probability *= 0.6 ** tier_penalty
        evidence_notes = [
            str(item.get("note"))
            for item in evidence_rows
            if isinstance(item, dict) and item.get("note")
        ]

        local = local_by_map.get(target.path, [])
        local_status = (
            "ready" if any(status == "ready" for _, status in local)
            else "needs_vbs" if any(status == "needs_vbs" for _, status in local)
            else "needs_rom" if local else "not_local"
        )
        play_rom = next(
            (
                rom
                for wanted in ("ready", "needs_rom", "needs_vbs")
                for rom, status in local
                if status == wanted
            ),
            indexed_roms[0],
        )
        impact = len(indexed_roms)
        expected = impact * probability
        rows.append(
            {
                "tier": tier,
                "tier_name": tier_names[tier],
                "map": target.path,
                "platform": target.platform,
                "play_rom": play_rom,
                "roms": indexed_roms,
                "rom_count": impact,
                "donor_confidence": round(confidence, 3),
                "layout_consensus": consensus,
                "donor_map": donor.path if donor else None,
                "estimated_success": round(probability, 2),
                "expected_roms": round(expected, 2),
                "local_status": local_status,
                "local_matches": [{"rom": rom, "status": status} for rom, status in local],
                "evidence_notes": evidence_notes,
            }
        )

    local_order = {"ready": 0, "needs_vbs": 1, "needs_rom": 2, "not_local": 3}
    rows.sort(
        key=lambda row: (
            row["tier"],
            -row["expected_roms"],
            -row["rom_count"],
            -row["donor_confidence"],
            local_order[row["local_status"]],
            row["map"],
        )
    )
    for rank, row in enumerate(rows, start=1):
        row["rank"] = rank

    if args.json:
        print(json.dumps({"queue": rows, "excluded_experimental": excluded}, indent=2))
        return

    print("# Missing game_state priority queue")
    print()
    print(
        "Sorted by donor-confidence tier first, then expected ROM IDs resolved. "
        "Prototype/FreeWPC-only maps are excluded by default."
    )
    print()
    print(f"Active maps: {len(rows)}; excluded experimental maps: {len(excluded)}")
    print()
    print("| # | Tier | Play ROM | ROMs | Donor | Confidence | Consensus | Local | Evidence | Map |")
    print("|---:|:---:|---|---:|---|---:|:---:|---|---|---|")
    for row in rows:
        donor_name = Path(row["donor_map"]).stem if row["donor_map"] else "—"
        print(
            f"| {row['rank']} | {row['tier']} | `{row['play_rom']}` | {row['rom_count']} | "
            f"{donor_name} | {row['donor_confidence']:.3f} | "
            f"{'yes' if row['layout_consensus'] else 'no'} | {row['local_status']} | "
            f"{'prior failure' if row['evidence_notes'] else '—'} | "
            f"`{row['map']}` |"
        )
    print()
    print("Tier legend: A near-direct donor; B strong donor; C layout-assisted; D weak donor; E from scratch.")
    if excluded:
        print()
        print("Excluded prototype/FreeWPC-only maps:")
        for path in sorted(excluded):
            print(f"- `{path}`")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument(
        "--maps-root",
        default="/Users/andremichi/workspace/scoretracker-maps",
        help="Path to scoretracker-maps/pinmame-nvram-maps style repository",
    )

    p_missing = sub.add_parser("missing", parents=[common], help="List maps missing game_state")
    p_missing.add_argument("--show", action="store_true", help="Print every missing map")
    p_missing.set_defaults(func=cmd_missing)

    p_suggest = sub.add_parser("suggest", parents=[common], help="Suggest solved sibling layouts")
    p_suggest.add_argument("--rom", required=True)
    p_suggest.add_argument("--limit", type=int, default=8)
    p_suggest.set_defaults(func=cmd_suggest)

    p_suggest_missing = sub.add_parser(
        "suggest-missing", parents=[common], help="Suggest solved sibling layouts for every missing map"
    )
    p_suggest_missing.add_argument("--limit", type=int, default=3)
    p_suggest_missing.set_defaults(func=cmd_suggest_missing)

    p_scan = sub.add_parser("scan-scores", parents=[common], help="Find known score values in NVRAM")
    p_scan.add_argument("--rom", required=True)
    p_scan.add_argument("--scores", required=True, help="Comma-separated scores")
    p_scan.add_argument("--nvram", action="append", default=[], help="NVRAM file; can be repeated")
    p_scan.add_argument("--captures", help="Capture directory containing *.nv files")
    p_scan.add_argument("--max-hits", type=int, default=20)
    p_scan.set_defaults(func=cmd_scan_scores)

    p_exercise = sub.add_parser(
        "exercise-plan", parents=[common], help="Emit a future libpinmame exercise plan for a ROM"
    )
    p_exercise.add_argument("--rom", required=True)
    p_exercise.set_defaults(func=cmd_exercise_plan)

    p_analyze_exercise = sub.add_parser(
        "analyze-exercise", help="Analyze NVRAM before/after pairs emitted by pinmame_exerciser"
    )
    p_analyze_exercise.add_argument("--exercise-dir", required=True)
    p_analyze_exercise.add_argument("--limit", type=int, default=40)
    p_analyze_exercise.add_argument("--max-diffs", type=int, default=16)
    p_analyze_exercise.add_argument("--max-score-candidates", type=int, default=8)
    p_analyze_exercise.set_defaults(func=cmd_analyze_exercise)

    p_inspect_exercise = sub.add_parser(
        "inspect-exercise", parents=[common], help="Decode mapped game_state fields for exercise snapshots"
    )
    p_inspect_exercise.add_argument("--rom", required=True)
    p_inspect_exercise.add_argument("--exercise-dir", required=True)
    p_inspect_exercise.add_argument("--changed", action="store_true", help="Only print snapshots whose decoded state changed")
    p_inspect_exercise.set_defaults(func=cmd_inspect_exercise)

    p_vbs_switches = sub.add_parser("vbs-switches", help="Mine a VPX/VBS table script for switch candidates")
    p_vbs_switches.add_argument("--script", required=True)
    p_vbs_switches.add_argument("--limit", type=int, default=80)
    p_vbs_switches.set_defaults(func=cmd_vbs_switches)

    p_candidate = sub.add_parser(
        "candidate-map", parents=[common], help="Write an unverified candidate map by copying a donor game_state"
    )
    p_candidate.add_argument("--rom", required=True, help="Target ROM whose map should receive a candidate game_state")
    p_candidate.add_argument("--donor-rom", required=True, help="Solved ROM to donate game_state")
    p_candidate.add_argument("--out-root", default="/tmp/scoretracker-candidate-maps")
    p_candidate.set_defaults(func=cmd_candidate_map)

    p_inventory = sub.add_parser(
        "local-inventory", parents=[common], help="Find unresolved ROMs with local table PinMAME state"
    )
    p_inventory.add_argument("--tables-root", default="/Users/andremichi/tables")
    p_inventory.add_argument("--roms-dir", default="/Users/andremichi/.pinmame/roms")
    p_inventory.add_argument("--json", action="store_true")
    p_inventory.set_defaults(func=cmd_local_inventory)

    p_priority = sub.add_parser(
        "prioritize-missing", parents=[common], help="Rank missing maps by donor confidence and ROM payoff"
    )
    p_priority.add_argument("--tables-root", default="/Users/andremichi/tables")
    p_priority.add_argument("--roms-dir", default="/Users/andremichi/.pinmame/roms")
    p_priority.add_argument("--include-experimental", action="store_true")
    p_priority.add_argument(
        "--evidence-file",
        default=str(Path(__file__).with_name("evidence.json")),
        help="Persistent ROM exercise evidence used to demote failed recipes",
    )
    p_priority.add_argument("--json", action="store_true")
    p_priority.add_argument("--output", help="Write the rendered queue to a file")
    p_priority.set_defaults(func=cmd_prioritize_missing)

    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    if getattr(args, "output", None):
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            args.func(args)
        Path(args.output).write_text(buffer.getvalue())
    else:
        args.func(args)


if __name__ == "__main__":
    main()
