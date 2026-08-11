// license:GPLv3+

#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "common.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/MsgPlugin.h"

namespace ScoreTracker
{

// Tracks PinMAME's decoded game states through the generic Controller state-source API. PinMAME
// remains responsible for interpreting the memory map; ScoreTracker only consumes named values
// and keeps the session/finalization policy needed to produce reliable scores.json records.
class PinMameStateTracker final
{
public:
   using ScoreSavedCallback = void (*)(void* userData);

   enum class MapStatus
   {
      Available,
      NotFound,
      Error,
   };

   PinMameStateTracker() = default;
   ~PinMameStateTracker();

   static MapStatus ProbeMap(const string& gameId, const string& mapsPath, string& detail);

   bool Start(const MsgPluginAPI* msgApi, uint32_t endpointId, uint32_t controllerEndpointId,
      unsigned int getStateSourcesId, const string& gameId, const string& mapsPath,
      const string& tablePath, const string& outputPath,
      ScoreSavedCallback scoreSavedCallback = nullptr, void* scoreSavedUserData = nullptr);
   void Stop();
   void Poll();

   // Provider-owned definitions are invalidated by OnStateSrcChanged. Re-enumerate and bind the
   // map labels again without disturbing the active game session.
   bool RefreshStateSource();

   uint32_t GetControllerEndpointId() const { return m_controllerEndpointId; }
   const string& GetGameId() const { return m_gameId; }

private:
   struct StateBinding
   {
      unsigned int inputIndex = 0;
      int typeMask = 0;
      bool bound = false;
   };

   struct NamedState
   {
      string key;
      string label;
      // libpinmame currently publishes the decoded boolean before applying the map's
      // invert flag. Keep this small piece of schema metadata so consumers see the
      // same value as the original map decoder.
      bool invertBoolean = false;
      StateBinding binding;
   };

   bool LoadSchema();
   bool ReadInteger(const StateBinding& binding, int64_t& value) const;
   bool ReadSnapshot();
   static string ReadLabel(const nlohmann::json& descriptor);

   vector<int64_t> BuildFinalScoresSnapshot(const vector<int64_t>& playerScores,
      bool gameOverObserved, size_t& authoritativeCount) const;
   nlohmann::json BuildCompletedGameState() const;
   void FinalizeSession(const vector<int64_t>& finalScores, bool writeScoresFile,
      size_t authoritativeCount);

   const MsgPluginAPI* m_msgApi = nullptr;
   uint32_t m_endpointId = 0;
   uint32_t m_controllerEndpointId = 0;
   unsigned int m_getStateSourcesId = 0;
   StateSrcId m_stateSource { };

   string m_gameId;
   string m_mapsPath;
   string m_tablePath;
   string m_outputPath;
   ScoreSavedCallback m_scoreSavedCallback = nullptr;
   void* m_scoreSavedUserData = nullptr;

   vector<string> m_scoreLabels;
   vector<string> m_finalScoreLabels;
   vector<NamedState> m_namedStates;
   vector<StateBinding> m_scoreBindings;
   vector<StateBinding> m_finalScoreBindings;
   bool m_finalScoresMostRecentFirst = false;

   vector<int64_t> m_playerScores;
   vector<int64_t> m_finalScores;
   std::unordered_map<string, int64_t> m_decodedValues;

   std::chrono::steady_clock::time_point m_sessionStartRealTime;
   std::chrono::steady_clock::time_point m_scoresStableSince;
   std::chrono::steady_clock::time_point m_gameOverSince;
   vector<int64_t> m_highestScores;
   std::unordered_map<string, int64_t> m_maxGameStateValues;
   vector<int64_t> m_prevPlayerScores;
   vector<int64_t> m_finalScoresBaseline;
   int64_t m_lastPlayerCountRead = -1;
   int m_gameOverAnomalies = 0;
   bool m_ignoreGameOver = false;
   bool m_hasFinalScoresBaseline = false;
   bool m_gameOverLast = false;
   bool m_gameOverPending = false;
   bool m_summarySent = false;
   bool m_hasBeenInPlay = false;
   bool m_monitoringStarted = false;
};

}
