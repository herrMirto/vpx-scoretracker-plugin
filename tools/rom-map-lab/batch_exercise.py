#!/usr/bin/env python3
"""Run a small isolated libpinmame exercise batch.

This is intentionally a lab helper, not plugin runtime code. It creates a fresh
PinMAME root per ROM under /tmp, symlinks ROM zips into it, optionally seeds the
root from the table's existing pinmame directory, runs pinmame_exerciser with
script-mined switch recipes, and writes a summary that distinguishes:

* the ROM booted and emitted snapshots;
* mapped game_state was decodable;
* decoded score state actually changed during the run.
"""

from __future__ import annotations

import argparse
import copy
import difflib
import json
import re
import shutil
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import map_lab


SWITCH_RE = re.compile(r"(?:Controller)?\.Switch\((\d+)\)\s*=\s*([01])", re.IGNORECASE)
PULSE_RE = re.compile(r"(?:vpmTimer\.)?PulseSw\s*\(?\s*(\d+)", re.IGNORECASE)
HIT_RE = re.compile(r"^\s*Sub\s+[A-Za-z_]*sw(\d+)[A-Za-z0-9_]*_?Hit\b", re.IGNORECASE)
SUB_RE = re.compile(r"^\s*Sub\s+([A-Za-z_][A-Za-z0-9_]*)", re.IGNORECASE)
END_SUB_RE = re.compile(r"\bEnd\s+Sub\b", re.IGNORECASE)
CREATED_BALL_RE = re.compile(
    r"\b(?:sw)?(\d+)\.CreateSizedball(?:WithMass)?\b", re.IGNORECASE
)
WITH_RE = re.compile(r"^\s*With\s+([A-Za-z_][A-Za-z0-9_]*)", re.IGNORECASE)
END_WITH_RE = re.compile(r"^\s*End\s+With\b", re.IGNORECASE)
INIT_SWITCHES_RE = re.compile(r"\.InitSwitches\s+Array\(([^)]*)\)", re.IGNORECASE)
INIT_SW_RE = re.compile(r"\.InitSw\s+([^']+)", re.IGNORECASE)
INIT_SAUCER_RE = re.compile(r"\.InitSaucer\s+(?:sw)?(\d+)", re.IGNORECASE)
TROUGH_SHIFT_RE = re.compile(
    r"if\s+sw(\d+)\.BallCntOver\s*=\s*0\s+then\s+sw(\d+)\.kick",
    re.IGNORECASE,
)
SWITCH_KICK_RE = re.compile(r"\bsw(\d+)\.kick\b", re.IGNORECASE)
SWITCH_SUB_RE = re.compile(r"^sw(\d+)_?(?:un)?hit$", re.IGNORECASE)
HOLD_CONTEXT_WORDS = (
    "trough",
    "drain",
    "outhole",
    "ballrelease",
    "ball release",
    "coin door",
    "coindoor",
    "keep it close",
)


DEFAULT_CASES = (
    ("stargat5", "/tmp/stargate_extract.vbs"),
    (
        "mtl_170h",
        "/Users/andremichi/tables/Metallica (Premium Monsters) (Stern 2013)/Metallica Premium Monsters (Stern 2013) VPW 2.0.2.vbs",
    ),
    (
        "bdk_294",
        "/Users/andremichi/tables/Batman (Stern 2008)/Batman [The Dark Knight] (Stern 2008).vbs",
    ),
    (
        "play203",
        "/Users/andremichi/tables/Playboy (Stern 2002)/Playboy (Stern 2002) v1.1.vbs",
    ),
    (
        "wof_500",
        "/Users/andremichi/tables/Wheel of Fortune (Stern 2007)/Wheel of Fortune (Stern 2007) 1.0.vbs",
    ),
    (
        "ss_15",
        "/Users/andremichi/tables/Scared Stiff (Bally 1996)/Scared Stiff (Bally 1996) VPW v1.1.vbs",
    ),
    ("smbmush", "/tmp/smbmush_extract.vbs"),
    ("sfight2", "/tmp/sfight2_extract.vbs"),
    ("rescu911", "/tmp/rescu911_extract.vbs"),
    ("bbb108", "/tmp/bbb108_extract.vbs"),
)

DEFAULT_SEEDS = {
    "stargat5": "/Users/andremichi/tables/Stargate (Gottlieb 1995)/pinmame",
}


@dataclass(frozen=True)
class Recipe:
    holds: list[int]
    post_start_sets: list[tuple[int, int]]
    pulses: list[int]
    drain_switch: int | None
    launch_switch: int | None
    score_switch: int | None


def parse_case(raw: str) -> tuple[str, Path]:
    if "=" not in raw:
        raise argparse.ArgumentTypeError("case must be ROM=/path/to/script.vbs")
    rom, script = raw.split("=", 1)
    rom = rom.strip()
    script = script.strip()
    if not rom or not script:
        raise argparse.ArgumentTypeError("case must be ROM=/path/to/script.vbs")
    return rom, Path(script)


def parse_seed(raw: str) -> tuple[str, Path]:
    if "=" not in raw:
        raise argparse.ArgumentTypeError("seed must be ROM=/path/to/pinmame")
    rom, seed = raw.split("=", 1)
    if not rom.strip() or not seed.strip():
        raise argparse.ArgumentTypeError("seed must be ROM=/path/to/pinmame")
    return rom.strip(), Path(seed.strip())


def parse_rom_alias(raw: str) -> tuple[str, str]:
    if "=" not in raw:
        raise argparse.ArgumentTypeError("seed-rom must be TARGET_ROM=SOURCE_ROM")
    target, source = raw.split("=", 1)
    if not target.strip() or not source.strip():
        raise argparse.ArgumentTypeError("seed-rom must be TARGET_ROM=SOURCE_ROM")
    return target.strip(), source.strip()


