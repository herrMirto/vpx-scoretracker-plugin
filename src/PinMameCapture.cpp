// license:GPLv3+
#include "PinMameCapture.h"
#include <array>
#include <charconv>
#include <limits>

namespace ScoreTracker {
namespace {
bool Read(const StateDef& state, int64_t& value) {
   if (!state.GetState) return false;
   switch (state.dataFormat) {
#define READ_TYPE(format, type) case format: { type v{}; state.GetState(state.callContext, &v); value = v; return true; }
   READ_TYPE(CTLPI_STATE_FORMAT_UINT8, uint8_t)
   READ_TYPE(CTLPI_STATE_FORMAT_UINT16, uint16_t)
   READ_TYPE(CTLPI_STATE_FORMAT_UINT32, uint32_t)
   READ_TYPE(CTLPI_STATE_FORMAT_INT8, int8_t)
   READ_TYPE(CTLPI_STATE_FORMAT_INT16, int16_t)
   READ_TYPE(CTLPI_STATE_FORMAT_INT32, int32_t)
#undef READ_TYPE
   case CTLPI_STATE_FORMAT_INT64: {
      value = std::numeric_limits<int64_t>::min(); state.GetState(state.callContext, &value);
      return value != std::numeric_limits<int64_t>::min();
   }
   case CTLPI_STATE_FORMAT_UINT64: {
      uint64_t v = std::numeric_limits<uint64_t>::max(); state.GetState(state.callContext, &v);
      if (v > uint64_t(std::numeric_limits<int64_t>::max())) return false;
      value = int64_t(v); return true;
   }
   default: return false;
   }
}
unsigned PlayerNumber(const std::string& label) {
   for (const std::string prefix : {"Player ", "Final P", "Final Score P"}) {
      if (!label.starts_with(prefix)) continue;
      unsigned n = 0;
      const auto result = std::from_chars(label.data() + prefix.size(), label.data() + label.size(), n);
      if (result.ec == std::errc{} && result.ptr == label.data() + label.size() && n >= 1 && n <= 8) return n;
   }
   return 0;
}
}
NativeGameState ReadNativeGameState(const std::vector<StateSrcId>& sources, uint32_t endpoint) {
   NativeGameState result;
   std::map<std::string, const StateDef*> fields;
   std::map<unsigned, const StateDef*> scores;
   bool ambiguous = false;
   for (const auto& source : sources) {
      if (source.id.endpointId != endpoint || !source.stateDefs) continue;
      for (unsigned i = 0; i < source.nStates; ++i) {
         const auto& state = source.stateDefs[i];
         const std::string path = state.desc ? state.desc : "";
         if (path.starts_with("game_state\\")) {
            const auto end = path.find('\\', 11);
            const auto field = path.substr(11, end == std::string::npos ? end : end - 11);
            if (field == "scores") {
               const auto n = PlayerNumber(state.name ? state.name : "");
               if (!n || !scores.emplace(n, &state).second) ambiguous = true;
            } else if (field == "game_over" || field == "player_count" || field == "current_player"
               || field == "current_ball" || field == "ball_count" || field == "extra_balls" || field == "ignore_scores") {
               if (!fields.emplace(field, &state).second) ambiguous = true;
            }
         } else if (path.starts_with("audits\\") && state.name && std::string(state.name) == "Games Started") {
            if (!fields.emplace("games_started", &state).second) ambiguous = true;
         }
      }
   }
   if (ambiguous || scores.empty()) return result;
   for (unsigned n = 1; n <= scores.size(); ++n) if (!scores.contains(n)) return result;
   const auto readField = [&](const char* name, int64_t& value, bool required = true) {
      const auto it = fields.find(name);
      return it == fields.end() ? !required : Read(*it->second, value);
   };
   // Read lifecycle fields on both sides of score reads. A transition during the
   // sequence is rejected, rather than joining players/scores from two games.
   auto controls = [&]() -> std::optional<std::array<int64_t, 8>> {
      std::array<int64_t, 8> v{};
      if (!readField("game_over", v[0]) || !readField("player_count", v[1])
         || !readField("current_player", v[2]) || !readField("current_ball", v[3])
         || !readField("ball_count", v[4], false) || !readField("extra_balls", v[5], false)
         || !readField("ignore_scores", v[6], false) || !readField("games_started", v[7], false)) return {};
      return v;
   };
   const auto before = controls();
   if (!before) return result;
   for (const auto& [n, state] : scores) {
      int64_t score;
      if (!Read(*state, score) || score < 0) return result;
      result.scores.push_back(score);
   }
   const auto after = controls();
   if (!after) return NativeGameState{};
   if (before != after) {
      NativeGameState crossed; crossed.changedDuringRead = true; return crossed;
   }
   const auto& v = *before;
   if ((v[0] != 0 && v[0] != 1) || (v[6] != 0 && v[6] != 1)
      || v[1] < 0 || v[1] > int64_t(scores.size()) || v[2] < 0 || v[2] > 8
      || v[3] < 0 || v[3] > 99 || v[4] < 0 || v[4] > 99 || v[5] < 0 || v[5] > 99) return NativeGameState{};
   result.readable = true; result.gameOver = v[0] != 0; result.ignoreScores = v[6] != 0;
   result.players = v[1]; result.player = v[2]; result.ball = v[3];
   result.ballsPerGame = v[4]; result.extraBalls = v[5];
   if (fields.contains("games_started")) result.gamesStarted = v[7];
   return result;
}
void PinMameCapture::Reset() { *this = {}; }
std::optional<CompletedGameRecord> PinMameCapture::Observe(const NativeGameState& s,
   int64_t unixMs, int64_t monoMs, const std::string& rom, const std::string& table, const std::string& output) {
   // A detected transition during one read is discarded, not interpreted as
   // missing a whole game. The next event still has to satisfy every guard.
   if (s.changedDuringRead) return {};
   if (!s.readable) { Reset(); return {}; }
   // Boot/self-test values (e.g. WPC 0x55/0xAA) never arm a game. An idle
   // observation is required; attaching while already playing cannot qualify.
   const bool playing = !s.gameOver && s.players >= 1 && s.players <= int64_t(s.scores.size())
      && s.player >= 1 && s.player <= s.players && s.ball >= 1
      && (s.ballsPerGame == 0 || s.ball <= s.ballsPerGame);
   if (active && s.gamesStarted != startCounter) {
      Reset(); // Explicit ROM audit changed: old game was restarted/reset, not completed.
      ready = true;
   }
   if (!active) {
      if (!playing) {
         if (s.gameOver || (s.ball == 0 && s.player == 0)) ready = true;
         return {};
      }
      if (!ready || s.ball != 1 || s.player != 1) return {};
      active = true; ready = false; players = s.players;
      startUnix = unixMs; startMono = monoMs; startCounter = s.gamesStarted;
      ballsPerGame = s.ballsPerGame;
   }
   if (s.gameOver) {
      // When the map provides configured balls, require observed play on the
      // final ball for the final participant. Between-ball pulses cannot finish
      // an earlier ball. Pending extra balls also prevent completion.
      if ((ballsPerGame > 0 && lastBall != ballsPerGame) || lastPlayer != players || s.extraBalls > 0) return {};
      if (!finishing) { finishing = true; endUnix = unixMs; endMono = monoMs; }
      if (s.ignoreScores) return {};
      if (s.players != players || int64_t(s.scores.size()) < players) { Reset(); return {}; }
      CompletedGameRecord record;
      record.id = NewGameId(); record.tablePath = table; record.outputPath = output; record.rom = rom;
      record.source = "pinmame";
      record.startedUnixMs = startUnix; record.endedUnixMs = endUnix; record.durationMs = endMono - startMono;
      record.scores.assign(s.scores.begin(), s.scores.begin() + players);
      Reset(); ready = true;
      return record;
   }
   if (!playing) { Reset(); return {}; }
   if (finishing || s.ball < lastBall || s.players < players
      || (s.players > players && s.ball != 1) || s.ballsPerGame != ballsPerGame) { Reset(); return {}; }
   players = s.players; lastBall = s.ball; lastPlayer = s.player;
   return {};
}
}
