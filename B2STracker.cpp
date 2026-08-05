// license:GPLv3+

#include "B2STracker.h"
#include "ScoresFileWriter.h"

#include <algorithm>
#include <cstring>

namespace ScoreTracker
{

static constexpr int kScoreStableConfirmSeconds = 8;
static constexpr int kMinGameDurationSeconds = 30;
static constexpr uint32_t kB2SPlayerScoresGroup = 0x0002;

B2STracker::~B2STracker() { Stop(); }

bool B2STracker::Start(const MsgPluginAPI* msgApi, uint32_t endpointId, uint32_t controllerEndpointId,
   unsigned int getStateSourcesId, const string& gameId, const string& tablePath,
   const string& outputPath, ScoreSavedCallback scoreSavedCallback, void* scoreSavedUserData)
{
   m_msgApi = msgApi;
   m_endpointId = endpointId;
   m_controllerEndpointId = controllerEndpointId;
   m_getStateSourcesId = getStateSourcesId;
   m_gameId = gameId;
   m_tablePath = tablePath;
   m_outputPath = outputPath;
   m_scoreSavedCallback = scoreSavedCallback;
   m_scoreSavedUserData = scoreSavedUserData;
   m_monitoringStarted = true;
   m_hasSnapshot = false;
   m_sessionActive = false;
   m_hasPlayStartHint = false;
   m_gameOverPending = false;
   m_waitingForScoreReset = false;
   m_previousScores.clear();
   m_highestScores.clear();
   RefreshStateSource();
   return true;
}

void B2STracker::Stop()
{
   if (m_monitoringStarted && m_sessionActive)
   {
      LOGI("No confirmed B2S game-over before VPX exit; writing the active session as an exit fallback");
      FinalizeSession(true);
   }
   m_monitoringStarted = false;
   m_stateSource = { };
   m_playerStates.clear();
}

void B2STracker::RefreshStateSource()
{
   m_stateSource = { };
   m_playerStates.clear();
   if (!m_monitoringStarted || m_msgApi == nullptr)
      return;

   GetStateSrcMsg query { 0, 0, nullptr };
   m_msgApi->BroadcastMsg(m_endpointId, m_getStateSourcesId, &query);
   if (query.count == 0)
      return;

   vector<StateSrcId> sources(query.count);
   query = { static_cast<unsigned int>(sources.size()), 0, sources.data() };
   m_msgApi->BroadcastMsg(m_endpointId, m_getStateSourcesId, &query);

   const unsigned int count = std::min(query.count, static_cast<unsigned int>(sources.size()));
   for (unsigned int sourceIndex = 0; sourceIndex < count; ++sourceIndex)
   {
      const StateSrcId& source = sources[sourceIndex];
      if (source.id.endpointId != m_controllerEndpointId || source.GetState == nullptr)
         continue;

      uint32_t playerScoresGroup = kB2SPlayerScoresGroup;
      for (unsigned int groupIndex = 0; groupIndex < source.nGroups; ++groupIndex)
      {
         const StateGroupDef& group = source.groupDefs[groupIndex];
         if (group.name != nullptr && std::strcmp(group.name, "Scores (players)") == 0)
         {
            playerScoresGroup = group.id;
            break;
         }
      }

      vector<PlayerState> playerStates;
      for (unsigned int stateIndex = 0; stateIndex < source.nStates; ++stateIndex)
      {
         const StateDef& state = source.stateDefs[stateIndex];
         if (state.id.groupId != playerScoresGroup || state.id.stateId < 1 || state.id.stateId > 8)
            continue;
         if ((state.typeMask & (CTLPI_STATE_TYPE_INT32 | CTLPI_STATE_TYPE_INT64)) == 0)
            continue;
         playerStates.push_back({ stateIndex, state.id.stateId, state.typeMask });
      }
      if (playerStates.empty())
         continue;

      std::sort(playerStates.begin(), playerStates.end(), [](const PlayerState& lhs, const PlayerState& rhs) { return lhs.playerNo < rhs.playerNo; });
      m_stateSource = source;
      m_playerStates = std::move(playerStates);
      LOGI("B2S exposed %u direct player score state(s)", static_cast<unsigned int>(m_playerStates.size()));
      return;
   }
}

bool B2STracker::ReadPlayerScores(vector<int64_t>& scores) const
{
   if (m_stateSource.GetState == nullptr || m_playerStates.empty())
      return false;

   scores.assign(m_playerStates.back().playerNo, 0);
   for (const PlayerState& player : m_playerStates)
   {
      int64_t value = 0;
      int result;
      if ((player.typeMask & CTLPI_STATE_TYPE_INT64) != 0)
      {
         result = m_stateSource.GetState(player.inputIndex, CTLPI_STATE_TYPE_INT64, &value);
      }
      else
      {
         int32_t value32 = 0;
         result = m_stateSource.GetState(player.inputIndex, CTLPI_STATE_TYPE_INT32, &value32);
         value = value32;
      }
      if (result != 0)
         return false;
      scores[player.playerNo - 1] = std::max<int64_t>(value, 0);
   }
   return true;
}

void B2STracker::BeginSession(const vector<int64_t>& scores)
{
   m_sessionActive = true;
   m_sessionStart = m_hasPlayStartHint ? m_playStartHint : std::chrono::steady_clock::now();
   m_hasPlayStartHint = false;
   m_scoresStableSince = m_sessionStart;
   m_highestScores = scores;
   LOGI("B2S score activity detected; tracking a new game");
}

void B2STracker::ResetSession(bool waitForScoreReset)
{
   m_sessionActive = false;
   m_gameOverPending = false;
   m_waitingForScoreReset = waitForScoreReset;
   m_hasPlayStartHint = false;
   m_highestScores.clear();
}

void B2STracker::FinalizeSession(bool writeScoresFile)
{
   if (!m_sessionActive)
      return;

   vector<int64_t> scores = m_highestScores;
   while (!scores.empty() && scores.back() == 0)
      scores.pop_back();
   const int64_t duration = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_sessionStart).count();
   if (!writeScoresFile || scores.empty())
      return;
   if (duration < kMinGameDurationSeconds)
   {
      LOGI("Ignoring B2S game shorter than %ds (was %llds) - not writing scores.json", kMinGameDurationSeconds, static_cast<long long>(duration));
      return;
   }

