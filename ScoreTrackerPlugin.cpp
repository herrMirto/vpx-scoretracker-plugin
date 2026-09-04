// license:GPLv3+

#include "common.h"
#include "B2STracker.h"
#include "PinMameStateTracker.h"
#include "NotificationOverlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "plugins/MsgPlugin.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/VPXPlugin.h"
#include "pinmame/PinMAMEPlugin.h"

namespace ScoreTracker
{

LPI_IMPLEMENT // Implement shared log support

static const MsgPluginAPI* msgApi = nullptr;
static uint32_t endpointId = 0;
static VPXPluginAPI* vpxApi = nullptr;

static unsigned int getVpxApiId = 0;
#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
static unsigned int getPinmameMachineStateId = 0;
static unsigned int onB2SStateChangeId = 0;
#else
static unsigned int onGameStartId = 0;
static unsigned int onGameEndId = 0;
#endif
static unsigned int onVpxGameEndId = 0;
static unsigned int onPrepareFrameId = 0;

static std::unique_ptr<StateSourceConsumer> stateSources;
#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
using ControllerSourceConsumer = PinballPlugin::Controller::CtrlItemConsumer<ControllerDef>;
static std::unique_ptr<ControllerSourceConsumer> controllerSources;
#endif
static std::mutex trackerMutex;
static std::atomic_bool shuttingDown { false };

static PinMameStateTracker* tracker = nullptr;
static B2STracker* b2sTracker = nullptr;
static string activeGameId;
static bool pollActive = false;
static std::chrono::steady_clock::time_point nextPollAt;
static NotificationOverlay notificationOverlay;
static bool notificationHidePending = false;
static std::chrono::steady_clock::time_point notificationHideAt;

MSGPI_STRING_VAL_SETTING(mapsFolderProp, "nvram_maps_folder", "NVRAM Maps Folder",
   "Folder with the PinMAME NVRAM maps (index.json, maps/, platforms/). When empty, the maps shipped with the plugin are used.", true, "", 1024);
MSGPI_INT_VAL_SETTING(pollIntervalMsProp, "PollIntervalMs", "Polling Interval (ms)", "Interval used to inspect the machine state. Higher values reduce overhead.", true, 50, 5000, 250);
MSGPI_STRING_VAL_SETTING(outputFolderProp, "OutputFolder", "Scores Output Folder", "Folder where scores.json is written. When empty, it is written next to the table file.", true, "", 1024);
MSGPI_BOOL_VAL_SETTING(notificationsProp, "Notifications", "Notifications",
   "Show a pop-up over the playfield when a score is saved.", true, true);

static void OnScoreSaved(void* userData)
{
   if (!notificationsProp_Val)
      return;
   if (notificationOverlay.ShowScoreSaved())
   {
      notificationHidePending = true;
      notificationHideAt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
   }
}

static string ResolveMapsPath()
{
   if (mapsFolderProp_Val[0] != '\0')
      return string(mapsFolderProp_Val);
   // Default to the maps shipped alongside the plugin binary
   return (GetPluginPath() / "maps").string();
}

static void OnPrepareFrame(const unsigned int eventId, void* userData, void* msgData)
{
   std::lock_guard lock(trackerMutex);
   const auto now = std::chrono::steady_clock::now();
   if (notificationHidePending && now >= notificationHideAt)
   {
      notificationHidePending = false;
      notificationOverlay.Hide();
   }

   if (!pollActive)
      return;
   if (now < nextPollAt)
      return;
   nextPollAt = now + std::chrono::milliseconds(pollIntervalMsProp_Val);

   if (tracker != nullptr)
      tracker->Poll();
   else if (b2sTracker != nullptr)
      b2sTracker->Poll();
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
   if (!tracker->Start(stateSources.get(), controllerEndpointId, gameId,
      mapsPath, tablePath, outputFolderProp_Val, OnScoreSaved))
   {
      LOGI("PinMAME did not publish usable decoded score states for %s", gameId.c_str());
      delete tracker;
      tracker = nullptr;
      return false;
   }
   activeGameId = gameId;
   pollActive = true;
   nextPollAt = std::chrono::steady_clock::now()
      + std::chrono::milliseconds(pollIntervalMsProp_Val);
   return true;
}

#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)

static string ResolvePinmameRom(uint32_t controllerEndpointId, const string& enumeratedId)
{
   PinMAMEMachineStateMsg state { 1, nullptr, nullptr, 0 };
   msgApi->SendMsg(endpointId, getPinmameMachineStateId, controllerEndpointId, &state);
   if (state.rom != nullptr && state.rom[0] != '\0')
      return state.rom;

   // Older providers may enumerate the controller without implementing GetMachineState. In that
   // case, use the requested game id after removing PinMAME's controller namespace prefix.
   static constexpr char kPinmameGamePrefix[] = PMPI_GAMEID_PREFIX;
   if (enumeratedId.rfind(kPinmameGamePrefix, 0) == 0)
      return enumeratedId.substr(sizeof(kPinmameGamePrefix) - 1);
   return enumeratedId;
}

static void RefreshControllers()
{
   if (controllerSources == nullptr)
      return;

   uint32_t pinmameEndpointId = 0;
   uint32_t b2sEndpointId = 0;
   string pinmameGameId;
   string b2sGameId;
   controllerSources->With([&](const vector<ControllerDef>& controllers)
   {
      for (const ControllerDef& controller : controllers)
      {
         if (controller.gameId == nullptr)
            continue;
         const string enumeratedId(controller.gameId);
         if (pinmameEndpointId == 0 && enumeratedId.rfind(PMPI_GAMEID_PREFIX, 0) == 0)
         {
            pinmameEndpointId = controller.endpointId;
            pinmameGameId = enumeratedId;
         }
         else if (b2sEndpointId == 0 && enumeratedId.rfind("b2s::", 0) == 0)
         {
            b2sEndpointId = controller.endpointId;
            b2sGameId = enumeratedId;
         }
      }
   });

   // Prefer the decoded states published by PinMAME. If the ROM has no usable game-state source,
   // fall back to direct player scores published by B2S.
   if (pinmameEndpointId != 0 && StartPinMameTrackerForGame(
      ResolvePinmameRom(pinmameEndpointId, pinmameGameId), pinmameEndpointId))
      return;

   if (b2sEndpointId != 0)
   {
      if (b2sTracker != nullptr && b2sTracker->GetControllerEndpointId() == b2sEndpointId
         && b2sTracker->GetGameId() == b2sGameId)
         return;

      StopTracker();
      LOGI("No supported PinMAME map is active; monitoring direct B2S player scores for %s", b2sGameId.c_str());
      b2sTracker = new B2STracker();
      if (!b2sTracker->Start(stateSources.get(), b2sEndpointId, b2sGameId,
         ResolveTablePath(), outputFolderProp_Val, OnScoreSaved))
      {
         delete b2sTracker;
         b2sTracker = nullptr;
         return;
      }
      activeGameId = b2sGameId;
      pollActive = true;
      nextPollAt = std::chrono::steady_clock::now()
         + std::chrono::milliseconds(pollIntervalMsProp_Val);
   }
}

static void OnControllersChanged()
{
   if (shuttingDown)
      return;
   std::lock_guard lock(trackerMutex);
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
   if (shuttingDown || msgData == nullptr)
      return;
   std::lock_guard lock(trackerMutex);
   if (b2sTracker == nullptr)
      return;
   const B2SPluginEvent* event = static_cast<const B2SPluginEvent*>(msgData);
   b2sTracker->OnB2SStateChange(event->type, event->index, event->value);
}

#else

static void OnGameStart(const unsigned int eventId, void* userData, void* msgData)
{
   const CtlOnGameStateChgMsg* msg = static_cast<const CtlOnGameStateChgMsg*>(msgData);
   if (msg != nullptr && msg->gameId != nullptr)
   {
      std::lock_guard lock(trackerMutex);
      StartPinMameTrackerForGame(msg->gameId);
   }
}

static void OnGameEnd(const unsigned int eventId, void* userData, void* msgData)
{
   // Keep the session open: the controller can stop while the plugin has not confirmed the
   // game-over yet (its confirmation delay may still be running). The session is finalized,
   // and persisted if a game was played, when VPX ends or when another game starts.
}

#endif

static void OnStateSourcesAboutToChange()
{
   std::lock_guard lock(trackerMutex);
   if (tracker != nullptr)
      tracker->InvalidateStateSource();
#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
   if (b2sTracker != nullptr)
      b2sTracker->InvalidateStateSource();
#endif
}

static void OnStateSourcesChanged()
{
   if (shuttingDown)
      return;
   std::lock_guard lock(trackerMutex);
   if (tracker != nullptr)
      tracker->RefreshStateSource();
#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
   else
      RefreshControllers();
   if (b2sTracker != nullptr)
      b2sTracker->RefreshStateSource();
#endif
}

static void OnVpxGameEnd(const unsigned int eventId, void* userData, void* msgData)
{
   if (shuttingDown)
      return;
   std::lock_guard lock(trackerMutex);
   StopTracker();
}

}

