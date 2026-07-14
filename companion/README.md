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

The companion globally configures both the VPX tables directory and the shared NVRAM maps directory,
then provides a read-only Rust scanner for ScoreTracker schema-v1 history, full-score progress
charts, map-based NVRAM high scores and settings, and optional table artwork. The companion resolves
table identity through the public VPinPlay API and loads matching wheel images from VPinMediaDB;
successful matches are cached for 30 days and missing network access never blocks local scores. VPX
hashing is deferred until after history is visible and is used only when lighter name and ROM matching
cannot identify the table. Supported
NVRAM edits require confirmation, update declared checksums, and create a timestamped backup beside
the original `.nv` file before saving.
