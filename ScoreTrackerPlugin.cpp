// license:GPLv3+

#include "common.h"
#include "NvramTracker.h"

#include <filesystem>

#include "plugins/MsgPlugin.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/VPXPlugin.h"

namespace ScoreTracker
{

LPI_IMPLEMENT // Implement shared log support

static const MsgPluginAPI* msgApi = nullptr;
static uint32_t endpointId = 0;
static VPXPluginAPI* vpxApi = nullptr;

static unsigned int getVpxApiId = 0;
static unsigned int onGameStartId = 0;
static unsigned int onGameEndId = 0;
static unsigned int onVpxGameEndId = 0;

static NvramTracker* tracker = nullptr;
static string activeGameId;
static bool pollActive = false;
static bool pollScheduled = false;

MSGPI_STRING_VAL_SETTING(mapsFolderProp, "nvram_maps_folder", "NVRAM Maps Folder",
   "Folder with the PinMAME NVRAM maps (index.json, maps/, platforms/). When empty, the maps shipped with the plugin are used.", true, "", 1024);
MSGPI_INT_VAL_SETTING(pollIntervalMsProp, "PollIntervalMs", "Polling Interval (ms)", "Interval used to inspect the machine state. Higher values reduce overhead.", true, 50, 5000, 250);
MSGPI_STRING_VAL_SETTING(outputFolderProp, "OutputFolder", "Scores Output Folder", "Folder where scores.json is written. When empty, it is written next to the table file.", true, "", 1024);

static string ResolveMapsPath()
{
   if (mapsFolderProp_Val[0] != '\0')
      return string(mapsFolderProp_Val);
   // Default to the maps shipped alongside the plugin binary
   return (GetPluginPath() / "maps").string();
}

static void SchedulePoll();

static void OnPoll(void* userData)
{
   pollScheduled = false;
   if (!pollActive || tracker == nullptr)
      return;
   tracker->Poll();
   SchedulePoll();
}

static void SchedulePoll()
{
   if (pollScheduled || !pollActive)
      return;
   pollScheduled = true;
   msgApi->RunOnMainThread(endpointId, pollIntervalMsProp_Val / 1000.0, OnPoll, nullptr);
}

static void StopTracker()
{
   pollActive = false;
   activeGameId.clear();
   if (tracker != nullptr)
   {
      tracker->Stop();
      delete tracker;
      tracker = nullptr;
   }
}

static void OnGameStart(const unsigned int eventId, void* userData, void* msgData)
{
   const CtlOnGameStartMsg* msg = static_cast<const CtlOnGameStartMsg*>(msgData);
   if (msg == nullptr || msg->gameId == nullptr || msg->gameId[0] == '\0')
      return;
   const string gameId(msg->gameId);

   // A controller may broadcast the same game start more than once
   if (tracker != nullptr && activeGameId == gameId)
      return;

   StopTracker();

   const string mapsPath = ResolveMapsPath();
   string mapDetail;
   const NvramTracker::MapStatus mapStatus = NvramTracker::ProbeMap(gameId, mapsPath, mapDetail);
   if (mapStatus == NvramTracker::MapStatus::NotFound)
   {
      LOGI("No NVRAM map for %s (%s); scores will not be tracked", gameId.c_str(), mapDetail.c_str());
      return;
   }
   if (mapStatus == NvramTracker::MapStatus::Error)
   {
      LOGE("Map lookup failed for %s: %s", gameId.c_str(), mapDetail.c_str());
      return;
   }

   string tablePath;
   if (vpxApi != nullptr)
   {
      VPXTableInfo tableInfo;
      vpxApi->GetTableInfo(&tableInfo);
      if (tableInfo.path != nullptr)
         tablePath = tableInfo.path;
   }

   LOGI("Tracking scores for rom %s using map %s", gameId.c_str(), mapDetail.c_str());
   tracker = new NvramTracker();
   if (!tracker->Start(gameId, mapsPath, tablePath, outputFolderProp_Val))
   {
      LOGE("NVRAM map exists but could not be loaded for %s", gameId.c_str());
      delete tracker;
      tracker = nullptr;
      return;
   }
   activeGameId = gameId;
   pollActive = true;
   SchedulePoll();
}

static void OnGameEnd(const unsigned int eventId, void* userData, void* msgData)
{
   // Keep the session open: the controller can stop while the plugin has not confirmed the
   // game-over yet (its confirmation delay may still be running). The session is finalized,
   // and persisted if a game was played, when VPX ends or when another game starts.
}

static void OnVpxGameEnd(const unsigned int eventId, void* userData, void* msgData) { StopTracker(); }

}

using namespace ScoreTracker;

MSGPI_EXPORT void MSGPIAPI ScoreTrackerPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
   msgApi = api;
   endpointId = sessionId;

   LPISetup(endpointId, msgApi);

   msgApi->RegisterSetting(endpointId, &mapsFolderProp);
   msgApi->RegisterSetting(endpointId, &pollIntervalMsProp);
   msgApi->RegisterSetting(endpointId, &outputFolderProp);

   msgApi->BroadcastMsg(endpointId, getVpxApiId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API), &vpxApi);

   msgApi->SubscribeMsg(endpointId, onGameStartId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_START), OnGameStart, nullptr);
   msgApi->SubscribeMsg(endpointId, onGameEndId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_END), OnGameEnd, nullptr);
   msgApi->SubscribeMsg(endpointId, onVpxGameEndId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END), OnVpxGameEnd, nullptr);
}

MSGPI_EXPORT void MSGPIAPI ScoreTrackerPluginUnload()
{
   StopTracker();

   msgApi->UnsubscribeMsg(onGameStartId, OnGameStart, nullptr);
   msgApi->UnsubscribeMsg(onGameEndId, OnGameEnd, nullptr);
   msgApi->UnsubscribeMsg(onVpxGameEndId, OnVpxGameEnd, nullptr);
   msgApi->ReleaseMsgID(onGameStartId);
   msgApi->ReleaseMsgID(onGameEndId);
   msgApi->ReleaseMsgID(onVpxGameEndId);
   msgApi->ReleaseMsgID(getVpxApiId);
   msgApi->FlushPendingCallbacks(endpointId);

   vpxApi = nullptr;
   msgApi = nullptr;
}
