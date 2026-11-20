' Integrate these calls into the table's actual game logic. Do not call
' CompleteGame from Table_Exit, a timer, B2SSetGameOver alone, or initialization.
Dim ScoreRecorder
Set ScoreRecorder = CreateObject("ScoreTracker.Recorder")

' In the actual new-game handler, after the game has accepted the start:
' If Not ScoreRecorder.BeginGame(1) Then ... report failure ...

' When another participant is explicitly accepted during the first ball:
' If Not ScoreRecorder.SetPlayerCount(2) Then ... report failure ...

' In the actual final-game handler AFTER the last bonus and every final score:
' Supply every participant, including a player who scored zero. Strings avoid
' passing a rounded floating-point value through the scripting API.
' If Not ScoreRecorder.SetPlayerScore(1, CStr(Player1Score)) Then ...
' If Not ScoreRecorder.SetPlayerScore(2, CStr(Player2Score)) Then ...
' If Not ScoreRecorder.CompleteGame() Then
'    ' The completed record is frozen. Retry CompleteGame on an explicit retry
'    ' action; do not change scores or call BeginGame to retry this record.
' End If

' On a deliberate restart/cancel, discard the incomplete game:
' ScoreRecorder.AbortGame
