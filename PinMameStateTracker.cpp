// license:GPLv3+

#include "PinMameStateTracker.h"
#include "ScoresFileWriter.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "pinmame/PinMAMEPlugin.h"

namespace ScoreTracker
{

static constexpr int kScoreStableConfirmSeconds = 8;
static constexpr int kMinGameDurationSeconds = 30;
static constexpr int kGameOverAnomalyLimit = 3;

PinMameStateTracker::~PinMameStateTracker() { Stop(); }

PinMameStateTracker::MapStatus PinMameStateTracker::ProbeMap(const string& gameId,
   const string& mapsPath, string& detail)
{
   detail.clear();
   if (gameId.empty())
      return MapStatus::NotFound;

   const std::filesystem::path indexPath = std::filesystem::path(mapsPath) / "index.json";
   std::ifstream indexFile(indexPath);
   if (!indexFile.is_open())
   {
      detail = "could not open " + indexPath.string();
      return MapStatus::Error;
   }

   try
   {
      nlohmann::json indexJson;
      indexFile >> indexJson;
      if (!indexJson.contains(gameId))
      {
         detail = "ROM is not listed in index.json";
         return MapStatus::NotFound;
      }
      if (!indexJson[gameId].is_string())
      {
         detail = "index.json entry is not a path string";
         return MapStatus::Error;
      }
      const std::filesystem::path mapPath = std::filesystem::path(mapsPath)
         / indexJson[gameId].get<string>();
      if (!std::filesystem::is_regular_file(mapPath))
      {
         detail = "declared map file is missing: " + mapPath.string();
         return MapStatus::Error;
      }
      detail = mapPath.string();
      return MapStatus::Available;
   }
   catch (const std::exception& e)
   {
      detail = "failed to parse index.json: " + string(e.what());
      return MapStatus::Error;
   }
}

string PinMameStateTracker::ReadLabel(const nlohmann::json& descriptor)
{
   if (!descriptor.is_object() || !descriptor.contains("label")
      || !descriptor["label"].is_string())
      return {};
   return descriptor["label"].get<string>();
}

bool PinMameStateTracker::LoadSchema()
{
   const std::filesystem::path mapsRoot(m_mapsPath);
   std::ifstream indexFile(mapsRoot / "index.json");
   if (!indexFile.is_open())
      return false;

   try
   {
      nlohmann::json indexJson;
      indexFile >> indexJson;
      if (!indexJson.contains(m_gameId) || !indexJson[m_gameId].is_string())
         return false;

      std::ifstream mapFile(mapsRoot / indexJson[m_gameId].get<string>());
      if (!mapFile.is_open())
         return false;
      nlohmann::json mapData;
      mapFile >> mapData;
      if (!mapData.contains("game_state") || !mapData["game_state"].is_object())
         return false;

      m_scoreLabels.clear();
      m_finalScoreLabels.clear();
      m_namedStates.clear();
      m_finalScoresMostRecentFirst = false;

      for (const auto& [key, value] : mapData["game_state"].items())
      {
         if (key == "scores" && value.is_array())
         {
            for (const auto& descriptor : value)
            {
               const string label = ReadLabel(descriptor);
               if (!label.empty())
                  m_scoreLabels.push_back(label);
            }
         }
         else if (key == "final_scores" && value.is_array())
         {
            for (const auto& descriptor : value)
            {
               const string label = ReadLabel(descriptor);
               if (!label.empty())
                  m_finalScoreLabels.push_back(label);
            }
         }
         else if (key == "final_scores_order" && value.is_string())
         {
            m_finalScoresMostRecentFirst = value.get<string>() == "most_recent_first";
         }
         else if (value.is_object())
         {
            const string label = ReadLabel(value);
            if (!label.empty())
            {
               const bool invertBoolean = value.value("encoding", string()) == "bool"
                  && value.value("invert", false);
               m_namedStates.push_back({ key, label, invertBoolean, {} });
            }
         }
      }
   }
   catch (const std::exception& e)
   {
      LOGE("Could not load game-state schema for %s: %s", m_gameId.c_str(), e.what());
      return false;
   }

   if (m_scoreLabels.empty() && m_finalScoreLabels.empty())
   {
      LOGE("Map for %s exposes neither scores nor final_scores", m_gameId.c_str());
      return false;
   }
   return true;
}

bool PinMameStateTracker::Start(const MsgPluginAPI* msgApi, uint32_t endpointId,
   uint32_t controllerEndpointId, unsigned int getStateSourcesId, const string& gameId,
   const string& mapsPath, const string& tablePath, const string& outputPath,
   ScoreSavedCallback scoreSavedCallback, void* scoreSavedUserData)
{
   m_msgApi = msgApi;
   m_endpointId = endpointId;
   m_controllerEndpointId = controllerEndpointId;
   m_getStateSourcesId = getStateSourcesId;
   m_gameId = gameId;
   m_mapsPath = mapsPath;
   m_tablePath = tablePath;
   m_outputPath = outputPath;
   m_scoreSavedCallback = scoreSavedCallback;
   m_scoreSavedUserData = scoreSavedUserData;

   if (!LoadSchema())
      return false;

   m_sessionStartRealTime = std::chrono::steady_clock::now();
   m_scoresStableSince = m_sessionStartRealTime;
   m_playerScores.clear();
   m_finalScores.clear();
   m_decodedValues.clear();
   m_highestScores.clear();
   m_maxGameStateValues.clear();
   m_prevPlayerScores.clear();
   m_finalScoresBaseline.clear();
   m_lastPlayerCountRead = -1;
   m_gameOverAnomalies = 0;
   m_ignoreGameOver = false;
   m_hasFinalScoresBaseline = false;
   m_gameOverLast = false;
   m_gameOverPending = false;
   m_summarySent = false;
   m_hasBeenInPlay = false;
   m_monitoringStarted = true;

   if (!RefreshStateSource())
   {
      LOGI("PinMAME has no decoded game-state source for %s", gameId.c_str());
      m_monitoringStarted = false;
      return false;
   }
   return true;
}

bool PinMameStateTracker::RefreshStateSource()
{
   m_stateSource = {};
   m_scoreBindings.assign(m_scoreLabels.size(), {});
   m_finalScoreBindings.assign(m_finalScoreLabels.size(), {});
   for (NamedState& state : m_namedStates)
      state.binding = {};

   if (!m_monitoringStarted || m_msgApi == nullptr)
      return false;

   GetStateSrcMsg query { 0, 0, nullptr };
   m_msgApi->BroadcastMsg(m_endpointId, m_getStateSourcesId, &query);
   if (query.count == 0)
      return false;

   vector<StateSrcId> sources(query.count);
   query = { static_cast<unsigned int>(sources.size()), 0, sources.data() };
   m_msgApi->BroadcastMsg(m_endpointId, m_getStateSourcesId, &query);

   const unsigned int sourceCount = std::min(query.count,
      static_cast<unsigned int>(sources.size()));
   for (unsigned int sourceIndex = 0; sourceIndex < sourceCount; ++sourceIndex)
   {
      const StateSrcId& source = sources[sourceIndex];
      if ((m_controllerEndpointId != 0 && source.id.endpointId != m_controllerEndpointId)
         || source.GetState == nullptr)
         continue;

      struct Candidate
      {
         string path;
         StateBinding binding;
      };
      std::unordered_map<string, vector<Candidate>> gameStates;
      for (unsigned int stateIndex = 0; stateIndex < source.nStates; ++stateIndex)
      {
         const StateDef& state = source.stateDefs[stateIndex];
         if ((state.id.groupId & PMPI_GROUP_MASK) != PMPI_GROUP_GAMESTATE
            || state.name == nullptr)
            continue;
         const int integerTypes = CTLPI_STATE_TYPE_UINT8 | CTLPI_STATE_TYPE_UINT16
            | CTLPI_STATE_TYPE_UINT32 | CTLPI_STATE_TYPE_UINT64 | CTLPI_STATE_TYPE_INT8
            | CTLPI_STATE_TYPE_INT16 | CTLPI_STATE_TYPE_INT32 | CTLPI_STATE_TYPE_INT64;
         if ((state.typeMask & integerTypes) == 0)
            continue;
         string path = state.desc != nullptr ? state.desc : "";
         std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) {
            return ch == '/' ? '\\' : static_cast<char>(std::tolower(ch));
         });
         gameStates[state.name].push_back({ std::move(path),
            StateBinding { stateIndex, state.typeMask, true } });
      }
      if (gameStates.empty())
         continue;