def mine_recipe(script: Path, pulse_limit: int) -> Recipe:
    text = script.read_text(errors="replace").splitlines()
    direct: dict[int, dict[int, int]] = {}
    pulses: set[int] = set()
    hits: set[int] = set()
    context_holds: set[int] = set()
    post_start_releases: set[int] = set()
    drain_event_switches: set[int] = set()
    release_exit_switches: set[int] = set()
    trough_shifts: dict[int, int] = {}
    launch_pulses: set[int] = set()
    launch_context_switches: set[int] = set()
    explicit_launch_switches: set[int] = set()
    current_sub = ""
    current_sub_context = ""
    current_with = ""

    for line in text:
        lower = line.lower()
        with_match = WITH_RE.search(line)
        if with_match:
            current_with = with_match.group(1).lower()
        sub_match = SUB_RE.search(line)
        if sub_match:
            current_sub = sub_match.group(1).lower()
            current_sub_context = lower
        semantic_context = f"{current_sub} {current_sub_context} {lower}"
        shift_match = TROUGH_SHIFT_RE.search(line)
        if shift_match:
            trough_shifts[int(shift_match.group(1))] = int(shift_match.group(2))
        if current_sub.startswith(("solrelease", "ballkick", "ballrelease")):
            for kick_match in SWITCH_KICK_RE.finditer(line):
                release_exit_switches.add(int(kick_match.group(1)))
        if "randomsounddrain" in lower:
            switch_sub = SWITCH_SUB_RE.match(current_sub)
            if switch_sub:
                drain_event_switches.add(int(switch_sub.group(1)))
        trough_context = "trough" in f"{current_with} {lower}"
        saucer_match = INIT_SAUCER_RE.search(line)
        if saucer_match and any(
            word in f"{current_with} {lower}" for word in ("shooter", "plunger", "launch")
        ):
            explicit_launch_switches.add(int(saucer_match.group(1)))
        array_init_match = INIT_SWITCHES_RE.search(line)
        stack_init_match = INIT_SW_RE.search(line)
        init_match = array_init_match or stack_init_match
        if trough_context and init_match and not lower.lstrip().startswith("'"):
            raw_switches = [int(value) for value in re.findall(r"\b\d+\b", init_match.group(1))]
            if stack_init_match:
                entry_switch = raw_switches[0] if raw_switches else 0
                if 1 <= entry_switch <= 128:
                    drain_event_switches.add(entry_switch)
                raw_switches = raw_switches[1:]
            switches = [sw for sw in raw_switches if 1 <= sw <= 128]
            context_holds.update(switches)
            if switches:
                # cvpmTrough arrays run from exit toward entry. After the first
                # launch, the entry-side (last) switch is the empty position.
                post_start_releases.add(switches[-1])
        for match in CREATED_BALL_RE.finditer(line):
            sw = int(match.group(1))
            # A ball created at table initialization closes its switch, whether
            # it belongs to the trough, drain, shooter lane, or a lock.
            context_holds.add(sw)
        for match in SWITCH_RE.finditer(line):
            sw = int(match.group(1))
            value = int(match.group(2))
            direct.setdefault(sw, {0: 0, 1: 0})[value] += 1
            physical_trough_context = any(
                word in semantic_context for word in ("trough", "ballrelease", "ball release")
            )
            drain_context = any(
                word in semantic_context for word in ("drain", "outhole")
            )
            if value == 1 and physical_trough_context and not drain_context:
                context_holds.add(sw)
            if value == 1 and drain_context and "hit" in current_sub:
                drain_event_switches.add(sw)
            if value == 1 and current_sub.startswith(("solrelease", "ballkick")):
                launch_context_switches.add(sw)
            release_sub = current_sub.startswith("ballkick") or (
                current_sub.startswith("ballrelease") and "hit" not in current_sub
            )
            if value == 0 and release_sub:
                release_exit_switches.add(sw)
                context_holds.add(sw)
            if (
                value == 1
                and "hit" not in current_sub
                and any(word in semantic_context for word in HOLD_CONTEXT_WORDS)
            ):
                context_holds.add(sw)
            if any(word in semantic_context for word in ("shooter", "plunger", "launch")):
                launch_context_switches.add(sw)
            if value == 0 and any(word in semantic_context for word in ("drain", "outhole")):
                post_start_releases.add(sw)
        for match in PULSE_RE.finditer(line):
            sw = int(match.group(1))
            pulses.add(sw)
            if any(word in semantic_context for word in ("trough", "release", "launch")):
                launch_pulses.add(sw)
        match = HIT_RE.search(line)
        if match:
            hits.add(int(match.group(1)))
        if END_SUB_RE.search(line):
            current_sub = ""
            current_sub_context = ""
        if END_WITH_RE.search(line):
            current_with = ""

    # Some scripts set trough/drain state in an init block with comments nearby,
    # while scoring switches usually have matching 1/0 assignments or PulseSw.
    scoring_from_direct = {
        sw for sw, row in direct.items() if row.get(1, 0) > 0 and row.get(0, 0) > 0
    }
    trough_entry_switch: int | None = None
    hold_list = sorted(sw for sw in context_holds if 1 <= sw <= 128)[:8]
    post_start_sets = sorted((sw, 0) for sw in post_start_releases & set(hold_list))
    if not post_start_sets and release_exit_switches and not trough_shifts:
        post_start_sets = sorted(
            (sw, 0) for sw in release_exit_switches & set(hold_list)
        )
    if not post_start_sets and trough_shifts and release_exit_switches:
        for exit_switch in sorted(release_exit_switches):
            current = exit_switch
            seen: set[int] = set()
            shift_actions: list[tuple[int, int]] = [(exit_switch, 0)]
            while current in trough_shifts and current not in seen:
                seen.add(current)
                source = trough_shifts[current]
                if source not in hold_list:
                    break
                shift_actions.extend([(source, 0), (current, 1)])
                current = source
            if len(shift_actions) > 1:
                post_start_sets = shift_actions
                trough_entry_switch = current
                break
    if not post_start_sets and len(hold_list) == 4 and hold_list == list(
        range(hold_list[0], hold_list[0] + 4)
    ):
        # Common physical trough scripts create four balls on consecutive
        # switches and shift toward the exit. The entry-side (lowest) switch is
        # empty after the first launch even when the VBS does not label it.
        post_start_sets = [(hold_list[0], 0)]
    pulse_list = sorted((pulses | hits | scoring_from_direct) - set(hold_list))
    pulse_list = [sw for sw in pulse_list if 1 <= sw <= 128][:pulse_limit]
    drain_switch = (
        min(drain_event_switches)
        if drain_event_switches
        else trough_entry_switch
        if trough_entry_switch is not None
        else post_start_sets[0][0] if post_start_sets else None
    )
    launch_candidates = explicit_launch_switches or (launch_pulses | launch_context_switches)
    launch_switch = min(launch_candidates) if launch_candidates else None
    score_switch = next(
        (sw for sw in pulse_list if sw not in {drain_switch, launch_switch}), None
    )
    return Recipe(
        holds=hold_list,
        post_start_sets=post_start_sets,
        pulses=pulse_list,
        drain_switch=drain_switch,
        launch_switch=launch_switch,
        score_switch=score_switch,
    )


