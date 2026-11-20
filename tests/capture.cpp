// Assertions remain active in Release: calls in this test exercise production behavior.
#ifdef NDEBUG
#undef NDEBUG
#endif
// license:GPLv3+
#include "GameRecord.h"
#include "ScoresFileWriter.h"
#include "ScriptRecorder.h"
#include "PinMameCapture.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace ScoreTracker { void LPILog(const char*, int, unsigned, const char*, ...) {} }
using namespace ScoreTracker;
namespace fs = std::filesystem;
static CompletedGameRecord Game(const fs::path& directory) {
   CompletedGameRecord g;
   g.id = NewGameId(); g.tablePath = PathUtf8(directory / Utf8Path("Café Table.vpx"));
   g.source = "pinmame"; g.rom = "test";
   g.startedUnixMs = 1700000000000; g.endedUnixMs = 1700000000001; g.durationMs = 1;
   g.scores = {0, 9007199254740993LL};
   return g;
}
static nlohmann::json Read(const fs::path& p) { std::ifstream f(p); return nlohmann::json::parse(f); }
static void Write(const fs::path& p, const std::string& text) { std::ofstream(p) << text; }
static void Lifecycle() {
   NativeGameState s; s.readable = true; s.players = 1; s.scores = {99, 0}; s.ballsPerGame = 3;
   PinMameCapture capture;
   const auto observe = [&](int64_t time) { return capture.Observe(s, time, time, "afm_113b", "/tmp/AFM.vpx", ""); };
   s.player = s.ball = 1;
   assert(!observe(1)); // Attaching to an existing game cannot arm.
   s.player = s.ball = 0; assert(!observe(2)); // WPC idle may have game_over=0.
   s.player = s.ball = 1; s.gamesStarted = 10; assert(!observe(3));
   s.changedDuringRead = true; assert(!observe(4) && capture.Active()); s.changedDuringRead = false;
   s.gameOver = true; assert(!observe(4)); // Pulse on an earlier ball is not a completed game.
   s.gameOver = false; s.ball = 2; assert(!observe(5));
   s.ball = 3; assert(!observe(6)); s.scores[0] = 123;
   s.gameOver = true; s.ignoreScores = true; assert(!observe(7));
   assert(!observe(999999)); // Time never resolves ignore_scores.
   s.ignoreScores = false; s.scores[0] = 234;
   auto game = observe(1000000);
   assert(game && game->scores == std::vector<int64_t>{234} && game->endedUnixMs == 7);
   assert(game->durationMs == 4 && game->rom == "afm_113b");
   assert(!observe(1000001)); // Exactly once.
   s.gameOver = false; s.ball = s.player = 1; s.gamesStarted = 11; assert(!observe(1000002));
   s.ball = 3; assert(!observe(1000003));
   s.ball = 1; s.gamesStarted = 12; assert(!observe(1000004)); // Restart is not a result.
   s.gameOver = true; assert(!observe(1000005));
   capture.Reset(); s.player = s.ball = 0; s.gameOver = false; assert(!observe(2000000));
   s.player = s.ball = 1; s.players = 2; assert(!observe(2000001));
   s.ball = 3; assert(!observe(2000002)); s.gameOver = true; assert(!observe(2000003)); // Last participant has not played.
   s.gameOver = false; s.player = 2; assert(!observe(2000004));
   s.gameOver = true; s.scores = {0, 42}; game = observe(2000005);
   assert(game && game->scores == std::vector<int64_t>({0,42}));
   s.gameOver = false; s.player = s.ball = 1; assert(!observe(3000000));
   capture.Reset(); s.gameOver = true; assert(!observe(3000001)); // Source loss/close never saves.
   capture.Reset(); s.readable = false; assert(!observe(3000002));
}
static int64_t readerValues[] = {1,1,1,1,3,0};
static bool mutateDuringScore = false;
static void ReadValue(void* context, void* result) {
   *static_cast<int64_t*>(result) = *static_cast<int64_t*>(context);
   if (context == &readerValues[5] && mutateDuringScore) readerValues[0] = 0;
}
static void NativeReads() {
   std::vector<StateDef> defs;
   const char* names[] = {"Game Over", "Players", "Current Player", "Ball", "Ball Count", "Player 1"};
   const char* paths[] = {"game_state\\game_over\\Game Over", "game_state\\player_count\\Players",
      "game_state\\current_player\\Current Player", "game_state\\current_ball\\Ball", "game_state\\ball_count\\Ball Count", "game_state\\scores\\Player 1"};
   for (unsigned i=0;i<6;++i) defs.push_back({names[i],paths[i],i,CTLPI_STATE_FORMAT_INT64,0,&readerValues[i],ReadValue,nullptr});
   StateSrcId source{};source.id.endpointId=42;source.nStates=unsigned(defs.size());source.stateDefs=defs.data();
   auto state=ReadNativeGameState({source},42);assert(state.readable&&state.scores==std::vector<int64_t>{0});
   mutateDuringScore=true;state=ReadNativeGameState({source},42);assert(!state.readable&&state.changedDuringRead);mutateDuringScore=false;
   assert(!ReadNativeGameState({source},41).readable); // Never mix two controllers.
   defs[5].dataFormat=CTLPI_STATE_FORMAT_FLOAT;assert(!ReadNativeGameState({source},42).readable);
   defs[5].dataFormat=CTLPI_STATE_FORMAT_INT64;readerValues[5]=-1;assert(!ReadNativeGameState({source},42).readable);readerValues[5]=0;
   defs.push_back(defs.back());source.stateDefs=defs.data();source.nStates=unsigned(defs.size());
   assert(!ReadNativeGameState({source},42).readable); // Duplicate labels are ambiguous.
   const auto fixture=Read(fs::path(__FILE__).parent_path()/"fixtures/afm-startup.json");
   assert(!fixture["samples"].empty());
   PinMameCapture capture;
   for(const auto& sample:fixture["samples"]){
      std::vector<std::string> labels,pathsCopy;std::vector<int64_t> values;
      for(const auto& field:sample["fields"]){
         pathsCopy.push_back(field["path"]);labels.push_back(pathsCopy.back().substr(pathsCopy.back().find_last_of('\\')+1));
         values.push_back(field["value"]);
      }
      defs.clear();for(unsigned i=0;i<values.size();++i)defs.push_back({labels[i].c_str(),pathsCopy[i].c_str(),i,CTLPI_STATE_FORMAT_INT64,0,&values[i],ReadValue,nullptr});
      source.nStates=unsigned(defs.size());source.stateDefs=defs.data();
      state=ReadNativeGameState({source},42);
      assert(!capture.Observe(state,sample["frame"],sample["frame"],"afm_113b","/tmp/AFM.vpx",""));
   }
}
static void Storage(const fs::path& root) {
   auto game = Game(root);
   StorageReport report;
   assert(ScoresFileWriter::AppendCompletedGame(game, &report) && report.savedGames == 1 && report.requestedGameSaved && report.error.empty());
   assert(ScoresFileWriter::AppendCompletedGame(game, &report) && report.savedGames == 0 && report.requestedGameSaved && report.error.empty());
   const auto legacy = root / "legacy"; fs::create_directory(legacy);
   Write(legacy / "scores.json", "{\"games\":[]}");
   assert(ScoresFileWriter::RetryPending(legacy, &report) && report.savedGames == 0 && report.error.empty());
   fs::create_directory(legacy / ".scoretracker-pending");
   assert(ScoresFileWriter::RetryPending(legacy, &report) && report.error.empty());
   assert(ScoresFileWriter::CheckHistory(legacy, &report) && report.error.empty());
   assert(Read(legacy / "scores.json") == nlohmann::json::parse("{\"games\":[]}"));
   auto legacyGame = Game(legacy);
   assert(ScoresFileWriter::AppendCompletedGame(legacyGame, &report) && report.savedGames == 1);
   assert(!report.archivedHistory.empty() && Read(report.archivedHistory) == nlohmann::json::parse("{\"games\":[]}"));
   assert(Read(legacy / "scores.json")["games"].size() == 1);
   auto doc = Read(root / "scores.json");
   assert(doc["schema_version"] == 1 && doc["games"].size() == 1);
   assert(doc["games"][0]["table"] == "Café Table.vpx");
   assert(doc["games"][0]["scores"][0]["score"] == 0);
   assert(doc["games"][0]["scores"][1]["score"] == 9007199254740993LL);
   assert(!doc["games"][0].contains("evidence"));
   assert(!doc["games"][0].contains("signature") && !doc["games"][0].contains("shared_game"));
   // Removing a descriptive property must not block histories/journals written
   // by the immediately preceding build, or change any recorded score/id.
   const auto retired = root / "retired-metadata"; fs::create_directory(retired);
   auto previous = Game(retired);
   auto previousJson = GameJson(previous); previousJson["evidence"] = "retired metadata";
   Write(retired / "scores.json", nlohmann::json({{"schema_version",1},{"games",{previousJson}}}).dump());
   assert(ScoresFileWriter::AppendCompletedGame(previous, &report) && report.savedGames == 0);
   assert(Read(retired / "scores.json")["games"][0] == GameJson(previous));
   auto next = Game(retired); auto oldJournal = GameJson(next); oldJournal["evidence"] = "retired metadata";
   Write(retired / ".scoretracker-pending" / (next.id + ".json"), oldJournal.dump());
   assert(ScoresFileWriter::AppendCompletedGame(next, &report) && report.savedGames == 1);
   const auto cleaned = Read(retired / "scores.json");
   assert(cleaned["games"].size() == 2);
   for (const auto& saved : cleaned["games"]) assert(!saved.contains("evidence"));
   auto conflict = game; conflict.scores[0] = 99;
   assert(!ScoresFileWriter::AppendCompletedGame(conflict));
   assert(Read(root / "scores.json") == doc);
   // Isolate the conflict journal: writer must preserve it rather than erase user data.
   const auto malformed = root / "malformed"; fs::create_directory(malformed);
   for (const std::string original : {"{", "[]", "{\"schema_version\":1,\"games\":{}}", "{\"schema_version\":2,\"games\":[]}", "{\"schema_version\":1,\"games\":[],\"games\":[]}"}) {
      Write(malformed / "scores.json", original);
      auto pending = Game(malformed);
      assert(!ScoresFileWriter::AppendCompletedGame(pending));
      std::ifstream f(malformed / "scores.json");
      assert(std::string(std::istreambuf_iterator<char>(f), {}) == original);
      assert(fs::exists(malformed / ".scoretracker-pending" / (pending.id + ".json")));
   }
   Write(malformed / "scores.json", "{\"schema_version\":1,\"games\":[]}");
   assert(ScoresFileWriter::RetryPending(malformed, &report) && report.savedGames == 5);
   assert(Read(malformed / "scores.json")["games"].size() == 5);
   assert(ScoresFileWriter::RetryPending(malformed));
   assert(Read(malformed / "scores.json")["games"].size() == 5);
   auto deletion = root / "deletion"; fs::create_directory(deletion);
   auto first = Game(deletion); auto second = Game(deletion);
   assert(ScoresFileWriter::AppendCompletedGame(first));
   assert(ScoresFileWriter::AppendCompletedGame(second));
   auto history = Read(deletion / "scores.json"); history["games"].erase(1);
   Write(deletion / "scores.json", history.dump());
   auto third = Game(deletion); assert(ScoresFileWriter::AppendCompletedGame(third));
   assert(first.id != second.id && second.id != third.id && first.id != third.id);
}
static void Script(const fs::path& root) {
   ScriptRecorder::Enabled = true; ++ScriptRecorder::Generation;
   int calls = 0; CompletedGameRecord captured;
   ScriptRecorder recorder((root / "Original.vpx").string(), "", ScriptRecorder::Generation,
      [&](const CompletedGameRecord& g) { captured = g; return ++calls > 1; });
   assert(!recorder.CompleteGame()); assert(recorder.BeginGame(1));
   assert(!recorder.BeginGame(1)); assert(recorder.SetPlayerCount(2));
   assert(!recorder.SetPlayerScore(1, "1.5"));
   assert(!recorder.SetPlayerScore(1, "9223372036854775808"));
   assert(!recorder.SetPlayerScore(1, "-1"));
   assert(recorder.SetPlayerScore(1, "0")); assert(!recorder.CompleteGame());
   assert(recorder.SetPlayerScore(2, "9007199254740993"));
   assert(!recorder.CompleteGame()); const auto id = captured.id; const auto ended = captured.endedUnixMs;
   assert(!recorder.SetPlayerScore(2, "7")); // Freeze on completion, including failed storage.
   assert(recorder.CompleteGame()); assert(captured.id == id && captured.endedUnixMs == ended);
   assert(calls == 2 && !recorder.CompleteGame());
   assert(recorder.BeginGame(1)); ++ScriptRecorder::Generation;
   assert(!recorder.SetPlayerScore(1, "123") && !recorder.CompleteGame());
   assert(calls == 2);
}
int main(int argc, char** argv) {
   assert(argc >= 2); const fs::path root = argc == 4 ? fs::path(argv[1]) : fs::path(argv[1]) / NewGameId(); fs::create_directories(root);
   if (argc == 4 && std::string(argv[2]) == "append") {
      auto g = Game(root); g.id = argv[3]; return ScoresFileWriter::AppendCompletedGame(g) ? 0 : 1;
   }
   Lifecycle(); NativeReads(); Storage(root); Script(root);
   fs::remove_all(root);
   std::cout << "PASS: native Controller reader, actual AFM boot replay, coherent-control guard, lifecycle evidence, ignore_scores, exact final values, short games, invalid continuity, resets, zero scores, journals, idempotency, malformed histories, script contract\n";
}
