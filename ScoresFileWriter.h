// license:GPLv3+

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ScoreTrackerPlugin {

struct CompletedGameRecord {
    std::string tablePath;
    std::string rom;
    std::vector<int64_t> scores;
    int64_t gameDuration = 0;
    nlohmann::json gameState = nlohmann::json::object();
};

class ScoresFileWriter {
public:
    static bool AppendCompletedGame(const CompletedGameRecord& record);

private:
    static nlohmann::json BuildGameObject(const CompletedGameRecord& record);
    static nlohmann::json BuildSignature(const nlohmann::json& root, const nlohmann::json& game);
    static std::string CurrentUtcTimestamp();
    static std::string BrokenBackupSuffix();
};

} // namespace ScoreTrackerPlugin
