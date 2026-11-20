// license:GPLv3+
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace ScoreTracker {
struct CompletedGameRecord {
   std::string id;
   std::string tablePath;
   std::string outputPath;
   std::string rom;
   std::string source;
   int64_t startedUnixMs = 0, endedUnixMs = 0, durationMs = 0;
   std::vector<int64_t> scores;
};
std::filesystem::path Utf8Path(const std::string& path);
std::string PathUtf8(const std::filesystem::path& path);
std::string NewGameId();
int64_t UnixMilliseconds();
int64_t MonotonicMilliseconds();
nlohmann::ordered_json GameJson(const CompletedGameRecord& game);
bool ValidGameJson(const nlohmann::json& game);
}