def table_pinmame_dir(script: Path) -> Path | None:
    """Return the table-local PinMAME directory beside a loose VBS, if any."""
    candidate = script.parent / "pinmame"
    return candidate if candidate.is_dir() else None


def seed_pinmame_root(
    vpm_root: Path,
    seed_dir: Path | None,
    target_rom: str,
    source_rom: str | None,
) -> list[str]:
    """Copy table-local persistent state into an isolated PinMAME root."""
    if seed_dir is None:
        return []

    copied: list[str] = []
    for dirname in ("nvram", "cfg", "memcard", "ini"):
        source = seed_dir / dirname
        if not source.is_dir():
            continue
        destination = vpm_root / dirname
        for item in source.rglob("*"):
            if not item.is_file():
                continue
            relative = item.relative_to(source)
            if any(part.startswith(".") for part in relative.parts):
                continue
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item, target)
            copied.append(str(Path(dirname) / relative))
            if source_rom and item.stem == source_rom:
                alias_relative = relative.with_name(target_rom + item.suffix)
                alias_target = destination / alias_relative
                shutil.copy2(item, alias_target)
                copied.append(
                    f"{Path(dirname) / alias_relative} (aliased from {item.name})"
                )
    return sorted(copied)


def prepare_pinmame_root(
    base: Path,
    rom: str,
    roms_dir: Path,
    seed_dir: Path | None,
    seed_rom: str | None,
) -> tuple[Path, list[str]]:
    # Never share writable PinMAME state between cases. A ROM can rewrite NVRAM
    # during a run, and that must not influence another case or the real table.
    vpm_root = base / "roots" / rom
    for dirname in ("roms", "nvram", "cfg", "memcard", "ini", "samples", "altcolor", "altsound"):
        (vpm_root / dirname).mkdir(parents=True, exist_ok=True)
    for rom_zip in roms_dir.glob("*.zip"):
        target = vpm_root / "roms" / rom_zip.name
        if not target.exists():
            target.symlink_to(rom_zip)
    if seed_dir is not None:
        local_rom = seed_dir / "roms" / f"{rom}.zip"
        target = vpm_root / "roms" / local_rom.name
        if local_rom.is_file() and not target.exists():
            target.symlink_to(local_rom)
    return vpm_root, seed_pinmame_root(vpm_root, seed_dir, rom, seed_rom)


def decode_snapshots(rom: str, exercise_dir: Path, maps_root: Path) -> tuple[list[dict[str, Any]], str | None]:
    try:
        index, records = map_lab.load_records(maps_root)
        record = map_lab.resolve_rom(index, records, rom)
        base = map_lab.platform_nvram_base(maps_root, record.platform)
    except SystemExit as exc:
        return [], str(exc)

    decoded: list[dict[str, Any]] = []
    files = sorted(exercise_dir.glob("*.nv"), key=lambda path: (path.stat().st_mtime_ns, path.name))
    for file in files:
        state = map_lab.decode_game_state_snapshot(file.read_bytes(), record, base)
        if state:
            decoded.append({"file": file.name, "state": state})
    return decoded, None


def decode_score_field_strict(data: bytes, spec: dict[str, Any], base: int) -> Any:
    """Decode a score, rejecting invalid packed-BCD nibbles used by transient RAM."""
    if str(spec.get("encoding", "")).lower() == "bcd":
        start = map_lab.normalized_addr(spec["start"], base)
        length = int(spec.get("length", 1))
        raw = data[start : start + length]
        if len(raw) != length or any((byte >> 4) > 9 or (byte & 0x0F) > 9 for byte in raw):
            return None
    return map_lab.decode_field(data, spec, base)