      const auto bindByLabel = [&gameStates](const string& label, const char* collection) {
         const auto it = gameStates.find(label);
         if (it == gameStates.end())
            return StateBinding {};
         if (collection != nullptr)
         {
            const string pathPart = "\\"s + collection + "\\";
            for (const Candidate& candidate : it->second)
               if (candidate.path.find(pathPart) != string::npos)
                  return candidate.binding;
         }
         // Labels are normally unique. This fallback also supports providers predating the
         // hierarchical desc path; ambiguous labels must use the path to avoid mixing live and
         // frozen scores.
         return it->second.size() == 1 ? it->second.front().binding : StateBinding {};
      };

      m_stateSource = source;
      for (size_t i = 0; i < m_scoreLabels.size(); ++i)
         m_scoreBindings[i] = bindByLabel(m_scoreLabels[i], "scores");
      for (size_t i = 0; i < m_finalScoreLabels.size(); ++i)
         m_finalScoreBindings[i] = bindByLabel(m_finalScoreLabels[i], "final_scores");
      for (NamedState& state : m_namedStates)
         state.binding = bindByLabel(state.label, state.key.c_str());

      const size_t scoreCount = std::count_if(m_scoreBindings.begin(),
         m_scoreBindings.end(), [](const StateBinding& binding) { return binding.bound; });
      const size_t finalScoreCount = std::count_if(m_finalScoreBindings.begin(),
         m_finalScoreBindings.end(), [](const StateBinding& binding) { return binding.bound; });
      const bool hasGameOver = std::any_of(m_namedStates.begin(), m_namedStates.end(),
         [](const NamedState& state) { return state.key == "game_over" && state.binding.bound; });
      const bool hasCompleteLiveScores = !m_scoreBindings.empty()
         && scoreCount == m_scoreBindings.size();
      const bool hasCompleteFinalScores = !m_finalScoreBindings.empty()
         && finalScoreCount == m_finalScoreBindings.size();
      if (!hasCompleteLiveScores && (!hasCompleteFinalScores || !hasGameOver))
      {
         m_stateSource = {};
         continue;
      }
      if (!hasCompleteLiveScores)
         m_scoreBindings.clear();
      if (!hasCompleteFinalScores)
         m_finalScoreBindings.clear();

