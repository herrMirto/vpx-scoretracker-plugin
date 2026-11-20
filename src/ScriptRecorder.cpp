// license:GPLv3+
#include "ScriptRecorder.h"
#include <charconv>
namespace ScoreTracker {
uint64_t ScriptRecorder::Generation = 0;
bool ScriptRecorder::Enabled = false;
ScriptRecorder::ScriptRecorder(std::string table, std::string output, uint64_t generation, SaveGame save)
   : m_save(std::move(save)), m_generation(generation) {
   m_record.tablePath = std::move(table);
   m_record.outputPath = std::move(output);
   m_record.source = "table_script";
}
bool ScriptRecorder::Current() const { return Enabled && m_generation == Generation && !m_record.tablePath.empty(); }
bool ScriptRecorder::BeginGame(int players) {
   if (!Current() || m_active || players < 1 || players > 8) return false;
   try { m_record.id = NewGameId(); } catch (const std::exception&) { return false; }
   m_record.startedUnixMs = UnixMilliseconds();
   m_startedMono = MonotonicMilliseconds();
   m_record.scores.assign(size_t(players), 0);
   m_written = 0;
   m_completed = false;
   m_active = true;
   return true;
}
bool ScriptRecorder::SetPlayerCount(int players) {
   if (!Current() || !m_active || m_completed || players < int(m_record.scores.size()) || players > 8) return false;
   m_record.scores.resize(size_t(players), 0);
   return true;
}
bool ScriptRecorder::SetPlayerScore(int player, const std::string& score) {
   if (!Current() || !m_active || m_completed || player < 1 || size_t(player) > m_record.scores.size() || score.empty()) return false;
   int64_t value;
   const auto result = std::from_chars(score.data(), score.data() + score.size(), value);
   if (result.ec != std::errc{} || result.ptr != score.data() + score.size() || value < 0) return false;
   m_record.scores[size_t(player - 1)] = value;
   m_written |= 1u << (player - 1);
   return true;
}
bool ScriptRecorder::CompleteGame() {
   if (!Current() || !m_active || m_written != (1u << m_record.scores.size()) - 1) return false;
   if (!m_completed) {
      m_completed = true;
      m_record.endedUnixMs = UnixMilliseconds();
      m_record.durationMs = MonotonicMilliseconds() - m_startedMono;
   }
   if (!m_save(m_record)) return false; // Frozen record/id retained for an explicit retry.
   m_active = false;
   return true;
}
void ScriptRecorder::AbortGame() { m_active = false; m_completed = false; m_written = 0; }
PSC_CLASS_START(ScoreTracker_Recorder, ScriptRecorder)
   PSC_FUNCTION1(bool, BeginGame, int)
   PSC_FUNCTION1(bool, SetPlayerCount, int)
   PSC_FUNCTION2(bool, SetPlayerScore, int, string)
   PSC_FUNCTION0(bool, CompleteGame)
   PSC_FUNCTION0(void, AbortGame)
PSC_CLASS_END()
static std::function<ScriptRecorder*()> factory;
void RegisterRecorder(ScriptablePluginAPI* api, uint32_t endpoint, std::function<ScriptRecorder*()> create) {
   factory = std::move(create);
   RegisterScoreTracker_Recorder([api](ScriptClassDef* def) { api->RegisterScriptClass(def); });
   ScoreTracker_Recorder_SCD->CreateObject = []() -> void* { return factory(); };
   api->SubmitTypeLibrary(endpoint);
   api->SetCOMObjectOverride("ScoreTracker.Recorder", ScoreTracker_Recorder_SCD);
}
void UnregisterRecorder(ScriptablePluginAPI* api) {
   api->SetCOMObjectOverride("ScoreTracker.Recorder", nullptr);
   UnregisterScoreTracker_Recorder([api](ScriptClassDef* def) { api->UnregisterScriptClass(def); });
   factory = {};
}
}
