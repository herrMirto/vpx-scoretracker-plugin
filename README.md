# VPX ScoreTracker Plugin

A third-party [Visual Pinball](https://github.com/vpinball/vpinball) plugin that records the
scores of played PinMAME games to a `scores.json` file. It is developed and distributed
independently of the vpinball project. The installer places the plugin in the correct VPX location;
no vpinball source changes are required.

While a game runs, the plugin periodically decodes the machine's NVRAM (and, on platforms that
keep live game state in volatile memory, main CPU RAM) using a pinned snapshot of the
[PinMAME NVRAM maps](https://github.com/herrMirto/pinmame-nvram-maps). When the game is over(after the 'Match' screen), the
per-player scores, game duration and selected `game_state` values are appended to `scores.json`.

The plugin has no effect for tables without a PinMAME controller or for ROMs without a
`game_state` block in their map(mostly EM tables and Original ones)

## How it works

- The plugin registers through VPX's public plugin API (`MsgPlugin`/`ControllerPlugin`/`VPXPlugin`
  headers) and reads machine memory through the libpinmame API (`PinmameGetNVRAM`,
  `PinmameReadMainCPUByte` — both read-only).
- On `OnGameStart` (controller event), the ROM id is looked up in the maps `index.json`; if a map
  exists, it is parsed once together with its platform description.
- The confirmed game is appended to `scores.json` with an atomic write (temporary file + rename).
  An unreadable existing file is moved aside (`scores.json.broken.<timestamp>`), never deleted.

## Settings (`[Plugin.ScoreTracker]` in VPinballX.ini)

| Setting | Default | Description |
|---|---|---|
| `Enable` | `1` | Enable the plugin. |

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

`scores` holds all scores seen per player during the session. 

## Maps

Releases include a pinned snapshot of
[herrMirto/pinmame-nvram-maps](https://github.com/herrMirto/pinmame-nvram-maps) in the plugin-default
maps folder (`plugins/scoretracker/maps`), so users do not download or select maps separately. The
exact source commit is recorded in `maps/source.json`. The `nvram_maps_folder` setting remains an
advanced development override. The maps are LGPL-3.0 and their license is installed alongside them.

## Companion app: VPX Scoretracker Viewer

`companion/` contains VPX Scoretracker Viewer, the cross-platform Tauri 2 desktop application for
browsing the local game history produced by this plugin. It is embedded in each platform's
self-contained installer, but is operationally independent: score recording never requires the app
to be installed or running.

See [`companion/README.md`](companion/README.md) for development instructions and feature details.

## Installation

Download the latest installer from [GitHub Releases](https://github.com/herrMirto/vpx-scoretracker-plugin/releases/latest).

Platform binaries:

- **macOS (Apple Silicon):** `scoretracker-installer-macos-arm64.dmg`
- **Windows (x64):** `scoretracker-installer-windows-x64.exe`
- **Linux (x64):** `scoretracker-installer-linux-x64`
- **Linux (ARM64):** `scoretracker-installer-linux-arm64`

Installed Viewer locations:

- **macOS:** `~/Applications/VPX Scoretracker Viewer.app`
- **Windows:** `%LOCALAPPDATA%\Programs\VPX Scoretracker Viewer\VPX Scoretracker Viewer.exe`
  with a Start Menu shortcut
- **Linux:** `~/.local/bin/vpx-scoretracker-viewer` with a desktop entry

1. On macOS, open the DMG and launch `ScoreTracker Installer.app` from it. On Linux, first run
   `chmod +x scoretracker-installer-linux-*`.
2. Run the installer and confirm the detected VPX installation and tables folder. It installs and
   enables the plugin, bundled maps, and Viewer.
3. Start VPX and play a supported PinMAME table. Its completed game is added to `scores.json`.

Manual installation of a locally built `scoretracker` folder also works.

## Updates

You can update the VPX Score Tracker plugin directly from the Viewer app, using the 'Check for Updates' button. Click on **Update and restart** to update the Viewer, plugin, and bundled maps together.

## Uninstall

The uninstall at the moment is manual, so you need to delete the scoretracker folder from the plugins folder, the binary of the Viewer and disable the plugin in VPinballX.ini.

## License

ScoreTracker is available under the [MIT License](LICENSE). The bundled NVRAM maps retain their
LGPL-3.0 license.
