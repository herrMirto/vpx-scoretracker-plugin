# ScoreTracker VPX Plugin

ScoreTracker is a local-first Visual Pinball X plugin that records completed game scores.

The plugin watches VPX/PinMAME and B2S score state, then appends one completed-game entry to `scores.json` in the table folder when a game finishes. It also keeps the existing optional WebSocket stream for companion apps.

## Goals

- Write every completed game to the table's local `scores.json` file.
- Keep the file human-readable and local-first.
- Avoid table script modifications.
- Use VPX plugin APIs, PinMAME APIs, and B2S script-object interception rather than memory hacks.
- Keep WebSocket output opt-in for live UIs and companion apps.

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

## Runtime overhead settings

ScoreTracker is optimized for completed-game persistence to `scores.json`.
Live WebSocket output is disabled by default and no local web server is started
unless `EnableWebSocket` is enabled.

`PollIntervalMs` controls how often score state is inspected. The default is
`250`; higher values reduce VPX process overhead at the cost of less frequent
live state updates.
