# ScoreTracker Companion

Cross-platform Tauri 2 companion application for local game history produced by the ScoreTracker
VPX plugin. See [ARCHITECTURE.md](ARCHITECTURE.md) and [REQUIREMENTS.md](REQUIREMENTS.md).

## Development

Install the platform-specific [Tauri prerequisites](https://v2.tauri.app/start/prerequisites/), then:

```sh
npm install
npm run tauri dev
```

Useful non-GUI checks:

```sh
npm run check
npm run build
cargo test --manifest-path src-tauri/Cargo.toml
cargo check --manifest-path src-tauri/Cargo.toml
```

The companion asks only for the VPX tables directory. Releases include a pinned NVRAM maps bundle,
and the plugin installer configures it automatically. The app then provides a Rust scanner
for ScoreTracker schema history (v1 and v2), full-score progress
charts, read-only map-based NVRAM high scores, and optional table artwork. Each score row has a
Remove action that permanently deletes that game from its `scores.json` — useful for pruning wrong
readings that some ROMs produce. Games are matched by the stable `score_id` field written in schema
v2 (legacy v1 records are removed by position). The companion resolves
table identity through the public VPinPlay API and loads matching wheel images from VPinMediaDB;
successful matches are cached for 30 days and missing network access never blocks local scores. VPX
hashing is deferred until after history is visible and is used only when lighter name and ROM matching
cannot identify the table. The companion only ever deletes games you remove yourself and never
modifies `.nv` files.
