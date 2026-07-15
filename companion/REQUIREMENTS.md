# ScoreTracker Companion v1 Requirements

## Product intent

ScoreTracker Companion gives one cabinet owner a clear history of their VPX play: what they played,
how often, their best results, whether they are improving, and how close they are to entering each
machine's persisted high-score board.

It is desktop-first and usable on ordinary desktop displays. Its responsive layout must remain
suitable for a future cabinet mode without requiring a separate frontend.

## Functional requirements

### Setup and configuration

- **COMP-001:** On first launch, the app asks the user to select a VPX tables directory with a
  native directory picker.
- **COMP-002:** The selected path is persisted in the companion's platform application data and can
  be changed later.
- **COMP-003:** No default path is assumed to be correct on Windows, macOS, or Linux.
- **COMP-004:** An unavailable or moved directory produces a recoverable setup state.
- **COMP-005:** The global NVRAM maps directory is configured beside the tables directory and is
  reused for every table.

### Game discovery

- **COMP-010:** The app recursively discovers `scores.json` files beneath the configured root.
- **COMP-011:** It reads ScoreTracker schema version 1 and ignores unknown fields.
- **COMP-012:** One malformed or unsupported file does not prevent valid files from loading.
- **COMP-013:** Each warning identifies its source without exposing an unnecessary stack trace.
- **COMP-014:** Scanning is read-only and does not follow directory symlinks.
- **COMP-015:** A manual refresh is always available. Automatic refresh after source changes may be
  added within v1 after the basic scanner is reliable.

### History and overview

- **COMP-020:** The overview shows total completed games, played tables, and total recorded play
  time.
- **COMP-021:** Recent activity lists date, table/ROM, score, and game duration.
- **COMP-022:** The user can search and sort tables by name, recently played, play count, and
  personal best.
- **COMP-023:** Multiplayer records may be displayed, but v1 treats the installation as belonging
  to one person and provides no player-profile assignment workflow.
- **COMP-024:** The overview resolves optional table metadata and wheel artwork through VPinPlay and
  VPinMediaDB, caches successful matches, and retains a complete text fallback when offline.

### Table detail and progress

- **COMP-030:** A table detail view shows chronological scores, personal best, play count, and total
  time played.
- **COMP-031:** It shows score progression in a form that remains useful with only a few games.
- **COMP-032:** When supported NVRAM leaderboard data is available, the app shows the score required
  to enter the board and progress toward that target.
- **COMP-033:** Progress calculations clearly distinguish an achieved target from an estimate below
  the target.
- **COMP-034:** Unsupported or missing NVRAM maps do not hide ordinary game history.
- **COMP-035:** The table detail view shows the machine's persisted high-score entries decoded from
  the matching `.nv` file rather than treating personal history as the machine leaderboard.
- **COMP-036:** NVRAM access is read-only and limited to mapped machine high scores.
- **COMP-037:** The companion exposes no command or interface that modifies `.nv` files.
- **COMP-038:** NVRAM decoding failures do not prevent personal score history from loading.
- **COMP-039:** The table detail header uses the same resolved wheel and canonical table identity as
  the overview without allowing remote media failure to hide score history.

### Refresh and resilience

- **COMP-040:** Repeated scans produce deterministic results and do not duplicate games.
- **COMP-041:** The UI stays responsive while scanning a large tables tree.
- **COMP-042:** Empty history has an actionable first-run state rather than an error screen.
- **COMP-043:** Dates and numbers are presented using the user's locale while stored values remain
  unchanged.
- **COMP-044:** Scores are always displayed in full with locale separators; compact abbreviations
  such as `1B` or `5M` are never used.

## Platform and distribution requirements

- **COMP-050:** Windows, macOS, and Linux are supported release targets.
- **COMP-051:** Release artifacts include all application code; users install no language runtime.
- **COMP-052:** Filesystem behavior is tested with Windows separators, Unix separators, Unicode,
  spaces, and case-sensitive filesystems.
- **COMP-053:** The plugin and companion can be downloaded together but installed, launched, and
  updated independently.
- **COMP-054:** CI runs frontend checks and Rust tests on all three operating systems.
- **COMP-055:** Release configuration supports Windows signing and macOS signing/notarization.

## Quality requirements

- **COMP-060:** The webview has no unrestricted filesystem or shell permission.
- **COMP-061:** No network service listens in v1.
- **COMP-062:** Rust parsing and aggregation logic has unit tests independent of the UI.
- **COMP-063:** The layout remains usable at desktop and cabinet-oriented resolutions.
- **COMP-064:** Accessibility basics include keyboard navigation, visible focus, semantic controls,
  sufficient contrast, and reduced-motion support.
- **COMP-065:** NVRAM lookup remains restricted to files inside the configured tables root.

## Deferred features

- Live in-game updates.
- Multiple users, profiles, or authentication.
- Cloud backup, global leaderboards, challenges, and third-party score submission.
- LAN, phone, or remote-browser access.
- Automatic updater activation; the architecture must allow signed updates later.
- Editing, deleting, or annotating source game records.
- Editing NVRAM files.

## Initial scaffold acceptance

The scaffold is complete when it can be developed on the current platform and contains:

1. A Tauri 2 Rust application and Vite/TypeScript frontend.
2. Native tables-directory selection.
3. A read-only Rust command that scans schema-v1 `scores.json` files.
4. A responsive page showing scan totals and recent games.
5. Focused Rust tests for valid and malformed source files.
