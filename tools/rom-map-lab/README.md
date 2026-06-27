# ROM map lab

Standalone helper for reducing manual game-state map work.

This is intentionally not part of the ScoreTracker plugin runtime. It is a lab
tool for exploring ROM/NVRAM patterns, comparing unsolved maps with solved
siblings, and preparing for a future headless PinMAME exercise harness.

## Why this exists

Playing every remaining table, especially four-player games, is slow. Many of
the remaining ROMs are likely close to maps we already solved. This helper tries
to automate the boring part:

- list indexed maps that still do not have `game_state`;
- group missing maps by platform and ROM family;
- compare missing maps with solved maps on the same platform;
- suggest reusable `game_state` layouts from similar solved maps;
- optionally scan local NVRAM/capture files for known scores;
- emit a machine-readable report that we can review before patching maps.

It does **not** update map files yet. That is deliberate.

## Current modes

### 1. Missing-map report

```bash
python3 tools/rom-map-lab/map_lab.py missing \
  --maps-root /Users/andremichi/workspace/scoretracker-maps
```

### 2. Suggest candidate layouts from solved sibling maps

```bash
python3 tools/rom-map-lab/map_lab.py suggest \
  --maps-root /Users/andremichi/workspace/scoretracker-maps \
  --rom mt_145
```

This looks up the ROM in `index.json`, checks whether its target map lacks
`game_state`, then ranks solved maps from the same platform. The ranking is based
on ROM-name similarity, map-path proximity, score layout shape, and nearby
high-score address patterns.

### 2b. Batch-suggest the missing maps

```bash
python3 tools/rom-map-lab/map_lab.py suggest-missing \
  --maps-root /Users/andremichi/workspace/scoretracker-maps \
  --limit 3
```

This prints the best solved sibling candidates for each missing map. It is the
most useful mode for deciding which maps are likely safe to copy/adapt without
playing another full table.

### 3. Verify score addresses from NVRAM/captures

```bash
python3 tools/rom-map-lab/map_lab.py scan-scores \
  --maps-root /Users/andremichi/workspace/scoretracker-maps \
  --rom mt_145h \
  --nvram "/Users/andremichi/tables/Mustang (Limited Edition) (Stern 2014)/pinmame/nvram/mt_145h.nv" \
  --scores 1411520,1944440,8896290,1888130
```

This searches for exact score values using common encodings:

- unsigned/signed 32-bit integer, little and big endian;
- unsigned 16-bit integer, little and big endian;
- simple decimal BCD byte sequences.

It is useful when a table has already been played once. It does not solve the
"never play the table" problem alone, but it lets us validate the output of a
future PinMAME exerciser.

## Future mode: headless PinMAME exercise

The experimental next layer has started as `pinmame_exerciser.cpp`.

Build it with:

```bash
./tools/rom-map-lab/build_exerciser.sh
```

Run a baseline boot/snapshot:

```bash
tools/rom-map-lab/bin/pinmame_exerciser \
  --rom mt_145h \
  --quiet-logs
```

Fuzz switches and capture NVRAM after each pulse:

```bash
tools/rom-map-lab/bin/pinmame_exerciser \
  --rom mt_145h \
  --quiet-logs \
  --fuzz-switches 1-128 \
  --out-dir /tmp/pinmame-exercise-mt_145h
```

Analyze an exerciser output directory:

```bash
python3 tools/rom-map-lab/map_lab.py analyze-exercise \
  --exercise-dir /tmp/pinmame-exercise-mt_145h
```

This compares `*_before.nv` / `*_after.nv` pairs, ranks switch pulses by changed
NVRAM bytes, and shows the first changed offsets. It does not know which switch
is “start” or “bumper” yet, but it immediately narrows the haystack.

Decode mapped `game_state` fields from every exerciser snapshot:

```bash
python3 tools/rom-map-lab/map_lab.py inspect-exercise \
  --maps-root /Users/andremichi/workspace/scoretracker-maps \
  --rom stargate \
  --exercise-dir /tmp/pinmame-exercise-stargate \
  --changed
```

This is the fastest way to confirm whether a headless run actually touched the
fields the plugin would read: scores, credits, player count, game-over, etc.

Manually pulse likely switches:

```bash
tools/rom-map-lab/bin/pinmame_exerciser \
  --rom mt_145h \
  --quiet-logs \
  --pulse-switch 1 \
  --pulse-switch 16
```

The exerciser emits JSON lines and `.nv` snapshots. This gives us the missing
primitive we wanted:

```text
boot ROM
snapshot NVRAM
pulse switch
snapshot NVRAM again
compare changed bytes
```

### Mining table scripts for switch recipes

Blind switch fuzzing is not enough for many games. A ROM often needs the same
physical-ball context the VPX table script sets up: trough switches, drain
switches, shooter-lane switches, coin-door state, and only then scoring switches.

If a loose `.vbs` exists, mine it directly:

