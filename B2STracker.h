// license:GPLv3+

#pragma once

#include <chrono>
#include <cstdint>

#include "common.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/MsgPlugin.h"

namespace ScoreTracker
{

// Tracks scores explicitly published by table scripts through
// B2SSetScorePlayer/B2SSetScorePlayerN. B2S exposes those values through the
// generic Controller state-source API in its "Scores (players)" group.
class B2STracker final
{
public:
   using ScoreSavedCallback = void (*)(void* userData);

   B2STracker() = default;
   ~B2STracker();

   bool Start(const MsgPluginAPI* msgApi, uint32_t endpointId, uint32_t controllerEndpointId,
      unsigned int getStateSourcesId, const string& gameId, const string& tablePath,
      const string& outputPath, ScoreSavedCallback scoreSavedCallback = nullptr,
      void* scoreSavedUserData = nullptr);
   void Stop();
   void Poll();

   // State source definitions are owned by their provider and become invalid whenever any
   // provider announces a source change, so reacquire the B2S definition on every such event.
   void RefreshStateSource();

   // B2S currently exposes its standard game-over helper through the compatibility event stream,
   // while the generic getter for illumination states is broken in current VPX master.
   void OnB2SStateChange(uint8_t type, int32_t index, int32_t value);

   uint32_t GetControllerEndpointId() const { return m_controllerEndpointId; }
   const string& GetGameId() const { return m_gameId; }

private:
   struct PlayerState
   {
      unsigned int inputIndex;
      unsigned int playerNo;
      int typeMask;
   };

   bool ReadPlayerScores(vector<int64_t>& scores) const;
   void BeginSession(const vector<int64_t>& scores);
   void FinalizeSession(bool writeScoresFile);
   void ResetSession(bool waitForScoreReset);

   const MsgPluginAPI* m_msgApi = nullptr;
   uint32_t m_endpointId = 0;
   uint32_t m_controllerEndpointId = 0;
   unsigned int m_getStateSourcesId = 0;
   StateSrcId m_stateSource { };
   vector<PlayerState> m_playerStates;

   string m_gameId;
   string m_tablePath;
   string m_outputPath;
   ScoreSavedCallback m_scoreSavedCallback = nullptr;
   void* m_scoreSavedUserData = nullptr;

   std::chrono::steady_clock::time_point m_sessionStart;
   std::chrono::steady_clock::time_point m_playStartHint;
   std::chrono::steady_clock::time_point m_scoresStableSince;
   vector<int64_t> m_previousScores;
   vector<int64_t> m_highestScores;
   bool m_monitoringStarted = false;
   bool m_hasSnapshot = false;
   bool m_sessionActive = false;
   bool m_hasPlayStartHint = false;
   bool m_gameOverPending = false;
   bool m_waitingForScoreReset = false;
};

}
