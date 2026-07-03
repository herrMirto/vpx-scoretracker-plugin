// license:GPLv3+

#include <cassert>
#include <string>
#include <filesystem>
#include <iostream>
#include "plugins/MsgPlugin.h"
#include "plugins/LoggingPlugin.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/VPXPlugin.h"
#include "plugins/ScriptablePlugin.h"
#include "ScoreTracker.h"
#include "B2STracker.h"

using namespace std::string_literals;

namespace ScoreTrackerPlugin {

LPI_IMPLEMENT_CPP // Implement shared log support

static const MsgPluginAPI* msgApi = nullptr;
static uint32_t endpointId;
static VPXPluginAPI* vpxApi = nullptr;

static unsigned int onControllerGameStartId = 0;
static unsigned int onControllerGameEndId = 0;
static unsigned int getVpxApiId = 0;
static unsigned int getScriptApiId = 0;
static unsigned int onPluginLoadedId = 0;
static unsigned int onVpxGameEndId = 0;
static const ScriptablePluginAPI* scriptApi = nullptr;

static ScoreTracker* scoreTracker = nullptr;
static B2STracker* b2sTracker = nullptr;

enum class TrackingSource {
   None,
   NvramMap,
   B2SFallback,
};

static TrackingSource trackingSource = TrackingSource::None;
static std::string activeGameId;

static const char* TrackingSourceName()
{
   switch (trackingSource) {
   case TrackingSource::NvramMap: return "NVRAM map";
   case TrackingSource::B2SFallback: return "B2S fallback";
   default: return "none";
   }
}

// Settings definitions
MSGPI_STRING_VAL_SETTING(mapsFolderProp, "nvram_maps_folder", "JSON Maps Folder", "Folder where JSON maps and index.json are located", true, "maps", 1024);
MSGPI_INT_VAL_SETTING(wsPortProp, "Port", "WebSocket Port", "Port number for the WebSocket server", true, 1024, 65535, 8889);
MSGPI_INT_VAL_SETTING(pollIntervalMsProp, "PollIntervalMs", "Polling Interval (ms)", "Interval used to inspect score state. Higher values reduce VPX process overhead.", true, 50, 5000, 250);
MSGPI_BOOL_VAL_SETTING(enableWebSocketProp, "EnableWebSocket", "Enable WebSocket Output", "Enable live WebSocket/HTTP output. Leave disabled when only scores.json persistence is needed.", true, false);

static void OnControllerGameStart(const unsigned int eventId, void* userData, void* msgData)
{
   const CtlOnGameStartMsg* msg = static_cast<const CtlOnGameStartMsg*>(msgData);
   if (msg == nullptr || msg->gameId == nullptr || msg->gameId[0] == '\0') {
      LPI_LOGI_CPP(std::string("[INFO] - Ignoring controller game-start with empty gameId; active source remains ") + TrackingSourceName());
      return;
   }

   const std::string gameId(msg->gameId);

   std::string tablePath = "";
   if (vpxApi != nullptr) {
      VPXTableInfo tableInfo;
      vpxApi->GetTableInfo(&tableInfo);
      if (tableInfo.path != nullptr) {
         tablePath = tableInfo.path;
      }
   }
   std::string tableName = tablePath.empty() ? "unknown" : std::filesystem::path(tablePath).filename().string();

   std::string mapDetail;
   const ScoreTracker::MapStatus mapStatus = ScoreTracker::ProbeMap(gameId, mapsFolderProp_Val, mapDetail);
   const bool hasMap = mapStatus == ScoreTracker::MapStatus::Available;

   // A mapped session is authoritative. Duplicate or fallback lifecycle events
   // must never restart it or make us inspect a second data source.
   if (trackingSource == TrackingSource::NvramMap && activeGameId == gameId) {
      LPI_LOGI_CPP("[INFO] - Ignoring duplicate game-start for "s + gameId + "; NVRAM map remains authoritative");
      return;
   }
   if (trackingSource == TrackingSource::NvramMap && !hasMap) {
      LPI_LOGI_CPP("[INFO] - Ignoring lower-priority controller game-start for "s + gameId
         + "; active NVRAM map for "s + activeGameId + " remains authoritative. Probe result: "s + mapDetail);
      return;
   }

   if (mapStatus == ScoreTracker::MapStatus::Error) {
      if (trackingSource == TrackingSource::B2SFallback && b2sTracker != nullptr)
         b2sTracker->DiscardSession();
      if (b2sTracker != nullptr)
         b2sTracker->SetPinmameActive(true);
      trackingSource = TrackingSource::None;
      activeGameId.clear();
      LPI_LOGE_CPP("[ERROR] - Map lookup failed for "s + gameId + ": "s + mapDetail
         + ". No fallback will be activated because map priority cannot be determined.");
      return;
   }

   if (trackingSource == TrackingSource::B2SFallback && activeGameId == gameId && !hasMap) {
      LPI_LOGI_CPP("[INFO] - Ignoring duplicate game-start for "s + gameId + "; B2S remains the sole fallback source");
      return;
   }

   if (hasMap) {
      if (trackingSource == TrackingSource::B2SFallback && b2sTracker != nullptr)
         b2sTracker->DiscardSession();

      if (scoreTracker != nullptr) {
         scoreTracker->Stop();
         delete scoreTracker;
         scoreTracker = nullptr;
      }

      trackingSource = TrackingSource::NvramMap;
      activeGameId = gameId;
      if (b2sTracker != nullptr)
         b2sTracker->SetPinmameActive(true);

      LPI_LOGI_CPP("[INFO] - Tracking mode selected: NVRAM map (authoritative), table="s + tableName
         + ", rom="s + gameId + ", map="s + mapDetail);
      std::cout << "[ScoreTracker] Using authoritative NVRAM map for " << gameId << std::endl;

      scoreTracker = new ScoreTracker(msgApi, endpointId);
      if (!scoreTracker->Start(gameId, mapsFolderProp_Val, wsPortProp_Val, pollIntervalMsProp_Val, enableWebSocketProp_Val, tablePath))
         LPI_LOGE_CPP("[ERROR] - NVRAM map exists but could not be loaded for "s + gameId + "; fallbacks remain disabled");
      return;
   }

   if (scoreTracker != nullptr) {
      scoreTracker->Stop();
      delete scoreTracker;
      scoreTracker = nullptr;
   }

   if (b2sTracker != nullptr) {
      if (trackingSource == TrackingSource::B2SFallback && activeGameId != gameId)
         b2sTracker->OnGameEnd();
      b2sTracker->SetPinmameActive(false);
      b2sTracker->OnGameStart(gameId, wsPortProp_Val, pollIntervalMsProp_Val, enableWebSocketProp_Val, tablePath);
   }
   trackingSource = TrackingSource::B2SFallback;
   activeGameId = gameId;
   LPI_LOGI_CPP("[INFO] - No NVRAM map for "s + gameId + " ("s + mapDetail
      + "); tracking mode selected: B2S fallback, table="s + tableName);
   std::cout << "[ScoreTracker] No NVRAM map for " << gameId << "; B2S fallback armed" << std::endl;
}

static void OnControllerGameEnd(const unsigned int eventId, void* userData, void* msgData)
{
   if (trackingSource == TrackingSource::NvramMap) {
      LPI_LOGI_CPP("[INFO] - Ignoring generic controller game-end; authoritative NVRAM session remains active until VPX ends or another mapped ROM starts"s);
      return;
   }

   if (trackingSource == TrackingSource::B2SFallback && b2sTracker != nullptr) {
      LPI_LOGI_CPP("[INFO] - Ending B2S fallback session"s);
      b2sTracker->OnGameEnd();
   }
   trackingSource = TrackingSource::None;
   activeGameId.clear();
}

static void OnVpxGameEnd(const unsigned int eventId, void* userData, void* msgData)
{
   LPI_LOGI_CPP(std::string("[INFO] - VPX game-end; stopping active tracking source: ") + TrackingSourceName());
   if (scoreTracker != nullptr) {
      scoreTracker->Stop();
      delete scoreTracker;
      scoreTracker = nullptr;
   }
   if (b2sTracker != nullptr)
      b2sTracker->OnGameEnd();
   trackingSource = TrackingSource::None;
   activeGameId.clear();
}

static void OnPluginLoaded(const unsigned int eventId, void* userData, void* msgData)
{
   // A B2S plugin may have (re-)registered the B2S.Server override after us;
   // re-claim it so our score-capturing proxy stays in front.
   if (b2sTracker != nullptr)
      b2sTracker->InstallProxy(scriptApi);
}

} // namespace ScoreTrackerPlugin