```bash
python3 tools/rom-map-lab/map_lab.py vbs-switches \
  --script "/Users/andremichi/tables/Metallica (Premium Monsters) (Stern 2013)/Metallica Premium Monsters (Stern 2013) VPW 2.0.2.vbs"
```

If the script is embedded in a `.vpx`, extract a copy first. To avoid touching a
real table folder, copy the table to `/tmp` and run VPX's extractor there:

```bash
cp "/Users/andremichi/tables/Stargate (Gottlieb 1995)/Stargate (Gottlieb 1995) v2.0.vpx" /tmp/stargate_extract.vpx
"/Users/andremichi/vpinball/build/VPinballX_BGFX.app/Contents/MacOS/VPinballX_BGFX" -ExtractVBS /tmp/stargate_extract.vpx
python3 tools/rom-map-lab/map_lab.py vbs-switches --script /tmp/stargate_extract.vbs
```

In this environment the VPX extractor may need to run outside the filesystem
sandbox, but the copied `/tmp/*.vpx` input keeps the original table untouched.

The first clean scoring proof-of-life was Stargate using a temp root seeded with
the table's existing `pinmame/nvram`, `pinmame/cfg`, and `pinmame/memcard`
files. A completely blank PinMAME root booted and reacted to keys, but did not
accept credits/start into the mapped state correctly. That means automation
should prefer a known-good table PinMAME state when available.

The important physical-state detail was: keep the trough switch occupied, and
release only the drain/outhole switch after start. For Stargate, that means hold
switches `24` and `34`, then set `24=0` after start while leaving `34=1`:

```bash
tools/rom-map-lab/bin/pinmame_exerciser \
  --rom stargat5 \
  --quiet-logs \
  --coins 5 \
  --starts 1 \
  --hold-switch 24 \
  --hold-switch 34 \
  --post-start-set-switch 24=0 \
  --pulse-switch 10 \
  --pulse-switch 11 \
  --pulse-switch 12 \
  --pulse-switch 13 \
  --pulse-switch 22 \
  --pulse-switch 25 \
  --pulse-switch 27 \
  --pulse-switch 37
```

With the table-seeded PinMAME state, this decoded as a real active game:
credits moved, `game_over` became false, and the mapped score advanced through
values like `100000`, `200000`, `200030`, `1700060`, and `1760060`. If switch
`34` is also released, the ROM enters a less realistic state and score pulses no
longer update mapped score bytes. That means the automation path is viable, but
it needs a table-script-derived physical switch recipe, not only `5` for coin and
`1` for start.

### Isolated batch runs

Use `batch_exercise.py` to run a small set of ROMs against a fresh temporary
PinMAME root per ROM. It symlinks local ROM zips and creates isolated `nvram`,
`cfg`, `memcard`, and `ini` directories under the output path. When a loose VBS
is beside a table-local `pinmame` directory, that persistent state is copied
automatically; the real table files are never modified:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tools/rom-map-lab/batch_exercise.py \
  --out-dir /tmp/pinmame-batch-isolated
```

The default batch currently covers the 10-ROM smoke set used while developing
the helper:

- `stargat5`
- `mtl_170h`
- `bdk_294`
- `play203`
- `wof_500`
- `ss_15`
- `smbmush`
- `sfight2`
- `rescu911`
- `bbb108`

You can pass custom cases as `ROM=/path/to/script.vbs`:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tools/rom-map-lab/batch_exercise.py \
  --case 'bdk_294=/path/to/Batman.vbs'
```

For a VBS extracted into `/tmp`, explicitly associate its original PinMAME
state:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tools/rom-map-lab/batch_exercise.py \
  --case stargat5=/tmp/stargate_extract.vbs \
  --seed 'stargat5=/path/to/Stargate/pinmame'
```

Nearby firmware can reuse copied persistent state under the target ROM name:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tools/rom-map-lab/batch_exercise.py \
  --case 'bdk_300=/path/to/Batman.vbs' \
  --seed 'bdk_300=/path/to/Batman/pinmame' \
  --seed-rom bdk_300=bdk_294
```

This is isolated and never renames or modifies the source files.

The miner understands direct `Controller.Switch` assignments, physical balls
created on switches, consecutive physical troughs, and the common
`cvpmTrough.InitSwitches`/`InitSw` forms. For an unresolved integer-score map, a strong live score hit also creates a
review-only `candidate_game_state.json` with the inferred Player 1 address and
a four-player layout borrowed only from a solved structural donor with that same
Player 1 address. Player 2-4 remain explicitly marked as requiring multiplayer
review; layouts are not always contiguous.

Request automatic four-player verification with:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tools/rom-map-lab/batch_exercise.py \
  --case 'ROM=/path/to/table.vbs' \
  --verify-players