def evaluate_candidate_layouts(
    rom: str, exercise_dir: Path, maps_root: Path, limit: int = 5
) -> list[dict[str, Any]]:
    """Decode an unsolved ROM using solved sibling layouts and rank useful hits."""
    try:
        index, records = map_lab.load_records(maps_root)
        target = map_lab.resolve_rom(index, records, rom)
        base = map_lab.platform_nvram_base(maps_root, target.platform)
    except SystemExit:
        return []

    files = sorted(exercise_dir.glob("*.nv"), key=lambda path: (path.stat().st_mtime_ns, path.name))
    evaluated: list[dict[str, Any]] = []
    for confidence, candidate in map_lab.ranked_suggestions(target, records, maps_root, limit):
        score_specs = (candidate.data.get("game_state") or {}).get("scores") or []
        states = []
        for file in files:
            data = file.read_bytes()
            if score_specs:
                states.append([
                    decode_score_field_strict(data, spec, base) for spec in score_specs
                ])
        unique = []
        for state in states:
            if state not in unique:
                unique.append(state)
        nonzero = any(
            any(isinstance(value, int) and value != 0 for value in state)
            for state in states
        )
        evaluated.append(
            {
                "map": candidate.path,
                "confidence": round(confidence, 3),
                "score_addresses": map_lab.game_state_summary(candidate, base)["scores"],
                "score_state_changed": len(unique) > 1,
                "score_nonzero_seen": nonzero,
                "unique_score_states": unique,
            }
        )
    return sorted(
        evaluated,
        key=lambda row: (
            row["score_state_changed"],
            row["score_nonzero_seen"],
            row["confidence"],
        ),
        reverse=True,
    )


def score_states(decoded: list[dict[str, Any]]) -> list[list[Any]]:
    states: list[list[Any]] = []
    for item in decoded:
        scores = item["state"].get("scores")
        if isinstance(scores, list):
            states.append(scores)
    return states


def discover_game_over_candidates(
    exercise_dir: Path,
    maps_root: Path,
    rom: str,
    limit: int = 40,
) -> list[dict[str, Any]]:
    """Rank NVRAM bits that are stable in attract and invert for a live game."""
    try:
        index, records = map_lab.load_records(maps_root)
        target = map_lab.resolve_rom(index, records, rom)
        base = map_lab.platform_nvram_base(maps_root, target.platform)
    except SystemExit:
        return []

    files = sorted(
        exercise_dir.glob("*.nv"), key=lambda path: (path.stat().st_mtime_ns, path.name)
    )
    attract_files = [
        path
        for path in files
        if path.name == "boot.nv"
        or path.name.startswith("coin")
        or (path.name.startswith("start1_") and path.name.endswith("_before.nv"))
        or "attract" in path.name.lower()
        or "game_over" in path.name.lower()
    ]
    live_files = [
        path
        for path in files
        if (path.name.startswith("start1_") and path.name.endswith("_after.nv"))
        or path.name.startswith(("poststart", "manual"))
        or any(
            marker in path.name.lower()
            for marker in ("game_started", "score_known", "last_ball")
        )
    ]
    if len(attract_files) < 2 or len(live_files) < 3:
        return []
    attract = [path.read_bytes() for path in attract_files]
    live = [path.read_bytes() for path in live_files]
    size = min(*(len(data) for data in attract), *(len(data) for data in live))
    candidates: list[dict[str, Any]] = []
    for offset in range(size):
        for mask in (1, 2, 4, 8, 16, 32, 64, 128):
            attract_bits = [bool(data[offset] & mask) for data in attract]
            live_bits = [bool(data[offset] & mask) for data in live]
            attract_value = max(set(attract_bits), key=attract_bits.count)
            live_value = max(set(live_bits), key=live_bits.count)
            if attract_value == live_value:
                continue
            attract_consistency = attract_bits.count(attract_value) / len(attract_bits)
            live_consistency = live_bits.count(live_value) / len(live_bits)
            if attract_consistency < 0.98 or live_consistency < 0.98:
                continue
            candidates.append(
                {
                    "offset": f"0x{offset:X}",
                    "start": map_lab.fmt_addr(base + offset),
                    "mask": f"0x{mask:02X}",
                    "attract": attract_value,
                    "live": live_value,
                    "invert": not attract_value,
                    "attract_consistency": round(attract_consistency, 3),
                    "live_consistency": round(live_consistency, 3),
                    "samples": {"attract": len(attract), "live": len(live)},
                }
            )
    candidates.sort(
        key=lambda row: (
            -(row["attract_consistency"] + row["live_consistency"]),
            map_lab.parse_int(row["offset"]),
            map_lab.parse_int(row["mask"]),
        )
    )
    return candidates[:limit]


def changed_count(before: bytes, after: bytes) -> int:
    n = min(len(before), len(after))
    count = abs(len(before) - len(after))
    count += sum(1 for i in range(n) if before[i] != after[i])
    return count


def max_pair_change(exercise_dir: Path) -> int:
    max_changed = 0
    for before in exercise_dir.glob("*_sw*_before.nv"):
        after = before.with_name(before.name.replace("_before.nv", "_after.nv"))
        if after.exists():
            max_changed = max(max_changed, changed_count(before.read_bytes(), after.read_bytes()))
    return max_changed


