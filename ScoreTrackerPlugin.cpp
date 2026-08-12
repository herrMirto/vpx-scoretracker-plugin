// license:GPLv3+

#include "common.h"
#include "B2STracker.h"
#include "PinMameStateTracker.h"
#include "NotificationOverlay.h"

#include <algorithm>
#include <filesystem>
#include <vector>

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
static unsigned int onStateSourcesChangedId = 0;
static unsigned int getStateSourcesId = 0;
#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
static unsigned int onControllersChangedId = 0;
static unsigned int getControllersId = 0;
static unsigned int getPinmameMachineStateId = 0;
static unsigned int onB2SStateChangeId = 0;
#else
static unsigned int onGameStartId = 0;
static unsigned int onGameEndId = 0;
#endif
static unsigned int onVpxGameEndId = 0;

static PinMameStateTracker* tracker = nullptr;
static B2STracker* b2sTracker = nullptr;
static string activeGameId;
static bool pollActive = false;
static bool pollScheduled = false;
static NotificationOverlay notificationOverlay;

MSGPI_STRING_VAL_SETTING(mapsFolderProp, "nvram_maps_folder", "NVRAM Maps Folder",
   "Folder with the PinMAME NVRAM maps (index.json, maps/, platforms/). When empty, the maps shipped with the plugin are used.", true, "", 1024);
MSGPI_INT_VAL_SETTING(pollIntervalMsProp, "PollIntervalMs", "Polling Interval (ms)", "Interval used to inspect the machine state. Higher values reduce overhead.", true, 50, 5000, 250);
MSGPI_STRING_VAL_SETTING(outputFolderProp, "OutputFolder", "Scores Output Folder", "Folder where scores.json is written. When empty, it is written next to the table file.", true, "", 1024);
MSGPI_BOOL_VAL_SETTING(notificationsProp, "Notifications", "Notifications",
   "Show a pop-up over the playfield when a score is saved.", true, true);

static void HideNotification(void* userData)
{
   notificationOverlay.Hide();
}

static void OnScoreSaved(void* userData)
{
   if (!notificationsProp_Val || msgApi == nullptr)
      return;
   if (notificationOverlay.ShowScoreSaved())
      msgApi->RunOnMainThread(endpointId, 2.0, HideNotification, nullptr);
}

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
   if (!pollActive)
      return;
   if (tracker != nullptr)
      tracker->Poll();
   else if (b2sTracker != nullptr)
      b2sTracker->Poll();
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
   if (b2sTracker != nullptr)
   {
      b2sTracker->Stop();
      delete b2sTracker;
      b2sTracker = nullptr;
   }
}

static string ResolveTablePath()
{
   if (vpxApi == nullptr)
      return {};
   VPXTableInfo tableInfo;
   vpxApi->GetTableInfo(&tableInfo);
   return tableInfo.path != nullptr ? tableInfo.path : "";
}

static bool StartPinMameTrackerForGame(const string& gameId, uint32_t controllerEndpointId = 0)
{
   if (gameId.empty())
      return false;

   // Controller source changes can be broadcast more than once for the same running machine.
   if (tracker != nullptr && activeGameId == gameId
      && (controllerEndpointId == 0 || tracker->GetControllerEndpointId() == controllerEndpointId))
      return true;

   const string mapsPath = ResolveMapsPath();
   string mapDetail;
   const PinMameStateTracker::MapStatus mapStatus = PinMameStateTracker::ProbeMap(gameId, mapsPath, mapDetail);
   if (mapStatus == PinMameStateTracker::MapStatus::NotFound)
   {
      LOGI("No NVRAM map for %s (%s); checking other score providers", gameId.c_str(), mapDetail.c_str());
      return false;
   }
   if (mapStatus == PinMameStateTracker::MapStatus::Error)
   {
      LOGE("Map lookup failed for %s: %s", gameId.c_str(), mapDetail.c_str());
      return false;
   }

   StopTracker();
   const string tablePath = ResolveTablePath();

   LOGI("Tracking decoded PinMAME states for rom %s using schema %s", gameId.c_str(), mapDetail.c_str());
   tracker = new PinMameStateTracker();
   if (!tracker->Start(msgApi, endpointId, controllerEndpointId, getStateSourcesId, gameId,
      mapsPath, tablePath, outputFolderProp_Val, OnScoreSaved))
   {
      LOGI("PinMAME did not publish usable decoded score states for %s", gameId.c_str());
      delete tracker;
      tracker = nullptr;
      return false;
   }
   activeGameId = gameId;
   pollActive = true;
   SchedulePoll();
   return true;
}