using namespace ScoreTrackerPlugin;

MSGPI_EXPORT void MSGPIAPI ScoreTrackerPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
   msgApi = api;
   endpointId = sessionId;

   LPISetup(endpointId, msgApi);

   // Register settings
   msgApi->RegisterSetting(endpointId, &mapsFolderProp);
   msgApi->RegisterSetting(endpointId, &wsPortProp);
   msgApi->RegisterSetting(endpointId, &pollIntervalMsProp);
   msgApi->RegisterSetting(endpointId, &enableWebSocketProp);

   // Fetch VPX API
   msgApi->BroadcastMsg(endpointId, getVpxApiId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API), &vpxApi);

   // Fetch the scripting API and install the B2S.Server score-capture proxy
   // (EM / original tables report scores through B2S script calls).
   msgApi->BroadcastMsg(endpointId, getScriptApiId = msgApi->GetMsgID(SCRIPTPI_NAMESPACE, SCRIPTPI_MSG_GET_API), &scriptApi);
   b2sTracker = new B2STracker(msgApi, endpointId);
   b2sTracker->InstallProxy(scriptApi);

   // Subscribe to game lifecycle events from Controller namespace
   msgApi->SubscribeMsg(endpointId, onControllerGameStartId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_START), OnControllerGameStart, nullptr);
   msgApi->SubscribeMsg(endpointId, onControllerGameEndId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_END), OnControllerGameEnd, nullptr);
   msgApi->SubscribeMsg(endpointId, onVpxGameEndId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END), OnVpxGameEnd, nullptr);
   msgApi->SubscribeMsg(endpointId, onPluginLoadedId = msgApi->GetMsgID(MSGPI_NAMESPACE, MSGPI_EVT_ON_PLUGIN_LOADED), OnPluginLoaded, nullptr);

   LPI_LOGI_CPP("ScoreTracker plugin loaded successfully."s);
}