def discover_int_score_candidates(
    rom: str, exercise_dir: Path, maps_root: Path, limit: int = 12
) -> list[dict[str, Any]]:
    """Rank aligned little-endian integers that behave like a running score."""
    try:
        index, records = map_lab.load_records(maps_root)
        target = map_lab.resolve_rom(index, records, rom)
        base = map_lab.platform_nvram_base(maps_root, target.platform)
    except SystemExit:
        base = 0

    files = sorted(
        exercise_dir.glob("manual*_sw*_after.nv"),
        key=lambda path: (path.stat().st_mtime_ns, path.name),
    )
    snapshots = [path.read_bytes() for path in files]
    if len(snapshots) < 2:
        return []

    length = min(len(data) for data in snapshots)
    ranked: list[tuple[tuple[float, int, int], dict[str, Any]]] = []
    for offset in range(0, length - 3, 4):
        values = [int.from_bytes(data[offset : offset + 4], "little") for data in snapshots]
        transitions = sum(a != b for a, b in zip(values, values[1:]))
        if transitions < 2 or max(values) == 0:
            continue
        nondecreasing = sum(a <= b for a, b in zip(values, values[1:]))
        ratio = nondecreasing / (len(values) - 1)
        # Scores normally rise. Permit one backwards transition for a mode reset,
        # but exclude noisy timers and signed/sentinel-looking values.
        if ratio < 0.95 or max(values) >= 1_000_000_000:
            continue
        unique: list[int] = []
        for value in values:
            if not unique or value != unique[-1]:
                unique.append(value)
        row = {
            "start": map_lab.fmt_addr(offset + base if base else offset),
            "offset": map_lab.fmt_addr(offset),
            "encoding": "int",
            "length": 4,
            "transitions": transitions,
            "monotonic_ratio": round(ratio, 3),
            "values": unique,
        }
        largest_jump = max((b - a for a, b in zip(values, values[1:])), default=0)
        # Real scores normally have much larger values/jumps than clocks and
        # bookkeeping counters, which may otherwise look perfectly monotonic.
        ranked.append(((ratio, transitions * largest_jump, max(values)), row))
    ranked.sort(key=lambda item: item[0], reverse=True)
    return [row for _, row in ranked[:limit]]


def propose_game_state(
    rom: str, discovered: list[dict[str, Any]], maps_root: Path
) -> dict[str, Any] | None:
    """Emit a review-only score block anchored by a proven live Player 1."""
    if not discovered or discovered[0]["transitions"] < 3:
        return None
    try:
        index, records = map_lab.load_records(maps_root)
        target = map_lab.resolve_rom(index, records, rom)
    except SystemExit:
        return None
    start = int(discovered[0]["start"], 16)
    target_high_score = map_lab.high_score_score_offsets(
        target, map_lab.platform_nvram_base(maps_root, target.platform)
    )
    donors: list[tuple[int, map_lab.MapRecord, list[int]]] = []
    base = map_lab.platform_nvram_base(maps_root, target.platform)
    for record in records:
        donor_specs = (record.data.get("game_state") or {}).get("scores") or []
        if not donor_specs or str(donor_specs[0].get("encoding", "")).lower() != "int":
            continue
        addresses = map_lab.game_score_offsets(record, base)
        if len(addresses) < 4 or addresses[0] + base != start:
            continue
        donor_high_score = map_lab.high_score_score_offsets(record, base)
        distance = (
            abs(target_high_score[0] - donor_high_score[0])
            if target_high_score and donor_high_score
            else 1_000_000
        )
        donors.append((distance, record, addresses))

    donor = min(donors, key=lambda item: item[0]) if donors else None
    if donor:
        _, donor_record, donor_offsets = donor
        score_starts = [start + (offset - donor_offsets[0]) for offset in donor_offsets[:4]]
        review = (
            f"Player 1 was proven live; Player 2-4 layout follows {donor_record.path} "
            "and requires multiplayer review."
        )
    else:
        return None
    return {
        "_review": review,
        "_donor": donor_record.path if donor_record else None,
        "scores": [
            {
                "label": f"Player {player}",
                "start": f"0x{address:08X}",
                "encoding": "int",
                "length": 4,
            }
            for player, address in enumerate(score_starts, start=1)
        ],
    }


def propose_live_donor_game_state(
    rom: str, layouts: list[dict[str, Any]], maps_root: Path
) -> dict[str, Any] | None:
    """Accept a donor layout only when it decodes a convincing live P1 series."""
    try:
        index, records = map_lab.load_records(maps_root)
        target = map_lab.resolve_rom(index, records, rom)
    except SystemExit:
        return None
    by_path = {record.path: record for record in records}
    for layout in layouts:
        if not layout.get("score_state_changed"):
            continue
        values = [
            state[0]
            for state in layout.get("unique_score_states", [])
            if state and isinstance(state[0], int) and 0 <= state[0] < 1_000_000_000
        ]
        transitions = sum(a != b for a, b in zip(values, values[1:]))
        nondecreasing = sum(a <= b for a, b in zip(values, values[1:]))
        if transitions < 3 or not values or nondecreasing / max(len(values) - 1, 1) < 0.8:
            continue
        donor = by_path.get(layout["map"])
        if donor is None:
            continue
        scores = copy.deepcopy((donor.data.get("game_state") or {}).get("scores") or [])
        if not scores:
            continue
        return {
            "_review": (
                f"The {donor.path} layout decoded a live changing Player 1 score "
                f"for {target.path}; Player 2-4 require multiplayer review."
            ),
            "_donor": donor.path,
            "scores": scores,
        }
    return None


def collect_player_evidence(
    exercise_dir: Path,
    proposed: dict[str, Any] | None,
    discovered: list[dict[str, Any]],
    maps_root: Path,
    rom: str,
) -> dict[str, Any]:
    if not proposed:
        return {"verified_players": [], "players": []}
    try:
        index, records = map_lab.load_records(maps_root)
        target = map_lab.resolve_rom(index, records, rom)
        base = map_lab.platform_nvram_base(maps_root, target.platform)
    except SystemExit:
        return {"verified_players": [], "players": []}

    files = sorted(
        [
            path
            for path in exercise_dir.glob("*.nv")
            if path.name.startswith(("manual", "action", "poststart"))
        ],
        key=lambda path: (path.stat().st_mtime_ns, path.name),
    )
    rows = []
    verified = []
    for player, spec in enumerate(proposed.get("scores", []), start=1):
        values = [
            decode_score_field_strict(path.read_bytes(), spec, base) for path in files
        ]
        values = [value for value in values if isinstance(value, int)]
        initial = values[0] if values else None
        changed_to_nonzero = bool(
            values
            and any(value > 0 and value != initial for value in values[1:])
        )
        if player == 1:
            transitions = sum(a != b for a, b in zip(values, values[1:]))
            changed_to_nonzero = bool(
                (discovered and discovered[0].get("transitions", 0) >= 3)
                or transitions >= 3
            )
        if changed_to_nonzero:
            verified.append(player)
        rows.append(
            {
                "player": player,
                "start": spec["start"],
                "initial": initial,
                "maximum": max(values) if values else None,
                "changed_to_nonzero": changed_to_nonzero,
            }
        )
    return {"verified_players": verified, "players": rows}