      const size_t namedStateCount = std::count_if(m_namedStates.begin(), m_namedStates.end(),
         [](const NamedState& state) { return state.binding.bound; });
      LOGI("PinMAME exposed %u live score, %u final score and %u game-state value(s) for %s",
         static_cast<unsigned int>(scoreCount), static_cast<unsigned int>(finalScoreCount),
         static_cast<unsigned int>(namedStateCount), m_gameId.c_str());
      return true;
   }
   return false;
}

bool PinMameStateTracker::ReadInteger(const StateBinding& binding, int64_t& value) const
{
   if (!binding.bound || m_stateSource.GetState == nullptr)
      return false;

#define READ_STATE(TYPE_FLAG, TYPE) \
   if ((binding.typeMask & TYPE_FLAG) != 0) \
   { \
      TYPE result = 0; \
      if (m_stateSource.GetState(binding.inputIndex, TYPE_FLAG, &result) != 0) \
         return false; \
      value = static_cast<int64_t>(result); \
      return true; \
   }
   READ_STATE(CTLPI_STATE_TYPE_INT64, int64_t)
   READ_STATE(CTLPI_STATE_TYPE_UINT64, uint64_t)
   READ_STATE(CTLPI_STATE_TYPE_INT32, int32_t)
   READ_STATE(CTLPI_STATE_TYPE_UINT32, uint32_t)
   READ_STATE(CTLPI_STATE_TYPE_INT16, int16_t)
   READ_STATE(CTLPI_STATE_TYPE_UINT16, uint16_t)
   READ_STATE(CTLPI_STATE_TYPE_INT8, int8_t)
   READ_STATE(CTLPI_STATE_TYPE_UINT8, uint8_t)
#undef READ_STATE
   return false;
}

