// license:GPLv3+
#pragma once
#include "GameRecord.h"
#include "plugins/ScriptablePlugin.h"
#include <functional>
namespace ScoreTracker {
using SaveGame = std::function<bool(const CompletedGameRecord&)>;
class ScriptRecorder final {
public:
   PSC_IMPLEMENT_REFCOUNT()
   ScriptRecorder(std::string table, std::string output, uint64_t generation, SaveGame save);
   bool BeginGame(int players);
   bool SetPlayerCount(int players);
   bool SetPlayerScore(int player, const std::string& score);
   bool CompleteGame();
   void AbortGame();
   static uint64_t Generation;
   static bool Enabled;
private:
   bool Current() const;
   CompletedGameRecord m_record;
   SaveGame m_save;
   uint64_t m_generation;
   int64_t m_startedMono = 0;
   unsigned m_written = 0;
   bool m_active = false, m_completed = false;
};
void RegisterRecorder(ScriptablePluginAPI* api, uint32_t endpoint,
   std::function<ScriptRecorder*()> create);
void UnregisterRecorder(ScriptablePluginAPI* api);
}