def emit_candidate_artifacts(
    *,
    rom: str,
    case_dir: Path,
    maps_root: Path,
    proposed: dict[str, Any] | None,
    evidence: dict[str, Any],
) -> dict[str, Any] | None:
    if not proposed:
        return None
    try:
        index, records = map_lab.load_records(maps_root)
        target = map_lab.resolve_rom(index, records, rom)
    except SystemExit:
        return None
    if target.has_game_state:
        return None

    verified = evidence.get("verified_players", [])
    donor = proposed.get("_donor")
    status = (
        "ready_for_review"
        if donor and 1 in verified and 2 in verified
        else "needs_multiplayer_verification"
    )
    clean = copy.deepcopy(target.data)
    metadata = clean.get("_metadata")
    if isinstance(metadata, dict) and isinstance(metadata.get("version"), int):
        metadata["version"] += 1
    notes = clean.setdefault("_notes", [])
    if not isinstance(notes, list):
        notes = [str(notes)]
        clean["_notes"] = notes
    notes.append(
        "Live headless PinMAME exercise discovered Player 1 score changes; "
        f"layout follows {donor or 'no donor'}; independently verified players {verified}."
    )
    clean["game_state"] = {"scores": copy.deepcopy(proposed["scores"])}

    candidate = copy.deepcopy(clean)
    candidate["_candidate"] = {
        "source": "tools/rom-map-lab/batch_exercise.py",
        "status": status,
        "target_rom": rom,
        "target_map": target.path,
        "donor_map": donor,
        "evidence": evidence,
    }
    candidate_path = case_dir / "candidate-map" / target.path
    candidate_path.parent.mkdir(parents=True, exist_ok=True)
    candidate_path.write_text(json.dumps(candidate, indent=2) + "\n")

    patch_path = None
    if status == "ready_for_review":
        original = target.full_path.read_text().splitlines(keepends=True)
        updated = (json.dumps(clean, indent=2) + "\n").splitlines(keepends=True)
        patch_text = "".join(
            difflib.unified_diff(
                original,
                updated,
                fromfile=f"a/{target.path}",
                tofile=f"b/{target.path}",
            )
        )
        patch_path = case_dir / "candidate.patch"
        patch_path.write_text(patch_text)

    report = {
        "status": status,
        "target_map": target.path,
        "candidate_map": str(candidate_path),
        "patch": str(patch_path) if patch_path else None,
        "verified_players": verified,
        "donor_map": donor,
    }
    (case_dir / "candidate_report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def clear_candidate_artifacts(
    rom: str, case_dir: Path, maps_root: Path
) -> None:
    for name in ("candidate_game_state.json", "candidate_report.json", "candidate.patch"):
        (case_dir / name).unlink(missing_ok=True)
    try:
        index, records = map_lab.load_records(maps_root)
        target = map_lab.resolve_rom(index, records, rom)
    except SystemExit:
        return
    (case_dir / "candidate-map" / target.path).unlink(missing_ok=True)


def analyze_case(rom: str, case_dir: Path, maps_root: Path) -> dict[str, Any]:
    clear_candidate_artifacts(rom, case_dir, maps_root)
    decoded, decode_error = decode_snapshots(rom, case_dir, maps_root)
    (case_dir / "decoded.json").write_text(json.dumps(decoded, indent=2))
    states = score_states(decoded)
    unique_score_states = {tuple(state) for state in states}
    nonzero_score_seen = any(
        any(isinstance(value, int) and value != 0 for value in state)
        for state in states
    )
    candidate_layouts = evaluate_candidate_layouts(rom, case_dir, maps_root)
    discovered_int_scores = discover_int_score_candidates(rom, case_dir, maps_root)
    proposed_game_state = propose_live_donor_game_state(
        rom, candidate_layouts, maps_root
    )
    if proposed_game_state is None:
        proposed_game_state = propose_game_state(rom, discovered_int_scores, maps_root)
    if proposed_game_state:
        (case_dir / "candidate_game_state.json").write_text(
            json.dumps(proposed_game_state, indent=2) + "\n"
        )
    player_evidence = collect_player_evidence(
        case_dir, proposed_game_state, discovered_int_scores, maps_root, rom
    )
    candidate_report = emit_candidate_artifacts(
        rom=rom,
        case_dir=case_dir,
        maps_root=maps_root,
        proposed=proposed_game_state,
        evidence=player_evidence,
    )
    game_over_candidates = discover_game_over_candidates(
        case_dir, maps_root, rom
    )
    return {
        "nv_files": len(list(case_dir.glob("*.nv"))),
        "decoded_snapshots": len(decoded),
        "decode_error": decode_error,
        "score_state_changed": len(unique_score_states) > 1,
        "score_nonzero_seen": nonzero_score_seen,
        "score_states": states,
        "max_switch_changed_bytes": max_pair_change(case_dir),
        "candidate_layouts": candidate_layouts,
        "discovered_int_scores": discovered_int_scores,
        "proposed_game_state": proposed_game_state,
        "player_evidence": player_evidence,
        "candidate_report": candidate_report,
        "game_over_candidates": game_over_candidates,
    }