bool PinMameStateTracker::ReadSnapshot()
{
   vector<int64_t> playerScores(m_scoreBindings.size(), 0);
   for (size_t i = 0; i < m_scoreBindings.size(); ++i)
   {
      if (!m_scoreBindings[i].bound)
         continue;
      if (!ReadInteger(m_scoreBindings[i], playerScores[i]))
         return false;
      playerScores[i] = std::max<int64_t>(playerScores[i], 0);
   }

   vector<int64_t> finalScores(m_finalScoreBindings.size(), 0);
   for (size_t i = 0; i < m_finalScoreBindings.size(); ++i)
   {
      if (!m_finalScoreBindings[i].bound)
         continue;
      if (!ReadInteger(m_finalScoreBindings[i], finalScores[i]))
         return false;
      finalScores[i] = std::max<int64_t>(finalScores[i], 0);
   }

   std::unordered_map<string, int64_t> decodedValues;
   for (const NamedState& state : m_namedStates)
   {
      if (!state.binding.bound)
         continue;
      int64_t value = 0;
      if (!ReadInteger(state.binding, value))
         return false;
      if (state.invertBoolean)
         value = value == 0 ? 1 : 0;
      decodedValues[state.key] = value;
   }

   m_playerScores = std::move(playerScores);
   m_finalScores = std::move(finalScores);
   m_decodedValues = std::move(decodedValues);
   return true;
}

vector<int64_t> PinMameStateTracker::BuildFinalScoresSnapshot(
   const vector<int64_t>& playerScores, bool gameOverObserved,
   size_t& authoritativeCount) const
{
   vector<int64_t> snapshot = playerScores;
   authoritativeCount = 0;
   if (m_finalScoreBindings.empty() || m_finalScores.empty())
      return snapshot;

   if (m_finalScoresMostRecentFirst)
   {
      if (!gameOverObserved)
         return snapshot;
      const size_t total = m_finalScores.size();
      size_t players = 0;
      const auto playerCount = m_maxGameStateValues.find("player_count");
      if (playerCount != m_maxGameStateValues.end() && playerCount->second >= 1)
         players = std::min(static_cast<size_t>(playerCount->second), total);
      else if (m_hasFinalScoresBaseline && m_finalScoresBaseline.size() == total)
      {
         for (size_t shift = 0; shift <= total; ++shift)
         {
            bool match = true;
            for (size_t i = shift; i < total && match; ++i)
               match = m_finalScores[i] == m_finalScoresBaseline[i - shift];
            if (match)
            {
               players = shift;
               break;
            }
         }
         if (players == 0)
            return snapshot;
      }
      else
         players = 1;

      snapshot.assign(players, 0);
      for (size_t i = 0; i < players; ++i)
         snapshot[i] = m_finalScores[players - 1 - i];
      authoritativeCount = players;
      return snapshot;
   }

   if (snapshot.size() < m_finalScores.size())
      snapshot.resize(m_finalScores.size(), 0);
   for (size_t i = 0; i < m_finalScores.size(); ++i)
      snapshot[i] = m_finalScores[i];
   for (size_t i = m_finalScores.size(); i < snapshot.size(); ++i)
      snapshot[i] = 0;
   authoritativeCount = m_finalScores.size();
   return snapshot;
}

