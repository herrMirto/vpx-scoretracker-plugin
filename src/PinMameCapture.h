// license:GPLv3+
#pragma once
#include "GameRecord.h"
#include "plugins/ControllerPlugin.h"
#include <functional>
#include <map>
#include <optional>

namespace ScoreTracker {
// Values copied during one scoped Controller source access. These reads are
// asynchronous; consistency guards do not turn them into an emulator snapshot.
struct NativeGameState {
   bool readable = false, changedDuringRead = false, gameOver = false, ignoreScores = false;
   int64_t players = 0, player = 0, ball = 0, ballsPerGame = 0, extraBalls = 0;
   std::optional<int64_t> gamesStarted;
   std::vector<int64_t> scores;
};
NativeGameState ReadNativeGameState(const std::vector<StateSrcId>& sources, uint32_t endpoint);

class PinMameCapture {
public:
   // No duration or stability threshold. Only decoded lifecycle transitions
   // qualify a game; table close and source loss always discard active play.
   void Reset();
   bool Active() const { return active; }
   std::optional<CompletedGameRecord> Observe(const NativeGameState& state,
      int64_t unixMs, int64_t monoMs, const std::string& rom,
      const std::string& table, const std::string& output);
private:
   bool ready = false, active = false, finishing = false;
   int64_t players = 0, lastPlayer = 0, lastBall = 0, ballsPerGame = 0;
   int64_t startUnix = 0, startMono = 0, endUnix = 0, endMono = 0;
   std::optional<int64_t> startCounter;
};
}
