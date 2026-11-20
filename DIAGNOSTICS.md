# Diagnosing capture

The startup log must contain native-controller-capture-r1. The active module uses
Controller states from stock PinMAME. It must never request
ScoreCapture/GetCompletedGame:1 or report a PinMAME integration mismatch.

Capture readiness is notified once decoded states are readable. Completed records
require observed idle, ball-1 play and an explicit guarded game_over transition.
Closing VPX is not a completion event. See docs/CAPTURE-CONTRACT.md for the public
API's asynchronous-read and ROM-specific semantic limitations.

Completed score saved logs include the id and full history path. Table shutdown
logs received, saved and memory_pending counts. A failed write logs its cause;
inspect .scoretracker-pending/ and preserve it before repairing storage. Completed
records are retried at close/unload or ScoreTracker/RetryPending:1. Native getters
have no delay, poll interval or score-stability setting.

Experimental histories are backed up as scores.legacy-<id>.json on the first real
completed write, with the exact backup path logged. Other corrupt/unsupported
histories are preserved and produce an explicit storage error.
