# VPX ScoreTracker

A standalone VPX plugin that writes completed games to readable `scores.json`,
plus a small desktop Viewer for those files. The plugin runs independently of
the Viewer. No PinMAME patch, separate PinMAME build, ROM path, signing key or
upload service is required.

## Plugin

Extract the plugin ZIP into the existing VPX plugins directory and enable it:

```ini
[Plugin.ScoreTracker]
Enable = 1
Notifications = 1
```

The [JSON schema](schema/scores.schema.json) and
[example history](examples/completed-game.json) define the data contract:
`schema_version`, stable game `id`, table filename, optional ROM identifier,
start/end timestamps, informational duration, source and ordered player scores.
Zero points are valid. Scores use signed 64-bit integers. Readers in JavaScript
need integer-preserving handling above Number.MAX_SAFE_INTEGER.

## Viewer

See [Viewer build and use](viewer/README.md). The Viewer reads the same schema,
displays local history and reads optional machine high scores from host-owned
NVRAM/maps. It does not install the plugin or upload scores.

## Repository

- `src/`: plugin capture, script integration and durable JSON writer.
- `schema/`, `examples/`, `docs/`: current format and capture contract.
- `viewer/`, `companion/`: Electron desktop Viewer, TypeScript interface and read-only decoders.
- `platforms/`: public VPX SDK staging for supported build targets.
- `scripts/`, `tests/`, `.github/`: validation, packaging, tests and CI.
- `pages/`: GitHub Pages downloads, installation instructions and Viewer screenshots.
- `third-party/`: SDK staging instructions; generated headers are ignored.

Generated output belongs in `build/`, `dist/` and `companion/desktop-dist/`. No profiling tools,
ROMs, PinMAME patches, collector sites or signing experiments are part of this tree.

## Build and verify

```sh
bash build-standalone.sh --no-install /path/to/vpinball
ctest --test-dir build --output-on-failure
python3 tests/run-capture.py --sanitize
python3 scripts/validate-project.py
bash viewer/build.sh
npm --prefix companion test
npm --prefix companion run test:desktop
```

The plugin build uses the supplied VPX checkout's SDK and never installs anything.
`third-party/include/revisions.txt` records that checkout's commit. Build the Viewer
separately; neither build changes the VPX installation. Packaging uses
`scripts/package-plugin.py` and `scripts/package-viewer.py`.

`validate-project.py` is an optional source consistency check, not a build tool.
CI runs it once on Linux, alongside a separate Bash syntax check. Each platform
then compiles the plugin, runs capture/storage and Viewer tests, and packages its
own binaries. Windows uses MSVC x64 and produces unsigned portable ZIPs.
