# ScoreTracker Desktop Viewer

The desktop Viewer is an Electron application. Both the backend and interface
are TypeScript. It reads the plugin's `schema_version: 1` histories and displays
local games, player scores and read-only machine high scores from NVRAM.
Choose the tables folder and, for NVRAM discovery, the existing VPX location.
The Viewer does not install plugins, move VPX, sign games or upload scores.
Updates are manual ZIP replacements.

## Development

Use Node.js 24 or later:

```sh
npm --prefix companion ci
npm --prefix companion run build
npm --prefix companion start
```

`npm --prefix companion run dev` opens the desktop window with Vite hot reload
and isolated temporary settings. Closing the window stops the backend.
`bash viewer/start.sh` opens the built application through Electron.

## Packaging

```sh
bash viewer/build.sh
python3 scripts/package-viewer.py --platform macos-arm64
```

The build stays in `build/viewer/`. macOS bundles are ad-hoc signed. Electron Builder produces
`mac-arm64/ScoreTracker.app` on Apple Silicon, `mac/ScoreTracker.app` on Intel,
`linux-unpacked/` on Linux x64, `linux-arm64-unpacked/` on Linux arm64 and
`win-unpacked/` on Windows x64. The portable ZIP includes the entire runtime;
users need neither Node.js nor other development tools. The plugin is packaged
separately; see the [plugin instructions](../INSTALL.txt).

CI produces packages for macOS arm64, Linux x64 and Windows x64. Windows
packages are currently unsigned.

## Structure and data

- `../companion/desktop/`: Electron window, native dialogs and TypeScript API,
  history scanning, stable-id deletion, host discovery and NVRAM decoding.
- `../companion/src/`: TypeScript/Vite interface, embedded in one HTML file.

History reads preserve player order and zero scores. Integers remain exact when
rewriting JSON histories; JavaScript charts use numbers and can round values
above Number.MAX_SAFE_INTEGER. Deleting a game requires confirmation and its
stable id. The same OS history lock used by the plugin prevents simultaneous
writes from being overwritten. The scanner reports bad histories separately,
skips table resources and symlinks, and supports cancellation.

NVRAM and maps are read-only. Maps are resolved from per-table PinMAME data, the
configured PinMAME data folder or the VPX PinMAME plugin. Nothing is bundled or
written into those locations. Settings and logs retain the platform's local-data
`ScoreTracker` directory, including settings saved by earlier versions.
The API binds to loopback with a random session token and Host/Origin checks.
The renderer is sandboxed with context isolation and no Node.js integration.
The OS history lock is accessed through Koffi's prebuilt native bindings.

## Checks

```sh
npm --prefix companion run check
npm --prefix companion test
npm --prefix companion run test:desktop
python3 scripts/validate-project.py
```

The desktop smoke test is for local execution only. It opens the packaged app
using disposable histories and settings, saves diagnostics in
`build/desktop-smoke/`, and closes the application. Build and package the Viewer
first, or set `SCORETRACKER_APP` to a packaged executable path. CI runs unit/API
tests and packaging checks without opening a window.

For a read-only Node.js service after building:

```sh
node companion/desktop-dist/desktop/headless.js --service --listen 127.0.0.1:8080 --tables-root /path/to/Tables
```

`--maps-root PATH` and `--vpx-root PATH` set host-owned map locations for the
service. All mutation endpoints are disabled in service mode. For integration
tests, the same entry point accepts `--no-open --data-dir PATH` with isolated
settings and a session URL. Normal desktop operation uses Electron.
