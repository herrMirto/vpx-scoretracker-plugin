// license:GPLv3+
#pragma once
#include "GameRecord.h"
#include <cstddef>
namespace ScoreTracker {
struct StorageReport {
   std::size_t savedGames = 0;
   bool requestedGameSaved = false;
   std::string error;
   std::filesystem::path archivedHistory;
};
class ScoresFileWriter final {
public:
   // Success includes an identical record already durably stored (idempotent retry).
   static bool AppendCompletedGame(const CompletedGameRecord& record, StorageReport* report = nullptr);
   static bool RetryPending(const std::filesystem::path& directory, StorageReport* report = nullptr);
   static bool CheckHistory(const std::filesystem::path& directory, StorageReport* report = nullptr);
};
}
