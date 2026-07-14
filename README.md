# ScoreTracker Plugin

A third-party [Visual Pinball X](https://github.com/vpinball/vpinball) plugin that records the
scores of played PinMAME games to a `scores.json` file. It is developed and distributed
independently of the vpinball project: installation is dropping the `scoretracker` folder into
VPX's `plugins` directory — no vpinball source change of any kind is required.

While a game runs, the plugin periodically decodes the machine's NVRAM (and, on platforms that
keep live game state in volatile memory, main CPU RAM) using the community-maintained
[PinMAME NVRAM maps](https://github.com/tomlogic/pinmame-nvram-maps). When the game is over, the
per-player scores, game duration and selected `game_state` values are appended to `scores.json`.

The plugin has no effect for tables without a PinMAME controller or for ROMs without a
`game_state` block in their map.

## How it works

- The plugin registers through VPX's public plugin API (`MsgPlugin`/`ControllerPlugin`/`VPXPlugin`
  headers) and reads machine memory through the stable libpinmame API (`PinmameGetNVRAM`,
  `PinmameReadMainCPUByte` — both read-only).
- On `OnGameStart` (controller event), the ROM id is looked up in the maps `index.json`; if a map
  exists, it is parsed once together with its platform description.
- The machine state is polled on the main thread via `RunOnMainThread` at the configured interval
  (250 ms by default) — no extra threads. When the NVRAM snapshot has not changed, no decoding
  happens; nothing at all runs while no machine is active.
- The map's `game_state.game_over` flag drives the session lifecycle. Since this flag can also be
  raised between balls (end-of-ball bonus, ball search), a game-over is only confirmed after the
  flag stays asserted for 25 s; games shorter than 30 s are considered table resumes and ignored.
- The confirmed game is appended to `scores.json` with an atomic write (temporary file + rename).
  An unreadable existing file is moved aside (`scores.json.broken.<timestamp>`), never deleted.

## Settings (`[Plugin.ScoreTracker]` in VPinballX.ini)

| Setting | Default | Description |
|---|---|---|
| `Enable` | `0` | Enable the plugin. |
| `nvram_maps_folder` | *(empty)* | Folder with the NVRAM maps (`index.json`, `maps/`, `platforms/`). When empty, the maps installed with the plugin (`plugins/scoretracker/maps`) are used. |
| `PollIntervalMs` | `250` | Interval used to inspect the machine state (50–5000 ms). |
| `OutputFolder` | *(empty)* | Folder where `scores.json` is written. When empty, it is written next to the table file. |

## scores.json format

```json
{
  "version": 1,
  "games": [
    {
      "date": "2026-07-03T18:30:12Z",
      "rom": "taf_l7",
      "scores": [12345678, 9876543],
      "game_duration": 310,
      "game_state": { "credits": 2, "player_count": 2 }
    }
  ]
}
```

`scores` holds the best value seen per player during the session (the instantaneous score at
game-over can lag the final bonus). On platforms providing `game_state.final_scores`, that
ROM-frozen snapshot is used instead for the players it covers.

## Building

The plugin builds against a vpinball checkout used purely as an SDK (headers +
`third-party/runtime-libs` libpinmame); the checkout is never modified.

- **Standalone (macOS)**: `./build-standalone.sh [vpinball-checkout] [nvram-maps-checkout]`
  compiles the plugin and installs it (plus, optionally, the maps) straight into the
  `VPinballX*.app` bundle found under the checkout's `build/` folder.
- **In-tree (all platforms)**: see the header of `CMakeLists_plugin_ScoreTracker.txt` for
  building as part of a vpinball CMake build without changing any vpinball source file.

## Maps

Any checkout of [tomlogic/pinmame-nvram-maps](https://github.com/tomlogic/pinmame-nvram-maps) or
a compatible fork works: point `nvram_maps_folder` at it, or install it as the plugin-default
maps folder (`plugins/scoretracker/maps`). The in-tree CMake build downloads a pinned revision
automatically; the maps are LGPL-3.0 (their LICENSE file is installed alongside).

## Development tools

`tools/rom-map-lab/` contains the reverse-engineering lab used to discover and validate
`game_state` fields (headless PinMAME exerciser, NVRAM diffing, candidate-map generation). It is
development tooling only and is not part of the plugin runtime.

## Companion app

`companion/` contains the cross-platform Tauri 2 desktop application for browsing the local game
history produced by this plugin. The companion is versioned and released alongside the plugin but
is operationally independent: score recording never requires the app to be installed or running.

See [`companion/README.md`](companion/README.md) for development instructions and the v1 design.