def run_case(
    *,
    rom: str,
    script: Path,
    recipe: Recipe,
    out_dir: Path,
    vpm_root: Path,
    exerciser: Path,
    coins: int,
    starts: int,
    start_gap_ms: int,
    boot_ms: int,
    hold_delay_ms: int,
    settle_ms: int,
    maps_root: Path,
    seed_dir: Path | None,
    seeded_files: list[str],
    player_cycles: int,
    ball_save_wait_ms: int,
    drain_pulse_ms: int,
    drain_settle_ms: int,
    dump_ram: bool = False,
    ram_windows: list[str] | None = None,
    cpu_windows: list[str] | None = None,
) -> dict[str, Any]:
    case_dir = out_dir / rom
    case_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(exerciser),
        "--rom",
        rom,
        "--vpm-path",
        str(vpm_root),
        "--quiet-logs",
        "--boot-ms",
        str(boot_ms),
        "--hold-delay-ms",
        str(hold_delay_ms),
        "--settle-ms",
        str(settle_ms),
        "--key-pulse-ms",
        "250",
        "--coins",
        str(coins),
        "--starts",
        str(starts),
        "--start-gap-ms",
        str(start_gap_ms),
        "--out-dir",
        str(case_dir),
    ]
    if dump_ram:
        cmd.append("--dump-ram")
    for window in ram_windows or []:
        cmd.extend(["--ram-window", window])
    for window in cpu_windows or []:
        cmd.extend(["--cpu-window", window])
    for sw in recipe.holds:
        cmd.extend(["--hold-switch", str(sw)])
    for sw, state in recipe.post_start_sets:
        cmd.extend(["--post-start-set-switch", f"{sw}={state}"])
    ordered_pulses = list(recipe.pulses)
    if recipe.launch_switch is not None:
        ordered_pulses = [
            recipe.launch_switch,
            *(sw for sw in ordered_pulses if sw != recipe.launch_switch),
        ]
    for sw in ordered_pulses:
        cmd.extend(["--pulse-switch", str(sw)])
    multiplayer_actions: list[str] = []
    if (
        player_cycles > 0
        and recipe.drain_switch is not None
        and recipe.launch_switch is not None
        and recipe.score_switch is not None
    ):
        # A drain pulse alone is not enough for ROMs backed by cvpmBallStack:
        # the returned ball must also occupy the trough position that was
        # cleared when the previous ball launched.  Refill every inferred
        # post-start trough switch before waiting for the ROM to end the ball,
        # then clear it again as the next ball is served.
        released_trough_switches = [
            sw for sw, state in recipe.post_start_sets if state == 0
        ]
        trough_switches = sorted(set(recipe.holds))
        if len(released_trough_switches) == 1:
            released = released_trough_switches[0]
            contiguous = [released]
            while contiguous[0] - 1 in trough_switches:
                contiguous.insert(0, contiguous[0] - 1)
            while contiguous[-1] + 1 in trough_switches:
                contiguous.append(contiguous[-1] + 1)
            trough_switches = contiguous
        for _ in range(player_cycles):
            drain_is_trough_slot = recipe.drain_switch in released_trough_switches
            multiplayer_actions.extend([
                f"wait:{ball_save_wait_ms}",
                f"set:{recipe.drain_switch}=1:"
                f"{drain_settle_ms if drain_is_trough_slot else drain_pulse_ms}",
            ])
            if not drain_is_trough_slot:
                multiplayer_actions.extend(
                    f"set:{sw}=1:{settle_ms}" for sw in released_trough_switches
                )
                multiplayer_actions.append(
                    f"set:{recipe.drain_switch}=0:{drain_settle_ms}"
                )
            if len(released_trough_switches) == 1 and len(trough_switches) > 1:
                # cvpmBallStack numbers these common linear troughs from the
                # exit back toward the entry.  Simulate the release ripple so
                # the ROM sees the exit switch open, the following balls roll
                # forward, and the entry-side slot becomes empty.
                released = released_trough_switches[0]
                ordered = (
                    list(reversed(trough_switches))
                    if released == trough_switches[0]
                    else trough_switches
                )
                multiplayer_actions.append(
                    f"set:{ordered[0]}=0:{settle_ms}"
                )
                for source, destination in zip(
                    ordered[1:],
                    ordered[:-1],
                ):
                    multiplayer_actions.extend([
                        f"set:{source}=0:250",
                        f"set:{destination}=1:{settle_ms}",
                    ])
            else:
                multiplayer_actions.extend(
                    f"set:{sw}=0:{settle_ms}" for sw in released_trough_switches
                )
            multiplayer_actions.extend([
                f"pulse:{recipe.launch_switch}:200:{drain_settle_ms}",
                f"pulse:{recipe.score_switch}:200:{settle_ms}",
            ])
    for action in multiplayer_actions:
        cmd.extend(["--action", action])

    started = time.time()
    proc = subprocess.run(cmd, text=True, capture_output=True, timeout=300)
    (case_dir / "stdout.jsonl").write_text(proc.stdout)
    (case_dir / "stderr.txt").write_text(proc.stderr)
    (case_dir / "command.json").write_text(json.dumps(cmd, indent=2))

    analysis = analyze_case(rom, case_dir, maps_root)

    result = {
        "rom": rom,
        "script": str(script),
        "seed_dir": str(seed_dir) if seed_dir else None,
        "seeded_files": seeded_files,
        "status": "ok" if proc.returncode == 0 else f"exit_{proc.returncode}",
        "seconds": round(time.time() - started, 1),
        "vpm_root": str(vpm_root),
        "out": str(case_dir),
        "holds": recipe.holds,
        "post_start_sets": recipe.post_start_sets,
        "pulses": ordered_pulses,
        "drain_switch": recipe.drain_switch,
        "launch_switch": recipe.launch_switch,
        "score_switch": recipe.score_switch,
        "multiplayer_actions": multiplayer_actions,
        **analysis,
    }
    (case_dir / "result.json").write_text(json.dumps(result, indent=2))
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", default="/tmp/pinmame-batch-isolated")
    parser.add_argument("--roms-dir", default="/Users/andremichi/.pinmame/roms")
    parser.add_argument("--maps-root", default="/Users/andremichi/workspace/scoretracker-maps")
    parser.add_argument("--exerciser", default=str(Path(__file__).with_name("bin") / "pinmame_exerciser"))
    parser.add_argument("--case", action="append", type=parse_case, help="ROM=/path/to/script.vbs; can repeat")
    parser.add_argument(
        "--analyze-existing",
        action="append",
        type=parse_case,
        metavar="ROM=CAPTURE_DIR",
        help="Re-analyze an existing exercise directory without running PinMAME",
    )
    parser.add_argument(
        "--seed",
        action="append",
        type=parse_seed,
        help="ROM=/path/to/pinmame; overrides automatic table-local state discovery",
    )
    parser.add_argument(
        "--seed-rom",
        action="append",
        type=parse_rom_alias,
        help="TARGET_ROM=SOURCE_ROM; copy matching NVRAM/config files under the target name",
    )
    parser.add_argument("--pulse-limit", type=int, default=16)
    parser.add_argument("--coins", type=int, default=5)
    parser.add_argument("--starts", type=int, default=1)
    parser.add_argument("--start-gap-ms", type=int, default=2500)
    parser.add_argument(
        "--verify-players",
        action="store_true",
        help="Start four players and simulate three timed drain/launch/score cycles",
    )
    parser.add_argument("--player-cycles", type=int, default=0)
    parser.add_argument("--ball-save-wait-ms", type=int, default=15000)
    parser.add_argument("--drain-pulse-ms", type=int, default=1000)
    parser.add_argument("--drain-settle-ms", type=int, default=5000)
    parser.add_argument("--boot-ms", type=int, default=3500)
    parser.add_argument("--hold-delay-ms", type=int, default=500)
    parser.add_argument("--settle-ms", type=int, default=500)
    parser.add_argument("--keep-root", action="store_true", help="Reuse existing per-ROM output roots")
    parser.add_argument("--dump-ram", action="store_true", help="Also dump CPU/RAM regions per snapshot (for game_over discovery)")
    parser.add_argument("--ram-window", action="append", default=[], help="START:LEN region slice to dump (hex ok); repeatable. Implies --dump-ram")
    parser.add_argument("--cpu-window", action="append", default=[], help="ADDR:LEN CPU-address window via PinmameReadMainCPUByte (hex ok); repeatable")
    return parser