void PinMameStateTracker::Stop()
{
   if (m_monitoringStarted && !m_summarySent && !m_gameId.empty())
   {
      size_t authoritativeCount = 0;
      const vector<int64_t> finalScores = BuildFinalScoresSnapshot(m_playerScores,
         m_gameOverPending, authoritativeCount);
      const bool hasScore = std::any_of(finalScores.begin(), finalScores.end(),
         [](int64_t value) { return value > 0; })
         || std::any_of(m_highestScores.begin(), m_highestScores.end(),
            [](int64_t value) { return value > 0; });
      const bool writeExitFallback = m_hasBeenInPlay && hasScore;
      if (writeExitFallback)
         LOGI("No confirmed game-over before VPX exit; writing the active session as an exit fallback");
      FinalizeSession(finalScores, writeExitFallback, authoritativeCount);
      m_summarySent = true;
   }
   m_monitoringStarted = false;
   m_stateSource = {};
   m_scoreBindings.clear();
   m_finalScoreBindings.clear();
}

void PinMameStateTracker::Poll()
{
   if (!m_monitoringStarted || !ReadSnapshot())
      return;

   const auto gameOverIt = m_decodedValues.find("game_over");
   bool isGameOver = gameOverIt != m_decodedValues.end() && gameOverIt->second != 0;

   if (isGameOver && !m_ignoreGameOver && !m_hasBeenInPlay
      && m_prevPlayerScores.size() == m_playerScores.size())
   {
      bool increased = false;
      for (size_t i = 0; i < m_playerScores.size() && !increased; ++i)
         increased = m_playerScores[i] > m_prevPlayerScores[i];
      if (increased && ++m_gameOverAnomalies >= kGameOverAnomalyLimit)
      {
         LOGW("game_over reads as asserted while scores are rising; ignoring the game_over field for this session");
         m_ignoreGameOver = true;
      }
   }
   if (m_ignoreGameOver)
      isGameOver = false;
   if (m_prevPlayerScores != m_playerScores)
      m_scoresStableSince = std::chrono::steady_clock::now();
   m_prevPlayerScores = m_playerScores;

   bool newGameStarted = false;
   if (!isGameOver && m_hasBeenInPlay && !m_playerScores.empty())
   {
      const bool allZero = std::all_of(m_playerScores.begin(), m_playerScores.end(),
         [](int64_t value) { return value == 0; });
      const bool hadScore = std::any_of(m_highestScores.begin(), m_highestScores.end(),
         [](int64_t value) { return value > 0; });
      newGameStarted = allZero && hadScore;
   }

   if (newGameStarted)
   {
      if (!m_summarySent)
      {
         LOGI("New game started for rom %s before the previous game-over was confirmed; finalizing the previous game", m_gameId.c_str());
         size_t authoritativeCount = 0;
         const vector<int64_t> finalScores = BuildFinalScoresSnapshot(m_playerScores,
            m_gameOverPending, authoritativeCount);
         FinalizeSession(finalScores, true, authoritativeCount);
         m_summarySent = true;
      }
      m_hasBeenInPlay = false;
   }

   bool sessionReset = false;
   if (!isGameOver)
   {
      m_gameOverPending = false;
      if (!m_hasBeenInPlay)
      {
         sessionReset = true;
         m_summarySent = false;
         m_highestScores.clear();
         m_maxGameStateValues.clear();
         m_sessionStartRealTime = std::chrono::steady_clock::now();
         if (m_finalScoresMostRecentFirst)
         {
            m_finalScoresBaseline = m_finalScores;
            m_hasFinalScoresBaseline = true;
         }
      }
      m_gameOverLast = false;
      m_hasBeenInPlay = true;
   }
   else if (!m_gameOverPending)
   {
      m_gameOverPending = true;
      m_gameOverSince = std::chrono::steady_clock::now();
   }

   if (m_hasBeenInPlay && !sessionReset)
   {
      if (m_highestScores.size() < m_playerScores.size())
         m_highestScores.resize(m_playerScores.size(), 0);
      for (size_t i = 0; i < m_playerScores.size(); ++i)
         m_highestScores[i] = std::max(m_highestScores[i], m_playerScores[i]);
      for (const auto& [key, value] : m_decodedValues)
      {
         if (key == "player_count")
         {
            const bool confirmed = value == m_lastPlayerCountRead;
            m_lastPlayerCountRead = value;
            if (isGameOver || !confirmed || value < 1 || value > 8)
               continue;
         }
         auto it = m_maxGameStateValues.find(key);
         if (it == m_maxGameStateValues.end())
            m_maxGameStateValues[key] = value;
         else
            it->second = std::max(it->second, value);
      }
   }

   const auto confirmSince = m_playerScores.empty() ? m_gameOverSince : m_scoresStableSince;
   const bool gameOverConfirmed = isGameOver && m_gameOverPending
      && std::chrono::steady_clock::now() - confirmSince
         >= std::chrono::seconds(kScoreStableConfirmSeconds);
   if (gameOverConfirmed && m_hasBeenInPlay && !m_gameOverLast)
   {
      LOGI("Game play for rom %s is over", m_gameId.c_str());
      if (!m_summarySent)
      {
         size_t authoritativeCount = 0;
         const vector<int64_t> finalScores = BuildFinalScoresSnapshot(m_playerScores, true,
            authoritativeCount);
         FinalizeSession(finalScores, true, authoritativeCount);
         m_summarySent = true;
      }
      m_gameOverLast = true;
      m_hasBeenInPlay = false;
   }
}