#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)

// PinMAME-specific machine information message. The build only imports libpinmame.h, so keep this
// small wire definition local instead of depending on PinMAME's optional plugin header as well.
struct PinmameMachineStateMsg
{
   int version;
   const char* game;
   const char* rom;
   uint64_t hardwareGen;
};

static string ResolvePinmameRom(const ControllerDef& controller)
{
   PinmameMachineStateMsg state { 1, nullptr, nullptr, 0 };
   msgApi->SendMsg(endpointId, getPinmameMachineStateId, controller.endpointId, &state);
   if (state.rom != nullptr && state.rom[0] != '\0')
      return state.rom;

   // Older providers may enumerate the controller without implementing GetMachineState. In that
   // case, use the requested game id after removing PinMAME's controller namespace prefix.
   const string enumeratedId = controller.gameId != nullptr ? controller.gameId : "";
   static constexpr char kPinmameGamePrefix[] = "pinmame::";
   if (enumeratedId.rfind(kPinmameGamePrefix, 0) == 0)
      return enumeratedId.substr(sizeof(kPinmameGamePrefix) - 1);
   return enumeratedId;
}

static void RefreshControllers()
{
   GetControllersMsg query { 0, 0, nullptr };
   msgApi->BroadcastMsg(endpointId, getControllersId, &query);
   if (query.count == 0)
      return;

   vector<ControllerDef> controllers(query.count);
   query = { static_cast<unsigned int>(controllers.size()), 0, controllers.data() };
   msgApi->BroadcastMsg(endpointId, getControllersId, &query);

   const unsigned int count = std::min(query.count, static_cast<unsigned int>(controllers.size()));
   const ControllerDef* pinmameController = nullptr;
   const ControllerDef* b2sController = nullptr;
   for (unsigned int i = 0; i < count; ++i)
   {
      if (controllers[i].gameId == nullptr)
         continue;
      const string enumeratedId(controllers[i].gameId);
      if (pinmameController == nullptr && enumeratedId.rfind("pinmame::", 0) == 0)
         pinmameController = &controllers[i];
      else if (b2sController == nullptr && enumeratedId.rfind("b2s::", 0) == 0)
         b2sController = &controllers[i];
   }

   // Prefer the decoded states published by PinMAME. If the ROM has no usable game-state source,
   // fall back to direct player scores published by B2S.
   if (pinmameController != nullptr && StartPinMameTrackerForGame(
      ResolvePinmameRom(*pinmameController), pinmameController->endpointId))
      return;

   if (b2sController != nullptr)
   {
      const string gameId(b2sController->gameId);
      if (b2sTracker != nullptr && b2sTracker->GetControllerEndpointId() == b2sController->endpointId
         && b2sTracker->GetGameId() == gameId)
         return;

      StopTracker();
      LOGI("No supported PinMAME map is active; monitoring direct B2S player scores for %s", gameId.c_str());
      b2sTracker = new B2STracker();
      if (!b2sTracker->Start(msgApi, endpointId, b2sController->endpointId, getStateSourcesId,
         gameId, ResolveTablePath(), outputFolderProp_Val, OnScoreSaved))
      {
         delete b2sTracker;
         b2sTracker = nullptr;
         return;
      }
      activeGameId = gameId;
      pollActive = true;
      SchedulePoll();
   }
}

static void OnControllersChanged(const unsigned int eventId, void* userData, void* msgData)
{
   // An empty enumeration means the machine stopped. Keep the tracker alive until VPX game-end:
   // Stop() may still need to persist the played session as an exit fallback.
   RefreshControllers();
}

struct B2SPluginEvent
{
   uint8_t type;
   int32_t index;
   int32_t value;
};