using namespace ScoreTracker;

MSGPI_EXPORT void MSGPIAPI ScoreTrackerPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
   msgApi = api;
   endpointId = sessionId;
   shuttingDown = false;

   LPISetup(endpointId, msgApi);

   msgApi->RegisterSetting(endpointId, &mapsFolderProp);
   msgApi->RegisterSetting(endpointId, &pollIntervalMsProp);
   msgApi->RegisterSetting(endpointId, &outputFolderProp);
   msgApi->RegisterSetting(endpointId, &notificationsProp);

   msgApi->BroadcastMsg(endpointId, getVpxApiId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API), &vpxApi);
   stateSources = std::make_unique<StateSourceConsumer>(msgApi, endpointId,
      CTLPI_STATE_GET_SRC_MSG, CTLPI_STATE_ON_SRC_CHG_MSG, nullptr,
      OnStateSourcesAboutToChange, OnStateSourcesChanged);

#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
   controllerSources = std::make_unique<ControllerSourceConsumer>(msgApi, endpointId,
      CTLPI_CONTROLLERS_GET_MSG, CTLPI_CONTROLLERS_ON_CHG_MSG, nullptr, nullptr,
      OnControllersChanged);
   getPinmameMachineStateId = msgApi->GetMsgID("PinMAME", "GetMachineState");
   msgApi->SubscribeMsg(endpointId,
      onB2SStateChangeId = msgApi->GetMsgID("B2S", "OnStateChange"),
      OnB2SStateChange, nullptr);