nlohmann::json PinMameStateTracker::BuildCompletedGameState() const
{
   nlohmann::json gameState = nlohmann::json::object();
   for (const auto& [key, value] : m_maxGameStateValues)
   {
      if (key == "game_over" || key == "current_player" || key == "current_ball"
         || value == 0)
         continue;
      gameState[key] = value;
   }
   return gameState;
}

void PinMameStateTracker::FinalizeSession(const vector<int64_t>& finalScores,
   bool writeScoresFile, size_t authoritativeCount)
{
   const auto endTime = m_gameOverPending ? m_gameOverSince
                                          : std::chrono::steady_clock::now();
   const int64_t duration = std::max<int64_t>(0,
      std::chrono::duration_cast<std::chrono::seconds>(endTime - m_sessionStartRealTime).count());

   vector<int64_t> bestScores = finalScores;
   const auto playerCount = m_maxGameStateValues.find("player_count");
   if (playerCount != m_maxGameStateValues.end() && playerCount->second >= 1
      && playerCount->second < static_cast<int64_t>(bestScores.size()))
      bestScores.resize(static_cast<size_t>(playerCount->second));
   for (size_t i = authoritativeCount; i < bestScores.size(); ++i)
      if (i < m_highestScores.size() && m_highestScores[i] > bestScores[i])
         bestScores[i] = m_highestScores[i];

   string scoreList;
   for (int64_t score : bestScores)
      scoreList += (scoreList.empty() ? ""s : ", "s) + std::to_string(score);
   LOGI("Game summary: rom=%s, duration=%llds, scores=[%s]", m_gameId.c_str(),
      static_cast<long long>(duration), scoreList.c_str());

   if (!writeScoresFile)
      return;
   if (duration < kMinGameDurationSeconds)
   {
      LOGI("Ignoring game shorter than %ds (was %llds) - not writing scores.json",
         kMinGameDurationSeconds, static_cast<long long>(duration));
      return;
   }

   CompletedGameRecord record;
   record.tablePath = m_tablePath;
   record.outputPath = m_outputPath;
   record.rom = m_gameId;
   record.scores = bestScores;
   record.gameDuration = duration;
   record.gameState = BuildCompletedGameState();
   if (ScoresFileWriter::AppendCompletedGame(record) && m_scoreSavedCallback != nullptr)
      m_scoreSavedCallback(m_scoreSavedUserData);
}

}
