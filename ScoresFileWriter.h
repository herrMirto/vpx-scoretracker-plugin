// license:GPLv3+

#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

#include "common.h"

namespace ScoreTracker
{

struct CompletedGameRecord
{
   string tablePath;
   string outputPath; // When empty, scores.json is written next to the table file
   string rom;
   vector<int64_t> scores;
   int64_t gameDuration = 0;
   nlohmann::json gameState = nlohmann::json::object();
};

class ScoresFileWriter final
{
public:
   static bool AppendCompletedGame(const CompletedGameRecord& record);

private:
   static nlohmann::json BuildGameObject(const CompletedGameRecord& record);
   static string CurrentUtcTimestamp();
   static string BrokenBackupSuffix();
};

}