#else
   msgApi->SubscribeMsg(endpointId, onGameStartId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_START), OnGameStart, nullptr);
   msgApi->SubscribeMsg(endpointId, onGameEndId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_END), OnGameEnd, nullptr);
#endif
   msgApi->SubscribeMsg(endpointId,
      onPrepareFrameId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME),
      OnPrepareFrame, nullptr);
   msgApi->SubscribeMsg(endpointId, onVpxGameEndId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END), OnVpxGameEnd, nullptr);

   // State sources must be available before controller discovery starts a tracker.
   stateSources->Subscribe();
#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
   controllerSources->Subscribe();
#endif
}

MSGPI_EXPORT void MSGPIAPI ScoreTrackerPluginUnload()
{
   shuttingDown = true;
   msgApi->UnsubscribeMsg(onPrepareFrameId, OnPrepareFrame, nullptr);
   msgApi->UnsubscribeMsg(onVpxGameEndId, OnVpxGameEnd, nullptr);
#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
   msgApi->UnsubscribeMsg(onB2SStateChangeId, OnB2SStateChange, nullptr);
#else
   msgApi->UnsubscribeMsg(onGameStartId, OnGameStart, nullptr);
   msgApi->UnsubscribeMsg(onGameEndId, OnGameEnd, nullptr);
#endif
   {
      std::lock_guard lock(trackerMutex);
      StopTracker();
   }
   notificationHidePending = false;
   notificationOverlay.Hide();

#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
   controllerSources->Unsubscribe();
#endif
   stateSources->Unsubscribe();
   msgApi->FlushPendingCallbacks(endpointId);

#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
   controllerSources.reset();
#endif
   stateSources.reset();

#if defined(CTLPI_CONTROLLERS_ON_CHG_MSG)
   msgApi->ReleaseMsgID(getPinmameMachineStateId);
   msgApi->ReleaseMsgID(onB2SStateChangeId);
#else
   msgApi->ReleaseMsgID(onGameStartId);
   msgApi->ReleaseMsgID(onGameEndId);
#endif
   msgApi->ReleaseMsgID(onPrepareFrameId);
   msgApi->ReleaseMsgID(onVpxGameEndId);
   msgApi->ReleaseMsgID(getVpxApiId);

   vpxApi = nullptr;
   msgApi = nullptr;
}