MSGPI_EXPORT void MSGPIAPI ScoreTrackerPluginUnload()
{
   if (scoreTracker != nullptr) {
      scoreTracker->Stop();
      delete scoreTracker;
      scoreTracker = nullptr;
   }

   if (b2sTracker != nullptr) {
      delete b2sTracker;
      b2sTracker = nullptr;
   }
   if (scriptApi != nullptr) {
      scriptApi->SetCOMObjectOverride("B2S.Server", nullptr);
      scriptApi = nullptr;
   }

   if (msgApi) {
      msgApi->UnsubscribeMsg(onControllerGameStartId, OnControllerGameStart, nullptr);
      msgApi->UnsubscribeMsg(onControllerGameEndId, OnControllerGameEnd, nullptr);
      msgApi->UnsubscribeMsg(onVpxGameEndId, OnVpxGameEnd, nullptr);
      msgApi->UnsubscribeMsg(onPluginLoadedId, OnPluginLoaded, nullptr);
      msgApi->ReleaseMsgID(onControllerGameStartId);
      onControllerGameStartId = 0;
      msgApi->ReleaseMsgID(onControllerGameEndId);
      onControllerGameEndId = 0;
      msgApi->ReleaseMsgID(onVpxGameEndId);
      onVpxGameEndId = 0;
      msgApi->ReleaseMsgID(onPluginLoadedId);
      onPluginLoadedId = 0;
      msgApi->ReleaseMsgID(getScriptApiId);
      getScriptApiId = 0;
      msgApi->ReleaseMsgID(getVpxApiId);
      getVpxApiId = 0;
      msgApi->FlushPendingCallbacks(endpointId);
   }

   vpxApi = nullptr;
   msgApi = nullptr;
}