static void OnB2SStateChange(const unsigned int eventId, void* userData, void* msgData)
{
   if (b2sTracker == nullptr || msgData == nullptr)
      return;
   const B2SPluginEvent* event = static_cast<const B2SPluginEvent*>(msgData);
   b2sTracker->OnB2SStateChange(event->type, event->index, event->value);
}

#else

static void OnGameStart(const unsigned int eventId, void* userData, void* msgData)
{
   const CtlOnGameStateChgMsg* msg = static_cast<const CtlOnGameStateChgMsg*>(msgData);
   if (msg != nullptr && msg->gameId != nullptr)
      StartPinMameTrackerForGame(msg->gameId);
}

static void OnGameEnd(const unsigned int eventId, void* userData, void* msgData)
{
   // Keep the session open: the controller can stop while the plugin has not confirmed the
   // game-over yet (its confirmation delay may still be running). The session is finalized,
   // and persisted if a game was played, when VPX ends or when another game starts.
}

#endif

static void OnStateSourcesChanged(const unsigned int eventId, void* userData, void* msgData)
{
   if (tracker != nullptr)
      tracker->RefreshStateSource();
#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
   else
      RefreshControllers();
   if (b2sTracker != nullptr)
      b2sTracker->RefreshStateSource();
#endif
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
   msgApi->RegisterSetting(endpointId, &notificationsProp);

   msgApi->BroadcastMsg(endpointId, getVpxApiId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API), &vpxApi);
   getStateSourcesId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_STATE_GET_SRC_MSG);
   msgApi->SubscribeMsg(endpointId,
      onStateSourcesChangedId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_STATE_ON_SRC_CHG_MSG),
      OnStateSourcesChanged, nullptr);

#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
   getControllersId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_CONTROLLERS_GET_MSG);
   getPinmameMachineStateId = msgApi->GetMsgID("PinMAME", "GetMachineState");
   msgApi->SubscribeMsg(endpointId,
      onB2SStateChangeId = msgApi->GetMsgID("B2S", "OnStateChange"),
      OnB2SStateChange, nullptr);
   msgApi->SubscribeMsg(endpointId,
      onControllersChangedId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_CONTROLLERS_ON_CHG_MSG),
      OnControllersChanged, nullptr);
   // A controller may already be running if ScoreTracker was loaded or enabled after PinMAME.
   RefreshControllers();
#else
   msgApi->SubscribeMsg(endpointId, onGameStartId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_START), OnGameStart, nullptr);
   msgApi->SubscribeMsg(endpointId, onGameEndId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_END), OnGameEnd, nullptr);
#endif
   msgApi->SubscribeMsg(endpointId, onVpxGameEndId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END), OnVpxGameEnd, nullptr);
}

MSGPI_EXPORT void MSGPIAPI ScoreTrackerPluginUnload()
{
   StopTracker();
   notificationOverlay.Hide();
   msgApi->UnsubscribeMsg(onStateSourcesChangedId, OnStateSourcesChanged, nullptr);

#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
   msgApi->UnsubscribeMsg(onControllersChangedId, OnControllersChanged, nullptr);
   msgApi->UnsubscribeMsg(onB2SStateChangeId, OnB2SStateChange, nullptr);
#else
   msgApi->UnsubscribeMsg(onGameStartId, OnGameStart, nullptr);
   msgApi->UnsubscribeMsg(onGameEndId, OnGameEnd, nullptr);
#endif
   msgApi->UnsubscribeMsg(onVpxGameEndId, OnVpxGameEnd, nullptr);
#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
   msgApi->ReleaseMsgID(onControllersChangedId);
   msgApi->ReleaseMsgID(getControllersId);
   msgApi->ReleaseMsgID(getPinmameMachineStateId);
   msgApi->ReleaseMsgID(onB2SStateChangeId);
#else
   msgApi->ReleaseMsgID(onGameStartId);
   msgApi->ReleaseMsgID(onGameEndId);
#endif
   msgApi->ReleaseMsgID(onStateSourcesChangedId);
   msgApi->ReleaseMsgID(getStateSourcesId);
   msgApi->ReleaseMsgID(onVpxGameEndId);
   msgApi->ReleaseMsgID(getVpxApiId);
   msgApi->FlushPendingCallbacks(endpointId);

   vpxApi = nullptr;
   msgApi = nullptr;
}