   CompletedGameRecord record;
   record.tablePath = m_tablePath;
   record.outputPath = m_outputPath;
   // A b2s::<name> id identifies the B2S controller session, not a ROM. Leaving rom empty lets
   // the scores file keep using the VPX table path as the identity for EM/original tables.
   record.scores = std::move(scores);
   record.gameDuration = duration;
   record.gameState = { { "player_count", static_cast<int64_t>(record.scores.size()) } };
   if (ScoresFileWriter::AppendCompletedGame(record) && m_scoreSavedCallback != nullptr)
      m_scoreSavedCallback(m_scoreSavedUserData);
}

void B2STracker::Poll()
{
   if (!m_monitoringStarted)
      return;

   vector<int64_t> scores;
   if (!ReadPlayerScores(scores))
      return;

   if (!m_hasSnapshot)
   {
      m_previousScores = scores;
      m_hasSnapshot = true;
      return;
   }

   const bool changed = scores != m_previousScores;
   const bool hasScore = std::any_of(scores.begin(), scores.end(), [](int64_t value) { return value > 0; });
   const bool allZero = !hasScore;
   if (changed)
      m_scoresStableSince = std::chrono::steady_clock::now();

   if (m_waitingForScoreReset)
   {
      if (allZero)
         m_waitingForScoreReset = false;
      m_previousScores = std::move(scores);
      return;
   }

   if (!m_sessionActive)
   {
      // Treat a change to a positive score as proof of play. This avoids recording a score that
      // a table publishes during initialization or attract mode as a new game.
      if (changed && hasScore)
         BeginSession(scores);
      m_previousScores = std::move(scores);
      return;
   }

   if (m_highestScores.size() < scores.size())
      m_highestScores.resize(scores.size(), 0);
   for (size_t i = 0; i < scores.size(); ++i)
      m_highestScores[i] = std::max(m_highestScores[i], scores[i]);

   if (changed && allZero)
   {
      LOGI("B2S scores reset; finalizing the previous game");
      FinalizeSession(true);
      ResetSession(false);
   }
   else if (m_gameOverPending && std::chrono::steady_clock::now() - m_scoresStableSince >= std::chrono::seconds(kScoreStableConfirmSeconds))
   {
      LOGI("B2S game-over confirmed after scores became stable");
      FinalizeSession(true);
      ResetSession(true);
   }

   m_previousScores = std::move(scores);
}

void B2STracker::OnB2SStateChange(uint8_t type, int32_t index, int32_t value)
{
   // B2SSetBallInPlay is a better duration start than the first scoring change when the table
   // publishes it. Keep it as a hint; score activity is still required before a game is saved.
   if (m_monitoringStarted && type == static_cast<uint8_t>('E') && index == 32 && value != 0
      && !m_sessionActive && !m_hasPlayStartHint)
   {
      m_hasPlayStartHint = true;
      m_playStartHint = std::chrono::steady_clock::now();
   }
   // 'E', 35 is the compatibility event generated by B2SSetGameOver(value).
   if (m_monitoringStarted && type == static_cast<uint8_t>('E') && index == 35 && value != 0)
      m_gameOverPending = true;
}

}
