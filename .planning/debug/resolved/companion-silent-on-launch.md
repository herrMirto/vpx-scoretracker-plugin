---
status: resolved
trigger: "Companion application opens but shows no useful activity and logs say nothing after VPinPlay artwork integration; possible excessive data work."
created: 2026-07-13
updated: 2026-07-13
---

## Symptoms

- Expected: the companion returns local score history promptly and enriches artwork afterward.
- Actual: the application appears silent after opening, with no useful logs.
- Errors: none reported.
- Timeline: began after adding VPinPlay/VPinMediaDB wheel enrichment.
- Reproduction: launch the companion against an existing VPX collection.

## Current Focus

- hypothesis: confirmed — synchronous SHA-256 hashing of large VPX files blocked the initial score scan
- test: scanner regression test asserts initial records contain filename metadata but no eager hash
- expecting: local scores return before any full VPX read or remote media request
- next_action: resolved
- reasoning_checkpoint: direct inspection showed read_source calling sha256_file before constructing ScanSnapshot
- tdd_checkpoint: hashes_vpx_only_when_explicitly_requested covers lazy hashing

## Evidence

- 2026-07-13: `read_source` called `sha256_file` for every unambiguously located VPX during `scan`.
- 2026-07-13: `scan_scores` was a synchronous Tauri command, keeping the work on the command path.
- 2026-07-13: artwork lookup already tolerated API failures, eliminating the network as the initial-history blocker.

## Eliminated

## Resolution

- root_cause: Eager full-file SHA-256 hashing was added to the initial synchronous score scan.
- fix: Initial scans collect only VPX filename metadata and run through `spawn_blocking`; exact hashing is an explicit blocking-worker command used only after name and ROM media lookup fail.
- verification: TypeScript check and production build pass; Rust formatting and cargo check pass; all 6 Rust tests pass, including the lazy-hash regression.
- files_changed: companion/src-tauri/src/scores.rs, companion/src-tauri/src/lib.rs, companion/src/main.ts, companion/README.md, companion/ARCHITECTURE.md
