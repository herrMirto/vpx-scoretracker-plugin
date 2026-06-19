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

// Settings definitions
MSGPI_STRING_VAL_SETTING(mapsFolderProp, "nvram_maps_folder", "JSON Maps Folder", "Folder where JSON maps and index.json are located", true, "maps", 1024);
MSGPI_INT_VAL_SETTING(wsPortProp, "Port", "WebSocket Port", "Port number for the WebSocket server", true, 1024, 65535, 8889);

static void OnControllerGameStart(const unsigned int eventId, void* userData, void* msgData)
{
   const CtlOnGameStartMsg* msg = static_cast<const CtlOnGameStartMsg*>(msgData);
   if (msg == nullptr || msg->gameId == nullptr)
      return;

   std::string tablePath = "";
   if (vpxApi != nullptr) {
      VPXTableInfo tableInfo;
      vpxApi->GetTableInfo(&tableInfo);
      if (tableInfo.path != nullptr) {
         tablePath = tableInfo.path;
      }
   }
   std::string tableName = tablePath.empty() ? "unknown" : std::filesystem::path(tablePath).filename().string();

   LPI_LOGI_CPP("[INFO] - Table "s + tableName + " with rom "s + msg->gameId + " started");
   std::cout << "[INFO] - Table " << tableName << " with rom " << msg->gameId << " started" << std::endl;

   if (scoreTracker != nullptr) {
      scoreTracker->Stop();
      delete scoreTracker;
      scoreTracker = nullptr;
   }

   scoreTracker = new ScoreTracker(msgApi, endpointId);
   const bool pinmameTracking = scoreTracker->Start(msg->gameId, mapsFolderProp_Val, wsPortProp_Val, tablePath);

   // EM / original tables have no NVRAM map; their scores arrive through the
   // intercepted B2S.Server calls instead.
   if (b2sTracker != nullptr) {
      b2sTracker->SetPinmameActive(pinmameTracking);
      b2sTracker->OnGameStart(msg->gameId, wsPortProp_Val, tablePath);
   }
}

static void OnControllerGameEnd(const unsigned int eventId, void* userData, void* msgData)
{
   LPI_LOGI_CPP("Game ending"s);
   if (scoreTracker != nullptr) {
      scoreTracker->Stop();
      delete scoreTracker;
      scoreTracker = nullptr;
   }
   if (b2sTracker != nullptr) {
      b2sTracker->OnGameEnd();
      b2sTracker->SetPinmameActive(false);
   }
}

static void OnVpxGameEnd(const unsigned int eventId, void* userData, void* msgData)
{
   // Player shutdown: make sure an EM session that never saw a controller
   // game-end still flushes its summary.
   if (b2sTracker != nullptr)
      b2sTracker->OnGameEnd();
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