def main() -> None:
    args = build_parser().parse_args()
    if args.analyze_existing:
        maps_root = Path(args.maps_root)
        results = []
        for rom, raw_dir in args.analyze_existing:
            case_dir = Path(raw_dir)
            if not case_dir.is_dir():
                result = {"rom": rom, "out": str(case_dir), "status": "missing_capture_dir"}
            else:
                result = {
                    "rom": rom,
                    "out": str(case_dir),
                    "status": "analyzed",
                    **analyze_case(rom, case_dir, maps_root),
                }
            results.append(result)
            print(json.dumps(result), flush=True)
        return
    if args.verify_players:
        args.starts = max(args.starts, 4)
        args.player_cycles = max(args.player_cycles, 3)
    out_dir = Path(args.out_dir)
    if out_dir.exists() and not args.keep_root:
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    cases = args.case if args.case else [(rom, Path(script)) for rom, script in DEFAULT_CASES]
    seeds = {rom: Path(path) for rom, path in DEFAULT_SEEDS.items()}
    if args.seed:
        seeds.update(dict(args.seed))
    seed_roms = dict(args.seed_rom or [])
    results = []
    for rom, script in cases:
        if not script.exists():
            result = {"rom": rom, "script": str(script), "status": "missing_script"}
            results.append(result)
            print(json.dumps(result), flush=True)
            continue
        seed_dir = seeds.get(rom) or table_pinmame_dir(script)
        if seed_dir is not None and not seed_dir.is_dir():
            result = {
                "rom": rom,
                "script": str(script),
                "seed_dir": str(seed_dir),
                "status": "missing_seed_dir",
            }
            results.append(result)
            print(json.dumps(result), flush=True)
            continue
        vpm_root, seeded_files = prepare_pinmame_root(
            out_dir, rom, Path(args.roms_dir), seed_dir, seed_roms.get(rom)
        )
        recipe = mine_recipe(script, args.pulse_limit)
        result = run_case(
            rom=rom,
            script=script,
            recipe=recipe,
            out_dir=out_dir,
            vpm_root=vpm_root,
            exerciser=Path(args.exerciser),
            coins=args.coins,
            starts=args.starts,
            start_gap_ms=args.start_gap_ms,
            boot_ms=args.boot_ms,
            hold_delay_ms=args.hold_delay_ms,
            settle_ms=args.settle_ms,
            maps_root=Path(args.maps_root),
            seed_dir=seed_dir,
            seeded_files=seeded_files,
            player_cycles=args.player_cycles,
            ball_save_wait_ms=args.ball_save_wait_ms,
            drain_pulse_ms=args.drain_pulse_ms,
            drain_settle_ms=args.drain_settle_ms,
            dump_ram=args.dump_ram,
            ram_windows=args.ram_window,
            cpu_windows=args.cpu_window,
        )
        results.append(result)
        print(json.dumps(result), flush=True)

    (out_dir / "summary.json").write_text(json.dumps(results, indent=2))
    print(f"SUMMARY_FILE {out_dir / 'summary.json'}")


if __name__ == "__main__":
    main()