```

This emits ordered, timed wait/drain/launch/score actions. The lower-level
exerciser also accepts repeatable `--action` values (`wait:ms`,
`pulse:switch:pulse_ms:settle_ms`, and `set:switch=state:settle_ms`).

Two regression cases currently prove the full path:

- `stargat5`: automatic start and score growth from 0 to 7,263,060;
- `wof_500`: existing four-player state decoded, new game reset, then automatic
  score growth to 960,220 at `0x02110A2C`.

The first new maps produced by the lab are `bdk_294` and `bdk_240`. A live
automated 2.94 game grew Player 1 to 1,437,590 at `0x021109F0`; a four-player
run then drained to Player 2 and wrote 75,070 at `0x021109F8`. The older 2.40
firmware independently scored at the same Player 1 address. Both maps therefore
use the confirmed eight-byte `F0/F8/A00/A08` score layout.

Each ROM gets its own output directory with:

- `stdout.jsonl` and `stderr.txt` from `pinmame_exerciser`;
- every `.nv` snapshot;
- `decoded.json` for mapped `game_state` fields, when a map exists;
- `command.json` for repeatability;
- `result.json` for the per-ROM summary.

The top-level `summary.json` is the main artifact. The most important fields are:

- `status`: whether libpinmame completed;
- `score_state_changed`: whether decoded mapped scores actually changed during
  the run;
- `score_nonzero_seen`: whether any decoded score was non-zero at any point;
- `max_switch_changed_bytes`: largest NVRAM diff caused by one scripted switch
  pulse, useful for missing maps that cannot decode scores yet.
- `discovered_int_scores`: ranked live-score-like integer addresses;
- `proposed_game_state`: review-only integer score block when the evidence and a
  matching structural donor are strong enough.
- `player_evidence`: addresses independently observed changing per player;
- `candidate_report`: evidence status and generated artifact paths.

For an unresolved map, the batch writes a candidate map whenever Player 1 is
convincing. It writes `candidate.patch` only when a structural donor matches and
both Player 1 and Player 2 were independently observed. The patch never applies
itself to the real maps repository.

Re-run improved analysis without re-running the ROM:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tools/rom-map-lab/batch_exercise.py \
  --maps-root /path/to/scoretracker-maps \
  --analyze-existing ROM=/tmp/existing-capture-directory
```

This runner intentionally uses an isolated copy of table-local persistent state
when available. Without that seed, some ROMs boot but do not accept credits or
start correctly. Even with it, many games need a stronger recipe than “coin,
start, pulse targets”: trough state, shooter-lane/launch, and drain state often
matter.

### Find locally runnable missing maps

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tools/rom-map-lab/map_lab.py local-inventory \
  --maps-root /Users/andremichi/workspace/scoretracker-maps
```

This matches unresolved indexed ROMs against table-local NVRAM, loose VBS files,
and available ROM zips, then marks each case `ready`, `needs_vbs`, or
`needs_rom`. Add `--json` for machine-readable output.

Generate the ordered work queue with:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tools/rom-map-lab/map_lab.py prioritize-missing \
  --maps-root /Users/andremichi/workspace/scoretracker-maps
```

This sorts by donor-confidence tier first and expected ROM IDs resolved second,
while also showing layout consensus and local readiness. Prototype/FreeWPC-only
maps are excluded by default; pass `--include-experimental` to include them or
`--json` for structured output. Known failed recipes in `evidence.json` demote a
case automatically so attractive donor similarity cannot hide contrary runtime
evidence.

### Temporary donor-only candidate maps

Use `candidate-map` to generate an unverified map in `/tmp` by copying
`game_state` from a solved sibling:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tools/rom-map-lab/map_lab.py candidate-map \
  --rom smbmush \
  --donor-rom smb \
  --out-root /tmp/scoretracker-candidate-maps
```

The command writes only under `--out-root`; it does not modify the real maps
repository. It adds an `_candidate` block and an `AUTO-CANDIDATE` note so the
file cannot be confused with a verified map.

To test a candidate with normal map lookup, make a temporary full maps root and
replace only the candidate file:

```bash
rm -rf /tmp/scoretracker-candidate-root
cp -R /Users/andremichi/workspace/scoretracker-maps /tmp/scoretracker-candidate-root
cp /tmp/scoretracker-candidate-maps/maps/gottlieb/system3/smbmush.map.json \
  /tmp/scoretracker-candidate-root/maps/gottlieb/system3/smbmush.map.json

PYTHONDONTWRITEBYTECODE=1 python3 tools/rom-map-lab/map_lab.py inspect-exercise \
  --maps-root /tmp/scoretracker-candidate-root \
  --rom smbmush \
  --exercise-dir /tmp/pinmame-batch-isolated2/smbmush \
  --changed
```

The implemented exercise pipeline is:

```text
boot ROM in libpinmame
insert coins
press start for 1..4 players
fuzz candidate scoring switches
snapshot RAM/NVRAM after every switch event
drain/end the game
rank live score addresses
verify multiple player slots
write candidate JSON and a gated review patch
```

This is intentionally separated from the plugin because it will be experimental,
ROM/platform-specific, and may need to call low-level PinMAME APIs directly.

You can emit a starter exercise plan with:

```bash
python3 tools/rom-map-lab/map_lab.py exercise-plan \
  --maps-root /Users/andremichi/workspace/scoretracker-maps \
  --rom mt_145
```
