# Capture contract

The active ScoreTracker module uses only the public VPX and Controller APIs from
its VPX SDK. It does not request ScoreCapture/GetCompletedGame:1, apply a PinMAME
patch, rebuild PinMAME, link libpinmame directly, locate ROM files or load maps.

## Native PinMAME input

Controller/GetControllers:1 identifies endpoints and pinmame::ROM-or-alias ids.
Controller/GetStateSrc:1 supplies typed getters. Controller/OnStateSrcChanged:1
invalidates borrowed state definitions. All getter calls occur inside the official
CtrlItemConsumer.With scope on active VPX/OnPrepareFrame:1 events. No getters or
live-game VPX API calls occur after OnGameEnd or before OnGameStart.

Only unambiguous game_state paths are used. Explicit Player N labels determine
player order, not enumeration order. Required fields: game_over, player_count,
current_player, current_ball and contiguous scores for players 1..N. Integer
formats are read without passing through floating point. String and floating
formats are rejected. Values not written by INT64 getters remain an invalid
sentinel. Negative scores and implausible player/ball values are rejected.
Optional ball_count, extra_balls, ignore_scores and Games Started audit values
provide additional guards. A missing map/required field cannot manufacture a game.

## Lifecycle

1. Observe idle (game_over or ball=0/player=0) before accepting active play.
2. Arm only observed ball 1/player 1 with a valid participant count.
3. Track actual player/ball context. A changed Games Started audit invalidates
   the previous game. Falling ball or player counts discard it. Adding players
   is accepted only on ball 1. Closing the table and source loss discard it.
4. Accept an explicit game_over state after observed active play. If ball_count
   exists, the final configured ball must have been observed, played by the final
   participant. Earlier-ball game_over pulses do not finalize. Extra balls pending
   prevent completion. Wait for ignore_scores to clear through a state change,
   never through elapsed time; a new start discards the unfinished result.
5. Copy participant scores exactly. Never select maxima or infer a game from an
   increasing/stable display. A completed zero-point score is valid. Record the
   event timestamps and monotonic duration for information only.
6. Save the immutable completed record with its stable random id. Native VPX
   confirms only a durable write; failures retain the record for retry.

There are no sleeps, delays, confirmation poll counts, score-stability windows,
minimum game durations, exit-score fallback, automatic B2S fallback or signing.

## Limits of the existing public API

GetState reads are asynchronous. Reading lifecycle controls before and after the
score reads detects crossing many lifecycle transitions; it cannot provide an
atomic emulator snapshot, detect an ABA transition or validate a decoder that
silently substitutes a default. The ROM/map must give game_over its intended
semantic meaning and expose scores after its last bonus. Final-ball/player guards
reduce false endings but do not prove all ROM-specific ordering. A first-ball
restart without an exposed audit or other distinguishing transition can remain
ambiguous. No generic map declaration or synthetic test certifies those semantics.

These are material limits, not guarantees this implementation claims to solve.
Real-ROM completion validation (bonus, multiplayer, tilt, extra ball, restart and
consecutive games) remains required. The committed AFM trace proves only that
startup/self-test and an old display score do not generate a completed record.

## JSON and storage

schema/scores.schema.json defines schema_version=1. ROM is an identifier, never a
filesystem path. The source field identifies the producer. No descriptive
implementation metadata is written per game. Preserve exact signed 64-bit
scores and zeros; readers in JavaScript need an integer-preserving parser for
large values. Duration is metadata, not a completion criterion.

Writes use a process lock, durable journal, fsynced atomic replacement and
idempotent record ids. Native failed results remain in memory for retry and are
journaled before history validation whenever possible. ScoreTracker/RetryPending:1,
table close and plugin unload retry completed results only. No pending journals
means no recovery attempt or misleading pending-game error.

The recognizable legacy root {"games":[...]} is archived byte-for-byte under a
unique scores.legacy-*.json filename during the first actual completed write,
then a fresh v1 history is created. No old game is migrated or retroactively
certified. Other incompatible or corrupt files remain untouched. Notifications
use live VPX UI; after table shutdown only logs are possible.

Scripted tables may use ScoreTracker.Recorder: explicit BeginGame, every final
player score, then CompleteGame. Incomplete script games expire at table end;
failed records retain their id and values for retry.
