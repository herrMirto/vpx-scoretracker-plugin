// license:GPLv3+
#include "common.h"
#include "ScoresFileWriter.h"
#include "ScriptRecorder.h"
#include "PinMameCapture.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/PluginStrings.h"
#include "plugins/VPXPlugin.h"
#include <deque>
#include <map>
#include <memory>
#include <set>

namespace ScoreTracker {
LPI_IMPLEMENT
static const MsgPluginAPI* msgApi = nullptr;
static VPXPluginAPI* vpxApi = nullptr;
static ScriptablePluginAPI* scriptApi = nullptr;
static uint32_t endpointId = 0;
static unsigned getVpxId, getScriptId, gameStartId, gameEndId, frameId, retryId;
using Controllers = PinballPlugin::Controller::CtrlItemConsumer<ControllerDef>;
using StateSources = PinballPlugin::Controller::CtrlItemConsumer<StateSrcId>;
static std::unique_ptr<Controllers> controllers;
static std::unique_ptr<StateSources> stateSources;
static bool stopping = false, tableActive = false;
static std::string cachedTablePath;
static std::deque<std::string> pendingNotices;
static std::set<std::string> announcedNotices, receivedResults;
static size_t savedGames = 0;
struct Machine { std::string game; PinMameCapture capture; };
static std::map<uint32_t, Machine> machines;
static std::map<std::string, CompletedGameRecord> pendingRecords;
static std::set<std::filesystem::path> outputDirectories;
MSGPI_STRING_VAL_SETTING(outputFolder, "OutputFolder", "Scores Output Folder",
   "Folder containing scores.json; empty writes beside the VPX table.", true, "", 1024);
MSGPI_BOOL_VAL_SETTING(notifications, "Notifications", "Notifications",
   "Show capture status, confirmed saves and storage failures in VPX.", true, true);

static std::string TablePath() {
   if (!tableActive || !vpxApi || !cachedTablePath.empty()) return cachedTablePath;
   VPXTableInfo info{}; vpxApi->GetTableInfo(&info);
   cachedTablePath = PluginStrings::PathToUTF8(PluginStrings::PathFromNative(info.path));
   return cachedTablePath;
}
static std::string OutputPath() {
   return PluginStrings::PathToUTF8(PluginStrings::PathFromUTF8OrNative(outputFolder_Val));
}
static void FlushNotices() {
   if (!notifications_Val) { pendingNotices.clear(); return; }
   if (stopping || !tableActive || !vpxApi || !vpxApi->PushNotification) return;
   while (!pendingNotices.empty()) {
      if (!vpxApi->PushNotification(pendingNotices.front().c_str(), 6000)) return;
      pendingNotices.pop_front();
   }
}
static void Notify(const std::string& key, const std::string& message) {
   if (!announcedNotices.insert(key).second) return;
   LOGI("%s", message.c_str());
   pendingNotices.push_back(message); FlushNotices();
}
static bool Save(const CompletedGameRecord& record) {
   receivedResults.insert(record.id);
   const auto directory = record.outputPath.empty() ? Utf8Path(record.tablePath).parent_path() : Utf8Path(record.outputPath);
   outputDirectories.insert(directory);
   StorageReport report;
   const bool committed = ScoresFileWriter::AppendCompletedGame(record, &report);
   savedGames += report.savedGames;
   if (!committed) {
      pendingRecords[record.id] = record;
      Notify("save-error:" + record.id, report.requestedGameSaved
         ? "ScoreTracker: score saved; failed to clean up pending files. See log."
         : "ScoreTracker: failed to save score. Result retained for retry; see log.");
      return false;
   }
   pendingRecords.erase(record.id);
   if (report.savedGames) {
      LOGI("Completed score saved: id=%s, history=%s", record.id.c_str(), PathUtf8(directory / "scores.json").c_str());
      Notify("saved:" + record.id, "ScoreTracker: score saved.");
   }
   return true;
}
static void RetryDirectory(const std::filesystem::path& directory) {
   StorageReport report;
   const bool recovered = ScoresFileWriter::RetryPending(directory, &report);
   savedGames += report.savedGames;
   if (report.savedGames)
      Notify("recovered:" + PathUtf8(directory) + ":" + std::to_string(savedGames),
         "ScoreTracker: recovered and saved " + std::to_string(report.savedGames) + " game(s).");
   if (!recovered) Notify("recovery-error:" + PathUtf8(directory), "ScoreTracker: failed to recover pending scores. See log.");
}
static void Retry(unsigned, void*, void*) {
   const auto pending = pendingRecords;
   for (const auto& [id, record] : pending) Save(record);
   if (!OutputPath().empty()) outputDirectories.insert(Utf8Path(OutputPath()));
   const auto table = TablePath();
   if (!table.empty()) outputDirectories.insert(Utf8Path(table).parent_path());
   for (const auto& directory : outputDirectories) RetryDirectory(directory);
}
static void ControllersChanged() {
   std::map<uint32_t, Machine> current;
   controllers->With([&](const std::vector<ControllerDef>& entries) {
      for (const auto& controller : entries) {
         const std::string game = controller.gameId ? controller.gameId : "";
         if (!game.starts_with("pinmame::")) continue;
         const auto found = machines.find(controller.endpointId);
         current.emplace(controller.endpointId, found != machines.end() && found->second.game == game
            ? found->second : Machine {game, {}});
      }
   });
   machines = std::move(current);
   if (stateSources) stateSources->Refresh();
}
static void OnFrame(unsigned, void*, void*) {
   if (stopping || !tableActive) return;
   try {
      if (stateSources) {
         for (auto& [endpoint, machine] : machines) {
            const NativeGameState sample = stateSources->With([&](const std::vector<StateSrcId>& sources) {
               return ReadNativeGameState(sources, endpoint);
            });
            if (sample.readable) Notify("native-ready:" + machine.game, "ScoreTracker: PinMAME capture active.");
            const bool wasActive = machine.capture.Active();
            const auto record = machine.capture.Observe(sample, UnixMilliseconds(), MonotonicMilliseconds(),
               machine.game.substr(9), TablePath(), OutputPath());
            if (!wasActive && machine.capture.Active())
               LOGI("Native game started: game=%s, players=%lld, ball=%lld", machine.game.c_str(),
                  static_cast<long long>(sample.players), static_cast<long long>(sample.ball));
            if (record) {
               LOGI("Native game ended: game=%s, players=%u, explicit game_over observed", machine.game.c_str(), unsigned(record->scores.size()));
               Save(*record);
            } else if (wasActive && !machine.capture.Active())
               LOGW("Native game discarded after an invalid state or restart: game=%s", machine.game.c_str());
         }
      }
   } catch (const std::exception& e) {
      LOGE("Native capture failed: %s", e.what());
      for (auto& [endpoint, machine] : machines) machine.capture.Reset();
      Notify("native-error", "ScoreTracker: failed to read game states. See log.");
   }
   FlushNotices();
}
static void OnTableStart(unsigned, void*, void*) {
   tableActive = true; cachedTablePath.clear(); ScriptRecorder::Enabled = true;
   for (auto& [endpoint, machine] : machines) machine.capture.Reset();
   const auto output = OutputPath(), table = TablePath();
   const auto directory = output.empty() ? Utf8Path(table).parent_path() : Utf8Path(output);
   StorageReport report;
   if (!ScoresFileWriter::CheckHistory(directory, &report))
      Notify("history:" + PathUtf8(directory), "ScoreTracker: scores.json is incompatible or unreadable; history preserved.");
   FlushNotices();
}
static void OnTableEnd(unsigned, void*, void*) {
   tableActive = false;
   for (auto& [endpoint, machine] : machines) machine.capture.Reset();
   Retry(0, nullptr, nullptr);
   LOGI("Table score summary: received=%u, saved=%u, memory_pending=%u",
      unsigned(receivedResults.size()), unsigned(savedGames), unsigned(pendingRecords.size()));
   if (receivedResults.empty()) LOGW("No completed game observed; closing the table does not create a score record");
   ScriptRecorder::Enabled = false; ++ScriptRecorder::Generation;
   pendingNotices.clear(); announcedNotices.clear(); receivedResults.clear(); savedGames = 0;
}
}
using namespace ScoreTracker;
MSGPI_EXPORT void MSGPIAPI ScoreTrackerPluginLoad(uint32_t sessionId, const MsgPluginAPI* api) {
   msgApi = api; endpointId = sessionId; stopping = false; tableActive = false;
   pendingNotices.clear(); announcedNotices.clear(); receivedResults.clear(); savedGames = 0;
   machines.clear(); outputDirectories.clear(); pendingRecords.clear(); cachedTablePath.clear();
   LPISetup(endpointId, msgApi);
   LOGI("ScoreTracker build: native-controller-capture-r1; public Controller API; no PinMAME patch or ROM filesystem path");
   msgApi->RegisterSetting(endpointId, &outputFolder); msgApi->RegisterSetting(endpointId, &notifications);
   msgApi->BroadcastMsg(endpointId, getVpxId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API), &vpxApi);
   msgApi->BroadcastMsg(endpointId, getScriptId = msgApi->GetMsgID(SCRIPTPI_NAMESPACE, SCRIPTPI_MSG_GET_API), &scriptApi);
   msgApi->SubscribeMsg(endpointId, gameEndId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END), OnTableEnd, nullptr);
   msgApi->SubscribeMsg(endpointId, retryId = msgApi->GetMsgID("ScoreTracker", "RetryPending:1"), Retry, nullptr);
   msgApi->SubscribeMsg(endpointId, gameStartId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_START), OnTableStart, nullptr);
   msgApi->SubscribeMsg(endpointId, frameId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME), OnFrame, nullptr);
   ScriptRecorder::Enabled = false; ++ScriptRecorder::Generation;
   if (scriptApi) RegisterRecorder(scriptApi, endpointId, []() {
      const auto table = TablePath(), output = OutputPath();
      const auto directory = output.empty() ? Utf8Path(table).parent_path() : Utf8Path(output);
      if (!directory.empty()) { outputDirectories.insert(directory); RetryDirectory(directory); }
      return new ScriptRecorder(table, output, ScriptRecorder::Generation, Save);
   });
   controllers = std::make_unique<Controllers>(msgApi, endpointId, CTLPI_CONTROLLERS_GET_MSG,
      CTLPI_CONTROLLERS_ON_CHG_MSG, nullptr, nullptr, ControllersChanged);
   controllers->Subscribe();
   stateSources = std::make_unique<StateSources>(msgApi, endpointId, CTLPI_STATE_GET_SRC_MSG,
      CTLPI_STATE_ON_SRC_CHG_MSG, [](std::vector<StateSrcId>& sources) {
         std::erase_if(sources, [](const StateSrcId& source) {
            if (!machines.contains(source.id.endpointId) || !source.stateDefs) return true;
            for (unsigned i = 0; i < source.nStates; ++i)
               if (source.stateDefs[i].desc && std::string_view(source.stateDefs[i].desc).starts_with("game_state\\")) return false;
            return true;
         });
      }, []() {
         for (auto& [endpoint, machine] : machines) machine.capture.Reset();
      }, nullptr);
   stateSources->Subscribe();
   Retry(0, nullptr, nullptr);
}
MSGPI_EXPORT void MSGPIAPI ScoreTrackerPluginUnload() {
   stopping = true; tableActive = false; Retry(0, nullptr, nullptr);
   if (!pendingRecords.empty()) LOGE("Unloading with %u completed results not confirmed saved; inspect the pending journal and preceding errors", unsigned(pendingRecords.size()));
   ScriptRecorder::Enabled = false; ++ScriptRecorder::Generation;
   if (scriptApi) UnregisterRecorder(scriptApi);
   stateSources->Unsubscribe(); stateSources.reset();
   controllers->Unsubscribe(); controllers.reset();
   msgApi->UnsubscribeMsg(gameStartId, OnTableStart, nullptr);
   msgApi->UnsubscribeMsg(gameEndId, OnTableEnd, nullptr);
   msgApi->UnsubscribeMsg(retryId, Retry, nullptr);
   msgApi->UnsubscribeMsg(frameId, OnFrame, nullptr);
   for (auto id : {getVpxId, getScriptId, gameStartId, gameEndId, frameId, retryId}) msgApi->ReleaseMsgID(id);
   machines.clear(); outputDirectories.clear(); pendingRecords.clear();
   pendingNotices.clear(); announcedNotices.clear(); receivedResults.clear();
   scriptApi = nullptr; vpxApi = nullptr; msgApi = nullptr;
}
