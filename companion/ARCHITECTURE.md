# ScoreTracker Companion Architecture

## Decision

ScoreTracker Companion is a Tauri 2 desktop application shipped from the same repository and
release as the ScoreTracker VPX plugin. It is a separate executable: the plugin must record games
without the companion running, and the companion must never load into or control VPX.

The application targets Windows, macOS, and Linux as first-class platforms. It uses a responsive
web frontend inside the platform webview and a Rust backend for local file discovery, parsing, and
derived statistics. End users do not need Python, Node.js, Rust, or a browser extension installed.

## Boundaries

```text
VPX + ScoreTracker plugin
        |
        | atomic, append-only scores.json updates
        v
Configured tables directory
        |
        | read history; explicitly confirmed NVRAM edits
        v
Rust scanner and decoders
        |
        | typed Tauri commands
        v
Responsive desktop frontend
```

### Plugin responsibilities

- Detect completed PinMAME games.
- Record the ROM, timestamp, player scores, duration, and mapped game state.
- Own the versioned `scores.json` interchange schema.
- Operate correctly when the companion is absent.

### Companion responsibilities

- Let the user select one or more VPX tables roots without assuming platform-specific paths.
- Discover and parse supported `scores.json` files without modifying them.
- Decode supported NVRAM leaderboard data through a dedicated Rust module.
- Let the user inspect and edit map-defined NVRAM values on supported platforms.
- Derive history, personal bests, trends, play counts, play time, and target progress.
- Resolve optional table metadata through VPinPlay and display wheel artwork from VPinMediaDB.
- Present useful partial results when individual files or ROM maps are invalid or unsupported.

### Explicit non-responsibilities for v1

- Live score display or communication with a running VPX process.
- Writing, repairing, or migrating plugin history automatically.
- Editing NVRAM while VPX is running, or writing platforms whose mirrored storage rules are not yet
  implemented safely.
- Accounts, cloud synchronization, social leaderboards, or score submission.
- Multiple local player profiles or assigning multiplayer slots to people.
- A LAN HTTP server. The frontend remains responsive so cabinet/browser mode can be added later.

## Data model

Plugin files remain the source of truth. The first release should derive views in memory on scan;
it does not need SQLite. A cache may be introduced later only as a disposable performance aid.

All interchange structs are version-aware and tolerate unknown fields. A malformed file produces a
source-scoped warning and does not prevent other tables from loading. Paths remain native internally
and are converted to display strings only at the UI boundary.

Artwork enrichment is disposable presentation data rather than a source of truth. Table name and
ROM searches run after local history is visible. Full VPX hashing is deliberately excluded from the
initial scan and runs in a blocking worker only as a last-resort exact match. Matches are cached
locally for 30 days. API failures and missing wheel files fall back to a typographic table identity
without affecting history or NVRAM features.

NVRAM parsing is isolated from score history parsing. It consumes the same pinned map data released
with ScoreTracker, but map discovery must not depend on the plugin being installed in a particular
directory. Release builds may embed or install one canonical map bundle for both products.

The selected NVRAM file remains the source of truth for the machine leaderboard and mapped machine
settings. A save updates only fields represented by the active ROM map, recalculates declared
checksums, and creates a timestamped sibling backup before replacing any bytes.

## Security model

- Rust performs filesystem access; the webview does not receive unrestricted filesystem APIs.
- The user explicitly chooses the tables root using a native directory picker.
- Scan commands canonicalize the selected root, do not follow directory symlinks, and are read-only.
- NVRAM writes require an explicit confirmation, are restricted to a file inside the selected
  tables root, and always create a backup first.
- Platforms with mirrored or platform-specific write semantics remain read-only until those rules
  have focused test coverage.
- No network listener is started in v1.
- Outbound access is restricted to the VPinPlay API and static wheel images hosted by VPinMediaDB
  on GitHub. No remote content is executed in the webview.
- Tauri capabilities remain minimal and Content Security Policy allows only those named origins.

## Cross-platform distribution

CI builds and tests on Windows, macOS, and Linux. Release artifacts should include:

- Windows: NSIS installer initially; MSI may be added when deployment demand justifies it.
- macOS: universal or separate Apple Silicon/Intel `.app` bundles distributed in a DMG.
- Linux: AppImage plus a Debian package initially; RPM can follow demand.

Signing and notarization are release-pipeline concerns, never runtime requirements. Unsigned local
development builds remain possible. The companion and plugin share a release version but keep
independent build targets and installation locations.

## Project layout

```text
companion/
  src/                    TypeScript frontend
  src-tauri/src/          Rust backend and Tauri commands
  src-tauri/capabilities/ Tauri permission declarations
  ARCHITECTURE.md
  REQUIREMENTS.md
```

The existing Python application under the separate `vpinleaders` checkout is a UX and behavior
reference. It is not a runtime dependency and should not be copied wholesale into the Tauri app.
