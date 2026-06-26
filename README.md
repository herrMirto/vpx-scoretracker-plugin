# ScoreTracker VPX Plugin

ScoreTracker is a local-first Visual Pinball X plugin that records completed game scores.

The plugin watches VPX/PinMAME and B2S score state, then appends one completed-game entry to `scores.json` in the table folder when a game finishes. It also keeps the existing optional WebSocket stream for companion apps.

## Goals

- Write every completed game to the table's local `scores.json` file.
- Keep the file human-readable and local-first.
- Avoid table script modifications.
- Use VPX plugin APIs, PinMAME APIs, and B2S script-object interception rather than memory hacks.
- Keep WebSocket output optional for live UIs and companion apps.

## scores.json

Current v1 shape:

```json
{
  "version": 1,
  "games": [
    {
      "date": "2026-06-19T18:30:12Z",
      "rom": "afm_113b",
      "scores": [123456789, 9876543],
      "game_duration": 310
    }
  ]
}
```

`game_state` is included only when useful non-empty state is available.

Official release builds are intended to add a per-game `_signature` generated from signing material injected by the GitHub release workflow.

## Map Data

PinMAME decoding depends on an external maps folder containing:

- `index.json`
- `romnames.json`
- `platforms/`
- `maps/`

The maps database should live in a separate repository and release cycle.

## Tracking Source Priority

ScoreTracker uses exactly one score source for a table:

1. A valid PinMAME NVRAM map is always authoritative.
2. B2S interception is armed only when the ROM is not listed in `index.json`.

While a map is active, B2S calls are forwarded normally to the backglass but
are not inspected or retained by ScoreTracker. Duplicate, empty, and
lower-priority controller lifecycle events cannot replace a mapped session.

If the map index is unreadable, malformed, or references a missing map file,
ScoreTracker fails closed instead of silently selecting B2S. Source selection,
fallback activation, ignored lifecycle events, and configuration failures are
reported explicitly in the VPX log.

## Live NVRAM diagnostic snapshots

Map authors can capture the complete live PinMAME NVRAM at known gameplay
moments. Enable `EnableNVRAMSnapshots` in the ScoreTracker plugin settings,
restart the table, and request labeled snapshots from the same machine:

```sh
curl "http://127.0.0.1:8889/snapshot?label=attract"
curl "http://127.0.0.1:8889/snapshot?label=ball_1"
curl "http://127.0.0.1:8889/snapshot?label=game_over"
```

Replace `8889` if the plugin's `Port` setting differs. Captures are written to
`scoretracker-captures/<rom>/` beside the active table. Labels are sanitized and
filenames include millisecond timestamps, so the endpoint cannot choose an
arbitrary output path. The endpoint returns HTTP 403 unless explicitly enabled
and HTTP 503 until PinMAME has supplied a live NVRAM frame. Each successful
capture is recorded in the VPX log with its full path and byte count. Platforms
that declare CPU RAM regions also produce a matching `-ram-ADDRESS.bin` file;
this allows map authors to correlate volatile scores and lifecycle fields that
are not persisted in NVRAM.
